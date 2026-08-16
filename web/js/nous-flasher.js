/**
 * Nous firmware flasher — X4 OTA flash via WebSerial.
 * Core flash engine originally by daveallie (esptool-js).
 * OTA slot logic adapted from the CrossPoint flash tool.
 */

let ESPLoader, Transport;

async function loadEsptool() {
  if (ESPLoader) return;
  const mod = await import('./esptool.bundle.js');
  ESPLoader = mod.ESPLoader;
  Transport = mod.Transport;
}

// --- CRC32 ---

const CRC32_TABLE = new Uint32Array(256);
(function () {
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    CRC32_TABLE[i] = c >>> 0;
  }
})();

function crc32(data, prev = 0) {
  let c = prev === 0 ? 0 : (prev ^ 0xFFFFFFFF) >>> 0;
  for (let i = 0; i < data.length; i++) c = CRC32_TABLE[(c ^ data[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

// --- Byte utils ---

function u32LE(v) { return new Uint8Array([v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF]); }
function readU32LE(b, o = 0) { return ((b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0); }
function eqBytes(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function otaCrc(seq) { return u32LE(crc32(u32LE(seq), 0xFFFFFFFF)); }

// --- MD5 (for partition table checksum row) ---

const MD5_K = new Uint32Array(64);
for (let i = 0; i < 64; i++) MD5_K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32);
const MD5_S = [7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21];

function md5(input) {
  const bl = input.length * 8;
  const pl = (Math.floor((input.length + 8) / 64) + 1) * 64;
  const msg = new Uint8Array(pl);
  msg.set(input);
  msg[input.length] = 0x80;
  msg.set(u32LE(bl >>> 0), pl - 8);
  msg.set(u32LE(Math.floor(bl / 2 ** 32)), pl - 4);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let chunk = 0; chunk < pl; chunk += 64) {
    for (let i = 0; i < 16; i++) M[i] = readU32LE(msg, chunk + i * 4);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      F = (F + A + MD5_K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      B = (B + ((F << MD5_S[i]) | (F >>> (32 - MD5_S[i])))) >>> 0;
    }
    a0=(a0+A)>>>0; b0=(b0+B)>>>0; c0=(c0+C)>>>0; d0=(d0+D)>>>0;
  }
  const out = new Uint8Array(16);
  out.set(u32LE(a0),0); out.set(u32LE(b0),4); out.set(u32LE(c0),8); out.set(u32LE(d0),12);
  return out;
}

// --- Firmware image validation ---

async function validateFirmware(data) {
  if (data.length < 24) throw new Error('Firmware too small.');
  if (data[0] !== 0xE9) throw new Error('Not a valid ESP firmware image (bad magic byte).');
  const segCount = data[1];
  const hashAppended = (data[23] & 0x01) !== 0;
  let xor = 0xEF, pos = 24;
  for (let i = 0; i < segCount; i++) {
    if (data.length - pos < 8) throw new Error('Firmware truncated (segment header).');
    const len = readU32LE(data, pos + 4);
    pos += 8;
    if (len > data.length - pos) throw new Error('Firmware truncated (segment data).');
    for (let j = pos, end = pos + len; j < end; j++) xor ^= data[j];
    pos += len;
  }
  const padEnd = (pos + 16) & ~15;
  const expected = padEnd + (hashAppended ? 32 : 0);
  if (expected !== data.length) throw new Error('Firmware size mismatch — file may be corrupt.');
  if ((xor & 0xFF) !== data[padEnd - 1]) throw new Error('Firmware checksum mismatch.');
  if (hashAppended) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', data.subarray(0, data.length - 32)));
    if (!eqBytes(digest, data.subarray(data.length - 32))) throw new Error('Firmware SHA-256 mismatch — file is corrupt.');
  }
}

// --- Partition table parsing ---

const PT_TYPES = {
  0x00: { 0x10: 'app-ota_0', 0x11: 'app-ota_1' },
  0x01: { 0x00: 'data-ota', 0x02: 'data-nvs', 0x03: 'data-coredump', 0x82: 'data-spiffs' },
};

function parsePartitionTable(data) {
  const parts = [];
  for (let o = 0; o < data.length; o += 32) {
    const c = data.slice(o, o + 32);
    if (c.length < 32 || c.every(b => b === 0xFF)) break;
    if (c[0] === 0xEB && c[1] === 0xEB) continue;
    parts.push({ type: PT_TYPES[c[2]]?.[c[3]] || 'unknown', offset: readU32LE(c, 4), size: readU32LE(c, 8) });
  }
  return parts;
}

function extractLayout(parts) {
  const otadata = parts.find(p => p.type === 'data-ota');
  const app0 = parts.find(p => p.type === 'app-ota_0');
  const app1 = parts.find(p => p.type === 'app-ota_1');
  if (!otadata || !app0 || !app1) throw new Error('Partition table missing required partitions. Is this an X4?');
  return { otadataOffset: otadata.offset, appSlots: [{ offset: app0.offset, size: app0.size }, { offset: app1.offset, size: app1.size }] };
}

// --- OTA slot logic ---

const OTA_SECTOR = 0x1000;
const OTA_SIZE = 0x2000;
const INVALID_OTA_STATES = new Set([3, 4]);

function parseOtaSlot(data, o) {
  const seq = readU32LE(data, o);
  const state = readU32LE(data, o + 0x18);
  const crcOk = eqBytes(data.slice(o + 0x1C, o + 0x20), otaCrc(seq));
  return { seq, state, crcOk };
}

function parseOtadata(data) {
  const s0 = parseOtaSlot(data, 0);
  const s1 = parseOtaSlot(data, 0x1000);
  const eligible = [];
  if (s0.seq !== 0xFFFFFFFF && s0.crcOk && !INVALID_OTA_STATES.has(s0.state)) eligible.push({ sector: 0, seq: s0.seq });
  if (s1.seq !== 0xFFFFFFFF && s1.crcOk && !INVALID_OTA_STATES.has(s1.state)) eligible.push({ sector: 1, seq: s1.seq });
  eligible.sort((a, b) => b.seq - a.seq);
  const activeSector = eligible.length ? eligible[0].sector : -1;
  const activeSeq = eligible.length ? eligible[0].seq : 0;
  const activeApp = eligible.length ? (activeSeq - 1) % 2 : 0;
  const inactiveApp = 1 - activeApp;
  let newSeq = activeSeq + 1;
  while (((newSeq - 1) % 2) !== inactiveApp) newSeq++;
  const targetSector = activeSector < 0 ? 0 : (1 - activeSector);
  return { activeApp, inactiveApp, activeSeq, newSeq, targetSector };
}

function buildNewOtaSector(existing, newSeq) {
  const d = new Uint8Array(existing);
  d.set(u32LE(newSeq), 0);
  d.set(u32LE(0), 0x18); // OTA_STATE_NEW
  d.set(otaCrc(newSeq), 0x1C);
  return d;
}

// --- Firmware download ---

export async function fetchNousFirmware(onStatus) {
  if (onStatus) onStatus('Fetching latest release info…');
  const meta = await fetch('https://api.github.com/repos/unitreign/nous/releases/latest').then(r => {
    if (!r.ok) throw new Error(`GitHub API error: ${r.status}`);
    return r.json();
  });
  const asset = meta.assets?.find(a => a.name.endsWith('.bin'));
  if (!asset) throw new Error('No .bin file found in the latest release.');
  if (onStatus) onStatus(`Downloading ${asset.name} (${(asset.size / 1024).toFixed(0)} KB)…`);
  const res = await fetch(asset.browser_download_url);
  if (!res.ok) throw new Error(`Firmware download failed: ${res.status}`);
  return new Uint8Array(await res.arrayBuffer());
}

// --- Main flash flow ---

const STEPS = [
  'Download firmware',
  'Connect to device',
  'Validate partition table',
  'Read OTA data',
  'Flash firmware',
  'Update boot partition',
  'Reset device',
];

export async function runFlash({ onStep, onProgress, onStatus }) {
  const step = (i, status) => { if (onStep) onStep(i, STEPS[i], status); };
  const progress = (label, done, total) => { if (onProgress) onProgress(label, done, total); };
  const status = (msg) => { if (onStatus) onStatus(msg); };

  await loadEsptool();

  // Step 0: download firmware
  step(0, 'running');
  const firmware = await fetchNousFirmware(status);
  await validateFirmware(firmware);
  step(0, 'done');

  // Step 1: connect — must be called synchronously from a click handler,
  // but we deferred here because loadEsptool() takes time. We call
  // requestPort() directly inside the click handler before this function
  // and pass the port in; see the note in index.html.
  step(1, 'running');
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303A, usbProductId: 0x1001 }] });
  const transport = new Transport(port, false);
  const espLoader = new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, enableTracing: false });
  await espLoader.main();
  step(1, 'done');

  try {
    // Step 2: read & validate partition table
    step(2, 'running');
    const ptData = await espLoader.readFlash(0x8000, 0x1000);
    const layout = extractLayout(parsePartitionTable(ptData));
    step(2, 'done');

    // Step 3: read OTA data
    step(3, 'running');
    const otaRaw = await espLoader.readFlash(layout.otadataOffset, OTA_SIZE, (_, p, t) => progress('Read OTA data', p, t));
    const ota = parseOtadata(otaRaw);
    step(3, 'done');

    // Step 4: flash firmware to inactive slot
    step(4, 'running');
    const destSlot = layout.appSlots[ota.inactiveApp];
    if (firmware.length > destSlot.size) throw new Error(`Firmware too large (${firmware.length} bytes) for slot (${destSlot.size} bytes).`);
    await espLoader.writeFlash({
      fileArray: [{ data: espLoader.ui8ToBstr(firmware), address: destSlot.offset }],
      flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep',
      eraseAll: false, compress: true,
      reportProgress: (_, w, t) => progress('Flash firmware', w, t),
    });
    step(4, 'done');

    // Step 5: update boot partition
    step(5, 'running');
    const sectorStart = ota.targetSector * OTA_SECTOR;
    const newSector = buildNewOtaSector(otaRaw.subarray(sectorStart, sectorStart + OTA_SECTOR), ota.newSeq);
    await espLoader.writeFlash({
      fileArray: [{ data: espLoader.ui8ToBstr(newSector), address: layout.otadataOffset + sectorStart }],
      flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep',
      eraseAll: false, compress: true,
      reportProgress: (_, w, t) => progress('Update boot partition', w, t),
    });
    step(5, 'done');

    // Step 6: reset
    step(6, 'running');
    try { await espLoader.transport.setDTR(false); } catch {}
    await espLoader.transport.setRTS(true);
    await new Promise(r => setTimeout(r, 100));
    await espLoader.after('hard_reset');
    try { await espLoader.transport.setDTR(false); await espLoader.transport.setRTS(false); } catch {}
    await espLoader.transport.disconnect();
    step(6, 'done');

  } catch (err) {
    try { await espLoader.transport.disconnect(); } catch {}
    throw err;
  }
}

export { STEPS };

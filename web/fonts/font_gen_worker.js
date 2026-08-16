// font_gen_worker.js - Web Worker for font generation and preview rendering.

let FG = null;
let modulePromise = null;
let taskChain = Promise.resolve();

function ensureModule() {
  if (FG) return Promise.resolve(FG);
  if (modulePromise) return modulePromise;

  modulePromise = import('./font_gen.js')
    .then(m => m.default({
      printErr: msg => postMessage({ type: 'log', msg }),
    }))
    .then(mod => {
      FG = mod;
      return mod;
    });

  return modulePromise;
}

function allocBytes(mod, bytes) {
  if (!bytes || bytes.length === 0) return [0, 0];
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  return [ptr, bytes.length];
}

function allocInt32Array(mod, values) {
  const ptr = mod._malloc(values.length * 4);
  new Int32Array(mod.HEAPU8.buffer, ptr, values.length).set(values);
  return ptr;
}

function allocUint32Array(mod, values) {
  const ptr = mod._malloc(values.length * 4);
  new Uint32Array(mod.HEAPU8.buffer, ptr, values.length).set(values);
  return ptr;
}

function parsePreviewResult(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
  if (magic !== 'FPRV') throw new Error('Invalid preview payload');

  const version = view.getUint16(4, true);
  if (version !== 2) throw new Error(`Unsupported preview version: ${version}`);

  const count = view.getUint16(6, true);
  let metaOffset = 8;
  const sections = [];
  for (let i = 0; i < count; i++) {
    sections.push({
      size: view.getUint16(metaOffset, true),
      width: view.getUint16(metaOffset + 2, true),
      height: view.getUint16(metaOffset + 4, true),
      styleId: view.getUint16(metaOffset + 6, true),
      length: view.getUint32(metaOffset + 8, true),
    });
    metaOffset += 12;
  }

  let payloadOffset = metaOffset;
  for (const section of sections) {
    section.pixels = bytes.slice(payloadOffset, payloadOffset + section.length);
    payloadOffset += section.length;
    delete section.length;
  }
  return { deviceSections: sections };
}

async function handleGenerate(data) {
  const mod = await ensureModule();
  const {
    regData, boldData, italData, biData, fbData,
    fontName, sizes, rangePairs, bwOnly,
  } = data;

  const [regPtr, regLen] = allocBytes(mod, regData);
  const [boldPtr, boldLen] = allocBytes(mod, boldData);
  const [italPtr, italLen] = allocBytes(mod, italData);
  const [biPtr, biLen] = allocBytes(mod, biData);
  const [fbPtr, fbLen] = allocBytes(mod, fbData);

  const nameEnc = new TextEncoder().encode(fontName.slice(0, 63) + '\0');
  const namePtr = mod._malloc(nameEnc.length);
  mod.HEAPU8.set(nameEnc, namePtr);

  const sizesPtr = allocInt32Array(mod, sizes);
  const flatRanges = rangePairs.flatMap(([lo, hi]) => [lo, hi]);
  const rangesPtr = allocUint32Array(mod, flatRanges);
  const outLenPtr = mod._malloc(4);
  new Int32Array(mod.HEAPU8.buffer, outLenPtr, 1).fill(0);

  try {
    const resultPtr = mod._font_gen_fnts_wasm(
      regPtr, regLen,
      boldPtr, boldLen,
      italPtr, italLen,
      biPtr, biLen,
      fbPtr, fbLen,
      namePtr,
      sizesPtr, sizes.length,
      rangesPtr, flatRanges.length / 2,
      bwOnly ? 1 : 0,
      outLenPtr,
    );

    const outLen = new Int32Array(mod.HEAPU8.buffer, outLenPtr, 1)[0];
    const result = (resultPtr && outLen > 0)
      ? mod.HEAPU8.slice(resultPtr, resultPtr + outLen)
      : null;

    if (resultPtr) mod._free(resultPtr);
    if (!result) throw new Error('Font generation returned no data');
    return result;
  } finally {
    mod._free(outLenPtr);
    mod._free(regPtr);
    mod._free(boldPtr);
    mod._free(italPtr);
    mod._free(biPtr);
    mod._free(fbPtr);
    mod._free(namePtr);
    mod._free(sizesPtr);
    mod._free(rangesPtr);
  }
}

async function handlePreview(data) {
  const mod = await ensureModule();
  const { regData, boldData, italData, biData, fbData, sizes, rangePairs, bwOnly, previewText } = data;

  const [regPtr, regLen] = allocBytes(mod, regData);
  const [boldPtr, boldLen] = allocBytes(mod, boldData);
  const [italPtr, italLen] = allocBytes(mod, italData);
  const [biPtr, biLen] = allocBytes(mod, biData);
  const [fbPtr, fbLen] = allocBytes(mod, fbData);
  const textEnc = new TextEncoder().encode((previewText || '') + '\0');
  const textPtr = mod._malloc(textEnc.length);
  mod.HEAPU8.set(textEnc, textPtr);
  const sizesPtr = allocInt32Array(mod, sizes);
  const flatRanges = rangePairs.flatMap(([lo, hi]) => [lo, hi]);
  const rangesPtr = allocUint32Array(mod, flatRanges);
  const outLenPtr = mod._malloc(4);
  new Int32Array(mod.HEAPU8.buffer, outLenPtr, 1).fill(0);

  try {
    const resultPtr = mod._font_gen_preview_wasm(
      regPtr, regLen,
      boldPtr, boldLen,
      italPtr, italLen,
      biPtr, biLen,
      fbPtr, fbLen,
      textPtr,
      sizesPtr, sizes.length,
      rangesPtr, flatRanges.length / 2,
      bwOnly ? 1 : 0,
      outLenPtr,
    );

    const outLen = new Int32Array(mod.HEAPU8.buffer, outLenPtr, 1)[0];
    const result = (resultPtr && outLen > 0)
      ? mod.HEAPU8.slice(resultPtr, resultPtr + outLen)
      : null;

    if (resultPtr) mod._free(resultPtr);
    if (!result) throw new Error('Preview generation returned no data');
    return parsePreviewResult(result);
  } finally {
    mod._free(outLenPtr);
    mod._free(regPtr);
    mod._free(boldPtr);
    mod._free(italPtr);
    mod._free(biPtr);
    mod._free(fbPtr);
    mod._free(textPtr);
    mod._free(sizesPtr);
    mod._free(rangesPtr);
  }
}

function enqueueTask(task) {
  const run = taskChain.then(task);
  taskChain = run.catch(() => {});
  return run;
}

onmessage = function (e) {
  const { type, requestId } = e.data || {};
  if (!type || !requestId) return;

  enqueueTask(async () => {
    try {
      const hadModule = !!FG;
      if (!hadModule) postMessage({ type: 'log', msg: 'Loading font generator WASM...' });
      const result = type === 'generate'
        ? await handleGenerate(e.data)
        : type === 'preview'
          ? await handlePreview(e.data)
          : (() => { throw new Error(`Unknown worker request: ${type}`); })();

      if (!hadModule) postMessage({ type: 'log', msg: 'Font generator ready.' });

      const transfer = [];
      if (result instanceof Uint8Array) {
        transfer.push(result.buffer);
      } else if (result && result.deviceSections) {
        for (const section of result.deviceSections) transfer.push(section.pixels.buffer);
      }

      postMessage({ type: 'result', requestId, result }, transfer);
    } catch (err) {
      postMessage({ type: 'error', requestId, msg: err.message || String(err) });
    }
  });
};

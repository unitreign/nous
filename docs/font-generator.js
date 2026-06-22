// ── Module-level state ─────────────────────────────────────────────────────────
let worker = null;
let fallbackFile = null;
const fontFiles = {};
let previewSeq = 0;
let previewTimer = null;
let workerRequestId = 1;
const pendingWorkerRequests = new Map();
const DEFAULT_PREVIEW_TEXT = `The quick brown fox jumps over the lazy dog.
Pack my box with five dozen liquor jugs.
How vexingly quick daft zebras jump!
It was the best of times, it was the worst of times.`;
const PREVIEW_STYLE_LABELS = ['Regular', 'Bold', 'Italic', 'Bold-Italic'];
const DEVICE_PREVIEW_SCALE = 1;
const SIZE_PRESETS = {
  small: [12, 14, 16, 18, 20, 22, 24],
  normal: [20, 22, 24, 26, 28, 30, 32],
  big: [28, 30, 32, 34, 36],
};

// ── Size chips ─────────────────────────────────────────────────────────
const selectedSizes = new Set(SIZE_PRESETS.normal);
const sizeChipsEl = document.getElementById('size-chips');
const sizePresetButtons = [...document.querySelectorAll('.size-preset-btn')];

function arraysEqual(a, b) {
  return a.length === b.length && a.every((value, index) => value === b[index]);
}

function updateSizePresetButtons() {
  const current = [...selectedSizes].sort((a, b) => a - b);
  for (const btn of sizePresetButtons) {
    const preset = SIZE_PRESETS[btn.dataset.preset] ?? [];
    btn.classList.toggle('active', arraysEqual(current, preset));
  }
}

function renderSizeChips() {
  sizeChipsEl.innerHTML = '';
  for (const s of [...selectedSizes].sort((a, b) => a - b)) {
    const chip = document.createElement('span');
    chip.className = 'chip';
    chip.innerHTML = `${s} <button class="chip-remove" title="Remove" data-size="${s}">x</button>`;
    chip.querySelector('.chip-remove').addEventListener('click', () => {
      selectedSizes.delete(s);
      renderSizeChips();
    });
    sizeChipsEl.appendChild(chip);
  }
  updateSizePresetButtons();
  scheduleLivePreview();
}

function setSizePreset(name) {
  const preset = SIZE_PRESETS[name];
  if (!preset) return;
  selectedSizes.clear();
  for (const size of preset) selectedSizes.add(size);
  renderSizeChips();
}

function addSize(val) {
  const n = parseInt(val, 10);
  if (!n || n < 8 || n > 64) return;
  if (selectedSizes.size >= 8) {
    alert('Maximum 8 sizes per bundle (device limit). Remove one before adding another.');
    return;
  }
  selectedSizes.add(n);
  renderSizeChips();
}

document.getElementById('size-add-btn').addEventListener('click', () => {
  const input = document.getElementById('size-input');
  addSize(input.value);
  input.value = '';
  input.focus();
});

document.getElementById('size-input').addEventListener('keydown', e => {
  if (e.key === 'Enter') document.getElementById('size-add-btn').click();
});

for (const btn of sizePresetButtons) {
  btn.addEventListener('click', () => setSizePreset(btn.dataset.preset));
}

renderSizeChips();
showPreviewSections(false);
document.getElementById('preview-text').value = DEFAULT_PREVIEW_TEXT;
document.getElementById('preview-text').addEventListener('input', () => scheduleLivePreview());

// ── Range presets ─────────────────────────────────────────────────────────
const RANGE_LABELS = [
  ['ascii',          'ASCII'],
  ['latin1',         'Latin-1'],
  ['latin-ext-a',    'Latin Extended-A'],
  ['latin-ext-b',    'Latin Extended-B'],
  ['latin-ext-add',  'Latin Extended Additional'],
  ['combining',      'Combining Diacritics'],
  ['spacing-mod',    'Spacing Modifiers'],
  ['general-punct',  'General Punctuation'],
  ['currency',       'Currency Symbols'],
  ['super-sub',      'Superscripts/Subscripts'],
  ['greek',          'Greek'],
  ['cyrillic',       'Cyrillic'],
  ['specials',       'Replacement/Specials'],
  ['hiragana',       'Hiragana'],
  ['katakana',       'Katakana'],
  ['cjk-punct',      'CJK Punctuation'],
  ['cjk',            'CJK Unified (main)'],
];

const DEFAULT_ON_RANGES = new Set([
  'ascii','latin1','latin-ext-a','general-punct','currency','super-sub','specials',
]);
const RANGE_PRESET_SETS = {
  western: ['ascii','latin1','latin-ext-a','general-punct','currency','super-sub','specials'],
  greek: ['ascii','latin1','latin-ext-a','general-punct','currency','super-sub','specials','greek'],
  cyrillic: ['ascii','latin1','general-punct','currency','super-sub','specials','cyrillic'],
  japanese: ['ascii','latin1','general-punct','specials','hiragana','katakana','cjk-punct','cjk'],
  all: [],
  clear: [],
};
const RANGE_GROUPS = [
  {
    title: 'Languages & Scripts',
    ids: ['ascii','latin1','latin-ext-a','latin-ext-b','latin-ext-add','greek','cyrillic','hiragana','katakana','cjk-punct','cjk'],
  },
  {
    title: 'Reading & Symbols',
    ids: ['combining','spacing-mod','general-punct','currency','super-sub','specials'],
  },
];

const rangeGrid = document.getElementById('range-grid');
const rangePresetButtons = [...document.querySelectorAll('.range-preset-btn')];
const rangeLabelMap = new Map(RANGE_LABELS);
const rangeMetaEl = document.getElementById('range-meta');
const manualRangeRowsEl = document.getElementById('manual-range-rows');
const manualRangesStatusEl = document.getElementById('manual-ranges-status');
let manualRangePairs = null;
for (const groupDef of RANGE_GROUPS) {
  const section = document.createElement('section');
  section.className = 'range-group';

  const title = document.createElement('div');
  title.className = 'range-group-title';
  title.textContent = groupDef.title;
  section.appendChild(title);

  const groupGrid = document.createElement('div');
  groupGrid.className = 'range-grid';

  for (const id of groupDef.ids) {
    const label = rangeLabelMap.get(id);
    if (!label) continue;
    const item = document.createElement('label');
    item.className = 'range-item';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.id = 'range-' + id;
    cb.checked = DEFAULT_ON_RANGES.has(id);
    cb.addEventListener('change', () => {
      if (manualRangePairs) disableManualRanges(false);
      updateRangePresetButtons();
      updateRangeMeta();
      scheduleLivePreview();
    });
    item.appendChild(cb);
    item.appendChild(document.createTextNode(label));
    groupGrid.appendChild(item);
  }

  section.appendChild(groupGrid);
  rangeGrid.appendChild(section);
}

RANGE_PRESET_SETS.all = RANGE_LABELS.map(([id]) => id);

function getSelectedRangeIds() {
  return RANGE_LABELS
    .map(([id]) => id)
    .filter(id => document.getElementById('range-' + id)?.checked);
}

function normalizeRangeIds(ids) {
  const wanted = new Set(ids);
  return RANGE_LABELS
    .map(([id]) => id)
    .filter(id => wanted.has(id));
}

function updateRangePresetButtons() {
  if (manualRangePairs) {
    for (const btn of rangePresetButtons) btn.classList.remove('active');
    return;
  }
  const current = getSelectedRangeIds();
  for (const btn of rangePresetButtons) {
    const preset = normalizeRangeIds(RANGE_PRESET_SETS[btn.dataset.rangePreset] ?? []);
    btn.classList.toggle('active', arraysEqual(current, preset));
  }
}

function mergeRangePairs(raw) {
  const sorted = [...raw].sort((a, b) => a[0] - b[0]);
  const merged = [];
  for (const [s, e] of sorted) {
    if (merged.length && s <= merged[merged.length - 1][1] + 1) {
      merged[merged.length - 1][1] = Math.max(merged[merged.length - 1][1], e);
    } else {
      merged.push([s, e]);
    }
  }
  return merged;
}

function countRangeCodepoints(ranges) {
  let total = 0;
  for (const [s, e] of ranges) total += (e - s + 1);
  return total;
}

function formatRangeHex(value) {
  return `U+${value.toString(16).toUpperCase().padStart(4, '0')}`;
}


function updateRangeMeta() {
  const ranges = getSelectedRanges();
  const codepoints = countRangeCodepoints(ranges);
  const mode = manualRangePairs ? 'Custom ranges active' : 'Checkbox ranges active';
  rangeMetaEl.textContent = `${mode} - ${ranges.length} block${ranges.length === 1 ? '' : 's'} - ${codepoints} codepoints`;
}

function setManualRangesStatus(msg, isError = false) {
  manualRangesStatusEl.textContent = msg;
  manualRangesStatusEl.classList.toggle('range-error', isError);
}

function formatCodepointPreview(cp) {
  if (cp < 0x20 || cp === 0x7F) return '(control)';
  return String.fromCodePoint(cp);
}

function parseHexCodepoint(value) {
  const cleaned = value.trim().replace(/^U\+/i, '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase();
  if (!cleaned) return null;
  const cp = parseInt(cleaned, 16);
  if (Number.isNaN(cp) || cp < 0 || cp > 0x10FFFF) return NaN;
  return cp;
}

function sanitizeHexInput(input) {
  input.value = input.value.replace(/^U\+/i, '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase().slice(0, 6);
}

function updateManualRangeRowPreview(row) {
  const fromInput = row.querySelector('.manual-range-from');
  const toInput = row.querySelector('.manual-range-to');
  const preview = row.querySelector('.manual-range-preview');
  const from = parseHexCodepoint(fromInput.value);
  const to = parseHexCodepoint(toInput.value);

  preview.classList.remove('invalid');
  if (!fromInput.value.trim() && !toInput.value.trim()) {
    preview.textContent = 'Empty row';
    return;
  }
  if (Number.isNaN(from) || Number.isNaN(to)) {
    preview.textContent = 'Invalid hex';
    preview.classList.add('invalid');
    return;
  }
  if (from === null) {
    preview.textContent = 'Enter a From value';
    preview.classList.add('invalid');
    return;
  }

  const end = to === null ? from : to;
  if (end < from) {
    preview.textContent = 'To must be >= From';
    preview.classList.add('invalid');
    return;
  }

  if (from === end) {
    preview.textContent = `${formatRangeHex(from)} ${formatCodepointPreview(from)}`;
    return;
  }
  preview.textContent = `${formatRangeHex(from)}-${formatRangeHex(end)} (${end - from + 1} cps)`;
}

function addManualRangeRow(fromValue = '', toValue = '') {
  const row = document.createElement('div');
  row.className = 'manual-range-row';

  const fromField = document.createElement('div');
  fromField.className = 'manual-range-field';
  const fromPrefix = document.createElement('span');
  fromPrefix.className = 'manual-range-prefix';
  fromPrefix.textContent = 'From U+';
  const fromInput = document.createElement('input');
  fromInput.type = 'text';
  fromInput.className = 'field-input manual-range-input manual-range-from';
  fromInput.placeholder = '0020';
  fromInput.value = fromValue;
  fromInput.addEventListener('input', () => {
    sanitizeHexInput(fromInput);
    updateManualRangeRowPreview(row);
  });
  fromField.appendChild(fromPrefix);
  fromField.appendChild(fromInput);

  const toField = document.createElement('div');
  toField.className = 'manual-range-field';
  const toPrefix = document.createElement('span');
  toPrefix.className = 'manual-range-prefix';
  toPrefix.textContent = 'To U+';
  const toInput = document.createElement('input');
  toInput.type = 'text';
  toInput.className = 'field-input manual-range-input manual-range-to';
  toInput.placeholder = '007E';
  toInput.value = toValue;
  toInput.addEventListener('input', () => {
    sanitizeHexInput(toInput);
    updateManualRangeRowPreview(row);
  });
  toField.appendChild(toPrefix);
  toField.appendChild(toInput);

  const preview = document.createElement('div');
  preview.className = 'manual-range-preview';

  const removeBtn = document.createElement('button');
  removeBtn.type = 'button';
  removeBtn.className = 'manual-range-remove';
  removeBtn.textContent = 'Remove';
  removeBtn.addEventListener('click', () => {
    row.remove();
    if (!manualRangeRowsEl.children.length) addManualRangeRow();
  });

  row.appendChild(fromField);
  row.appendChild(toField);
  row.appendChild(preview);
  row.appendChild(removeBtn);
  manualRangeRowsEl.appendChild(row);
  updateManualRangeRowPreview(row);
  return fromInput;
}

function getManualRangeRows() {
  return [...manualRangeRowsEl.querySelectorAll('.manual-range-row')];
}

function countFilledManualRangeRows() {
  return getManualRangeRows().filter(row => row.querySelector('.manual-range-from').value.trim()).length;
}

function disableManualRanges(clearText = false) {
  manualRangePairs = null;
  if (clearText) {
    manualRangeRowsEl.innerHTML = '';
    addManualRangeRow();
  }
  setManualRangesStatus(clearText ? '' : 'Custom range mode disabled. Using checkbox selection.');
}

function collectManualRangePairs() {
  const ranges = [];
  for (const row of getManualRangeRows()) {
    const fromInput = row.querySelector('.manual-range-from');
    const toInput = row.querySelector('.manual-range-to');
    const from = parseHexCodepoint(fromInput.value);
    const to = parseHexCodepoint(toInput.value);

    if (!fromInput.value.trim() && !toInput.value.trim()) continue;
    if (Number.isNaN(from) || Number.isNaN(to)) throw new Error('One or more custom range rows contain invalid hex.');
    if (from === null) throw new Error('Every custom range row needs a From value.');
    const end = to === null ? from : to;
    if (end < from) throw new Error('A custom range row has To smaller than From.');
    ranges.push([from, end]);
  }
  if (!ranges.length) throw new Error('Enter at least one custom range.');
  return mergeRangePairs(ranges);
}

function setRangePreset(name) {
  const preset = new Set(RANGE_PRESET_SETS[name] ?? []);
  disableManualRanges(false);
  for (const [id] of RANGE_LABELS) {
    const cb = document.getElementById('range-' + id);
    if (cb) cb.checked = preset.has(id);
  }
  updateRangePresetButtons();
  updateRangeMeta();
  scheduleLivePreview();
}

for (const btn of rangePresetButtons) {
  btn.addEventListener('click', () => setRangePreset(btn.dataset.rangePreset));
}
updateRangePresetButtons();

document.getElementById('manual-ranges-add').addEventListener('click', () => {
  addManualRangeRow().focus();
});

document.getElementById('manual-ranges-apply').addEventListener('click', () => {
  try {
    const parsed = collectManualRangePairs();
    const rowCount = countFilledManualRangeRows();
    manualRangePairs = parsed;
    for (const [id] of RANGE_LABELS) {
      const cb = document.getElementById('range-' + id);
      if (cb) cb.checked = false;
    }
    updateRangePresetButtons();
    updateRangeMeta();
    setManualRangesStatus(`Applied ${countRangeCodepoints(parsed)} codepoints from ${rowCount} row${rowCount === 1 ? '' : 's'}.`);
    scheduleLivePreview();
  } catch (err) {
    setManualRangesStatus(err.message, true);
  }
});

document.getElementById('manual-ranges-clear').addEventListener('click', () => {
  disableManualRanges(true);
  setRangePreset('western');
});

addManualRangeRow();

// ── File inputs ─────────────────────────────────────────────────────────
const STYLES = ['regular', 'bold', 'italic', 'bold-italic'];

function assignFontFile(style, file) {
  fontFiles[style] = file;
  const nameEl = document.getElementById('name-' + style);
  nameEl.textContent = file.name;
  nameEl.classList.remove('empty');
  scheduleLivePreview();
}

// Folder picker: score each .ttf/.otf file against each style slot
document.getElementById('btn-browse-folder').addEventListener('click', () => {
  document.getElementById('file-folder').click();
});

document.getElementById('file-folder').addEventListener('change', function () {
  const files = [...this.files].filter(f => /\.(ttf|otf)$/i.test(f.name));
  if (!files.length) return;

  // Score function: higher = better match for that style
  function score(name, style) {
    const n = name.toLowerCase().replace(/[-_ ]/g, '');
    const isBold   = /bold/.test(n);
    const isItalic = /italic|oblique/.test(n);
    if (style === 'regular')     return (!isBold && !isItalic) ? 2 : (/regular|normal/.test(n) ? 1 : 0);
    if (style === 'bold')        return (isBold && !isItalic)  ? 2 : 0;
    if (style === 'italic')      return (!isBold && isItalic)  ? 2 : 0;
    if (style === 'bold-italic') return (isBold && isItalic)   ? 2 : 0;
    return 0;
  }

  const slots = ['regular', 'bold', 'italic', 'bold-italic'];
  const used = new Set();

  for (const slot of slots) {
    let best = null, bestScore = -1;
    for (const file of files) {
      if (used.has(file)) continue;
      const s = score(file.name, slot);
      if (s > bestScore) { best = file; bestScore = s; }
    }
    if (best && bestScore > 0) {
      assignFontFile(slot, best);
      used.add(best);
    }
  }

  // Auto-fill font name from the regular file
  if (fontFiles['regular'] && !document.getElementById('input-font-name').value) {
    let name = fontFiles['regular'].name.replace(/\.(ttf|otf)$/i, '');
    name = name.replace(/[-_ ](regular|bold|italic|bolditalic|bold-italic)$/i, '');
    document.getElementById('input-font-name').value = name;
  }

  updateGenerateBtn();
});

for (const style of STYLES) {
  const btn = document.querySelector(`.font-file-btn[data-style="${style}"]`);
  const input = document.getElementById('file-' + style);
  const nameEl = document.getElementById('name-' + style);

  btn.addEventListener('click', () => input.click());
  input.addEventListener('change', () => {
    const file = input.files[0];
    if (!file) return;
    assignFontFile(style, file);
    if (style === 'regular' && !document.getElementById('input-font-name').value) {
      let name = file.name.replace(/\.(ttf|otf)$/i, '');
      name = name.replace(/[-_ ](regular|bold|italic|bolditalic|bold-italic)$/i, '');
      document.getElementById('input-font-name').value = name;
    }
    updateGenerateBtn();
  });
}

// ── Fallback font file picker ─────────────────────────────────────────────────────────
document.getElementById('btn-fallback').addEventListener('click', () => document.getElementById('file-fallback').click());
document.getElementById('file-fallback').addEventListener('change', function () {
  const file = this.files[0];
  if (!file) return;
  fallbackFile = file;
  const nameEl = document.getElementById('name-fallback');
  nameEl.textContent = file.name;
  nameEl.classList.remove('empty');
  scheduleLivePreview();
});

function updateGenerateBtn() {
  document.getElementById('btn-generate').disabled = !fontFiles['regular'];
}

document.getElementById('cb-bw-only').addEventListener('change', () => {
  scheduleLivePreview();
});

// ── UI helpers ─────────────────────────────────────────────────────────
function log(msg) {
  const ts = new Date().toLocaleTimeString();
  const box = document.getElementById('log-box');
  box.textContent += `[${ts}] ${msg}\n`;
  box.scrollTop = box.scrollHeight;
}
function setStatus(msg) {
  const statusEl = document.getElementById('status-bar');
  statusEl.textContent = msg;
  statusEl.classList.remove('status-warning', 'status-error', 'status-ok');
  if (!msg) return;
  if (msg.startsWith('WARNING:')) {
    statusEl.classList.add('status-warning');
  } else if (msg.startsWith('Error:')) {
    statusEl.classList.add('status-error');
  } else if (msg.startsWith('OK:')) {
    statusEl.classList.add('status-ok');
  }
}
function setProgress(pct, label) {
  document.getElementById('progress-fill').style.width = Number(pct).toFixed(1) + '%';
  if (label !== undefined) document.getElementById('progress-label').textContent = label;
}
function showProgress(visible) {
  document.getElementById('progress-wrap').classList.toggle('visible', visible);
}

function showPreviewSections(visible) {
  document.getElementById('device-preview-empty').style.display = visible ? 'none' : '';
  document.getElementById('device-preview-grid').style.display = visible ? 'flex' : 'none';
}

function quantizePreviewValue(whiteLevel, bwOnly) {
  if (bwOnly) return whiteLevel >= 154 ? 255 : 0;
  if (whiteLevel >= 205) return 255;
  if (whiteLevel >= 154) return 200;
  if (whiteLevel >= 103) return 140;
  if (whiteLevel >= 52)  return 80;
  return 0;
}

function paintPreviewImage(canvas, image, scale = 1, displayScale = scale) {
  canvas.width = image.width * scale;
  canvas.height = image.height * scale;
  canvas.style.width = (image.width * displayScale) + 'px';
  canvas.style.height = (image.height * displayScale) + 'px';
  canvas.style.imageRendering = 'pixelated';
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = false;
  const img = ctx.createImageData(canvas.width, canvas.height);
  const data = img.data;
  for (let y = 0; y < image.height; y++) {
    for (let x = 0; x < image.width; x++) {
      const value = image.pixels[y * image.width + x];
      for (let sy = 0; sy < scale; sy++) {
        for (let sx = 0; sx < scale; sx++) {
          const dx = x * scale + sx;
          const dy = y * scale + sy;
          const i = (dy * canvas.width + dx) * 4;
          data[i] = data[i + 1] = data[i + 2] = value;
          data[i + 3] = 255;
        }
      }
    }
  }
  ctx.putImageData(img, 0, 0);
}

async function renderLivePreview(seq) {
  if (!fontFiles['regular']) {
    showPreviewSections(false);
    return;
  }
  try {
    const sizes = [...selectedSizes].sort((a, b) => a - b);
    if (!sizes.length) {
      showPreviewSections(false);
      return;
    }
    const ranges = getSelectedRanges();
    if (!ranges.length) {
      showPreviewSections(false);
      return;
    }

    const readFile = f => f ? f.arrayBuffer().then(b => new Uint8Array(b)) : Promise.resolve(new Uint8Array(0));
    const [regData, boldData, italData, biData, fbData] = await Promise.all([
      readFile(fontFiles['regular']),
      readFile(fontFiles['bold']),
      readFile(fontFiles['italic']),
      readFile(fontFiles['bold-italic']),
      readFile(fallbackFile),
    ]);
    if (seq !== previewSeq) return;

    const preview = await callPreview(
      regData,
      boldData,
      italData,
      biData,
      fbData,
      sizes,
      ranges,
      document.getElementById('cb-bw-only').checked,
      document.getElementById('preview-text').value
    );
    if (seq !== previewSeq) return;

    const grid = document.getElementById('device-preview-grid');
    grid.innerHTML = '';
    const groupedSections = new Map();
    for (const section of preview.deviceSections) {
      if (!groupedSections.has(section.size)) groupedSections.set(section.size, []);
      groupedSections.get(section.size).push(section);
    }

    for (const [size, sections] of groupedSections) {
      const group = document.createElement('section');
      group.className = 'preview-size-group';

      const title = document.createElement('div');
      title.className = 'preview-size-title';
      title.textContent = `${size}px`;
      group.appendChild(title);

      const strip = document.createElement('div');
      strip.className = 'preview-style-strip';

      sections.sort((a, b) => a.styleId - b.styleId);
      for (const section of sections) {
        const wrapper = document.createElement('div');
        wrapper.className = 'preview-swatch';

        const label = document.createElement('div');
        const styleLabel = PREVIEW_STYLE_LABELS[section.styleId] || `Style ${section.styleId}`;
        label.textContent = styleLabel;
        label.className = 'preview-label';
        wrapper.appendChild(label);

        const viewport = document.createElement('div');
        viewport.className = 'preview-viewport';

        const canvas = document.createElement('canvas');
        canvas.style.display = 'block';
        paintPreviewImage(canvas, section, 1, DEVICE_PREVIEW_SCALE);
        viewport.appendChild(canvas);
        wrapper.appendChild(viewport);
        strip.appendChild(wrapper);
      }

      group.appendChild(strip);
      grid.appendChild(group);
    }
    showPreviewSections(preview.deviceSections.length > 0);
  } catch (err) {
    showPreviewSections(false);
    console.warn('Live preview failed:', err);
  }
}

function scheduleLivePreview() {
  previewSeq += 1;
  const seq = previewSeq;
  if (previewTimer) clearTimeout(previewTimer);
  previewTimer = window.setTimeout(() => {
    void renderLivePreview(seq);
  }, 60);
}

// ── Worker communication ─────────────────────────────────────────────────────────
// ── Range presets ─────────────────────────────────────────────────────────
const RANGE_PRESETS = {
  'ascii':         [[0x0020, 0x007E]],
  'latin1':        [[0x00A0, 0x00FF]],
  'latin-ext-a':   [[0x0100, 0x017F]],
  'latin-ext-b':   [[0x0180, 0x024F]],
  'latin-ext-add': [[0x1E00, 0x1EFF]],
  'combining':     [[0x0300, 0x036F]],
  'spacing-mod':   [[0x02B0, 0x02FF]],
  'greek':         [[0x0370, 0x03FF]],
  'cyrillic':      [[0x0400, 0x04FF]],
  'general-punct': [[0x2000, 0x206F]],
  'super-sub':     [[0x2070, 0x209F]],
  'currency':      [[0x20A0, 0x20CF]],
  'specials':      [[0xFFF0, 0xFFFF]],
  'hiragana':      [[0x3040, 0x309F]],
  'katakana':      [[0x30A0, 0x30FF]],
  'cjk-punct':     [[0x3000, 0x303F]],
  'cjk':           [[0x4E00, 0x9FFF]],
};

function getSelectedRanges() {
  if (manualRangePairs) return manualRangePairs.map(([s, e]) => [s, e]);
  const raw = [];
  for (const [id] of RANGE_LABELS) {
    if (document.getElementById('range-' + id)?.checked)
      raw.push(...(RANGE_PRESETS[id] ?? []));
  }
  return mergeRangePairs(raw);
}

updateRangeMeta();

function ensureWorker() {
  if (worker) return;
  worker = new Worker('./font_gen_worker.js', { type: 'module' });
  worker.addEventListener('message', e => {
    const { type, requestId } = e.data;
    if (type === 'log') {
      log(e.data.msg);
      return;
    }
    if (type === 'progress') {
      setProgress(e.data.pct, e.data.label);
      return;
    }
    if (type !== 'result' && type !== 'error') return;
    const pending = pendingWorkerRequests.get(requestId);
    if (!pending) return;
    pendingWorkerRequests.delete(requestId);
    if (type === 'result') pending.resolve(e.data.result);
    else pending.reject(new Error(e.data.msg));
  });
}

function callWorker(type, payload) {
  ensureWorker();
  const requestId = `req-${workerRequestId++}`;
  return new Promise((resolve, reject) => {
    pendingWorkerRequests.set(requestId, { resolve, reject });
    worker.postMessage({ type, requestId, ...payload });
  });
}

function callFontGen(regData, boldData, italData, biData, fbData, fontName, sizes, rangePairs, bwOnly) {
  return callWorker('generate', {
    regData, boldData, italData, biData, fbData,
    fontName, sizes, rangePairs, bwOnly
  });
}

function callPreview(regData, boldData, italData, biData, fbData, sizes, rangePairs, bwOnly, previewText) {
  return callWorker('preview', {
    regData, boldData, italData, biData, fbData, sizes, rangePairs, bwOnly, previewText
  });
}

// ── MBF / FNTS parser ─────────────────────────────────────────────────────────
function parseFnts(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset);
  const num = bytes[4];
  const mbfSizes = [];
  for (let i = 0; i < num; i++) mbfSizes.push(v.getUint32(40 + i * 4, true));
  let off = 40 + num * 4;
  return mbfSizes.map(sz => { const m = bytes.slice(off, off + sz); off += sz; return m; });
}

function parseMbf(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset);
  const nr      = v.getUint16(10, true);
  const ng      = v.getUint16(12, true);
  const pxSize  = v.getUint16(14, true);
  const bdo     = v.getUint32(20, true);
  const lsbOff  = v.getUint32(40, true);
  const msbOff  = v.getUint32(44, true);
  const baseline = bytes[6];
  const yAdvance = bytes[7];

  const ranges = [];
  for (let i = 0; i < nr; i++) {
    const o = 50 + i * 8;
    ranges.push({ fcp: v.getUint32(o, true), cnt: v.getUint16(o+4, true), gs: v.getUint16(o+6, true) });
  }
  const glyphs = [];
  const goff = 50 + nr * 8;
  for (let i = 0; i < ng; i++) {
    const o = goff + i * 10;
    glyphs.push({ bmpOff: v.getUint32(o, true), xadv: bytes[o+4],
                  bw: bytes[o+5], bh: bytes[o+6], xo: v.getInt8(o+7), yo: v.getInt8(o+8) });
  }
  const cpMap = new Map();
  for (const { fcp, cnt, gs } of ranges)
    for (let j = 0; j < cnt; j++) cpMap.set(fcp + j, gs + j);
  return { pxSize, bdo, lsbOff, msbOff, baseline, yAdvance, ranges, glyphs, cpMap };
}

// Decode one glyph's pixels from MBF byte array.
// Returns { pixels: Uint8Array (0=black ... 255=white), w, h } or null.
function decodeMbfGlyph(mbfData, mbf, glyphIdx) {
  const g = mbf.glyphs[glyphIdx];
  if (!g || g.bw === 0 || g.bh === 0) return null;
  const stride = Math.ceil(g.bw / 8);
  const bmpOff = g.bmpOff;
  const hasGray = mbf.lsbOff && mbf.msbOff;
  const pixels = new Uint8Array(g.bw * g.bh);
  for (let y = 0; y < g.bh; y++) {
    for (let x = 0; x < g.bw; x++) {
      const bi   = y * stride + (x >> 3);
      const mask = 0x80 >> (x & 7);
      const bwBit = (mbfData[mbf.bdo + bmpOff + bi] & mask) !== 0; // 1=white/bg
      if (!bwBit) {
        pixels[y * g.bw + x] = 0;   // black
      } else if (!hasGray) {
        pixels[y * g.bw + x] = 255; // BW-only: white
      } else {
        const lsb = (mbfData[mbf.lsbOff + bmpOff + bi] & mask) !== 0;
        const msb = (mbfData[mbf.msbOff + bmpOff + bi] & mask) !== 0;
        pixels[y * g.bw + x] = [255, 200, 140, 80][(msb ? 2 : 0) | (lsb ? 1 : 0)];
      }
    }
  }
  return { pixels, w: g.bw, h: g.bh };
}

// ── Glyph grid preview ─────────────────────────────────────────────────────────
const SAMPLE_CHARS = 'ABCDEFGHIJKLabcdefghijkl0123456789';

function drawGlyphPreview(mbfData, mbf, canvasId, scale) {
  const PAD = 2;
  const glyphs = [];
  for (const ch of SAMPLE_CHARS) {
    const gi = mbf.cpMap.get(ch.codePointAt(0));
    if (gi === undefined) continue;
    const d = decodeMbfGlyph(mbfData, mbf, gi);
    if (d) glyphs.push(d);
  }
  if (!glyphs.length) return;

  const maxH   = Math.max(...glyphs.map(g => g.h), 1);
  const totalW = glyphs.reduce((s, g) => s + g.w + PAD, PAD);
  const canvas = document.getElementById(canvasId);
  canvas.width  = totalW * scale;
  canvas.height = maxH  * scale;
  canvas.style.width  = (totalW * scale) + 'px';
  canvas.style.height = (maxH  * scale) + 'px';

  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(canvas.width, canvas.height);
  const d = img.data;
  for (let i = 0; i < d.length; i += 4) { d[i]=d[i+1]=d[i+2]=255; d[i+3]=255; }
  let cx = PAD;
  for (const { pixels, w, h } of glyphs) {
    const cy = Math.floor((maxH - h) / 2);
    for (let y = 0; y < h; y++)
      for (let x = 0; x < w; x++) {
        const v = pixels[y * w + x];
        for (let sy = 0; sy < scale; sy++)
          for (let sx = 0; sx < scale; sx++) {
            const pi = (((cy+y)*scale+sy)*canvas.width + (cx+x)*scale+sx) * 4;
            d[pi]=d[pi+1]=d[pi+2]=v; d[pi+3]=255;
          }
      }
    cx += w + PAD;
  }
  ctx.putImageData(img, 0, 0);
}

// ── Device preview from MBF ─────────────────────────────────────────────────────────
const DEVICE_SAMPLE = 'The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs. How vexingly quick daft zebras jump! It was the best of times, it was the worst of times, it was the age of wisdom, it was the age of foolishness.';

function buildDevicePreviewCanvas(mbfBufs) {
  const W = 480, MX = 24, SECTION_GAP = 20, LABEL_H = 14;

  const sections = mbfBufs.map(bytes => {
    const mbf = parseMbf(bytes);
    const spaceIdx = mbf.cpMap.get(0x20);
    const spaceAdv = spaceIdx !== undefined ? mbf.glyphs[spaceIdx].xadv / 4 : mbf.pxSize * 0.3;
    const maxW = W - MX * 2;

    const advOf = cp => {
      const gi = mbf.cpMap.get(cp);
      return gi !== undefined ? mbf.glyphs[gi].xadv / 4 : mbf.pxSize * 0.5;
    };

    const lines = [];
    let curLine = [], lineX = 0;
    for (const tok of DEVICE_SAMPLE.split(' ')) {
      const wW = [...tok].reduce((s, ch) => s + advOf(ch.codePointAt(0)), 0);
      if (curLine.length && lineX + spaceAdv + wW > maxW) { lines.push(curLine); curLine = []; lineX = 0; }
      if (curLine.length) lineX += spaceAdv;
      let gx = lineX;
      for (const ch of tok) {
        const cp = ch.codePointAt(0);
        const gi = mbf.cpMap.get(cp);
        if (gi !== undefined) { curLine.push({ gi, x: gx + MX }); }
        gx += advOf(cp);
      }
      lineX += wW;
    }
    if (curLine.length) lines.push(curLine);
    return { mbf, bytes, lines };
  });

  let totalH = SECTION_GAP;
  for (const { mbf, lines } of sections)
    totalH += LABEL_H + 4 + lines.length * mbf.yAdvance + SECTION_GAP;

  const canvas = document.createElement('canvas');
  canvas.width = W; canvas.height = totalH;
  canvas.style.cssText = `width:${W}px;height:${totalH}px;display:block;border:1px solid var(--border-default);border-radius:4px`;

  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, W, totalH);
  const imgData = ctx.getImageData(0, 0, W, totalH);
  const d = imgData.data;
  const labels = [];

  let curY = SECTION_GAP;
  for (const { mbf, bytes, lines } of sections) {
    labels.push({ text: `${mbf.pxSize}px`, y: curY + LABEL_H - 2 });
    curY += LABEL_H + 4;
    for (const glyphs of lines) {
      const base = curY + mbf.baseline;
      for (const { gi, x } of glyphs) {
        const g = mbf.glyphs[gi];
        if (!g || !g.bw || !g.bh) continue;
        const dec = decodeMbfGlyph(bytes, mbf, gi);
        if (!dec) continue;
        const gx = x + g.xo, gy = base + g.yo;
        for (let py = 0; py < dec.h; py++)
          for (let px = 0; px < dec.w; px++) {
            const dx = gx + px, dy = gy + py;
            if (dx < 0 || dx >= W || dy < 0 || dy >= totalH) continue;
            const v = dec.pixels[py * dec.w + px];
            if (v >= 255) continue;
            const i = (dy * W + dx) * 4;
            if (v < d[i]) { d[i]=d[i+1]=d[i+2]=v; }
          }
      }
      curY += mbf.yAdvance;
    }
    curY += SECTION_GAP;
  }
  ctx.putImageData(imgData, 0, 0);
  ctx.fillStyle = '#bbb';
  ctx.font = '11px system-ui,sans-serif';
  for (const label of labels) ctx.fillText(label.text, MX, label.y);
  return canvas;
}

// ── Download helper ─────────────────────────────────────────────────────────
let resultBytes = null, resultFilename = 'font.mfb';

document.getElementById('btn-download').addEventListener('click', () => {
  if (!resultBytes) return;
  const url = URL.createObjectURL(new Blob([resultBytes], { type: 'application/octet-stream' }));
  const a = Object.assign(document.createElement('a'), { href: url, download: resultFilename });
  a.click();
  URL.revokeObjectURL(url);
});

// ── Generate ─────────────────────────────────────────────────────────
document.getElementById('btn-generate').addEventListener('click', async () => {
  const btn   = document.getElementById('btn-generate');
  const dlBtn = document.getElementById('btn-download');
  btn.disabled = true;
  dlBtn.style.display = 'none';
  resultBytes = null;
  showProgress(true);

  try {
    const bwOnly   = document.getElementById('cb-bw-only').checked;
    const sizes    = [...selectedSizes].sort((a, b) => a - b);
    const ranges   = getSelectedRanges();
    const fontName = document.getElementById('input-font-name').value.trim() || 'Font';

    if (!fontFiles['regular']) throw new Error('Regular font file required');
    if (!sizes.length)         throw new Error('Select at least one size');
    if (!ranges.length)        throw new Error('Select at least one character range');

    const readFile = f => f ? f.arrayBuffer().then(b => new Uint8Array(b)) : Promise.resolve(new Uint8Array(0));

    log('Reading font files...');
    setProgress(0, 'Reading font files...');
    const [regData, boldData, italData, biData, fbData] = await Promise.all([
      readFile(fontFiles['regular']),
      readFile(fontFiles['bold']),
      readFile(fontFiles['italic']),
      readFile(fontFiles['bold-italic']),
      readFile(fallbackFile),
    ]);
    log(`Regular: ${fontFiles['regular'].name} (${(regData.length/1024).toFixed(0)} KB)`);
    if (boldData.length) log(`Bold: ${fontFiles['bold'].name}`);
    if (italData.length) log(`Italic: ${fontFiles['italic'].name}`);
    if (biData.length)   log(`Bold-Italic: ${fontFiles['bold-italic'].name}`);
    if (fbData.length)   log(`Fallback: ${fallbackFile.name}`);

    setProgress(3, 'Starting generation...');
    const fntsBytes = await callFontGen(regData, boldData, italData, biData, fbData,
                                        fontName, sizes, ranges, bwOnly);

    const SPIFFS_MAX = 0x360000;
    const bundleKB   = (fntsBytes.length / 1024).toFixed(1);
    const maxKB      = (SPIFFS_MAX / 1024).toFixed(0);
    if (fntsBytes.length > SPIFFS_MAX)
      log(`WARNING: FNTS: ${bundleKB} KB - exceeds size limit (${maxKB} KB)`);
    else
      log(`FNTS bundle: ${bundleKB} KB / ${maxKB} KB max - ${((fntsBytes.length/SPIFFS_MAX)*100).toFixed(0)}% used`);

    scheduleLivePreview();

    resultBytes    = fntsBytes;
    resultFilename = fontName + '.mfb';
    dlBtn.style.display = '';
    setProgress(100, 'Done');
    setStatus(fntsBytes.length > SPIFFS_MAX
      ? `WARNING: ${resultFilename} - ${bundleKB} KB - exceeds size limit`
      : `OK: ${resultFilename} - ${sizes.length} size${sizes.length>1?'s':''}, ${bundleKB} KB`);
    log(`Done - click Download to save ${resultFilename}`);

  } catch (e) {
    log('Error: ' + e.message);
    setStatus('Error: ' + e.message);
    console.error(e);
  } finally {
    btn.disabled = !fontFiles['regular'];
    showProgress(false);
  }
});
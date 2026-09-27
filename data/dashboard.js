/*
 * Copyright (c) 2026 Alexander Fuchs / Bits Und Bolts
 * Licensed under the MIT License.
 *
 * dashboard.js - behaviour of the HAWK 370 dashboard (index.html): CPU family /
 * base clock with APPLY, throttle readout and slider, % buttons, user presets,
 * power-rail graphs, device controls, SSE live updates.
 * index.html holds only the markup and styles; hawk370.js the shared helpers.
 */
/* ── THROTTLE: CPU FAMILY / BASE MHZ / SPEED SLIDER ─────────────────── */
let cpuFamilies     = [];
let selectedFamily  = null;
let cpuMenuOpen     = false;
let baseMhzCache    = 300;
let deviceFamily    = null;    // family / base clock the device runs with
let deviceBaseMhz   = null;    //   (the page may hold a staged change until APPLY)
let speedSendTimer  = null;
let speedSeq        = 0;       // only the newest response may repaint
let localChangeAt   = 0;       // ignore SSE echoes while the user is dragging
let delivered       = null;    // { mhz, pct, paused } as reported by the firmware
let inFlight        = 0;       // requests from this browser not answered yet
let lastRev         = 0;       // newest firmware state revision applied
let lastBootId      = null;    // revisions restart when the device reboots
let userPresets     = [];      // [{name, mhz, refBaseMhz}] for the selected family, A-Z
let presetMax       = 100;     // slots shared by all families
let presetTotal     = 0;       // used slots, all families
let presetMenuOpen  = false;
let presetFocus     = -1;      // keyboard-highlighted row in the filtered list
let presetEditing   = null;    // original name while editing, null for a new preset
const SLIDER_DEBOUNCE_MS = 250;   // slider / arrow keys: send once they rest this long

// targetMhz is the single source of truth for "what speed is commanded"
// in this UI. The wire format is still speedPercent (0-100, now a
// float — see /api/throttle/speed) so we convert both ways rather than
// storing percent as the source of truth, since the whole point of this
// UI is to let people think and type in MHz, not percent.
let targetMhz = 0;

function clampMhz(mhz) {
  if (!Number.isFinite(mhz)) mhz = 0;
  return Math.min(baseMhzCache, Math.max(0, Math.round(mhz)));
}

// Repaints the big number + the "≈ approx. NN% of base clock" line and
// the slider position from the current targetMhz/baseMhzCache. Does
// NOT talk to the device — call sendSpeedMhz() separately for that.
// The big number shows what the CPU really gets (delivered), falling back
// to the requested value until the firmware has answered.
function formatMhz(mhz) {
  return String(Math.round(mhz));
}

function updateReadoutDisplay() {
  const shownMhz = delivered ? delivered.mhz : targetMhz;
  const shownPct = delivered ? delivered.pct
                             : (baseMhzCache > 0 ? (targetMhz / baseMhzCache * 100) : 0);
  const valEl = $('readout-value');
  if (valEl) valEl.textContent = formatMhz(shownMhz); // null while the click-to-edit input is open

  const paused = delivered ? delivered.paused : targetMhz === 0;
  $('readout-sub').innerHTML = paused
    ? '<span class="approx">Paused</span> — STPCLK# held, CPU halted until you move the slider'
    : `<span class="approx">≈ approx.</span> <span id="readout-sub-pct">${shownPct.toFixed(1)}%</span> of base clock`;

  renderPresets();
  renderPresetLabel();

  // Live speed in the browser tab, handy when the dashboard is in the background.
  document.title = paused ? 'Paused · HAWK 370' : `${formatMhz(shownMhz)} MHz · HAWK 370`;

  const slider = $('speed-slider');
  slider.max   = baseMhzCache;
  slider.value = targetMhz;
  $('slider-max-label').textContent = `${baseMhzCache} MHz — full speed`;
}

// Sets the commanded target (clamped to [0, baseMhzCache]), repaints,
// and optionally pushes it to the device as a speedPercent float.
function setTargetMhz(mhz, send, immediate) {
  let newMhzValue = clampMhz(mhz);
  if (newMhzValue === targetMhz) return;

  targetMhz = newMhzValue;
  delivered = null;            // until the firmware reports the real value
  localChangeAt = Date.now();
  updateReadoutDisplay();
  if (send && !configPending()) sendSpeedMhz(immediate);   // staged: goes out with APPLY
}

// Applies a throttle state from the firmware (POST response or SSE).
function applyThrottleState(data, moveSlider) {
  if (!data || data.deliveredMhz === undefined) return;
  // Drop anything older than what is already shown. A power-saving Wi-Fi
  // link can deliver SSE events late; without this they moved the slider
  // back to positions from the middle of a drag.
  if (data.bootId !== undefined && data.bootId !== lastBootId) { lastBootId = data.bootId; lastRev = 0; }
  if (data.rev !== undefined) {
    if (data.rev < lastRev) return;
    lastRev = data.rev;
  }
  // A staged family / base clock stays on the page until APPLY, whatever
  // the device reports meanwhile (unless it now matches the staged one).
  const wasPending = configPending();
  if (data.family !== undefined) deviceFamily = data.family;
  if (data.baseMhz) deviceBaseMhz = data.baseMhz;
  if (wasPending && configPending()) {
    delivered = null;
    updateApplyButton();
    updateReadoutDisplay();
    return;
  }
  if (data.baseMhz && data.baseMhz !== baseMhzCache && document.activeElement !== $('base-mhz')) {
    baseMhzCache = data.baseMhz;
    $('base-mhz').value = data.baseMhz;
  }
  if (moveSlider) targetMhz = clampMhz(data.baseMhz * (data.requestedPercent / 100));
  // Only show "delivered" when it answers the value now on the slider; a
  // reply for an earlier drag position would make the big number flicker.
  const answersCurrent = clampMhz(data.baseMhz * (data.requestedPercent / 100)) === targetMhz;
  delivered = answersCurrent
    ? { mhz: data.deliveredMhz, pct: data.deliveredPercent, paused: !!data.paused }
    : null;
  if (data.family !== undefined && data.family !== selectedFamily) {
    selectedFamily = data.family;
    const fam = cpuFamilies.find(f => f.id === data.family);
    $('cpu-select-label').textContent = fam ? fam.name : data.familyName || '—';
    renderCpuMenu();
    loadPresets();
  }
  if (data.verified !== undefined) renderCalibWarning(data.verified);
  updateApplyButton();
  updateReadoutDisplay();
}

// Buttons, presets, typed values and the slider's release send at once.
// Dragging the slider and holding an arrow key only send once the value
// has rested SLIDER_DEBOUNCE_MS, so the CPU is not re-throttled on every
// single step of a drag.
function sendSpeedMhz(immediate) {
  cancelSpeedSend();
  if (immediate) doSendSpeed();
  else speedSendTimer = setTimeout(() => { speedSendTimer = null; doSendSpeed(); }, SLIDER_DEBOUNCE_MS);
}

function flushSpeedSend() {         // a debounced value is waiting: send it now
  if (speedSendTimer === null) return;
  cancelSpeedSend();
  doSendSpeed();
}

function cancelSpeedSend() {
  clearTimeout(speedSendTimer);
  speedSendTimer = null;
}

function doSendSpeed() {
  if (configPending()) return;      // staged: goes out with APPLY
  const speedPercent = baseMhzCache > 0 ? (targetMhz / baseMhzCache * 100) : 0;
  const seq = ++speedSeq;
  inFlight++;
  fetch('/api/throttle/speed', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ speedPercent })
  })
    .then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(data => { if (seq === speedSeq) applyThrottleState(data, false); })
    .catch(() => showToast('Failed to set speed', 'fail'))
    .finally(() => { inFlight--; });
}

/* ── CLICK-TO-EDIT MHZ READOUT (same pattern as file rename in files.html) ── */
// Click the number to edit it; the caret lands where you clicked, and
// further clicks inside just move it. Enter or a click anywhere outside
// applies the value and ends editing; Escape cancels.
function startMhzEdit(e) {
  if ($('mhz-edit-input')) return;   // already editing: the click only moves the caret
  const wrap  = $('readout-value-wrap');
  const shown = $('readout-value');
  const value = String(targetMhz);

  let caret = value.length;
  if (e && shown) {
    const r = shown.getBoundingClientRect();   // monospace digits: position -> index
    if (r.width > 0) caret = Math.round(((e.clientX - r.left) / r.width) * value.length);
    caret = Math.min(value.length, Math.max(0, caret));
  }

  wrap.innerHTML = `<input type="text" id="mhz-edit-input" class="readout-input" inputmode="numeric"
                           maxlength="4" autocomplete="off" spellcheck="false" value="${value}">`;
  const input = $('mhz-edit-input');
  const fit = () => { input.style.width = `${Math.max(input.value.length, 1) + 1.5}ch`; };
  fit();

  let finished = false;
  const finish = apply => {
    if (finished) return;
    finished = true;
    const parsed = parseInt(input.value, 10);
    renderReadoutValue();
    if (apply && Number.isFinite(parsed)) setTargetMhz(parsed, true, true);
  };

  input.addEventListener('input', () => {
    input.value = input.value.replace(/[^0-9]/g, '');
    fit();
  });
  input.addEventListener('keydown', ev => {
    if (ev.key === 'Enter')  { ev.preventDefault(); finish(true); }
    if (ev.key === 'Escape') { ev.preventDefault(); finish(false); }
  });
  input.addEventListener('blur', () => finish(true));
  input.focus();
  input.setSelectionRange(caret, caret);
}

function renderReadoutValue() {
  $('readout-value-wrap').innerHTML = `<span id="readout-value"></span>`;
  updateReadoutDisplay();
}

$('readout-value-wrap').addEventListener('click', startMhzEdit);

/* ── USER PRESETS ──────────────────────────────────────────────────────
   Stored on the device per CPU family as an effective MHz, so a preset
   measured on a Tualatin 1400 gives the same speed on a Tualatin 1133
   (the firmware just recomputes the duty). Sorted A-Z, searchable. */
function loadPresets() {
  if (selectedFamily === null) return Promise.resolve();
  const family = selectedFamily;
  return fetch(`/api/presets?family=${family}`, { cache: 'no-store' })
    .then(r => r.ok ? r.json() : Promise.reject(r.status))
    .then(data => {
      if (family !== selectedFamily) return;      // family changed meanwhile
      userPresets = (data.presets || []).slice().sort((a, b) =>
        a.name.localeCompare(b.name, undefined, { sensitivity: 'base', numeric: true }));
      presetMax = data.max || presetMax;
      presetTotal = data.total !== undefined ? data.total : presetTotal;
      renderPresetList();
      renderPresetLabel();
    })
    .catch(() => {});
}

function presetEffectiveMhz(p) { return Math.min(p.mhz, baseMhzCache); }

function filteredPresets() {
  const q = $('preset-search').value.trim().toLowerCase();
  return q ? userPresets.filter(p => p.name.toLowerCase().includes(q)) : userPresets;
}

function renderPresetLabel() {
  const label = $('preset-label');
  if (!label) return;
  const match = userPresets.find(p => presetEffectiveMhz(p) === targetMhz && targetMhz > 0);
  const famName = (cpuFamilies.find(f => f.id === selectedFamily) || {}).name || '';
  if (match) {
    label.innerHTML = `${escH(match.name)} <span class="muted">· ${match.mhz} MHz</span>`;
  } else {
    label.innerHTML = userPresets.length
      ? `<span class="muted">My presets · ${escH(famName)} (${userPresets.length})</span>`
      : `<span class="muted">My presets · ${escH(famName)} — none yet</span>`;
  }
}

function renderPresetList() {
  const list = $('preset-list');
  const items = filteredPresets();
  if (presetFocus >= items.length) presetFocus = items.length - 1;
  if (!userPresets.length) {
    list.innerHTML = `<div class="preset-empty">No presets for this CPU family yet. Set a speed, then use “+ Save current”.</div>`;
    return;
  }
  if (!items.length) {
    list.innerHTML = `<div class="preset-empty">No preset matches “${escH($('preset-search').value.trim())}”.</div>`;
    return;
  }
  list.innerHTML = items.map((p, i) => {
    const over = p.mhz > baseMhzCache;
    const tip = over ? `Above this CPU's ${baseMhzCache} MHz base clock — runs at full speed`
                     : (p.refBaseMhz ? `Measured on a ${p.refBaseMhz} MHz CPU` : '');
    return `<div class="select-option preset-option${over ? ' over' : ''}${i === presetFocus ? ' focused' : ''}"
                 role="option" data-i="${i}" title="${escH(tip)}">
              <span class="preset-name">${escH(p.name)}</span>
              <span class="preset-mhz">${p.mhz} MHz</span>
              <button type="button" class="preset-edit" data-edit="${i}" title="Edit or delete">✎</button>
            </div>`;
  }).join('');
}

function applyPreset(p) {
  closePresetMenu();
  if (p.mhz > baseMhzCache) {
    showToast(`${p.name} needs ${p.mhz} MHz — running at full speed (${baseMhzCache} MHz)`, 'fail');
  }
  setTargetMhz(presetEffectiveMhz(p), true, true);
  renderPresetLabel();
}

function openPresetMenu() {
  if (cpuMenuOpen) closeCpuMenu();
  $('preset-menu').classList.remove('hidden');
  $('preset-select').classList.add('open');
  $('preset-trigger').setAttribute('aria-expanded', 'true');
  presetMenuOpen = true;
  $('preset-search').value = '';
  presetFocus = -1;
  renderPresetList();
  $('preset-search').focus();
}

function closePresetMenu() {
  $('preset-menu').classList.add('hidden');
  $('preset-select').classList.remove('open');
  $('preset-trigger').setAttribute('aria-expanded', 'false');
  presetMenuOpen = false;
}

$('preset-trigger').addEventListener('click', () => presetMenuOpen ? closePresetMenu() : openPresetMenu());
document.addEventListener('click', e => {
  if (presetMenuOpen && !$('preset-select').contains(e.target)) closePresetMenu();
});
$('preset-search').addEventListener('input', () => { presetFocus = -1; renderPresetList(); });
$('preset-search').addEventListener('keydown', e => {
  const items = filteredPresets();
  if (e.key === 'ArrowDown') { e.preventDefault(); presetFocus = Math.min(items.length - 1, presetFocus + 1); renderPresetList(); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); presetFocus = Math.max(0, presetFocus - 1); renderPresetList(); }
  else if (e.key === 'Enter') {
    e.preventDefault();
    const p = items[presetFocus >= 0 ? presetFocus : 0];
    const typed = $('preset-search').value.trim();
    if (p) applyPreset(p);
    else if (typed) openPresetModal(null, typed);   // nothing found: offer to create it
  } else if (e.key === 'Escape') { e.preventDefault(); closePresetMenu(); $('preset-trigger').focus(); }
});
$('preset-list').addEventListener('click', e => {
  const items = filteredPresets();
  const edit = e.target.closest('[data-edit]');
  if (edit) { e.stopPropagation(); openPresetModal(items[Number(edit.dataset.edit)]); return; }
  const row = e.target.closest('[data-i]');
  if (row) applyPreset(items[Number(row.dataset.i)]);
});

/* Editor modal: new preset (from the current speed) or edit/delete. */
// `prefillName` carries what was typed into the search box, so a name
// that was searched for and not found does not have to be typed again.
function openPresetModal(preset, prefillName) {
  closePresetMenu();
  presetEditing = preset ? preset.name : null;
  $('preset-modal-title').textContent = preset ? 'Edit Preset' : 'New Preset';
  $('preset-name').value = preset ? preset.name : (prefillName || '');
  $('preset-mhz').value  = preset ? preset.mhz : targetMhz;
  $('preset-ref').value  = preset ? (preset.refBaseMhz || '') : baseMhzCache;
  const famName = (cpuFamilies.find(f => f.id === selectedFamily) || {}).name || 'this family';
  $('preset-family-hint').textContent =
    `Saved for ${famName}. The MHz is kept as-is on other ${famName} CPUs; the base clock is just a note. ` +
    `${presetTotal} of ${presetMax} preset slots used (all families).`;
  $('preset-error').textContent = '';
  $('preset-delete').classList.toggle('hidden', !preset);
  $('preset-modal-overlay').classList.add('visible');
  setTimeout(() => $('preset-name').focus(), 50);
}
function closePresetModal() { $('preset-modal-overlay').classList.remove('visible'); }

['preset-mhz', 'preset-ref'].forEach(id =>
  $(id).addEventListener('input', () => { $(id).value = $(id).value.replace(/[^0-9]/g, ''); }));
['preset-name', 'preset-mhz', 'preset-ref'].forEach(id =>
  $(id).addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); savePreset(); }
    if (e.key === 'Escape') { e.preventDefault(); closePresetModal(); }
  }));
$('preset-save').addEventListener('click', savePreset);

async function savePreset() {
  const name = $('preset-name').value.trim();
  const mhz  = parseInt($('preset-mhz').value, 10);
  const ref  = parseInt($('preset-ref').value, 10) || 0;
  if (!name) { $('preset-error').textContent = 'Please enter a name.'; return; }
  if (!(mhz >= 1 && mhz <= 9999)) { $('preset-error').textContent = 'MHz must be between 1 and 9999.'; return; }
  if (!presetEditing && presetTotal >= presetMax) {
    $('preset-error').textContent = `All ${presetMax} preset slots are used. Delete or export some first.`;
    return;
  }
  const body = { family: selectedFamily, name, mhz, refBaseMhz: ref };
  if (presetEditing) body.originalName = presetEditing;
  try {
    const r = await fetch('/api/presets/save', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    });
    const res = await r.json().catch(() => ({}));
    if (!r.ok) { $('preset-error').textContent = res.message || `Save failed (${r.status})`; return; }
    closePresetModal();
    showToast(`Preset “${name}” saved`);
    loadPresets();
  } catch (_) {
    $('preset-error').textContent = 'Device not reachable.';
  }
}

// The editor itself is the confirmation step, so one click deletes.
$('preset-delete').addEventListener('click', async () => {
  try {
    const r = await fetch('/api/presets/delete', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ family: selectedFamily, name: presetEditing })
    });
    const res = await r.json().catch(() => ({}));
    if (!r.ok) { $('preset-error').textContent = res.message || `Delete failed (${r.status})`; return; }
    closePresetModal();
    showToast(`Preset “${presetEditing}” deleted`);
    loadPresets();
  } catch (_) {
    $('preset-error').textContent = 'Device not reachable.';
  }
});

$('preset-add').addEventListener('click', () => {
  const typed = $('preset-search').value.trim();
  const exists = userPresets.some(p => p.name.toLowerCase() === typed.toLowerCase());
  openPresetModal(null, exists ? '' : typed);
});

$('preset-export').addEventListener('click', async () => {
  closePresetMenu();
  try {
    const data = await exportAllPresets();
    downloadJson(data, `hawk370-presets-${new Date().toISOString().slice(0, 10)}.json`);
    showToast(`Exported ${data.presets.length} preset${data.presets.length === 1 ? '' : 's'}`);
  } catch (_) {
    showToast('Export failed', 'fail');
  }
});

$('preset-import').addEventListener('click', () => { closePresetMenu(); $('preset-file').click(); });
$('preset-file').addEventListener('change', async () => {
  const file = $('preset-file').files[0];
  $('preset-file').value = '';
  if (!file) return;
  try {
    const res = await importPresetsObject(JSON.parse(await file.text()));
    showToast(`Imported: ${res.added} new, ${res.updated} updated` + (res.skipped ? `, ${res.skipped} skipped` : ''));
    loadPresets();
  } catch (err) {
    showToast(`Import failed: ${err.message || 'invalid file'}`, 'fail');
  }
});

/* ── KEYBOARD SHORTCUTS ──
   ← / →  ±1 MHz, with Shift ±10 MHz;  1-4  the percent presets.
   Ignored while typing in a field, with a modal open, or with Ctrl/Alt/Cmd. */
document.addEventListener('keydown', e => {
  if (e.ctrlKey || e.altKey || e.metaKey) return;
  const el = document.activeElement;
  if (el && (el.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(el.tagName)) &&
      el.id !== 'speed-slider') return;
  if (document.querySelector('.modal-overlay.visible')) return;
  if (cpuMenuOpen || presetMenuOpen) return;

  if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
    e.preventDefault();   // also stops the focused slider's own ±1 step
    const step = (e.shiftKey ? 10 : 1) * (e.key === 'ArrowLeft' ? -1 : 1);
    setTargetMhz(targetMhz + step, true, false);
    return;
  }
  const presetKey = /^(?:Digit|Numpad)([1-4])$/.exec(e.code);
  if (presetKey) {
    const btn = document.querySelectorAll('#speed-presets .freq-btn')[Number(presetKey[1]) - 1];
    if (btn) { e.preventDefault(); btn.click(); }
  }
});
// Holding an arrow key repeats keydown; send the final value on release.
document.addEventListener('keyup', e => {
  if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') flushSpeedSend();
});

/* ── QUICK SPEED PRESETS (percent of the base clock) ── */
function renderPresets() {
  document.querySelectorAll('#speed-presets .freq-btn').forEach(btn => {
    const mhz = clampMhz(baseMhzCache * Number(btn.dataset.pct) / 100);
    btn.classList.toggle('active', mhz === targetMhz);
    btn.title = `${mhz} MHz`;
  });
}
document.querySelectorAll('#speed-presets .freq-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    setTargetMhz(baseMhzCache * Number(btn.dataset.pct) / 100, true, true);
  });
});

/* ── SPEED SLIDER (drags in whole MHz, converted to speedPercent on send) ── */
$('speed-slider').addEventListener('input', () => {
  setTargetMhz(parseInt($('speed-slider').value, 10), true, false);
});
$('speed-slider').addEventListener('change', () => {   // released
  setTargetMhz(parseInt($('speed-slider').value, 10), true, false);
  flushSpeedSend();
});

function renderCalibWarning(verified) {
  $('calib-warning').classList.toggle('visible', !verified);
}

/* ── CUSTOM CPU-FAMILY DROPDOWN ─────────────────────────────────────
   Replaces a native <select> so the open menu can actually be themed
   (browsers won't let CSS touch a native option list's colors/fonts). */
function renderCpuMenu() {
  const menu = $('cpu-select-menu');
  menu.innerHTML = cpuFamilies.map(f => `
    <div class="select-option${f.id === selectedFamily ? ' selected' : ''}" data-id="${f.id}" role="option" aria-selected="${f.id === selectedFamily}">
      ${escH(f.name)}
    </div>
  `).join('');
  menu.querySelectorAll('.select-option').forEach(opt => {
    opt.addEventListener('click', () => {
      selectCpuFamily(parseInt(opt.dataset.id, 10));
      closeCpuMenu();
    });
  });
}

function selectCpuFamily(id) {
  selectedFamily = id;
  const fam = cpuFamilies.find(f => f.id === id);
  $('cpu-select-label').textContent = fam ? fam.name : '—';
  renderCpuMenu();
  stageConfig();
  loadPresets();
}

function openCpuMenu() {
  if (presetMenuOpen) closePresetMenu();
  $('cpu-select-menu').classList.remove('hidden');
  $('cpu-select').classList.add('open');
  $('cpu-select-trigger').setAttribute('aria-expanded', 'true');
  cpuMenuOpen = true;
}

function closeCpuMenu() {
  $('cpu-select-menu').classList.add('hidden');
  $('cpu-select').classList.remove('open');
  $('cpu-select-trigger').setAttribute('aria-expanded', 'false');
  cpuMenuOpen = false;
}

$('cpu-select-trigger').addEventListener('click', () => {
  cpuMenuOpen ? closeCpuMenu() : openCpuMenu();
});
document.addEventListener('click', e => {
  if (cpuMenuOpen && !$('cpu-select').contains(e.target)) closeCpuMenu();
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && cpuMenuOpen) closeCpuMenu();
});

function loadCpuFamilies() {
  return fetch('/api/cpu-families')
    .then(r => r.json())
    .then(list => {
      cpuFamilies = list;
      renderCpuMenu();
    })
    .catch(() => {});
}

function loadThrottleStatus() {
  return fetch('/api/throttle')
    .then(r => r.json())
    .then(data => {
      selectedFamily = data.family;
      const fam = cpuFamilies.find(f => f.id === data.family);
      $('cpu-select-label').textContent = fam ? fam.name : data.familyName || '—';
      renderCpuMenu();

      $('base-mhz').value = data.baseMhz;
      baseMhzCache  = data.baseMhz;
      deviceFamily  = data.family;     // page load: shows what the device runs,
      deviceBaseMhz = data.baseMhz;    // so APPLY starts inactive

      applyThrottleState(data, true);
    })
    .catch(() => {});
}

/* ── APPLY: CPU FAMILY + BASE CLOCK ──────────────────────────────────
   Changing the family or the base clock only changes the page. While
   such a change is pending, the slider, % buttons, presets, keys and the
   typed MHz move the target on screen only (against the pending base
   clock). APPLY sends family, base clock and target in one request, so
   the device never runs the old speed on the new configuration.
   Afterwards the slider is live again until the next family / base
   clock change. */
function pageBaseMhz() {
  const v = parseInt($('base-mhz').value, 10);
  return Number.isFinite(v) && v >= 1 && v <= 9999 ? v : baseMhzCache;
}

function configPending() {
  if (deviceFamily === null || deviceBaseMhz === null) return false;
  return selectedFamily !== deviceFamily || pageBaseMhz() !== deviceBaseMhz;
}

function updateApplyButton() {
  const pending = configPending();
  const btn = $('config-apply');
  btn.disabled = !pending;
  btn.title = pending ? 'Send the CPU family, base clock and speed to the HAWK 370' : 'Nothing to apply';
}

// Takes the base clock field's value into the page state (an empty or
// invalid field falls back to the last good value).
function commitBaseField() {
  const v = pageBaseMhz();
  $('base-mhz').value = v;
  if (v !== baseMhzCache) {
    baseMhzCache = v;
    targetMhz = clampMhz(targetMhz);   // only lowers it if it no longer fits
  }
}

// The family or the base clock changed on the page: nothing is sent.
function stageConfig() {
  commitBaseField();
  cancelSpeedSend();                   // a debounced slider value must not go out now
  const fam = cpuFamilies.find(f => f.id === selectedFamily);
  if (fam) renderCalibWarning(fam.verified);
  delivered = null;
  localChangeAt = Date.now();
  updateApplyButton();
  updateReadoutDisplay();
  // Changed back to what the device runs: send a target moved meanwhile.
  if (!configPending()) sendSpeedMhz(true);
}

function applyConfig() {
  commitBaseField();
  if (!configPending()) { updateApplyButton(); return; }
  cancelSpeedSend();
  const family  = selectedFamily;
  const baseMhz = baseMhzCache;
  const speedPercent = Math.min(100, Math.max(0, targetMhz / baseMhz * 100));
  $('config-apply').disabled = true;
  localChangeAt = Date.now();
  ++speedSeq;                          // older speed replies must not repaint
  inFlight++;
  fetch('/api/throttle/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ family, baseMhz, speedPercent })
  })
    .then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(data => applyThrottleState(data, false))
    .catch(() => showToast('Failed to apply the CPU configuration', 'fail'))
    .finally(() => { inFlight--; updateApplyButton(); });
}

$('config-apply').addEventListener('click', applyConfig);

/* ── MHZ STEPPER BUTTONS (base clock field) ── */
function stepBaseMhz(delta) {
  const input = $('base-mhz');
  const min = parseInt(input.min, 10) || 1;
  const max = parseInt(input.max, 10) || 9999;
  let val = parseInt(input.value, 10);
  if (!Number.isFinite(val)) val = 0;
  val = Math.min(max, Math.max(min, val + delta));
  input.value = val;
  stageConfig();
}

$('mhz-up').addEventListener('click', () => stepBaseMhz(1));
$('mhz-down').addEventListener('click', () => stepBaseMhz(-1));

$('base-mhz').addEventListener('input', updateApplyButton);   // while typing
$('base-mhz').addEventListener('change', stageConfig);         // Enter / leaving the field
$('base-mhz').addEventListener('keydown', e => {
  if (e.key !== 'Enter') return;
  e.preventDefault();
  stageConfig();
  if (configPending()) applyConfig();   // Enter presses APPLY
  $('base-mhz').blur();
});

/* ── POWER RAILS: SSE VOLTAGE STREAM + SPARKLINES ───────────────────── */
const VOLT_HISTORY_LEN = 40;
const vcoreHistory = [];
const vttHistory   = [];

// Scales to the window's own min..max (at least SPARK_MIN_SPAN volts), so
// a 1.5 V rail wobbling by 20 mV is visible instead of a flat line.
const SPARK_MIN_SPAN = 0.1;
function renderSpark(name, hist) {
  if (hist.length < 2) return;
  let min = Math.min(...hist);
  let max = Math.max(...hist);
  if (max - min < SPARK_MIN_SPAN) {
    const mid = (max + min) / 2;
    min = mid - SPARK_MIN_SPAN / 2;
    max = mid + SPARK_MIN_SPAN / 2;
  }
  min = Math.max(0, min);
  const range = (max - min) || SPARK_MIN_SPAN;
  $(name + '-spark-floor').textContent = min.toFixed(2) + 'V';
  const w = 200, h = 40, pad = 3;
  const step = w / (VOLT_HISTORY_LEN - 1);
  const startX = w - (hist.length - 1) * step;

  const line = hist.map((v, i) => {
    const x = startX + i * step;
    const y = h - pad - ((v - min) / range) * (h - pad * 2);
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');

  $(name + '-spark-line').setAttribute('points', line);
  $(name + '-spark-fill').setAttribute('points', `${startX.toFixed(1)},${h} ${line} ${w},${h}`);

  // ── Current-value tracker line/label ──
  const latest = hist[hist.length - 1];
  const yCurrent = h - pad - ((latest - min) / range) * (h - pad * 2);

  const curLine = $(name + '-spark-current');
  curLine.setAttribute('y1', yCurrent.toFixed(1));
  curLine.setAttribute('y2', yCurrent.toFixed(1));

  const curLabel = $(name + '-spark-current-label');
  curLabel.style.top = ((yCurrent / h) * 100 + 20).toFixed(1) + '%';
  curLabel.textContent = latest.toFixed(2) + 'V';
}

function pushVoltage(name, value) {
  if (typeof value !== 'number') {        // ADC unavailable on this unit
    $(name + '-value').textContent = '--';
    return;
  }
  const hist = name === 'vcore' ? vcoreHistory : vttHistory;
  hist.push(value);
  if (hist.length > VOLT_HISTORY_LEN) hist.shift();
  renderSpark(name, hist);
  $(name + '-value').textContent = value.toFixed(2);
}

function updateWifiBars(rssi) {
  const bars = getSignalBars(rssi);
  const wifiContainer = $('wifi-bars');
  const currentLitCount = wifiContainer.querySelectorAll('.lit').length;
  if (currentLitCount !== bars || wifiContainer.children.length === 0) {
    wifiContainer.innerHTML = [1, 2, 3, 4].map(n => `<span class="${n <= bars ? 'lit' : ''}"></span>`).join('');
  }
}

// Single handler for the one combined telemetry payload (vcore, vtt,
// temp, rssi, mem, wifiPs) — deliberately kept as one SSE event rather
// than split into several, so there's only ever one place that
// consumes it. This also covers what used to require polling
// /api/system every second (see getSystemData below).
function pushTelemetry(data) {
  pushVoltage('vcore', data.vcore);
  pushVoltage('vtt', data.vtt);
  if (data.temp !== undefined)    $('esp_temp').innerText = `${data.temp.toFixed(1)} °C`;
  if (data.rssi !== undefined)    updateWifiBars(data.rssi);
  if (data.mem !== undefined)     $('free_memory').innerText = `${Math.floor(data.mem / 1024)} KB`;
  if (data.wifiPs !== undefined)  updateWifiPsToggle(data.wifiPs);
  lastSSEActivity = Date.now();
}

fetch('/api/telemetry')
  .then(r => r.json())
  .then(pushTelemetry)
  .catch(() => {});

let throttleEvents = new EventSource('/events');

function attachSSEHandlers(es) {
  es.addEventListener('telemetry', e => pushTelemetry(JSON.parse(e.data)));
  // Throttle changes made from another browser (or this one) are pushed here.
  es.addEventListener('throttle', e => {
    lastSSEActivity = Date.now();
    if (inFlight > 0 || Date.now() - localChangeAt < 800) return;   // our own change is still settling
    if (document.getElementById('mhz-edit-input')) return;
    applyThrottleState(JSON.parse(e.data), true);
  });
  es.addEventListener('ping', () => { lastSSEActivity = Date.now(); });
  // Presets changed (maybe from another browser): reload if it is our family.
  es.addEventListener('presets', e => {
    const d = JSON.parse(e.data);
    if (d.family === selectedFamily) loadPresets();
  });
}
attachSSEHandlers(throttleEvents);

function reconnectSSE() {
  try { throttleEvents.close(); } catch (_) {}
  throttleEvents = new EventSource('/events');
  attachSSEHandlers(throttleEvents);
}

/* ── WI-FI POWER SAVING TOGGLE ───────────────────────────────────────
   confirmedWifiPs: true = MAX_MODEM (power saving ON), false = WIFI_PS_NONE.
   pendingWifiPs:   value requested but not yet confirmed. null = unknown. */
let confirmedWifiPs = null;
let pendingWifiPs   = null;

function updateWifiPsToggle(livePs) {
  if (pendingWifiPs !== null && livePs === pendingWifiPs) pendingWifiPs = null;
  confirmedWifiPs = livePs;

  const bOn  = $('ps-on');
  const bOff = $('ps-off');
  bOn.classList.remove('active', 'pending');
  bOff.classList.remove('active', 'pending');

  if (pendingWifiPs !== null) {
    const pendingBtn   = pendingWifiPs ? bOn : bOff;
    const confirmedBtn = pendingWifiPs ? bOff : bOn;
    confirmedBtn.classList.add('active');
    pendingBtn.classList.add('pending');
  } else {
    (livePs ? bOn : bOff).classList.add('active');
  }
}

async function setWifiPs(psOn) {
  if (pendingWifiPs !== null) return;
  if (psOn === confirmedWifiPs) return;

  pendingWifiPs = psOn;
  updateWifiPsToggle(confirmedWifiPs);

  try {
    const res = await fetch('/api/system/wifi-ps', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ps: psOn })
    });
    if (!res.ok) throw new Error('failed');
    pendingWifiPs = null;
    updateWifiPsToggle(psOn);   // confirm directly, no poll needed
  } catch (_) {
    pendingWifiPs = null;
    updateWifiPsToggle(confirmedWifiPs);
  }
}

/* ── RESET / RESTART MODALS ──────────────────────────────────────────── */
function openResetModal() { $('modal-overlay').classList.add('visible'); }
function closeResetModal() {
  $('modal-overlay').classList.remove('visible');
  setTimeout(() => {
    $('step-confirm').classList.remove('hidden');
    $('step-resetting').classList.add('hidden');
  }, 200);
}
function executeReset() {
  $('step-confirm').classList.add('hidden');
  $('step-resetting').classList.remove('hidden');
  fetch('/api/factory-reset', { method: 'POST' })
    .then(res => res.json())
    .catch(() => console.log('Device connection severed. System rebooting.'));
  setTimeout(findSetupDevice, 3000);
}

// After a Wi-Fi reset the device comes back as the HAWK370-Setup hotspot.
// Keep asking both of its addresses until one answers in setup mode (the
// old LAN address may still answer for a moment, but not in setup mode),
// then open the setup wizard there.
const SETUP_ADDRESSES = ['http://hawk370.local', 'http://192.168.8.1'];
async function findSetupDevice() {
  for (const base of SETUP_ADDRESSES) {
    const info = await probeDevice(base);
    if (info && info.provisioning) {
      $('reset-probe-status').textContent = 'HAWK 370 found — opening setup…';
      window.location.href = base + '/';
      return;
    }
  }
  setTimeout(findSetupDevice, 1500);
}

function openRestartModal() {
  $('restart-step-confirm').classList.remove('hidden');
  $('restart-step-restarting').classList.add('hidden');
  $('restart-modal-overlay').classList.add('visible');
}
function closeRestartModal() { $('restart-modal-overlay').classList.remove('visible'); }
async function executeRestart() {
  $('restart-step-confirm').classList.add('hidden');
  $('restart-step-restarting').classList.remove('hidden');
  const before = await probeDevice();
  try { await fetch('/api/restart', { method: 'POST' }); } catch (_) { /* expected */ }

  // Reload once the device answers with a new boot id, however long it takes.
  const started = Date.now();
  const poll = async () => {
    const info = await probeDevice();
    if (info && (!before || info.bootId !== before.bootId)) {
      window.location.reload();
      return;
    }
    if (Date.now() - started > 60000) {
      $('restart-status').textContent = 'Still waiting for the device after 60 s. Check its power and Wi-Fi; this page keeps trying.';
    }
    setTimeout(poll, 1000);
  };
  setTimeout(poll, 1500);
}

let lastSSEActivity = Date.now();
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') checkSSEStale();
});

function checkSSEStale() {
  if (Date.now() - lastSSEActivity > 40000) reconnectSSE(); // > ~1.5x your 25s ping interval
}
setInterval(checkSSEStale, 15000);

/* ── SYSTEM INFO (firmware, IP, SSID, model, storage) ──────────────────
   Fetched once on load rather than polled: memory and Wi-Fi power-
   saving state are covered continuously by the SSE telemetry stream
   (see pushTelemetry above), so there's nothing here that needs a
   repeating timer. ─────────────────────────────────────────────────── */
function getSystemData() {
  fetch('/api/system')
    .then(res => res.json())
    .then(data => {
      const esp = data.esp || {};

      if ($('firmware_version').innerText !== `v${data.firmware}`) {
        $('firmware_version').innerText = `v${data.firmware}`;
      }
      if ($('ip_address').innerText !== data.ipAddress) {
        $('ip_address').innerText = data.ipAddress;
        $('ip_address').href = `http://${data.ipAddress}/`;
      }
      if ($('ssid').innerText !== data.ssid) {
        $('ssid').innerText = data.ssid;
      }

      if (esp.model) {
        const modelEl = $('esp_model');
        modelEl.textContent = esp.model;
        const tipParts = [];
        if (esp.cores !== undefined) tipParts.push(`Cores: ${esp.cores}`);
        if (esp.cpuMhz !== undefined) tipParts.push(`CPU: ${esp.cpuMhz} MHz`);
        if (esp.rev   !== undefined) tipParts.push(`Rev: ${esp.rev}`);
        if (esp.ver)                 tipParts.push(`SDK: ${esp.ver}`);
        modelEl.title = tipParts.join(' · ');
      }

      // Wi-Fi power saving has no effect in standalone AP mode (no
      // upstream Wi-Fi to save power against) and leaving the radio at
      // full power there runs the module noticeably hotter — so the
      // toggle is simply hidden rather than shown greyed-out.
      if (data.apMode !== undefined) {
        $('wifi-power-item').classList.toggle('hidden', data.apMode);
      }

      if (data.littlefs_total && data.littlefs_total > 0) {
        const total = data.littlefs_total;
        const free  = data.littlefs_free;
        const used  = total - free;
        const usedPercent = ((used / total) * 100).toFixed(1);
        $('storage_text').textContent     = `${fmtBytes(free)} free`;
        $('storage-progress').style.width = `${usedPercent}%`;
        document.querySelector('.storage-wrapper')
          .setAttribute('title', `${fmtBytes(used)} / ${fmtBytes(total)} (${usedPercent}% used)`);
      } else {
        $('storage_text').textContent     = 'Offline';
        $('storage-progress').style.width = '0%';
        document.querySelector('.storage-wrapper').setAttribute('title', 'LittleFS Unmounted');
      }
    })
    .catch(err => console.log('Could not read system data.', err));
}

/* ── INIT ────────────────────────────────────────────────────────────── */
loadCpuFamilies().then(loadThrottleStatus).then(loadPresets);
getSystemData();

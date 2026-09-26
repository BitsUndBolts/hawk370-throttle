/* HAWK 370 shared utilities */
const $ = id => document.getElementById(id);

function escH(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')
          .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function fmtBytes(b) {
  if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
  if (b >= 1024)    return (b / 1024).toFixed(0) + ' KB';
  return b + ' B';
}

function getSignalBars(rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  return 1;
}

// Asks a HAWK 370 whether it is alive. `base` is '' for this page's own host,
// or e.g. 'http://192.168.8.1'. Resolves to the /api/ping JSON, or null.
async function probeDevice(base = '', timeoutMs = 2500) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const r = await fetch(base + '/api/ping', { cache: 'no-store', signal: ctl.signal });
    if (!r.ok) return null;
    const info = await r.json();
    return info && info.device === 'hawk370' ? info : null;
  } catch (_) {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

let _toastTimer = null;
function showToast(msg, type = 'success') {
  const t = $('toast');
  if (!t) return;
  t.textContent = msg;
  t.className   = `toast ${type} visible`;
  clearTimeout(_toastTimer);
  _toastTimer = setTimeout(() => t.classList.remove('visible'), 2800);
}
/* ── PRESET EXPORT / IMPORT (shared by the dashboard and the Files page) ──
   File format:
   { "format": "hawk370-presets", "version": 1, "exported": ISO date,
     "presets": [ { "family": "Tualatin", "familySlug": "tualatin",
                    "name": "386DX-40", "mhz": 25, "refBaseMhz": 1400 } ] }
   Families are matched by slug, then by name, so files stay valid if the
   firmware ever reorders its family list. */
const PRESET_FILE_FORMAT = 'hawk370-presets';

async function fetchCpuFamilies() {
  const r = await fetch('/api/cpu-families', { cache: 'no-store' });
  if (!r.ok) throw new Error('cpu-families ' + r.status);
  return r.json();
}

// Collects every family's presets into one export object.
async function exportAllPresets() {
  const families = await fetchCpuFamilies();
  const out = { format: PRESET_FILE_FORMAT, version: 1, exported: new Date().toISOString(), presets: [] };
  for (const fam of families) {
    const r = await fetch(`/api/presets?family=${fam.id}`, { cache: 'no-store' });
    if (!r.ok) throw new Error('presets ' + r.status);
    const data = await r.json();
    for (const p of data.presets || []) {
      out.presets.push({ family: fam.name, familySlug: fam.slug || data.familySlug || '', name: p.name,
                         mhz: p.mhz, refBaseMhz: p.refBaseMhz || 0 });
    }
  }
  return out;
}

// Merges an export object into the device, one request per family.
// Resolves to { added, updated, skipped }.
async function importPresetsObject(obj) {
  if (!obj || !Array.isArray(obj.presets)) throw new Error('Not a HAWK 370 preset file');
  if (obj.format && obj.format !== PRESET_FILE_FORMAT) throw new Error('Not a HAWK 370 preset file');

  const families = await fetchCpuFamilies();
  const bySlug = {}, byName = {};
  const slugOf = f => (f.name || '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
  families.forEach(f => { bySlug[f.slug || slugOf(f)] = f.id; byName[(f.name || '').toLowerCase()] = f.id; });

  const groups = {};
  const totals = { added: 0, updated: 0, skipped: 0 };
  for (const p of obj.presets) {
    const id = bySlug[(p.familySlug || '').toLowerCase()] ?? byName[(p.family || '').toLowerCase()];
    if (id === undefined) { totals.skipped++; continue; }
    (groups[id] = groups[id] || []).push({ name: String(p.name || ''), mhz: Math.round(Number(p.mhz)),
                                           refBaseMhz: Math.round(Number(p.refBaseMhz) || 0) });
  }
  for (const [id, presets] of Object.entries(groups)) {
    const r = await fetch('/api/presets/import', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ family: Number(id), presets })
    });
    const res = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(res.message || ('import ' + r.status));
    totals.added += res.added || 0;
    totals.updated += res.updated || 0;
    totals.skipped += res.skipped || 0;
  }
  return totals;
}

function downloadJson(obj, filename) {
  const blob = new Blob([JSON.stringify(obj, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}

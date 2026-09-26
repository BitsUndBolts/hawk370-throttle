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
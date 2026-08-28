'use strict';

const $ = (id) => document.getElementById(id);
const PRESETS = [
  'In a Meeting!', 'Welcome', 'I am Free', 'I am Hungry :(',
  'Doing Paperwork!', 'Do Not Disturb', 'On Break', 'Done For The Day!',
];

let running = false;

document.querySelectorAll('.tab').forEach((t) => {
  t.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach((x) => x.classList.remove('on'));
    document.querySelectorAll('.panel').forEach((x) => x.classList.remove('on'));
    t.classList.add('on');
    $('tab-' + t.dataset.tab).classList.add('on');
  });
});

// textContent everywhere - log lines and event titles are outside data, never
// markup, so they must not be able to inject into the page.
function addLog(e) {
  const box = $('log');
  const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
  const row = document.createElement('div');
  const ts = document.createElement('span');
  ts.className = 't';
  ts.textContent = new Date(e.at).toLocaleTimeString();
  const msg = document.createElement('span');
  msg.className = e.level;
  msg.textContent = e.message;
  row.appendChild(ts);
  row.appendChild(msg);
  box.appendChild(row);
  while (box.children.length > 500) box.removeChild(box.firstChild);
  if (atBottom) box.scrollTop = box.scrollHeight;
}

function fmtTime(d) {
  return d ? new Date(d).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) : '';
}

function render(s) {
  running = !!s.running;
  $('dot').className = 'dot' + (s.running ? (s.deviceOnline === false ? ' err' : ' run') : '');
  $('statusText').textContent = !s.running ? 'stopped'
    : s.paused ? 'calendar paused (rate limited) - weather still updating'
      : s.deviceOnline === false ? 'ticker unreachable: ' + (s.lastError || '')
        : 'running';
  $('btnToggle').textContent = s.running ? 'Stop' : 'Start';

  $('preview').textContent = s.lastMessageSent ? s.lastMessageSent.split('|').join('\n') : '\u2014';
  $('previewMeta').textContent = s.lastSync ? 'last sync ' + fmtTime(s.lastSync) : '';

  $('nowTitle').textContent = s.current ? s.current.summary : '\u2014';
  $('nowTime').textContent = s.current
    ? fmtTime(s.current.start) + ' - ' + fmtTime(s.current.end) : 'nothing in progress';
  $('nextTitle').textContent = s.next ? s.next.summary : '\u2014';
  $('nextTime').textContent = s.next ? 'starts ' + fmtTime(s.next.start) : 'nothing upcoming';

  $('wxText').textContent = s.weather ? s.weather.text : '\u2014';
  $('wxMeta').textContent = s.weather && s.weather.place ? s.weather.place : '';
  $('calList').textContent = (s.calendars && s.calendars.length) ? s.calendars.join(', ') : '\u2014';
  $('lastSync').textContent = s.lastSync ? 'updated ' + fmtTime(s.lastSync) : '';

  renderClock(s.deviceTime);
}

// Offer the Sync button only when the clock is actually wrong. A drift of a
// second or two is just round-trip latency, not a problem worth fixing.
var DRIFT_WARN_SEC = 30;

function renderClock(t) {
  var btn = $('btnSyncTime');
  if (!t) {
    $('devTime').textContent = '\u2014';
    $('devTimeMeta').textContent = 'ticker clock unavailable';
    $('drift').textContent = '\u2014';
    btn.style.display = 'none';
    return;
  }

  $('devTime').textContent = t.local || new Date(t.epoch * 1000).toLocaleString();

  var bits = ['source: ' + (t.source === 'manual' ? 'set from this PC' : 'NTP')];
  if (!t.valid) bits.push('clock not yet set');
  $('devTimeMeta').textContent = bits.join('  \u00b7  ');

  var d = t.driftSec;
  var mag = Math.abs(d);
  var human = mag < 90 ? mag + 's'
    : mag < 5400 ? Math.round(mag / 60) + ' min'
      : Math.round(mag / 3600) + ' h';
  $('drift').textContent = d === 0 ? 'in sync' : (d > 0 ? '+' : '-') + human;

  var bad = mag >= DRIFT_WARN_SEC || !t.valid;
  $('drift').style.color = bad ? 'var(--err)' : 'var(--txt)';
  btn.style.display = bad ? '' : 'none';
}

const FIELDS = ['tickerIp', 'tickerUser', 'tickerPass', 'appleId', 'appPassword',
  'calendars', 'ignoreNext', 'weatherZip', 'weatherUnits', 'pollSecs', 'weatherMins'];
const CHECKS = ['autoStart', 'startMinimized'];

function fillSettings(cfg) {
  FIELDS.forEach((k) => { if ($(k)) $(k).value = cfg[k] === undefined ? '' : cfg[k]; });
  CHECKS.forEach((k) => { if ($(k)) $(k).checked = !!cfg[k]; });
}

function collectSettings() {
  const out = {};
  FIELDS.forEach((k) => {
    if (!$(k)) return;
    out[k] = ($(k).type === 'number') ? Number($(k).value) : $(k).value;
  });
  CHECKS.forEach((k) => { if ($(k)) out[k] = $(k).checked; });
  return out;
}


// ---- panel settings (read from and written to the ticker) ----------------
// Sliders show a formatted value, so each field carries how to render itself.
var DEVICE_FIELDS = {
  th: { el: 'd_th' },
  ts: { el: 'd_ts' },
  sd: { el: 'd_sd' },
  cl: { el: 'd_cl' },
  br: { el: 'd_br', out: 'd_br_v', fmt: function (v) { return v + '%'; } },
  lg: { el: 'd_lg', out: 'd_lg_v', fmt: function (v) { return v + ' px'; } },
  sp: { el: 'd_sp', out: 'd_sp_v', fmt: function (v) { return v + ' ms'; } },
  pk: { el: 'd_pk', out: 'd_pk_v', fmt: function (v) { return (v / 1000).toFixed(2) + ' s'; } },
  nb: { el: 'd_nb', out: 'd_nb_v', fmt: function (v) { return v + '%'; } },
  pd: { el: 'd_pd', out: 'd_pd_v', fmt: function (v) { return Number(v) === 0 ? 'any' : (v / 1000).toFixed(2) + ' m'; } },
  ph: { el: 'd_ph', out: 'd_ph_v', fmt: function (v) { return v + ' s'; } },
  ns: { el: 'd_ns' },
  ne: { el: 'd_ne' },
  sk: { el: 'd_sk', bool: true },
  nd: { el: 'd_nd', bool: true },
  pr: { el: 'd_pr', bool: true },
  wp: { el: 'd_wp', bool: true },
};

function paintFieldValue(key) {
  var f = DEVICE_FIELDS[key];
  if (!f || !f.out) return;
  $(f.out).textContent = f.fmt($(f.el).value);
}

function fillDeviceConfig(c) {
  if (!c) {
    $('cfgMeta').textContent = 'ticker unreachable';
    $('radarStat').textContent = '';
    return;
  }
  Object.keys(DEVICE_FIELDS).forEach(function (key) {
    var f = DEVICE_FIELDS[key];
    if (c[key] === undefined) return;
    if (f.bool) $(f.el).checked = Number(c[key]) === 1;
    else $(f.el).value = c[key];
    paintFieldValue(key);
  });
  $('cfgMeta').textContent = 'firmware v' + c.ver;

  var radar = Number(c.frames) === 0
    ? 'No LD2450 data - check wiring (sensor TX to board RX/GPIO8) and 5V power.'
    : 'Radar OK, ' + c.frames + ' frames  ·  ' +
      (Number(c.seen) ? 'person detected' : 'no one detected') +
      (Number(c.mm) ? '  ·  nearest ' + c.mm + ' mm' : '');
  $('radarStat').textContent = radar + '   Panel: ' +
    (Number(c.panel) ? 'on' : 'asleep') + ', ' + c.rows + ' row(s) drawn';
}

function collectDeviceConfig() {
  var out = {};
  Object.keys(DEVICE_FIELDS).forEach(function (key) {
    var f = DEVICE_FIELDS[key];
    out[key] = f.bool ? ($(f.el).checked ? 1 : 0) : Number($(f.el).value);
  });
  return out;
}

async function refreshDeviceConfig() {
  $('cfgMeta').textContent = 'reading...';
  fillDeviceConfig(await window.ticker.getDeviceConfig());
}


// ---- "when free" wording -------------------------------------------------
// Anything not in the preset list is treated as custom, so a value typed here
// (or carried over from the old script config) is preserved rather than being
// silently snapped to the nearest preset.
var FREE_PRESETS = ['I am Free', 'Doing Paperwork', 'Do Not Disturb'];

function fillFreeMessage(value) {
  var v = value || 'I am Free';
  if (FREE_PRESETS.indexOf(v) === -1) {
    $('freeSel').value = '__custom';
    $('freeCustom').style.display = '';
    $('freeCustom').value = v;
  } else {
    $('freeSel').value = v;
    $('freeCustom').style.display = 'none';
    $('freeCustom').value = '';
  }
}

async function saveFreeMessage() {
  var v = $('freeSel').value === '__custom'
    ? $('freeCustom').value.trim()
    : $('freeSel').value;
  if (!v) return;                       // empty custom box: nothing to save yet
  await window.ticker.saveSettings({ freeMessage: v });
  $('freeMsg').textContent = 'saved';
  setTimeout(function () { $('freeMsg').textContent = ''; }, 1800);
}

window.addEventListener('DOMContentLoaded', async () => {
  PRESETS.forEach((p) => {
    const b = document.createElement('button');
    b.className = 'btn';
    b.textContent = p;
    b.addEventListener('click', () => window.ticker.sendMessage(p));
    $('presets').appendChild(b);
  });

  const st = await window.ticker.getState();
  (st.logs || []).forEach(addLog);
  render(st);
  const initialCfg = await window.ticker.getSettings();
  fillSettings(initialCfg);
  fillFreeMessage(initialCfg.freeMessage);

  window.ticker.onLog(addLog);
  window.ticker.onState(render);

  $('freeSel').addEventListener('change', function () {
    var custom = $('freeSel').value === '__custom';
    $('freeCustom').style.display = custom ? '' : 'none';
    if (custom) $('freeCustom').focus();
    else saveFreeMessage();
  });
  $('freeCustom').addEventListener('change', saveFreeMessage);
  $('freeCustom').addEventListener('keydown', function (e) { if (e.key === 'Enter') saveFreeMessage(); });

  $('btnSyncNow').addEventListener('click', () => window.ticker.syncNow());
  $('btnSyncTime').addEventListener('click', async () => {
    $('btnSyncTime').disabled = true;
    await window.ticker.syncDeviceTime();
    $('btnSyncTime').disabled = false;
  });
  $('btnToggle').addEventListener('click', () => (running ? window.ticker.stopSync() : window.ticker.startSync()));

  $('btnSend').addEventListener('click', () => {
    const v = $('msgInput').value.trim();
    if (v) window.ticker.sendMessage(v);
  });
  $('msgInput').addEventListener('keydown', (e) => { if (e.key === 'Enter') $('btnSend').click(); });
  $('btnBlank').addEventListener('click', () => window.ticker.sendMessage('BLANK'));

  Object.keys(DEVICE_FIELDS).forEach(function (key) {
    var f = DEVICE_FIELDS[key];
    if (f.out) $(f.el).addEventListener('input', function () { paintFieldValue(key); });
  });
  $('btnCfgRefresh').addEventListener('click', refreshDeviceConfig);
  $('btnCfgApply').addEventListener('click', async () => {
    $('btnCfgApply').disabled = true;
    await window.ticker.applyDeviceConfig(collectDeviceConfig());
    await refreshDeviceConfig();          // show what the device actually took
    $('btnCfgApply').disabled = false;
  });
  refreshDeviceConfig();

  $('btnTestDevice').addEventListener('click', () => window.ticker.testDevice());
  $('btnTestCal').addEventListener('click', () => window.ticker.testCalendar());
  $('btnClearLog').addEventListener('click', () => { $('log').innerHTML = ''; });

  $('btnSave').addEventListener('click', async () => {
    await window.ticker.saveSettings(collectSettings());
    $('saveMsg').textContent = 'saved';
    setTimeout(() => { $('saveMsg').textContent = ''; }, 2000);
  });
});

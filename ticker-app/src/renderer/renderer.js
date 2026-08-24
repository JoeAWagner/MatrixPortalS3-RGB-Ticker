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
  'calendars', 'weatherZip', 'weatherUnits', 'pollSecs', 'weatherMins'];
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
  fillSettings(await window.ticker.getSettings());

  window.ticker.onLog(addLog);
  window.ticker.onState(render);

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

  $('bright').addEventListener('input', (e) => { $('brightVal').textContent = e.target.value; });
  $('btnApply').addEventListener('click', () =>
    window.ticker.sendRaw('/&TH=' + $('theme').value + '/&BR=' + $('bright').value));

  $('btnOpenWeb').addEventListener('click', async () => {
    const cfg = await window.ticker.getSettings();
    addLog({ level: 'info', at: new Date().toISOString(), message: 'Ticker UI: http://' + cfg.tickerIp + '/' });
  });

  $('btnTestDevice').addEventListener('click', () => window.ticker.testDevice());
  $('btnTestCal').addEventListener('click', () => window.ticker.testCalendar());
  $('btnClearLog').addEventListener('click', () => { $('log').innerHTML = ''; });

  $('btnSave').addEventListener('click', async () => {
    await window.ticker.saveSettings(collectSettings());
    $('saveMsg').textContent = 'saved';
    setTimeout(() => { $('saveMsg').textContent = ''; }, 2000);
  });
});

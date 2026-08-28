'use strict';

const { app, BrowserWindow, ipcMain, Tray, Menu, nativeImage, shell } = require('electron');
const path = require('path');
const { Settings } = require('./settings');
const { panelIcon } = require('./trayicon');
const { SyncEngine } = require('./sync/engine');
const device = require('./sync/device');

let win = null;
let tray = null;
let settings = null;
let engine = null;
let quitting = false;

const LOG_CAP = 500;
const logBuffer = [];
let logFile = null;

// A tray app with no log is a black box: when it silently stops working the
// only way to tell is to guess. Everything the UI shows also goes to disk.
function initFileLog() {
  logFile = path.join(app.getPath('userData'), 'ticker.log');
  try {
    const fs = require('fs');
    if (fs.existsSync(logFile) && fs.statSync(logFile).size > 1024 * 1024) {
      fs.renameSync(logFile, logFile + '.1');   // keep one previous file
    }
  } catch { /* logging must never break startup */ }
}

function pushLog(entry) {
  logBuffer.push(entry);
  if (logBuffer.length > LOG_CAP) logBuffer.shift();
  if (win && !win.isDestroyed()) win.webContents.send('log', entry);
  if (logFile) {
    try {
      const line = `${new Date(entry.at).toISOString()}  ${entry.level.toUpperCase().padEnd(5)}  ${entry.message}
`;
      require('fs').appendFileSync(logFile, line, 'utf8');
    } catch { /* ignore */ }
  }
}

// Single instance: a second launch just surfaces the existing window.
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', showWindow);
  app.whenReady().then(init);
}

function init() {
  initFileLog();
  settings = new Settings(app.getPath('userData'));
  pushLog({ level: 'info', message: `Ticker Manager ${app.getVersion()} starting (packaged=${app.isPackaged})`, at: new Date().toISOString() });
  if (settings.loadError) {
    pushLog({ level: 'error', message: settings.loadError, at: new Date().toISOString() });
  }
  if (settings.importedFrom) {
    pushLog({ level: 'info', message: `Imported settings from ${settings.importedFrom}`, at: new Date().toISOString() });
  }

  engine = new SyncEngine(settings);
  engine.on('log', pushLog);
  engine.on('state', (s) => {
    if (win && !win.isDestroyed()) win.webContents.send('state', s);
    updateTray(s);
  });

  createTray();
  createWindow();

  applyAutoStart(settings.get().autoStart);

  const cfg = settings.get();
  if (cfg.appleId && cfg.appPassword) engine.start();
  else pushLog({ level: 'warn', message: 'Add your iCloud credentials in Settings to start syncing', at: new Date().toISOString() });
}

function createWindow() {
  win = new BrowserWindow({
    width: 940,
    height: 760,
    minWidth: 720,
    minHeight: 560,
    show: !settings.get().startMinimized,
    autoHideMenuBar: true,
    icon: nativeImage.createFromBuffer(panelIcon('#ffb000')),
    backgroundColor: '#0b0b0d',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));

  // Closing hides to tray; only an explicit Quit exits.
  win.on('close', (e) => {
    if (!quitting) { e.preventDefault(); win.hide(); }
  });
}

function showWindow() {
  if (!win || win.isDestroyed()) createWindow();
  win.show();
  win.focus();
}

function trayIcon(state) {
  // amber = running, red = ticker unreachable, grey = stopped.
  // Must be a PNG: nativeImage cannot decode SVG, and silently returns an
  // empty image, which is why the tray slot was blank.
  const color = !state || !state.running ? '#8a8a92'
    : state.deviceOnline === false ? '#e05a5a'
      : '#ffb000';
  return nativeImage.createFromBuffer(panelIcon(color));
}

function createTray() {
  tray = new Tray(trayIcon(null));
  tray.setToolTip('Ticker Manager');
  tray.on('click', showWindow);
  updateTray(engine ? engine.state : null);
}

function updateTray(state) {
  if (!tray) return;
  const running = state && state.running;
  const menu = Menu.buildFromTemplate([
    { label: state && state.lastMessageSent ? state.lastMessageSent.replace(/\|/g, '  |  ') : 'No message sent yet', enabled: false },
    { type: 'separator' },
    { label: 'Open Manager', click: showWindow },
    { label: 'Sync now', click: () => engine && engine.tick() },
    { label: running ? 'Stop sync' : 'Start sync', click: () => (running ? engine.stop() : engine.start()) },
    { type: 'separator' },
    { label: 'Open ticker web UI', click: () => shell.openExternal(`http://${settings.get().tickerIp}/`) },
    { type: 'separator' },
    { label: 'Quit', click: () => { quitting = true; app.quit(); } },
  ]);
  tray.setContextMenu(menu);
  tray.setImage(trayIcon(state));
}

function applyAutoStart(enabled) {
  // Electron registers the app itself, so there is no script sitting in a
  // synced folder to go missing at logon.
  //
  // Unpackaged, process.execPath is electron.exe, which has no idea which app
  // to run - it needs the project directory as its first argument. Packaged,
  // the exe IS the app and must not be given one.
  const args = app.isPackaged
    ? ['--hidden']
    : [path.resolve(__dirname, '..'), '--hidden'];

  app.setLoginItemSettings({
    openAtLogin: !!enabled,
    path: process.execPath,
    args,
  });
}

app.on('window-all-closed', (e) => { /* stay alive in the tray */ });
app.on('before-quit', () => { quitting = true; if (engine) engine.stop(); });

// ---- IPC ----------------------------------------------------------------
ipcMain.handle('getState', () => ({ ...engine.state, logs: logBuffer.slice(-200) }));
ipcMain.handle('getSettings', () => settings.get());
ipcMain.handle('getLogPath', () => logFile);

// Changing these means the CalDAV session or poll loop has to be rebuilt.
// Anything else is just wording or presentation, and a full restart would
// reconnect to iCloud for no reason - which is how rate limits get hit.
const RECONNECT_KEYS = ['appleId', 'appPassword', 'calendars', 'tickerIp',
  'tickerUser', 'tickerPass', 'pollSecs', 'lookaheadHours'];

ipcMain.handle('saveSettings', (_e, patch) => {
  const before = settings.get();
  const after = settings.update(patch);

  if (patch.autoStart !== undefined && patch.autoStart !== before.autoStart) {
    applyAutoStart(after.autoStart);
  }
  pushLog({ level: 'info', message: 'Settings saved', at: new Date().toISOString() });

  if (!after.appleId || !after.appPassword) return after;

  const needsReconnect = RECONNECT_KEYS.some((k) => k in patch && patch[k] !== before[k]);
  if (needsReconnect) engine.restart();
  else engine.refresh();          // re-push with the new wording, keep the session
  return after;
});

ipcMain.handle('syncNow', () => engine.tick());
ipcMain.handle('syncDeviceTime', () => engine.syncDeviceTime());

ipcMain.handle('getDeviceConfig', () => device.getConfig(settings.get()));

ipcMain.handle('applyDeviceConfig', async (_e, values) => {
  const res = await device.applyConfig(settings.get(), values);
  pushLog({
    level: res.ok ? 'info' : 'warn',
    message: res.ok ? 'Panel settings applied' : `Applying panel settings failed: ${res.reason}`,
    at: new Date().toISOString(),
  });
  return res;
});
ipcMain.handle('startSync', () => engine.start());
ipcMain.handle('stopSync', () => engine.stop());

ipcMain.handle('sendMessage', (_e, text) => device.sendMessage(settings.get(), text));
ipcMain.handle('sendRaw', (_e, pathPart) => device.sendRaw(settings.get(), pathPart));

ipcMain.handle('testDevice', async () => {
  const res = await device.sendRaw(settings.get(), '/&nc=probe');
  pushLog({ level: res.ok ? 'info' : 'warn', message: res.ok ? 'Ticker reachable' : `Ticker test failed: ${res.reason}`, at: new Date().toISOString() });
  return res;
});

ipcMain.handle('testCalendar', async () => {
  try {
    const { CalendarSource } = require('./sync/caldav');
    const src = new CalendarSource(settings.get(), () => {});
    await src.connect();
    const names = src.calendars.map((c) => c.displayName);
    pushLog({ level: 'info', message: `iCloud OK - ${names.length} calendar(s): ${names.join(', ')}`, at: new Date().toISOString() });
    return { ok: true, calendars: names };
  } catch (err) {
    pushLog({ level: 'error', message: `iCloud test failed: ${err.message}`, at: new Date().toISOString() });
    return { ok: false, reason: err.message };
  }
});

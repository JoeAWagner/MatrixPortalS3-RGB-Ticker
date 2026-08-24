'use strict';

const fs = require('fs');
const path = require('path');

const DEFAULTS = {
  tickerIp: 'ticker.local',
  tickerUser: 'joe',
  tickerPass: '',
  appleId: '',
  appPassword: '',
  calendars: '',            // comma-separated names; empty = all
  weatherZip: '11786',
  weatherUnits: 'fahrenheit',
  pollSecs: 60,
  weatherMins: 15,
  heartbeatMins: 10,
  lookaheadHours: 12,
  maxTitle: 28,
  freeMessage: 'I am Free',
  // Titles skipped when choosing "Up next" - recurring placeholders like a
  // daily Lunch block would otherwise hide the actual next meeting. Still
  // shown as NOW while they are happening, which is useful status.
  ignoreNext: 'Lunch',
  startMinimized: true,
  autoStart: true,
};

// Legacy PowerShell config, so upgrading from the script version doesn't mean
// retyping credentials. Line-based on purpose: a hand-built RegExp for this
// is easy to get subtly wrong.
function parsePs1Config(text) {
  const out = {};
  for (const line of text.split(/\r?\n/)) {
    const m = line.match(/^\s*\$env:(\w+)\s*=\s*"([^"]*)"/);
    if (m) out[m[1]] = m[2];
  }
  return out;
}

function importLegacy(candidates) {
  for (const file of candidates) {
    try {
      if (!fs.existsSync(file)) continue;
      const env = parsePs1Config(fs.readFileSync(file, 'utf8'));
      if (!env.APPLE_ID && !env.TICKER_IP) continue;
      return {
        imported: file,
        values: {
          tickerIp: env.TICKER_IP || DEFAULTS.tickerIp,
          tickerUser: env.TICKER_USER || DEFAULTS.tickerUser,
          tickerPass: env.TICKER_PASS || '',
          appleId: env.APPLE_ID || '',
          appPassword: env.APPLE_APP_PASSWORD || '',
          calendars: env.CALENDARS || '',
          weatherZip: env.WEATHER_ZIP || DEFAULTS.weatherZip,
        },
      };
    } catch { /* try the next candidate */ }
  }
  return null;
}

class Settings {
  constructor(userDataDir) {
    this.file = path.join(userDataDir, 'settings.json');
    this.values = { ...DEFAULTS };
    this.importedFrom = null;
    this.loadError = null;
    this.load();
  }

  load() {
    if (fs.existsSync(this.file)) {
      try {
        // Strip a UTF-8 BOM: anything that rewrites this file with a Windows
        // editor or PowerShell's Set-Content adds one, and JSON.parse rejects
        // it. Silently falling through would discard every saved setting.
        const text = fs.readFileSync(this.file, 'utf8').replace(/^﻿/, '');
        Object.assign(this.values, JSON.parse(text));
        return;
      } catch (err) {
        // Never quietly reset someone's configuration. Keep the bad file so
        // it can be inspected, and record why the defaults came back.
        this.loadError = `settings.json could not be read (${err.message})`;
        try {
          const backup = this.file + '.bad';
          fs.copyFileSync(this.file, backup);
          this.loadError += `; kept a copy at ${backup}`;
        } catch { /* best effort */ }
      }
    }
    const legacy = importLegacy([
      'C:/dev/ticker/sync.config.ps1',
      path.join(process.env.USERPROFILE || '', 'iCloudDrive/Projects/Arduino/MatrixPortalS3_Scrolling_Web_JW_10/sync.config.ps1'),
    ]);
    if (legacy) {
      Object.assign(this.values, legacy.values);
      this.importedFrom = legacy.imported;
      this.save();
    }
  }

  save() {
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    fs.writeFileSync(this.file, JSON.stringify(this.values, null, 2), 'utf8');
  }

  get() { return { ...this.values }; }

  update(patch) {
    Object.assign(this.values, patch);
    this.save();
    return this.get();
  }
}

module.exports = { Settings, DEFAULTS, parsePs1Config };

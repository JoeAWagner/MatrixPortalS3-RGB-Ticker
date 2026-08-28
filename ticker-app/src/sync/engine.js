'use strict';

const { EventEmitter } = require('events');
const { CalendarSource } = require('./caldav');
const { getWeather } = require('./weather');
const device = require('./device');

const SEP = '|';   // firmware splits the message on this into stacked rows

function truncate(s, max) {
  return s.length <= max ? s : s.slice(0, Math.max(1, max - 3)) + '...';
}

// Relative time only when an event is imminent; otherwise a clock time. A
// live countdown would rewrite the message every minute and restart the
// scroll on the panel each time.
function formatWhen(date, now) {
  const mins = Math.floor((date - now) / 60000);
  if (mins >= 0 && mins < 15) return mins === 1 ? 'in 1 min' : `in ${mins} min`;
  let h = date.getHours() % 12;
  if (h === 0) h = 12;
  const ampm = date.getHours() < 12 ? 'AM' : 'PM';
  const clock = `${h}:${String(date.getMinutes()).padStart(2, '0')} ${ampm}`;
  if (date.toDateString() === now.toDateString()) return clock;
  return `${date.toLocaleDateString(undefined, { weekday: 'short' })} ${clock}`;
}

// The same meeting can live in two calendars, and recurrence expansion can
// re-emit an overridden instance.
function dedupe(events) {
  const seen = new Set();
  return events.filter((e) => {
    const key = `${e.start.getTime()}|${e.end.getTime()}|${e.summary}`;
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
}

function parsePatterns(list) {
  return String(list || '')
    .split(',')
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean);
}

function isIgnored(summary, patterns) {
  const t = String(summary || '').toLowerCase();
  return patterns.some((p) => t.includes(p));
}

// ignoreNext holds titles that should never be announced as "Up next" -
// standing placeholders such as a daily Lunch block, which would otherwise
// mask the real next meeting. They still count as NOW while in progress,
// because "NOW: Lunch" is genuinely useful status.
function nowAndNext(events, now, ignoreNext) {
  const patterns = parsePatterns(ignoreNext);
  const active = events.filter((e) => e.start <= now && now <= e.end);
  let upcoming = events.filter((e) => e.start > now);
  if (patterns.length) upcoming = upcoming.filter((e) => !isIgnored(e.summary, patterns));
  active.sort((a, b) => b.start - a.start);
  upcoming.sort((a, b) => a.start - b.start);
  return { current: active[0] || null, next: upcoming[0] || null };
}

class SyncEngine extends EventEmitter {
  constructor(settings) {
    super();
    this.settings = settings;
    this.timer = null;
    this.source = null;
    this.state = {
      running: false, deviceOnline: null, lastError: null,
      current: null, next: null, weather: null, calendars: [], deviceTime: null,
      lastSync: null, lastMessageSent: null,
      paused: false, pausedUntil: null,
    };
    this._lastMessage = null;
    this._lastWeather = null;
    this._weatherDue = 0;
    this._heartbeatDue = 0;
  }

  log(level, message) {
    this.emit('log', { level, message, at: new Date().toISOString() });
  }

  patch(p) {
    Object.assign(this.state, p);
    this.emit('state', { ...this.state });
  }

  start() {
    if (this.state.running) return;
    const cfg = this.settings.get();
    this.source = new CalendarSource(cfg, (l, m) => this.log(l, m));
    this.patch({ running: true, lastError: null });
    this.log('info', 'Sync started');
    this.tick();
    this.timer = setInterval(() => this.tick(), (cfg.pollSecs || 60) * 1000);
  }

  stop() {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    this.patch({ running: false });
    this.log('info', 'Sync stopped');
  }

  // Force the next tick to re-push even if the composed text is unchanged.
  // Used when a display preference changes: there is no reason to tear down
  // the CalDAV session (and risk Apple's rate limiter) just to reword a row.
  refresh() {
    this._lastMessage = null;
    this._lastWeather = null;
    return this.tick();
  }

  restart() {
    this.stop();
    this._lastMessage = null;
    this._lastWeather = null;
    this._weatherDue = 0;
    this._heartbeatDue = 0;
    this.start();
  }

  async tick() {
    const cfg = this.settings.get();
    const now = new Date();
    const beat = Date.now() >= this._heartbeatDue;
    if (beat) this._heartbeatDue = Date.now() + (cfg.heartbeatMins || 10) * 60000;

    await this.tickWeather(cfg, beat);
    await this.tickDeviceTime(cfg);

    if (this.state.paused && Date.now() < this.state.pausedUntil) return;
    if (this.state.paused) {
      this.patch({ paused: false, pausedUntil: null });
      this.log('info', 'Backoff over, resuming calendar polling');
    }
    await this.tickCalendar(cfg, now, beat);
  }

  // Weather runs in its own try block so an iCloud outage cannot stop it.
  async tickWeather(cfg, beat) {
    if (Date.now() < this._weatherDue && !beat) return;
    try {
      const wx = await getWeather(cfg.weatherZip, cfg.weatherUnits);
      if (Date.now() >= this._weatherDue) {
        this._weatherDue = Date.now() + (cfg.weatherMins || 15) * 60000;
      }
      this.patch({ weather: wx });
      const key = `${wx.text}|${wx.icon}`;
      if (!wx.text || (key === this._lastWeather && !beat)) return;
      const res = await device.sendWeather(cfg, wx.text, wx.icon);
      if (res.ok) {
        this._lastWeather = key;
        this.log('info', `Weather: ${wx.text}`);
      } else {
        this.log('warn', `Weather push failed: ${res.reason}`);
      }
    } catch (err) {
      this.log('warn', `Weather lookup failed: ${err.message}`);
    }
  }

  // Read the ticker's own clock so the UI can show drift. Silent on failure:
  // if the panel is unreachable the message push already reports that.
  async tickDeviceTime(cfg) {
    const t = await device.getTime(cfg);
    this.patch({ deviceTime: t });
  }

  async syncDeviceTime() {
    const cfg = this.settings.get();
    const res = await device.setTime(cfg);
    if (res.ok) {
      this.log('info', 'Pushed this machine clock to the ticker');
      await this.tickDeviceTime(cfg);
    } else {
      this.log('warn', `Time sync failed: ${res.reason}`);
    }
    return res;
  }

  async tickCalendar(cfg, now, beat) {
    try {
      const { events, failures, total } = await this.source.fetchWindow(now);
      const { current, next } = nowAndNext(dedupe(events), now, cfg.ignoreNext);

      // A calendar that did not answer is not a calendar that is empty.
      // Announcing free time on a partial read is the worst possible output.
      if (failures > 0 && !current) {
        this.log('warn', `${failures} of ${total} calendars did not respond - holding last status`);
        this.patch({ lastSync: now.toISOString() });
        return;
      }

      const parts = [current ? `NOW: ${truncate(current.summary, cfg.maxTitle)}` : cfg.freeMessage];
      if (next) parts.push(`NEXT: ${formatWhen(next.start, now)} ${truncate(next.summary, cfg.maxTitle)}`);
      const message = parts.join(SEP);

      this.patch({
        current, next,
        lastSync: now.toISOString(),
        calendars: this.source.calendars.map((c) => c.displayName),
      });

      if (message === this._lastMessage && !beat) return;

      const res = await device.sendMessage(cfg, message);
      if (res.ok) {
        this._lastMessage = message;
        if (this.state.deviceOnline === false) this.log('info', 'Ticker reachable again');
        this.patch({ deviceOnline: true, lastError: null, lastMessageSent: message });
        this.log('info', `Sent: ${message}`);
      } else {
        if (this.state.deviceOnline !== false) this.log('warn', `Ticker update failed: ${res.reason}`);
        this.patch({ deviceOnline: false, lastError: res.reason });
      }
    } catch (err) {
      const msg = String(err.message || err);
      if (/rate|429|too many/i.test(msg)) {
        this.patch({ paused: true, pausedUntil: Date.now() + 30 * 60000 });
        this.log('warn', 'iCloud rate limited - pausing calendar 30 min (weather keeps updating)');
      } else {
        this.log('error', `Calendar error: ${msg}`);
        this.patch({ lastError: msg });
      }
      if (this.source) this.source.invalidate('error during poll');
    }
  }
}

module.exports = { SyncEngine, formatWhen, truncate, dedupe, nowAndNext, isIgnored, parsePatterns };

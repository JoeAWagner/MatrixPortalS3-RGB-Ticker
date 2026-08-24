'use strict';

// iCloud CalDAV via tsdav, with ICS parsed by node-ical (which expands RRULEs).
//
// Design note: the Python version kept one long-lived session and, when every
// calendar started failing, incremented a counter but never rebuilt the
// session - so a stale login froze the calendar rows indefinitely while
// weather kept updating. Here a "total failure" explicitly invalidates the
// client so the next poll reconnects.

const ical = require('node-ical');

const CALDAV_URL = 'https://caldav.icloud.com';

class CalendarSource {
  constructor(cfg, logger) {
    this.cfg = cfg;
    this.log = logger;
    this.client = null;
    this.calendars = [];
  }

  invalidate(why) {
    if (this.client) this.log('info', `Dropping iCloud session (${why}); will reconnect`);
    this.client = null;
    this.calendars = [];
  }

  async connect() {
    const { createDAVClient } = await import('tsdav');
    this.client = await createDAVClient({
      serverUrl: CALDAV_URL,
      credentials: { username: this.cfg.appleId, password: this.cfg.appPassword },
      authMethod: 'Basic',
      defaultAccountType: 'caldav',
    });

    let calendars = await this.client.fetchCalendars();

    // Only keep collections that actually hold events (skips Reminders).
    calendars = calendars.filter((c) => {
      const ct = c.components;
      return !ct || ct.length === 0 || ct.includes('VEVENT');
    });

    const wanted = (this.cfg.calendars || '')
      .split(',').map((s) => s.trim().toLowerCase()).filter(Boolean);
    if (wanted.length) {
      const picked = calendars.filter((c) => wanted.includes(String(c.displayName || '').toLowerCase()));
      if (picked.length) calendars = picked;
      else this.log('warn', `Calendar filter matched nothing; using all`);
    }

    this.calendars = calendars;
    this.log('info', `Connected to iCloud - ${calendars.length} calendar(s): `
      + calendars.map((c) => c.displayName).join(', '));
  }

  /**
   * Returns { events, failures, total }. An event we could not read counts as
   * a failure, never as "nothing scheduled" - claiming free time on a partial
   * read is the one outcome worse than showing stale data.
   */
  async fetchWindow(now) {
    if (!this.client) await this.connect();

    const start = new Date(now.getTime() - 8 * 3600 * 1000);
    const end = new Date(now.getTime() + (this.cfg.lookaheadHours || 12) * 3600 * 1000);

    const events = [];
    let failures = 0;

    for (const calendar of this.calendars) {
      let objects;
      try {
        objects = await this.client.fetchCalendarObjects({
          calendar,
          timeRange: { start: start.toISOString(), end: end.toISOString() },
        });
      } catch (err) {
        failures++;
        this.log('debug', `Query failed for ${calendar.displayName}: ${err.message}`);
        continue;
      }

      for (const obj of objects) {
        if (!obj.data) continue;
        try {
          const parsed = ical.sync.parseICS(obj.data);
          for (const item of Object.values(parsed)) {
            if (!item || item.type !== 'VEVENT') continue;
            collectOccurrences(item, start, end, events);
          }
        } catch (err) {
          failures++;
          this.log('debug', `Parse failed: ${err.message}`);
        }
      }
    }

    // Every calendar failing is the signature of a dead or throttled session.
    if (failures > 0 && this.calendars.length > 0 && failures >= this.calendars.length) {
      this.invalidate('all calendars failed');
    }

    return { events, failures, total: this.calendars.length };
  }
}

// rrule.js returns occurrences whose UTC fields encode the intended WALL
// CLOCK time, not a real UTC instant. Reading them directly shifts every
// recurring event by the local UTC offset - a 2:30 PM meeting shows up as
// 10:30 AM in US Eastern. Rebuild the date from those fields in local time,
// which also picks up the correct DST offset for that particular date.
//
// This assumes the event's timezone matches this machine's, which is true for
// your own calendar; a meeting authored in another timezone is approximate.
function wallClock(d) {
  return new Date(
    d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(),
    d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds(),
  );
}

// node-ical keys exdate/recurrences by local calendar day.
function dayKey(d) {
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

// Push an event (and any recurrences inside the window) into `out`.
function collectOccurrences(item, windowStart, windowEnd, out) {
  const summary = String(item.summary || '(busy)').trim();

  // All-day events are dates, not datetimes - they are not "meetings".
  if (item.datetype === 'date') return;
  if (!item.start) return;

  const durationMs = item.end && item.start
    ? new Date(item.end).getTime() - new Date(item.start).getTime()
    : 0;

  if (!item.rrule) {
    out.push({ start: new Date(item.start), end: new Date(item.end || item.start), summary });
    return;
  }

  // Recurring: expand within the window, honoring EXDATE and overrides.
  // Widen the query window so an occurrence near the edge is not lost to the
  // wall-clock correction shifting it by up to a day.
  const pad = 36 * 3600 * 1000;
  const raw = item.rrule.between(
    new Date(windowStart.getTime() - pad),
    new Date(windowEnd.getTime() + pad),
    true,
  );

  for (const r of raw) {
    const start = wallClock(r);
    const key = dayKey(start);
    if (item.exdate && (item.exdate[key] || item.exdate[r.toISOString().slice(0, 10)])) continue;

    const override = item.recurrences
      && (item.recurrences[key] || item.recurrences[r.toISOString().slice(0, 10)]);

    if (override) {
      // Overrides are parsed VEVENTs with real Dates, so no correction here.
      out.push({
        start: new Date(override.start),
        end: new Date(override.end || override.start),
        summary: String(override.summary || summary).trim(),
      });
      continue;
    }

    const end = new Date(start.getTime() + durationMs);
    if (end < windowStart || start > windowEnd) continue;   // re-apply the real window
    out.push({ start, end, summary });
  }
}

module.exports = { CalendarSource };

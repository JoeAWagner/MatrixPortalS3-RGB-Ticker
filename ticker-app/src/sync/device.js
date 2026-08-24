'use strict';

// Talking to the Matrix Portal's HTTP API. Every call is a plain GET with
// Basic Auth, matching the firmware's /&PARAM= scheme.

function authHeader(user, pass) {
  return 'Basic ' + Buffer.from(`${user}:${pass}`).toString('base64');
}

/**
 * Send one request to the ticker. Never throws for network problems - callers
 * get {ok:false, reason} so a powered-off panel can't take the app down.
 */
async function send(cfg, path, { timeoutMs = 5000, attempts = 3, delayMs = 1500 } = {}) {
  let reason = 'unknown error';

  for (let i = 0; i < attempts; i++) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const nc = Math.floor(Math.random() * 99999);
      const url = `http://${cfg.tickerIp}${path}/&nc=${nc}`;
      const res = await fetch(url, {
        headers: { Authorization: authHeader(cfg.tickerUser, cfg.tickerPass) },
        signal: controller.signal,
      });
      if (res.status === 401) {
        // Reached the device but it rejected us - retrying cannot help.
        return { ok: false, reason: 'ticker rejected login - check username/password' };
      }
      if (!res.ok) return { ok: false, reason: `ticker returned HTTP ${res.status}` };
      return { ok: true };
    } catch (err) {
      reason = err.name === 'AbortError'
        ? `timed out reaching ${cfg.tickerIp}`
        : `cannot reach ${cfg.tickerIp} (device off, or wrong IP?)`;
    } finally {
      clearTimeout(timer);
    }
    if (i + 1 < attempts) await new Promise((r) => setTimeout(r, delayMs));
  }
  return { ok: false, reason };
}

const enc = (s) => encodeURIComponent(s);

const sendMessage = (cfg, text) => send(cfg, `/&MSG=${enc(text)}`);

const sendWeather = (cfg, text, icon) =>
  send(cfg, `/&WX=${enc(text)}/&WI=${icon === null || icon === undefined ? -1 : icon}`);

const sendRaw = (cfg, path) => send(cfg, path);

/**
 * Ask the ticker what time it thinks it is. Returns null when unreachable -
 * a missing clock reading is not an error worth surfacing on every poll.
 */
async function getTime(cfg, { timeoutMs = 5000 } = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const nc = Math.floor(Math.random() * 99999);
    const res = await fetch(`http://${cfg.tickerIp}/&TM=1/&nc=${nc}`, {
      headers: { Authorization: authHeader(cfg.tickerUser, cfg.tickerPass) },
      signal: controller.signal,
    });
    if (!res.ok) return null;
    const j = await res.json();
    if (typeof j.epoch !== 'number') return null;
    // Drift is measured against this machine, which is the reference the user
    // is comparing against anyway.
    j.driftSec = j.epoch - Math.floor(Date.now() / 1000);
    j.readAt = Date.now();
    return j;
  } catch {
    return null;
  } finally {
    clearTimeout(timer);
  }
}

/** Push this machine's clock to the ticker. */
function setTime(cfg) {
  return send(cfg, `/&ST=${Math.floor(Date.now() / 1000)}`);
}

module.exports = { send, sendMessage, sendWeather, sendRaw, getTime, setTime };

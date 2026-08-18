#!/usr/bin/env python3
"""
Apple Calendar -> LED Ticker Sync  (Matrix Portal S3 / RGB version)

Polls iCloud CalDAV and shows both what's happening NOW and what's UP NEXT
on the ticker, e.g.:

    NOW: Standup    |    UP NEXT: 2:00 PM Design Review
    I am Free    |    UP NEXT: in 25 min 1:1 with Sam

It only uses the /&MSG= web API, which the RGB firmware keeps compatible,
so nothing on the device needs to change.

Setup:
  1. pip install caldav requests
  2. Generate an app-specific password at appleid.apple.com
     -> Sign-In & Security -> App-Specific Passwords -> "LED Ticker"
  3. Configure via environment variables (preferred) or edit the block below.
  4. Run: python calendar_sync.py

Environment variables (override the defaults below):
  TICKER_IP, TICKER_USER, TICKER_PASS   -> the Matrix Portal web UI
  APPLE_ID, APPLE_APP_PASSWORD          -> your iCloud login
Using env vars keeps your credentials out of the source file.
"""

import os
import re
import time
import datetime
import random
import logging
import urllib.parse
import warnings

import requests
import caldav
import caldav.lib.error          # submodule needs an explicit import

# caldav's date_search still works fine; silence its deprecation notice.
warnings.filterwarnings("ignore", message=r".*date_search.*")

# Present in newer caldav releases; treated as "never matches" if absent.
RATE_LIMIT_ERROR = getattr(caldav.lib.error, "RateLimitError", ())

# ── Configuration (env vars win; edit the fallbacks if you prefer) ────────────

ESP32_IP   = os.environ.get("TICKER_IP",   "192.168.1.XXX")  # Serial Monitor shows it
ESP32_USER = os.environ.get("TICKER_USER", "joe")
ESP32_PASS = os.environ.get("TICKER_PASS", "YOUR_ESP32_PASSWORD")

APPLE_ID   = os.environ.get("APPLE_ID",           "you@icloud.com")
APP_PASS   = os.environ.get("APPLE_APP_PASSWORD", "xxxx-xxxx-xxxx-xxxx")

# Optional: limit which calendars are checked, e.g. CALENDARS="Work,Home".
# Empty means use every calendar on the account.
ONLY_CALENDARS = os.environ.get("CALENDARS", "").strip()

FREE_MSG        = "I am Free"   # shown as the "now" part when no meeting is active
POLL_SECS       = 60           # how often to check (seconds)
LOOKAHEAD_HOURS = 12           # how far ahead "Up Next" looks
# At text size 1 a 64px row shows ~10 chars, so anything longer scrolls.
# 28 keeps a typical title to about two scroll-widths instead of a marathon.
MAX_TITLE       = 28           # truncate long event titles (keeps scrolls sane)
SEP             = "|"          # row separator; firmware splits on '|' into lines

REQUEST_TIMEOUT = 5            # seconds per HTTP attempt to the ticker
SEND_ATTEMPTS   = 3            # attempts per update before giving up this cycle
RETRY_DELAY     = 2            # seconds between those attempts

# Weather (top row). Open-Meteo is free and needs no API key.
WEATHER_ZIP     = os.environ.get("WEATHER_ZIP", "11786")   # Shoreham, NY
WEATHER_UNITS   = "fahrenheit"   # or "celsius"
WEATHER_MINS    = 15             # refresh interval (minutes)

GEOCODE_URL = "https://geocoding-api.open-meteo.com/v1/search"
FORECAST_URL = "https://api.open-meteo.com/v1/forecast"

# Firmware icon ids (must match iconBits[] in the .ino)
ICON_SUN, ICON_CLOUD, ICON_PARTLY, ICON_RAIN, ICON_SNOW, ICON_STORM, ICON_FOG = range(7)

# WMO weather codes -> (short label that fits a 64px row, icon id)
WMO = {
    0:  ("Clear",    ICON_SUN),
    1:  ("Clear",    ICON_SUN),
    2:  ("Cloudy",   ICON_PARTLY),
    3:  ("Overcast", ICON_CLOUD),
    45: ("Fog",      ICON_FOG),
    48: ("Fog",      ICON_FOG),
    51: ("Drizzle",  ICON_RAIN),
    53: ("Drizzle",  ICON_RAIN),
    55: ("Drizzle",  ICON_RAIN),
    56: ("Freezing", ICON_SNOW),
    57: ("Freezing", ICON_SNOW),
    61: ("Rain",     ICON_RAIN),
    63: ("Rain",     ICON_RAIN),
    65: ("Hvy Rain", ICON_RAIN),
    66: ("Icy Rain", ICON_SNOW),
    67: ("Icy Rain", ICON_SNOW),
    71: ("Snow",     ICON_SNOW),
    73: ("Snow",     ICON_SNOW),
    75: ("Hvy Snow", ICON_SNOW),
    77: ("Snow",     ICON_SNOW),
    80: ("Showers",  ICON_RAIN),
    81: ("Showers",  ICON_RAIN),
    82: ("Showers",  ICON_RAIN),
    85: ("Snow",     ICON_SNOW),
    86: ("Snow",     ICON_SNOW),
    95: ("Storm",    ICON_STORM),
    96: ("Storm",    ICON_STORM),
    99: ("Storm",    ICON_STORM),
}

# ──────────────────────────────────────────────────────────────────────────────

CALDAV_URL = "https://caldav.icloud.com"

# Always log to the console; also log to a rotating file if SYNC_LOG is set
# (the background-service launcher sets this).
_handlers = [logging.StreamHandler()]
_log_file = os.environ.get("SYNC_LOG")
if _log_file:
    from logging.handlers import RotatingFileHandler
    _handlers.append(RotatingFileHandler(
        _log_file, maxBytes=1_000_000, backupCount=3, encoding="utf-8"))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
    handlers=_handlers,
)
log = logging.getLogger(__name__)


# ── Calendar helpers ──────────────────────────────────────────────────────────

def retry_after_seconds(exc, default: int = 1800) -> int:
    """
    Pull the server's Retry-After hint out of a rate-limit error.
    Apple throttles repeated CalDAV logins and tells us how long to wait;
    ignoring it just extends the block.
    """
    m = re.search(r"Retry after:?\s*(\d+)", str(exc))
    seconds = int(m.group(1)) if m else default
    return max(60, min(seconds, 7200))     # clamp to a sane 1 min .. 2 h


def connect() -> list:
    """Open a CalDAV session once and return the list of calendars to reuse."""
    client = caldav.DAVClient(url=CALDAV_URL, username=APPLE_ID, password=APP_PASS)
    calendars = client.principal().calendars()

    # Optional whitelist: CALENDARS="Work,Personal". Fewer calendars means
    # fewer CalDAV requests per poll, which also reduces rate-limit risk.
    if ONLY_CALENDARS:
        wanted = {n.strip().lower() for n in ONLY_CALENDARS.split(",") if n.strip()}
        picked = [c for c in calendars
                  if str(getattr(c, "name", "") or "").lower() in wanted]
        if picked:
            calendars = picked
        else:
            log.warning("CALENDARS=%r matched nothing; using all calendars",
                        ONLY_CALENDARS)

    names = ", ".join(str(getattr(c, "name", "?")) for c in calendars)
    log.info("Connected to iCloud - %d calendar(s): %s", len(calendars), names)
    return calendars


def _as_utc(dt):
    """Normalize a datetime to UTC-aware. Returns date objects unchanged."""
    if isinstance(dt, datetime.datetime):
        if dt.tzinfo is None:
            return dt.replace(tzinfo=datetime.timezone.utc)
        return dt.astimezone(datetime.timezone.utc)
    return dt


def collect_events(calendars, now):
    """
    Return (events, failures) where events is a list of (start, end, summary)
    tuples and failures counts calendars that did not answer.

    The distinction matters: a calendar that fails to respond is NOT the same
    as a calendar with nothing on it. If we treated a failure as "empty" we
    could report "I am Free" while a meeting is actually in progress.
    """
    window_start = now - datetime.timedelta(hours=8)   # catch long meetings
    window_end   = now + datetime.timedelta(hours=LOOKAHEAD_HOURS)

    events   = []
    failures = 0
    for calendar in calendars:
        try:
            found = calendar.date_search(start=window_start, end=window_end, expand=True)
        except caldav.lib.error.ReportError:
            # Reminders/contacts collections don't support event reports.
            # That's a genuine "nothing here", not a failure.
            continue
        except Exception as e:
            failures += 1
            log.debug("Calendar query failed (%s): %s", getattr(calendar, "name", "?"), e)
            continue

        for event in found:
            try:
                ve = event.vobject_instance.vevent
                dtstart = ve.dtstart.value
                dtend   = ve.dtend.value if hasattr(ve, "dtend") else dtstart

                # Skip all-day events — they are date objects, not datetimes
                if isinstance(dtstart, datetime.date) and not isinstance(dtstart, datetime.datetime):
                    continue

                dtstart = _as_utc(dtstart)
                dtend   = _as_utc(dtend)
                summary = ve.summary.value.strip() if hasattr(ve, "summary") else "(busy)"
                events.append((dtstart, dtend, summary))
            except AttributeError:
                continue   # event missing expected fields

    return events, failures


def now_and_next(events, now):
    """From the event list, pick the currently-active event and the soonest upcoming one."""
    active   = [e for e in events if e[0] <= now <= e[1]]
    upcoming = [e for e in events if e[0] > now]
    now_ev  = max(active,   key=lambda e: e[0]) if active   else None   # most recently started
    next_ev = min(upcoming, key=lambda e: e[0]) if upcoming else None   # soonest to start
    return now_ev, next_ev


def _title(summary: str) -> str:
    return summary if len(summary) <= MAX_TITLE else summary[:MAX_TITLE - 3] + "..."


def _fmt_when(dt, now) -> str:
    """
    Human-friendly start time: 'in 12 min', '2:00 PM', or 'Wed 9:00 AM'.
    Relative ('in X min') is only used when the event is imminent, so the
    message (and therefore the scroll) doesn't churn every minute for the
    whole hour beforehand — a clock time stays stable until the event starts.
    """
    mins = int((dt - now).total_seconds() // 60)
    if 0 <= mins < 15:
        return "in 1 min" if mins == 1 else f"in {mins} min"

    local = dt.astimezone()
    hour12 = local.hour % 12 or 12
    ampm = "AM" if local.hour < 12 else "PM"
    clock = f"{hour12}:{local.minute:02d} {ampm}"
    if local.date() != now.astimezone().date():
        clock = f"{local:%a} {clock}"      # e.g. "Wed 9:00 AM"
    return clock


def compose(now_ev, next_ev, now) -> str:
    """
    Build the ticker string from the now/next events. Rows are separated by
    '|', which the firmware splits into stacked lines on the panel.
    """
    parts = [f"NOW: {_title(now_ev[2])}" if now_ev else FREE_MSG]
    if next_ev:
        parts.append(f"NEXT: {_fmt_when(next_ev[0], now)} {_title(next_ev[2])}")
    return SEP.join(parts)


# ── Weather ───────────────────────────────────────────────────────────────────

_geo_cache = {}


def _geocode(zip_code):
    """Resolve a US ZIP to (lat, lon, name). Cached for the process lifetime."""
    if zip_code in _geo_cache:
        return _geo_cache[zip_code]
    r = requests.get(GEOCODE_URL,
                     params={"name": zip_code, "count": 1, "country": "US"},
                     timeout=REQUEST_TIMEOUT)
    r.raise_for_status()
    results = r.json().get("results") or []
    if not results:
        raise ValueError(f"could not geocode ZIP {zip_code}")
    hit = results[0]
    loc = (hit["latitude"], hit["longitude"], hit.get("name", zip_code))
    _geo_cache[zip_code] = loc
    return loc


def get_weather():
    """
    Return (text, icon_id) for the ticker's top row, e.g. ('72F Clear', 0).
    Returns (None, None) if the lookup fails (caller keeps the previous value).
    """
    try:
        lat, lon, _name = _geocode(WEATHER_ZIP)
        r = requests.get(FORECAST_URL, params={
            "latitude": lat, "longitude": lon,
            "current": "temperature_2m,weather_code",
            "temperature_unit": WEATHER_UNITS,
            "timezone": "auto",
        }, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        cur = r.json()["current"]
        temp = round(cur["temperature_2m"])
        unit = "F" if WEATHER_UNITS == "fahrenheit" else "C"
        desc, icon = WMO.get(cur["weather_code"], ("", -1))
        return f"{temp}{unit} {desc}".strip(), icon
    except Exception as e:
        log.warning("Weather lookup failed: %s", e)
        return None, None


# ── Ticker output ─────────────────────────────────────────────────────────────

def send_ticker(value: str, param: str = "MSG", icon: int | None = None):
    """
    Push a URL-encoded value to the ESP32 (param is MSG or WX), retrying a few
    times on a failed connection. Returns (ok, reason). Never raises for network
    problems, so an unplugged or unreachable device can't crash the sync loop.
    """
    encoded = urllib.parse.quote(value)
    suffix  = f"/&WI={icon}" if icon is not None else ""
    reason  = "unknown error"
    for attempt in range(SEND_ATTEMPTS):
        nc  = random.randint(1, 99999)   # cache-buster, same as the web UI
        url = f"http://{ESP32_IP}/&{param}={encoded}{suffix}/&nc={nc}"
        try:
            resp = requests.get(url, auth=(ESP32_USER, ESP32_PASS), timeout=REQUEST_TIMEOUT)
            resp.raise_for_status()
            return True, "ok"
        except requests.exceptions.HTTPError as e:
            # Reached the device but it refused us (e.g. 401) — retrying won't help.
            code = e.response.status_code if e.response is not None else "?"
            return False, f"ticker rejected request (HTTP {code}) - check TICKER_USER/TICKER_PASS"
        except requests.exceptions.ConnectionError:
            reason = f"cannot reach {ESP32_IP} (device off, or wrong IP?)"
        except requests.exceptions.Timeout:
            reason = f"timed out reaching {ESP32_IP} (device slow or busy?)"
        except requests.exceptions.RequestException as e:
            reason = f"request failed: {e}"
        if attempt + 1 < SEND_ATTEMPTS:
            time.sleep(RETRY_DELAY)
    return False, reason


# ── Main loop ─────────────────────────────────────────────────────────────────

def main() -> None:
    log.info("Calendar sync started - polling every %ds", POLL_SECS)
    calendars    = None
    last_message = None
    online       = True     # ticker reachability, tracked only for tidy logging
    last_reason  = None
    last_weather = None
    weather_due  = 0.0      # monotonic deadline for the next weather refresh
    stale        = False    # true while some calendar is failing to respond

    calendar_due = 0.0      # set ahead when iCloud rate-limits us

    while True:
        # Weather first, and in its own try: it must keep working even when
        # iCloud is unreachable or throttling us.
        try:
            if time.monotonic() >= weather_due:
                wx, icon = get_weather()
                weather_due = time.monotonic() + WEATHER_MINS * 60
                if wx and (wx, icon) != last_weather:
                    ok, reason = send_ticker(wx, param="WX", icon=icon)
                    if ok:
                        last_weather = (wx, icon)
                        log.info("Weather: %s (icon %s)", wx, icon)
                    else:
                        log.warning("Weather push failed: %s", reason)
        except Exception as e:
            log.warning("Weather refresh error: %s", e)

        # Calendar, paused entirely while a rate-limit backoff is in effect.
        if time.monotonic() < calendar_due:
            time.sleep(POLL_SECS)
            continue

        try:
            if calendars is None:
                calendars = connect()

            now = datetime.datetime.now(datetime.timezone.utc)
            events, failures = collect_events(calendars, now)
            now_ev, next_ev = now_and_next(events, now)

            # If some calendar didn't answer we have an incomplete picture.
            # Saying "I am Free" then risks blanking a real meeting, so hold
            # the last known status until every calendar reports again.
            if failures and now_ev is None:
                if not stale:
                    log.warning("%d of %d calendars did not respond - holding "
                                "last status instead of showing '%s'",
                                failures, len(calendars), FREE_MSG)
                stale = True
                time.sleep(POLL_SECS)
                continue
            if stale and not failures:
                log.info("All calendars responding again")
            stale = False

            message = compose(now_ev, next_ev, now)

            # Only push when the text changes. Because last_message is updated
            # ONLY on a successful send, a pending update keeps retrying every
            # poll until the device is reachable again — then it goes through.
            if message != last_message:
                ok, reason = send_ticker(message)
                if ok:
                    last_message = message
                    if not online:
                        log.info("Ticker reachable again - updates resumed")
                    online, last_reason = True, None
                    log.info("Sent: %r", message)
                else:
                    # Log once when it goes offline (or the reason changes),
                    # then stay quiet while it keeps retrying in the background.
                    if online or reason != last_reason:
                        log.warning("Ticker update failed: %s (retrying every %ds)",
                                    reason, POLL_SECS)
                    online, last_reason = False, reason

        except caldav.lib.error.AuthorizationError:
            log.critical("iCloud auth failed - check APPLE_ID and APPLE_APP_PASSWORD")
            break   # wrong credentials won't fix themselves, stop retrying

        except Exception as e:
            if RATE_LIMIT_ERROR and isinstance(e, RATE_LIMIT_ERROR):
                # Apple throttles frequent CalDAV logins. Retrying on the normal
                # cadence would only extend the block, so wait it out. Weather
                # keeps updating in the meantime.
                wait = retry_after_seconds(e)
                log.warning("iCloud rate limited - pausing calendar for %d min "
                            "(weather keeps updating)", wait // 60)
                calendars    = None
                calendar_due = time.monotonic() + wait
            else:
                log.exception("Calendar error: %s", e)
                calendars = None   # force a fresh CalDAV connection next loop

        time.sleep(POLL_SECS)


if __name__ == "__main__":
    main()

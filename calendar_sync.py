#!/usr/bin/env python3
"""
Apple Calendar -> LED Ticker Sync  (Matrix Portal S3 / RGB version)
Polls iCloud CalDAV every 60 seconds and updates the LED display.

Unchanged from the MAX7219 version: it only uses the /&MSG= web API,
which the RGB firmware keeps 100% compatible. Point ESP32_IP at the
Matrix Portal S3's IP (shown on the Serial Monitor at startup).

Setup:
  1. pip install caldav requests
  2. Generate an app-specific password at appleid.apple.com
     -> Sign-In & Security -> App-Specific Passwords -> "LED Ticker"
  3. Fill in the configuration block below
  4. Run: python calendar_sync.py
"""

import time
import datetime
import random
import logging
import urllib.parse

import requests
import caldav

# ── Configuration ─────────────────────────────────────────────────────────────

ESP32_IP    = "192.168.1.XXX"      # ESP32 IP — check Serial Monitor on startup
ESP32_USER  = "joe"                    # ESP32 web UI username
ESP32_PASS  = "YOUR_ESP32_PASSWORD"    # ESP32 web UI password (from arduino_secrets.h)

APPLE_ID    = "you@icloud.com"     # Your Apple ID / iCloud email
APP_PASS    = "xxxx-xxxx-xxxx-xxxx"  # App-specific password from appleid.apple.com

FREE_MSG    = "       I am Free"   # Message shown when no meeting is active
POLL_SECS   = 60                   # How often to check (seconds)

# ─────────────────────────────────────────────────────────────────────────────

CALDAV_URL = "https://caldav.icloud.com"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)


def get_active_meeting() -> str | None:
    """
    Connect to iCloud CalDAV and return the summary of any event
    currently in progress, or None if the calendar is clear.
    """
    client = caldav.DAVClient(
        url=CALDAV_URL,
        username=APPLE_ID,
        password=APP_PASS,
    )

    principal = client.principal()
    calendars = principal.calendars()

    now          = datetime.datetime.now(datetime.timezone.utc)
    window_start = now - datetime.timedelta(hours=8)   # catch long meetings
    window_end   = now + datetime.timedelta(minutes=1)

    for calendar in calendars:
        try:
            events = calendar.date_search(
                start=window_start,
                end=window_end,
                expand=True,
            )
        except Exception:
            # Reminders and other non-event calendars will raise here — skip them
            continue

        for event in events:
            try:
                ve = event.vobject_instance.vevent

                dtstart = ve.dtstart.value
                dtend   = ve.dtend.value

                # Skip all-day events — they are date objects, not datetimes
                if isinstance(dtstart, datetime.date) and not isinstance(dtstart, datetime.datetime):
                    continue

                # Normalize to UTC-aware datetimes
                if dtstart.tzinfo is None:
                    dtstart = dtstart.replace(tzinfo=datetime.timezone.utc)
                if dtend.tzinfo is None:
                    dtend = dtend.replace(tzinfo=datetime.timezone.utc)

                if dtstart <= now <= dtend:
                    return ve.summary.value

            except AttributeError:
                continue   # event is missing expected fields

    return None


def send_to_ticker(message: str) -> None:
    """Send a URL-encoded message to the ESP32 via its web interface."""
    encoded = urllib.parse.quote(message)
    nc      = random.randint(1, 99999)   # cache-buster, same as the web UI
    url     = f"http://{ESP32_IP}/&MSG={encoded}/&nc={nc}"
    resp    = requests.get(url, auth=(ESP32_USER, ESP32_PASS), timeout=5)
    resp.raise_for_status()
    log.info("Sent: %r", message)


def main() -> None:
    log.info("Calendar sync started — polling every %ds", POLL_SECS)
    last_message = None

    while True:
        try:
            meeting = get_active_meeting()
            message = f"  In a Meeting: {meeting}" if meeting else FREE_MSG

            # Only push an update when the message actually changes,
            # so the display isn't reset every 60 seconds unnecessarily.
            if message != last_message:
                send_to_ticker(message)
                last_message = message

        except requests.exceptions.ConnectionError:
            log.error("Cannot reach ESP32 at %s — is it on the network?", ESP32_IP)

        except requests.exceptions.HTTPError as e:
            log.error("ESP32 returned an error: %s", e)

        except caldav.lib.error.AuthorizationError:
            log.critical("iCloud auth failed — check APPLE_ID and APP_PASS")
            break   # wrong credentials won't fix themselves, stop retrying

        except Exception as e:
            log.exception("Unexpected error: %s", e)

        time.sleep(POLL_SECS)


if __name__ == "__main__":
    main()

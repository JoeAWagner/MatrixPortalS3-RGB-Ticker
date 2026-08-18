# RGB Matrix Ticker — Matrix Portal S3

A Wi-Fi controlled scrolling **RGB LED ticker** for the
[Adafruit MatrixPortal S3](https://www.adafruit.com/product/5778) driving a
single **64×32 HUB75 panel** (P4, 256×128 mm, 1/16 scan). Send messages, pick
colors, and control scroll from a phone/browser over your local network — plus
an optional Python companion that mirrors your Apple Calendar to the display.

This is the RGB successor to an earlier MAX7219/`MD_Parola` monochrome version;
the display layer was rewritten on **Adafruit Protomatter + Adafruit GFX**, while
the web server, Basic Auth, and HTTP API were kept compatible.

---

## Hardware

| Part | Notes |
|------|-------|
| Adafruit MatrixPortal S3 | ESP32-S3 with a HUB75 socket on the back |
| 64×32 HUB75 RGB panel | P4 pitch (256×128 mm), 1/16 scan, SMD2121 |
| 5 V power supply | Size for the panel — a 64×32 can pull ~2–4 A at full white |

Plug the MatrixPortal S3 into the panel's HUB75 input, feed 5 V into the panel's
power terminals (and the board's screw terminals per Adafruit's guide), and
connect USB-C for flashing.

## Firmware setup (Arduino IDE)

1. **Board support** — install the *esp32* boards package, then select
   **Adafruit MatrixPortal ESP32-S3**.
2. **Libraries** (Library Manager):
   - Adafruit Protomatter
   - Adafruit GFX Library
   - Adafruit BusIO
3. **Credentials** — copy the template and fill in your own values:
   ```bash
   cp arduino_secrets.h.example arduino_secrets.h
   ```
   Edit `arduino_secrets.h` with your Wi-Fi SSID/password and the web-UI
   login accounts. This file is gitignored and never committed.
4. Open `MatrixPortalS3_Scrolling_Web_JW_10.ino`, compile, and upload.
5. Open the Serial Monitor at **57600 baud** — it prints the device IP once
   Wi-Fi connects. Browse to that IP and log in with one of your accounts.

## Web UI

- **Custom message** — type and send any text (scrolls continuously)
- **Presets** — one-tap status messages (In a Meeting, On Break, …)
- **Display controls** — scroll speed, text size (S/M/L/XL), brightness
  (0–100 %), text color picker, **solid / rainbow** color mode, and scroll
  direction
- **Firmware** — shows the installed version and a **Check for Updates**
  button that asks GitHub whether a newer version exists
- **Blank** — clears the panel

## HTTP API

All control is plain `GET` requests with Basic Auth, so it's easy to script.

| Parameter | Meaning | Example |
|-----------|---------|---------|
| `/&MSG=` | Set scrolling text (URL-encoded; `BLANK` clears) | `/&MSG=Hello%20World/&` |
| `/&SP=`  | Scroll step interval, ms (5–200; lower = faster) | `/&SP=25` |
| `/&TS=`  | Text size 1–4 (S/M/L/XL; 4 = full 32 px height) | `/&TS=3` |
| `/&BR=`  | Brightness percent (0–100) | `/&BR=60` |
| `/&CO=`  | Solid text color, `RRGGBB` hex | `/&CO=f0a500` |
| `/&CM=`  | Color mode: `S` solid, `R` rainbow | `/&CM=R` |
| `/&SD=`  | Scroll direction: `L` left, `R` right | `/&SD=L` |
| `/&CHK=` | Check GitHub for a newer version; returns JSON | `/&CHK=1` |

Example:
```bash
curl -u joe:yourpass "http://<device-ip>/&MSG=On%20Air/&CO=ff0000/&CM=S/&"
```

## Calendar sync (optional)

`calendar_sync.py` polls iCloud CalDAV and shows both what's on **now** and what's
**up next**, e.g. `NOW: Standup    |    UP NEXT: 2:00 PM Design Review` (or
`I am Free    |    UP NEXT: in 25 min 1:1 with Sam`). It uses only the `/&MSG=`
endpoint, so nothing on the device changes.

```bash
pip install caldav requests
```

Configure via environment variables (keeps credentials out of the file):

```bash
export TICKER_IP=192.168.1.42          # from the Serial Monitor
export TICKER_USER=joe
export TICKER_PASS=your_web_ui_password
export APPLE_ID=you@icloud.com
export APPLE_APP_PASSWORD=xxxx-xxxx-xxxx-xxxx   # appleid.apple.com
python calendar_sync.py
```

On Windows PowerShell use `$env:TICKER_IP="192.168.1.42"` etc. You can also just
edit the fallback values at the top of the script. Generate the Apple
app-specific password at [appleid.apple.com](https://appleid.apple.com) →
Sign-In & Security → App-Specific Passwords.

If the ticker is unplugged or off the network, the sync keeps running: it retries
each update a few times, logs the outage **once**, stays quiet while it's down,
and automatically resends the current status (and logs "reachable again") as soon
as the device comes back.

### Run it automatically at logon (Windows)

So the sync isn't a terminal you have to keep open, register it as a Scheduled
Task that starts at logon and restarts itself if it stops:

1. `Copy-Item sync.config.example.ps1 sync.config.ps1` and fill in your values
   (this file is gitignored). At minimum set `APPLE_APP_PASSWORD`.
2. Register the task (no admin needed):
   ```powershell
   powershell -ExecutionPolicy Bypass -File install-task.ps1
   ```
3. Start it now without waiting for a logon:
   ```powershell
   Start-ScheduledTask -TaskName MatrixPortalCalendarSync
   ```

It logs to `calendar_sync.log` (rotating, next to the scripts). Remove the task
with `install-task.ps1 -Uninstall`. You can also just run `run_sync.ps1` directly
in a terminal if you prefer to watch it live.

### Is the Python bridge the right approach?

For **iCloud specifically, yes** — it's the pragmatic choice. iCloud has no clean
public calendar API, so CalDAV is the only real option, and doing CalDAV + TLS +
recurring-event/timezone parsing on the ESP32 itself is painful and fragile. The
bridge also keeps your Apple app-password on a trusted machine instead of flashed
into firmware. The main thing to improve is **reliability**: run it as a
background service (Windows Task Scheduler / NSSM, or `launchd`/`systemd`) so it
survives reboots instead of living in a terminal window.

The one alternative worth knowing: if eliminating the always-on computer matters
more than privacy, you can publish the calendar as a **secret `.ics` URL** (iCloud
public calendar, or a Google "secret address") and have the ESP32 fetch and parse
it directly — no PC required. The trade-offs are that the URL exposes event
details to anyone who has it, and on-device `.ics` parsing handles recurring
events and time zones poorly.

## Panel notes

The firmware is configured for a genuine 64×32 / 1/16-scan panel (4 address
lines, `NUM_ADDR 4`). If your panel shows the image split or doubled, it likely
uses a different scan rate — adjust `NUM_ADDR` / `PANEL_HEIGHT` and the address
pins near the top of the `.ino`. Pin mapping there matches Adafruit's standard
MatrixPortal S3 wiring.

## Update checking & releasing

The **Check for Updates** button makes the device fetch
[`version.txt`](version.txt) from the `main` branch over HTTPS and compare it to
the `FW_VERSION` compiled into the running firmware. It's a *notifier* — it tells
you an update exists and links to the repo; it does not flash the device itself.

To publish a new version:

1. Bump **`FW_VERSION`** near the top of the `.ino`.
2. Set the same value in **`version.txt`**.
3. Commit and push to `main`.

Devices still running an older build will then report "Update available" and you
can reflash them over USB. (Versions are compared numerically, e.g. `1.10.0` is
newer than `1.9.0`.)

## License

MIT — see `LICENSE`.

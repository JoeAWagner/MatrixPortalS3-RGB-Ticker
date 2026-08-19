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
   The device also advertises itself over mDNS, so **http://ticker.local/**
   works even if its DHCP lease changes.

## Web UI

- **Multi-line layout** — the 64×32 panel shows up to four rows at the default
  small font. Messages split on `|` into separate rows, and a row only scrolls
  if it's too wide to fit; short rows sit centered and still.
- **Sticky labels** — a leading label like `NOW:` or `NEXT:` stays pinned at the
  left edge while only the text after it scrolls past, so you can always tell
  which row you're reading. Toggle it off to scroll the whole row instead.
- **Tunable motion** — line gap (0–6 px), park time (how long a row holds still
  before it scrolls again), speed, and direction are all live sliders.
- **Weather row** — a line pinned to the top of the display with a small icon
  after the text (sun, cloud, partly, rain, snow, storm, fog), set from the web
  UI or pushed automatically by the calendar sync
- **Color themes** — Sunset (default), Amber, Matrix, Siren, Neon, and Rainbow.
  Each theme colors rows *by role* using a distinct hue per row, so the weather,
  NOW, and NEXT rows are readable at a glance. Icons keep their own natural
  colors in every theme.

  | Theme | Weather | NOW | NEXT |
  |---|---|---|---|
  | Sunset | gold | orange | pink |
  | Amber | yellow | orange | white |
  | Matrix | lime | green | aqua |
  | Siren | white | red | blue |
  | Neon | yellow | magenta | cyan |
- **Custom message** — type and send any text
- **Presets** — one-tap status messages (In a Meeting, On Break, …)
- **Display controls** — scroll speed, text size (S/M/L/XL), brightness,
  line gap, park time, clock mode, night dimming, and scroll direction
- **Firmware** — shows the installed version and a **Check for Updates**
  button that asks GitHub whether a newer version exists
- **Settings persist** — every display setting is saved to the ESP32's NVS
  flash and restored on boot, so a power cut doesn't reset your tuning
- **Clock fallback** — if no update arrives for 20 minutes (PC asleep, sync
  stopped) the panel shows the time and date instead of a frozen status
- **Night dimming** — drops to a low brightness overnight on a schedule
- **Blank** — clears the panel

## HTTP API

All control is plain `GET` requests with Basic Auth, so it's easy to script.

| Parameter | Meaning | Example |
|-----------|---------|---------|
| `/&MSG=` | Set scrolling text (URL-encoded; `BLANK` clears) | `/&MSG=Hello%20World/&` |
| `/&SP=`  | Scroll step interval, ms (5–200; lower = faster) | `/&SP=25` |
| `/&TS=`  | Text size 1–4 (S/M/L/XL; 4 = full 32 px height) | `/&TS=3` |
| `/&LG=`  | Line gap 0–6 (blank pixels between rows) | `/&LG=2` |
| `/&PK=`  | Park time 0–10000 ms a row holds still before scrolling | `/&PK=1500` |
| `/&SK=`  | Sticky labels: `1` pins `NOW:`/`NEXT:`, `0` scrolls the whole row | `/&SK=1` |
| `/&CL=`  | Clock: `0` off, `1` always a row, `2` only when idle | `/&CL=2` |
| `/&ND=`  | Night dimming on/off | `/&ND=1` |
| `/&NB=`  | Night brightness percent | `/&NB=8` |
| `/&NS=` `/&NE=` | Night window start / end hour (24h) | `/&NS=22` `/&NE=7` |
| `/&BR=`  | Brightness percent (0–100) | `/&BR=60` |
| `/&CO=`  | Flat color override, `RRGGBB` hex (overrides the theme) | `/&CO=f0a500` |
| `/&CM=`  | Color mode: `S` solid, `R` rainbow | `/&CM=R` |
| `/&SD=`  | Scroll direction: `L` left, `R` right | `/&SD=L` |
| `/&WX=`  | Weather text pinned to the top row (empty clears) | `/&WX=82F%20Clear/&` |
| `/&WI=`  | Weather icon 0-6 (`-1` none): sun, cloud, partly, rain, snow, storm, fog | `/&WI=0` |
| `/&TH=`  | Color theme 0–5 (sunset, amber, matrix, siren, neon, rainbow) | `/&TH=0` |
| `/&CHK=` | Check GitHub for a newer version; returns JSON | `/&CHK=1` |

Messages are split on `|` into stacked rows, so
`/&MSG=NOW:%20Lunch|NEXT:%201:00%20PM%20Review/&` renders as two lines.

Example:
```bash
curl -u joe:yourpass "http://<device-ip>/&MSG=On%20Air/&CO=ff0000/&CM=S/&"
```

## Calendar sync (optional)

`calendar_sync.py` polls iCloud CalDAV and shows what's on **now** and what's
**next** on separate rows, with current **weather** on the top row:

```
82F Clear
NOW: Lunch
NEXT: 1:00 PM Review
```

It sends the calendar rows via `/&MSG=` (separated by `|`) and the weather via
`/&WX=`. Weather comes from [Open-Meteo](https://open-meteo.com) (free, no API
key) for `WEATHER_ZIP` — set that env var to change location, and
`WEATHER_UNITS` in the script to switch to Celsius.

**Only the calendars you care about.** By default every calendar on the account
is polled, which on a busy iCloud account can mean a lot of CalDAV requests each
minute (and Apple rate-limits). The startup log lists the calendar names it
found; set `CALENDARS` to just the ones that should drive your status:

```powershell
$env:CALENDARS = "Calendar,Work,Home"
```

**"I am Free" is only shown when every calendar answers.** A calendar that fails
to respond is not treated as an empty one — otherwise a transient iCloud error
could blank out a meeting that is actually in progress. When a calendar doesn't
answer, the sync holds the last known status and logs it, then resumes once all
calendars report again.

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

So the sync isn't a terminal you have to keep open:

1. `Copy-Item sync.config.example.ps1 sync.config.ps1` and fill in your values
   (this file is gitignored). At minimum set `APPLE_APP_PASSWORD`.
2. Install the logon launcher (**no admin needed** — adds a shortcut to your
   Startup folder):
   ```powershell
   powershell -ExecutionPolicy Bypass -File install-startup.ps1
   ```
3. Start it now without waiting for a logon:
   ```powershell
   powershell -ExecutionPolicy Bypass -File run_sync.ps1
   ```

It runs hidden, logs to `calendar_sync.log` (rotating, next to the scripts), and
only ever runs one copy at a time. Remove it with `install-startup.ps1 -Uninstall`.

**Prefer a real Scheduled Task?** `install-task.ps1` registers one (runs at logon,
auto-restarts on failure) — but on many machines creating a task needs an
**elevated** PowerShell ("Run as administrator"). If it can't, it now tells you so
and points you back to `install-startup.ps1`.

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

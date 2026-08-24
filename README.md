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
- **Presence sensing** — with an LD2450 mmWave radar attached, the panel powers
  down when nobody is nearby and wakes when you walk up (see below)
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
| `/&PR=`  | Presence sensing on/off (LD2450) | `/&PR=1` |
| `/&PD=`  | Wake range in mm (`0` = any distance) | `/&PD=3000` |
| `/&PH=`  | Keep the panel lit this many seconds after the last detection | `/&PH=60` |
| `/&WP=`  | Wi-Fi modem sleep (saves power, adds a little latency) | `/&WP=1` |
| `/&BR=`  | Brightness percent (0–100) | `/&BR=60` |
| `/&CO=`  | Flat color override, `RRGGBB` hex (overrides the theme) | `/&CO=f0a500` |
| `/&CM=`  | Color mode: `S` solid, `R` rainbow | `/&CM=R` |
| `/&SD=`  | Scroll direction: `L` left, `R` right | `/&SD=L` |
| `/&WX=`  | Weather text pinned to the top row (empty clears) | `/&WX=82F%20Clear/&` |
| `/&WI=`  | Weather icon 0-6 (`-1` none): sun, cloud, partly, rain, snow, storm, fog | `/&WI=0` |
| `/&TH=`  | Color theme 0–5 (sunset, amber, matrix, siren, neon, rainbow) | `/&TH=0` |
| `/&CHK=` | Check GitHub for a newer version; returns JSON | `/&CHK=1` |
| `/&TM=`  | Report the device clock as JSON (epoch, local, source, tz) | `/&TM=1` |
| `/&ST=`  | Set the device clock from a Unix epoch (UTC seconds) | `/&ST=1787582340` |

Messages are split on `|` into stacked rows, so
`/&MSG=NOW:%20Lunch|NEXT:%201:00%20PM%20Review/&` renders as two lines.

Example:
```bash
curl -u joe:yourpass "http://<device-ip>/&MSG=On%20Air/&CO=ff0000/&CM=S/&"
```

## Battery use & presence sensing (optional)

For a battery build the panel — not the ESP32 — is what drains the pack. An
**LD2450 24 GHz mmWave radar** lets the display sleep whenever nobody is there.

### Wiring

| LD2450 | MatrixPortal S3 | Note |
|---|---|---|
| VCC | 5 V | ~80–100 mA continuous |
| GND | GND | |
| TX | **RX** (GPIO 8) | sensor → board, this is the one that matters |
| RX | **TX** (GPIO 18) | only needed to reconfigure the sensor |

The sensor talks UART at **256000 baud** (an unusual rate — it is not a typo).
The web UI's PRESENCE card shows a live frame counter, so if the sensor is
miswired or on the wrong baud it says so explicitly instead of failing quietly.

### How the sleep works

On no-presence the firmware blanks the framebuffer, which removes all LED
current — the dominant term in panel draw. The refresh engine keeps running, so
the panel can sleep and wake indefinitely.

> **Why not `matrix.stop()`?** It saves more (OE high, timer halted), but the
> only way back is `matrix.resume()`, and on ESP32-S3 that re-runs
> `_PM_timerInit()`, which calls `gdma_new_channel()` **every time** — the
> "already allocated" guard in Adafruit_Protomatter is compiled only for
> CircuitPython. Each wake therefore leaks a DMA channel; the timer ISR keeps
> counting frames while the panel stays permanently dark. Verified the hard
> way. Blanking has no such limit.

For a *deep* saving on battery, fit a logic-level high-side MOSFET on the
panel's 5 V rail and set `PANEL_PWR_PIN` to the GPIO that drives it. The
firmware then cuts panel power entirely when asleep and restores it on wake,
with the ESP32 (and Wi-Fi) staying up throughout.

Tune **WAKE RANGE** (how close you must be) and **STAY LIT** (how long it stays
awake after you leave) in the web UI.

### Power budget

A 3000 mAh 1S LiPo holds ~11 Wh; after boosting to 5 V at ~88 % you get roughly
**1900 mAh at 5 V** to spend. Rough figures for a 64×32 P4 panel — measure your
own, they vary a lot with content and brightness:

| State | Approx draw |
|---|---|
| Panel showing text @ 30 % brightness | 150–400 mA |
| Panel black but still refreshing | 80–150 mA |
| **Panel `stop()`ed** | ~5–20 mA |
| ESP32-S3 + Wi-Fi active | 90–130 mA |
| ESP32-S3 + Wi-Fi modem sleep | 30–50 mA |
| LD2450 radar | 80–100 mA |

| Setup | Estimated runtime |
|---|---|
| Always lit, no sensing | **~5 h** |
| Presence blanking, lit ~20 % of the time | **~6–7 h** |
| Presence blanking + Wi-Fi saver | **~8 h** |
| Presence + `PANEL_PWR_PIN` MOSFET + Wi-Fi saver | **~13–15 h** |

Blanking removes LED current but the panel keeps scanning (80–150 mA), which is
why the MOSFET row is so much better — it takes panel draw to zero.

Note the radar itself costs about as much as the ESP32, which caps the benefit.
For multi-day life you want a bigger pack (a 10000 mAh USB power bank gets you
into the 24–30 h range) rather than a more aggressive sleep.

### Powering it

The MatrixPortal S3 has **no onboard LiPo charger or battery connector**, and
the panel needs 5 V at up to a couple of amps on bright content. A bare 1S LiPo
therefore needs a **boost converter rated ≥ 2 A** (plus a charger), or you can
skip the loose-cell approach entirely and run it from a USB-C power bank.

## Ticker Manager (desktop app)

`ticker-app/` is an Electron desktop app that replaces the Python + PowerShell
sync. It runs in the system tray, shows what the panel is displaying, and
handles the calendar and weather sync itself.

```bash
cd ticker-app
npm install
npm start          # run from source
npm run dist       # build a Windows installer into dist/
```

The **Status** tab also shows the ticker's own clock alongside its drift from
this machine. The panel gets its time over NTP, so it is normally within a
second or two; a **Sync time** button appears only once drift exceeds 30 s (or
the clock was never set), and pushes this machine's time via `/&ST=`. That
matters for the idle clock fallback and the night-dimming schedule, both of
which depend on the device knowing what time it is.

On first run it imports credentials from an existing `sync.config.ps1`, so
upgrading from the script version needs no retyping. Settings live in
`%APPDATA%/ticker-app/settings.json` and activity is logged to
`%APPDATA%/ticker-app/ticker.log` (rotated at 1 MB) as well as the Log tab -
a tray app with no log on disk is a black box the moment it stops working.

Install the built `dist/Ticker Manager Setup *.exe` rather than running from
source: a packaged install lives outside the synced folder, so the login item
Electron registers cannot point at a path that has not mounted yet.

**Why replace the scripts?** Every outage this project hit came from the
plumbing rather than the logic:

| Failure | Cause | Gone because |
|---|---|---|
| Sync silently did nothing | `python` on PATH resolved to the Microsoft Store stub | No Python at all |
| "I am Free" during meetings | caldav 2.0 dropped `vobject`, so every event failed to parse | Node CalDAV client, and parse failures count as failures |
| Nothing started after reboot | Logon shortcut pointed into iCloud Drive, unmounted at logon | Electron registers its own login item; install it outside a synced folder |
| Calendar froze while weather kept working | A stale CalDAV session was never rebuilt | A total-failure poll invalidates the session so the next poll reconnects |

The app keeps the behaviours that were learned the hard way: weather refreshes
independently of the calendar, "I am Free" is only shown when every calendar
answers, the message is re-pushed on a heartbeat so a rebooted panel recovers,
and an unreachable panel is logged once rather than every poll.

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
py -3 -m pip install caldav requests
```

Use `py -3 -m pip`, not a bare `pip`. On Windows, if a real Python install is
removed or drops off `PATH`, the **Microsoft Store alias** at
`...\AppData\Local\Microsoft\WindowsApps\python.exe` silently takes its
place — it is a stub that opens the Store and exits, so anything launched
through it appears to start and then does nothing. `run_sync.ps1` explicitly
skips that path and proves each candidate interpreter by importing `caldav`
and `requests` before using it, logging which one it picked.

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

**If the project lives in a synced folder** (iCloud Drive, OneDrive, Dropbox),
do not point the Startup shortcut at `run_sync.ps1` directly. At logon the sync
service may not have mounted yet, so the shortcut's target does not exist and it
fails silently. Use `bootstrap.example.ps1` instead:

1. Copy `bootstrap.example.ps1` and `sync.config.ps1` to a **local** folder,
   e.g. `C:\dev	icker`, and edit `$project` to point at the project folder.
2. Point the Startup shortcut at that copy.

The bootstrap always exists at logon, waits (up to 20 minutes) for the project
folder to appear, logs what it is waiting for, and only then hands off to
`run_sync.ps1`. Keeping `sync.config.ps1` there too means your Apple
app-specific password is never uploaded to a cloud service.

**Prefer a real Scheduled Task?** `install-task.ps1` registers one (runs at logon,
auto-restarts on failure) — but on many machines creating a task needs an
**elevated** PowerShell ("Run as administrator"). If it can't, it now tells you so
and points you back to `install-startup.ps1`.

### Troubleshooting

**Nothing on the panel and nothing in `calendar_sync.log`** — the launcher never
got a working interpreter. The log now records which Python it chose, or an
explicit error if none had the dependencies.

**Everything shows "I am Free" despite real meetings** — this is what happens
when events cannot be parsed at all. caldav 2.0 dropped `vobject` as a
dependency; the sync now uses the `icalendar` API that ships with modern caldav
and falls back to `vobject_instance` only on older installs. Unparseable events
also count as failures, so an incomplete read holds the last known status
instead of claiming you are free.

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

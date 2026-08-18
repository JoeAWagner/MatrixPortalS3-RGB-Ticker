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

Example:
```bash
curl -u joe:yourpass "http://<device-ip>/&MSG=On%20Air/&CO=ff0000/&CM=S/&"
```

## Calendar sync (optional)

`calendar_sync.py` polls iCloud CalDAV every 60 s and pushes "In a Meeting: …"
to the ticker when an event is active, or a free message otherwise. It uses only
the `/&MSG=` endpoint.

```bash
pip install caldav requests
```
Then edit the config block at the top of `calendar_sync.py` (device IP, web-UI
login, Apple ID + an [app-specific password](https://appleid.apple.com)) and run:
```bash
python calendar_sync.py
```

## Panel notes

The firmware is configured for a genuine 64×32 / 1/16-scan panel (4 address
lines, `NUM_ADDR 4`). If your panel shows the image split or doubled, it likely
uses a different scan rate — adjust `NUM_ADDR` / `PANEL_HEIGHT` and the address
pins near the top of the `.ino`. Pin mapping there matches Adafruit's standard
MatrixPortal S3 wiring.

## License

MIT — see `LICENSE`.

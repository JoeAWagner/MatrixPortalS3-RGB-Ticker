
// RGB LED Matrix Ticker Tape - HTTP Web Controller
// Hardware: Adafruit MatrixPortal S3 (ESP32-S3) driving a single
//           64x32 HUB75 RGB panel (P4, 256x128mm, 1/16 scan).
//
// Display stack: Adafruit Protomatter + Adafruit GFX.
// Web API is unchanged from the MAX7219 version, so calendar_sync.py
// (which only sends /&MSG=) keeps working as-is.
//
// Libraries required (Arduino Library Manager):
//   - Adafruit Protomatter
//   - Adafruit GFX Library
//   - Adafruit BusIO   (dependency of GFX)
// Board: "Adafruit MatrixPortal ESP32-S3"  (esp32 core >= 2.0.x)

#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <math.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_Protomatter.h>

// Credentials live in a separate, gitignored file. Copy
// arduino_secrets.h.example to arduino_secrets.h and fill it in.
#include "arduino_secrets.h"

// ============================================================
//  VERSION / UPDATE CHECK
// ============================================================
// FW_VERSION is the version built into this firmware.
// version.txt in the repo holds the latest published version; the
// "Check for Updates" button compares the two.
#define FW_VERSION   "1.9.0"
#define VERSION_URL  "https://raw.githubusercontent.com/JoeAWagner/MatrixPortalS3-RGB-Ticker/main/version.txt"
#define REPO_URL     "https://github.com/JoeAWagner/MatrixPortalS3-RGB-Ticker"

// ============================================================
//  NETWORK NAME / TIME
// ============================================================
// Reachable as http://ticker.local/ so a changed DHCP lease doesn't matter.
#define HOSTNAME  "ticker"

// POSIX timezone for the clock. This is US Eastern with automatic DST;
// see https://github.com/nayarsystems/posix_tz_db for other zones.
#define TZ_INFO   "EST5EDT,M3.2.0,M11.1.0"
#define NTP_1     "pool.ntp.org"
#define NTP_2     "time.nist.gov"

// ============================================================
//  DEBUG
// ============================================================
#define DEBUG 1
#if DEBUG
#define PRINT(s, x)  { Serial.print(F(s)); Serial.print(x); }
#define PRINTS(x)    Serial.print(F(x))
#else
#define PRINT(s, x)
#define PRINTS(x)
#endif

// ============================================================
//  MATRIX HARDWARE  (Adafruit MatrixPortal S3 pin mapping)
// ============================================================
#define PANEL_WIDTH   64      // pixels wide  (P4 256mm / 4mm = 64)
#define PANEL_HEIGHT  32      // pixels tall  (P4 128mm / 4mm = 32)
#define BIT_DEPTH     6       // color depth per channel (1-6); 6 = best color
#define NUM_ADDR      4       // 4 address lines => 1/16 scan => 32px tall

// MatrixPortal S3 HUB75 pins (from Adafruit_Protomatter examples).
uint8_t rgbPins[]  = { 42, 41, 40, 38, 39, 37 };
uint8_t addrPins[] = { 45, 36, 48, 35, 21 };  // A, B, C, D, (E unused at 1/16)
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;

Adafruit_Protomatter matrix(
  PANEL_WIDTH,        // width in pixels
  BIT_DEPTH,          // bit (color) depth
  1, rgbPins,         // # of matrix chains, RGB pin list
  NUM_ADDR, addrPins, // # of address pins, address pin list
  clockPin, latchPin, oePin,
  true);              // double-buffered for tear-free scrolling

// ============================================================
//  TYPES
// ============================================================
// These must appear before the first function definition in the sketch:
// the Arduino build step auto-generates prototypes and inserts them above
// that point, so any type used in a signature has to be declared first.
#define BUF_SIZE 512

enum ScrollDir  { DIR_LEFT, DIR_RIGHT };
enum ColorMode  { CM_SOLID, CM_RAINBOW };

// Each theme colors the rows by role rather than one flat color:
//   wx = weather row, c1 = first message row (NOW), c2 = later rows (NEXT).
struct Theme {
  const char *name;
  uint32_t    wx, c1, c2;
  bool        rainbow;
};

// One display row. Short lines sit still; only overflowing lines scroll.
//
// A row may have a "sticky" label (e.g. "NOW:") that stays pinned at the left
// while only the text after it scrolls. bodyOff splits text into the pinned
// prefix [0, bodyOff) and the scrolling body [bodyOff, end).
struct Line {
  char     text[BUF_SIZE];
  uint16_t bodyOff;     // where the scrolling part starts (0 = whole row scrolls)
  int16_t  labelW;      // pixels reserved for the pinned label (0 = none)
  int16_t  bodyW;       // width of the scrolling part
  int16_t  pixW;        // rendered width in pixels (text + icon)
  int16_t  textW;       // text-only width, so the icon lands after it
  int8_t   icon;        // -1 = none
  uint8_t  role;        // 0 = weather, 1 = first msg row, 2 = later rows
  bool     scrolls;     // true when the body is wider than its window
  int32_t  x;           // current x offset (scrolling lines only)
  uint32_t holdUntil;   // pause before a scroll cycle restarts
};

// ============================================================
//  TEXT / SCROLL GEOMETRY
// ============================================================
// GFX classic font is a 6x8 cell (5x7 glyph). textSize scales it:
//   1 = 6x8   2 = 12x16   3 = 18x24   4 = 24x32 (fills panel height)
#define TEXT_SIZE_MIN 1
#define TEXT_SIZE_MAX 4

uint8_t textSize = 1;   // small font default; runtime-adjustable via /&TS=

static inline int16_t charW(void)  { return 6 * textSize; }              // cell width
static inline int16_t glyphH(void) { return 8 * textSize; }             // cell height

// Panel is 64x32, so at size 1 we get four 8px rows. The layout stacks:
//   row 0: weather (if set)
//   rows below: the message, split on '|' into separate lines
#define MAX_LINES     4
#define LINE_GAP_MAX  6
#define SCROLL_PAD   12            // blank pixels between scroll repeats
#define PARK_MS_MAX  10000         // longest park time offered

uint8_t  lineGap  = 4;             // blank pixels between rows (/&LG=)
uint16_t parkMs   = 2500;          // how long a row sits still before scrolling (/&PK=)
bool     stickyLabels = true;      // pin "NOW:" / "NEXT:" labels (/&SK=)

// Clock: 0 = off, 1 = always show a clock row, 2 = only when the message
// has gone stale (the sync stopped, so the PC is probably off).
uint8_t  clockMode  = 2;           // (/&CL=)
#define  STALE_MINS 20             // no message for this long => "stale"

// Night dimming - an RGB panel at daytime brightness is blinding at 2am.
bool     nightDim      = true;     // (/&ND=)
uint8_t  nightBrightPct = 8;       // (/&NB=)
uint8_t  nightStartHr  = 22;       // (/&NS=)
uint8_t  nightEndHr    = 7;        // (/&NE=)

uint32_t lastMsgMs = 0;            // when the last /&MSG= arrived

// ---- LD2450 presence sensing (battery mode) -----------------------------
// A 24GHz mmWave radar on Serial1. When nobody is in front of the panel the
// display is fully powered down via matrix.stop(), which drops OE and halts
// the refresh timer - far bigger a saving than merely drawing a black frame.
#define LD2450_BAUD   256000       // note: not a typical baud rate
#define LD2450_RX_PIN 8            // board silk "RX"  <- LD2450 TX
#define LD2450_TX_PIN 18           // board silk "TX"  -> LD2450 RX (config only)
#define LD2450_FRAME  30           // AA FF 03 00 + 3 targets x 8 + 55 CC

bool     presenceEnabled = false;  // (/&PR=)
uint16_t presenceRangeMm = 3000;   // wake within this range; 0 = any (/&PD=)
uint16_t presenceHoldSec = 60;     // keep lit this long after the last hit (/&PH=)
bool     wifiPowerSave   = false;  // modem sleep between beacons (/&WP=)

bool     displayOn    = true;      // is the panel currently powered up?
bool     targetSeen   = false;     // a qualifying target in the latest frame
uint16_t lastTargetMm = 0;         // nearest target distance, for the web UI
uint32_t lastSeenMs   = 0;
uint32_t radarFrames  = 0;         // sanity counter: 0 => wiring/baud problem
#define LABEL_MAX_CHARS 8          // longest label we'll pin (keeps the window usable)

// Height of one row including its spacing, and how many rows fit:
// n rows occupy n*glyphH + (n-1)*gap pixels.
static inline int16_t rowH(void)   { return glyphH() + lineGap; }
static inline uint8_t rowCap(void)
{
  int16_t n = (PANEL_HEIGHT + lineGap) / rowH();
  if (n < 1) n = 1;
  if (n > MAX_LINES) n = MAX_LINES;
  return (uint8_t)n;
}

// ============================================================
//  WIFI
// ============================================================
const char ssid[]     = SECRET_SSID;
const char password[] = SECRET_PASS;

WiFiServer server(80);

// ============================================================
//  USER ACCOUNTS  - edit entries here (max 8)
//  Bump numUsers to match how many you fill in.
// ============================================================
#define MAX_USERS 8
struct UserAccount { char username[32]; char password[32]; };

// Account list and count come from arduino_secrets.h.
// Unfilled slots are zero-initialized and skipped.
UserAccount accounts[MAX_USERS] = SECRET_ACCOUNTS;
uint8_t numUsers = SECRET_NUM_USERS;

// ============================================================
//  DISPLAY STATE
// ============================================================
uint16_t   scrollSpeed   = 35;      // ms between 1px steps (lower = faster)
ScrollDir  scrollDir     = DIR_LEFT;
ColorMode  colorMode     = CM_SOLID;

uint8_t    brightnessPct = 30;      // 0-100 (RGB panels are very bright)

// ---- Color themes (see struct Theme in TYPES) ---------------------------
// Rows are separated by HUE, not by brightness: a dimmed shade of the same
// color just looks muddy on an LED panel, so every role gets its own bright,
// fully-saturated color instead.
const Theme themes[] = {
  //           weather     NOW         NEXT
  { "SUNSET",  0xFFC400,   0xFF5722,   0xFF4081, false },  // gold / orange / pink
  { "AMBER",   0xFFD500,   0xFF8A00,   0xFFFFFF, false },  // yellow / orange / white
  { "MATRIX",  0xCCFF00,   0x00FF41,   0x00FFC8, false },  // lime / green / aqua
  { "SIREN",   0xFFFFFF,   0xFF2020,   0x2E7BFF, false },  // white / red / blue
  { "NEON",    0xFFFF00,   0xFF00E0,   0x00E5FF, false },  // yellow / magenta / cyan
  { "RAINBOW", 0,          0,          0,        true  },
};
#define NUM_THEMES (sizeof(themes) / sizeof(themes[0]))

uint8_t themeIdx = 0;

// A /&CO= request overrides the theme with one flat color (API back-compat).
bool     customColor    = false;
uint32_t customColorRGB = 0xF0A500;

#define RAINBOW_STEP  700           // hue advance per scroll step
uint16_t   huePhase    = 0;

char curMessage[BUF_SIZE];
char newMessage[BUF_SIZE];
bool newMessageAvailable = false;
bool checkRequested      = false;   // web UI asked for a GitHub update check

#define WX_SIZE 64
char weatherMsg[WX_SIZE] = "";      // top-row weather, pushed via /&WX=

// ---- Weather icons ------------------------------------------------------
// 8x8 monochrome bitmaps, one byte per row, MSB = leftmost pixel. Drawn
// after the weather text and scaled to match the current text size.
#define ICON_W 8
#define ICON_H 8
#define ICON_GAP 2                  // pixels between text and icon

const uint8_t iconBits[][ICON_H] PROGMEM = {
  { 0x18,0x99,0x3C,0x7E,0x7E,0x3C,0x99,0x18 },  // 0 SUN   (clear)
  { 0x00,0x18,0x3C,0x7E,0xFF,0x7E,0x00,0x00 },  // 1 CLOUD (overcast)
  { 0x10,0x54,0x38,0x7C,0xFE,0x7C,0x00,0x00 },  // 2 PARTLY
  { 0x38,0x7C,0xFE,0x7C,0x00,0x48,0x90,0x24 },  // 3 RAIN
  { 0x10,0x54,0x38,0xFE,0x38,0x54,0x10,0x00 },  // 4 SNOW
  { 0x38,0x7C,0xFE,0x7C,0x18,0x30,0x7C,0x18 },  // 5 STORM
  { 0x00,0x7C,0x00,0xFE,0x00,0x7C,0x00,0xFE },  // 6 FOG
};
#define NUM_ICONS (sizeof(iconBits) / sizeof(iconBits[0]))

// Natural colors so icons read at a glance regardless of the text theme.
const uint32_t iconColor[NUM_ICONS] = {
  0xFFD000,   // sun    - yellow
  0xB0B0B0,   // cloud  - grey
  0xE0C060,   // partly - pale gold
  0x40A0FF,   // rain   - blue
  0xFFFFFF,   // snow   - white
  0xFF40C0,   // storm  - magenta
  0x9AA0A6,   // fog    - slate
};

int8_t weatherIcon = -1;            // -1 = none, else index into iconBits

Line     lines[MAX_LINES];
uint8_t  numLines = 0;
uint32_t lastStep = 0;

// ============================================================
//  TIME / CLOCK
// ============================================================
// True once NTP has actually set the clock (epoch past 2023-11).
bool timeValid(void)
{
  return time(nullptr) > 1700000000;
}

void formatClock(char *out, size_t n)
{
  struct tm t;
  if (!getLocalTime(&t, 0)) { out[0] = '\0'; return; }
  int h = t.tm_hour % 12; if (!h) h = 12;
  snprintf(out, n, "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}

void formatDate(char *out, size_t n)
{
  struct tm t;
  if (!getLocalTime(&t, 0)) { out[0] = '\0'; return; }
  strftime(out, n, "%a %b %d", &t);
}

// Inside the night window? Handles windows that wrap past midnight.
bool isNight(void)
{
  if (!nightDim || !timeValid()) return false;
  if (nightStartHr == nightEndHr) return false;
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  int h = t.tm_hour;
  return (nightStartHr < nightEndHr) ? (h >= nightStartHr && h < nightEndHr)
                                     : (h >= nightStartHr || h < nightEndHr);
}

// The message is stale when the sync has stopped pushing updates. Only
// meaningful once the clock is valid, and only used to decide whether the
// panel should fall back to showing the time.
bool messageIsStale(void)
{
  return (millis() - lastMsgMs) > (uint32_t)STALE_MINS * 60000UL;
}

// ============================================================
//  PRESENCE (LD2450 mmWave radar)
// ============================================================
// Frame: AA FF 03 00 | 3 targets x 8 bytes | 55 CC
// Target: x(2) y(2) speed(2) resolution(2), little-endian.
void pollPresence(void)
{
  static uint8_t buf[LD2450_FRAME];
  static uint8_t idx = 0;

  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    // Resync on the 4-byte header rather than trusting stream alignment.
    if (idx == 0 && b != 0xAA) continue;
    if (idx == 1 && b != 0xFF) { idx = 0; continue; }
    if (idx == 2 && b != 0x03) { idx = 0; continue; }
    if (idx == 3 && b != 0x00) { idx = 0; continue; }

    buf[idx++] = b;
    if (idx < LD2450_FRAME) continue;
    idx = 0;

    if (buf[28] != 0x55 || buf[29] != 0xCC) continue;   // tail mismatch
    radarFrames++;

    bool     hit     = false;
    uint32_t nearest = 0xFFFFFFFF;
    for (uint8_t t = 0; t < 3; t++) {
      const uint8_t *q = buf + 4 + t * 8;
      uint16_t xr = (uint16_t)q[0] | ((uint16_t)q[1] << 8);
      uint16_t yr = (uint16_t)q[2] | ((uint16_t)q[3] << 8);
      if (!xr && !yr) continue;                          // empty target slot

      // Only the magnitude matters for range gating, and the magnitude is
      // the low 15 bits whichever way round the sign bit is encoded - so
      // this stays correct without depending on that detail.
      uint32_t x = xr & 0x7FFF, y = yr & 0x7FFF;
      uint32_t d = (uint32_t)sqrtf((float)(x * x + y * y));
      if (d < nearest) nearest = d;
      if (!presenceRangeMm || d <= presenceRangeMm) hit = true;
    }

    if (nearest != 0xFFFFFFFF) lastTargetMm = (uint16_t)nearest;
    targetSeen = hit;
    if (hit) lastSeenMs = millis();
  }
}

// Power the panel up or down to match presence. matrix.stop() sets OE high
// and halts the refresh timer, so a blanked panel costs almost nothing.
void updateDisplayPower(void)
{
  bool want;
  if (!presenceEnabled) {
    want = true;                       // sensing off => always lit
  } else {
    want = targetSeen ||
           (millis() - lastSeenMs) < (uint32_t)presenceHoldSec * 1000UL;
  }

  if (want && !displayOn) {
    matrix.resume();
    displayOn = true;
    layoutLines();
    renderFrame();
    PRINTS("\nPresence: display ON");
  } else if (!want && displayOn) {
    matrix.stop();
    displayOn = false;
    PRINTS("\nPresence: display OFF");
  }
}

void applyWifiPowerSave(void)
{
  esp_wifi_set_ps(wifiPowerSave ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

// ============================================================
//  SETTINGS PERSISTENCE (NVS)
// ============================================================
// Display settings survive a reboot. Writes are debounced: dragging a
// slider marks the settings dirty and one write lands a few seconds later,
// rather than hammering flash on every pixel of slider travel.
Preferences prefs;
bool     settingsDirty = false;
uint32_t settingsDueMs = 0;
#define  SETTINGS_DEBOUNCE_MS 4000

void markSettingsDirty(void)
{
  settingsDirty = true;
  settingsDueMs = millis() + SETTINGS_DEBOUNCE_MS;
}

void saveSettings(void)
{
  prefs.begin("ticker", false);
  prefs.putUShort("sp", scrollSpeed);
  prefs.putUChar ("br", brightnessPct);
  prefs.putUChar ("lg", lineGap);
  prefs.putUShort("pk", parkMs);
  prefs.putUChar ("ts", textSize);
  prefs.putUChar ("th", themeIdx);
  prefs.putBool  ("sk", stickyLabels);
  prefs.putUChar ("sd", (uint8_t)scrollDir);
  prefs.putUChar ("cm", (uint8_t)colorMode);
  prefs.putBool  ("cc", customColor);
  prefs.putUInt  ("co", customColorRGB);
  prefs.putUChar ("cl", clockMode);
  prefs.putBool  ("nd", nightDim);
  prefs.putUChar ("nb", nightBrightPct);
  prefs.putUChar ("ns", nightStartHr);
  prefs.putUChar ("ne", nightEndHr);
  prefs.putBool  ("pr", presenceEnabled);
  prefs.putUShort("pd", presenceRangeMm);
  prefs.putUShort("ph", presenceHoldSec);
  prefs.putBool  ("wp", wifiPowerSave);
  prefs.end();
  settingsDirty = false;
  PRINTS("\nSettings saved to NVS");
}

void loadSettings(void)
{
  prefs.begin("ticker", true);          // read-only; missing keys keep defaults
  scrollSpeed    = prefs.getUShort("sp", scrollSpeed);
  brightnessPct  = prefs.getUChar ("br", brightnessPct);
  lineGap        = prefs.getUChar ("lg", lineGap);
  parkMs         = prefs.getUShort("pk", parkMs);
  textSize       = prefs.getUChar ("ts", textSize);
  themeIdx       = prefs.getUChar ("th", themeIdx);
  stickyLabels   = prefs.getBool  ("sk", stickyLabels);
  scrollDir      = (ScrollDir)prefs.getUChar("sd", (uint8_t)scrollDir);
  colorMode      = (ColorMode)prefs.getUChar("cm", (uint8_t)colorMode);
  customColor    = prefs.getBool  ("cc", customColor);
  customColorRGB = prefs.getUInt  ("co", customColorRGB);
  clockMode      = prefs.getUChar ("cl", clockMode);
  nightDim       = prefs.getBool  ("nd", nightDim);
  nightBrightPct = prefs.getUChar ("nb", nightBrightPct);
  nightStartHr   = prefs.getUChar ("ns", nightStartHr);
  nightEndHr     = prefs.getUChar ("ne", nightEndHr);
  presenceEnabled = prefs.getBool  ("pr", presenceEnabled);
  presenceRangeMm = prefs.getUShort("pd", presenceRangeMm);
  presenceHoldSec = prefs.getUShort("ph", presenceHoldSec);
  wifiPowerSave   = prefs.getBool  ("wp", wifiPowerSave);
  prefs.end();

  // Clamp everything: NVS could hold values from an older build.
  scrollSpeed    = constrain(scrollSpeed, 5, 200);
  brightnessPct  = constrain(brightnessPct, 1, 100);
  lineGap        = constrain(lineGap, 0, LINE_GAP_MAX);
  parkMs         = constrain(parkMs, 0, PARK_MS_MAX);
  textSize       = constrain(textSize, TEXT_SIZE_MIN, TEXT_SIZE_MAX);
  if (themeIdx >= NUM_THEMES) themeIdx = 0;
  if (clockMode > 2)          clockMode = 2;
  nightBrightPct = constrain(nightBrightPct, 1, 100);
  if (nightStartHr > 23) nightStartHr = 22;
  if (nightEndHr   > 23) nightEndHr   = 7;
  presenceRangeMm = constrain(presenceRangeMm, 0, 8000);
  presenceHoldSec = constrain(presenceHoldSec, 5, 3600);
  PRINTS("\nSettings loaded from NVS");
}

// ============================================================
//  COLOR HELPERS
// ============================================================
// Scale an 8-bit channel by the global brightness percentage.
// Brightness actually used for the current frame. isNight() consults the RTC,
// which is far too slow to call per pixel, so renderFrame() caches it here.
uint8_t curBright = 30;
static inline uint8_t dim(uint8_t v) { return (uint16_t)v * curBright / 100; }

// HSV -> RGB565, with value already scaled by brightness.
uint16_t hsv565(uint16_t hue, uint8_t sat, uint8_t val)
{
  uint8_t r, g, b;
  hue = (uint32_t)hue * 1530 / 65536;          // 0..1529
  uint8_t region = hue / 255;                   // 0..5
  uint8_t rem    = hue % 255;
  uint8_t p = 0;
  uint8_t q = 255 - rem;
  uint8_t t = rem;
  switch (region) {
    case 0: r = 255; g = t;   b = p;   break;
    case 1: r = q;   g = 255; b = p;   break;
    case 2: r = p;   g = 255; b = t;   break;
    case 3: r = p;   g = q;   b = 255; break;
    case 4: r = t;   g = p;   b = 255; break;
    default:r = 255; g = p;   b = q;   break;
  }
  // apply saturation
  r = 255 - ((255 - r) * sat / 255);
  g = 255 - ((255 - g) * sat / 255);
  b = 255 - ((255 - b) * sat / 255);
  // apply value/brightness
  r = (uint16_t)r * val / 255;
  g = (uint16_t)g * val / 255;
  b = (uint16_t)b * val / 255;
  return matrix.color565(r, g, b);
}

// Pack a 24-bit RGB value into RGB565, scaled by the brightness setting.
uint16_t rgb565(uint32_t rgb)
{
  return matrix.color565(dim((rgb >> 16) & 0xFF),
                         dim((rgb >>  8) & 0xFF),
                         dim( rgb        & 0xFF));
}

// Color for a row, honoring the active theme (or a /&CO= override).
uint16_t roleColor(uint8_t role)
{
  if (customColor) return rgb565(customColorRGB);
  const Theme &t = themes[themeIdx];
  return rgb565(role == 0 ? t.wx : (role == 1 ? t.c1 : t.c2));
}

// Draw an 8x8 icon scaled by 'scale', skipping pixels off-panel.
void drawIcon(int32_t x, int16_t y, uint8_t idx, uint8_t scale, uint16_t color)
{
  if (idx >= NUM_ICONS) return;
  for (uint8_t row = 0; row < ICON_H; row++) {
    uint8_t bits = pgm_read_byte(&iconBits[idx][row]);
    for (uint8_t col = 0; col < ICON_W; col++) {
      if (!(bits & (0x80 >> col))) continue;
      int32_t px = x + (int32_t)col * scale;
      if (px + scale <= 0 || px >= PANEL_WIDTH) continue;
      matrix.fillRect(px, y + row * scale, scale, scale, color);
    }
  }
}

// ============================================================
//  SCROLL RENDERER
// ============================================================
// Trim leading/trailing spaces (old presets padded text to fake centering).
static void trimInto(char *dst, const char *src, size_t cap)
{
  while (*src == ' ') src++;
  size_t n = strlen(src);
  while (n && src[n-1] == ' ') n--;
  if (n > cap - 1) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// A row like "NOW: Lunch" is split so "NOW:" can stay pinned. Returns the
// offset of the scrolling body, or 0 when there's no label worth pinning.
static uint16_t findLabelSplit(const char *s)
{
  if (!stickyLabels) return 0;
  for (uint16_t i = 0; i < LABEL_MAX_CHARS && s[i]; i++) {
    if (s[i] == ':') {
      uint16_t j = i + 1;
      while (s[j] == ' ') j++;      // skip the space after the colon
      return s[j] ? j : 0;          // need something left to scroll
    }
  }
  return 0;
}

static void initLine(Line &ln, const char *text, uint8_t role, int8_t icon)
{
  trimInto(ln.text, text, sizeof(ln.text));
  ln.role  = role;
  ln.icon  = icon;
  ln.textW = (int16_t)strlen(ln.text) * charW();

  ln.pixW = ln.textW;
  if (icon >= 0) ln.pixW += ICON_GAP * textSize + ICON_W * textSize;

  // Pinned label (never on the weather row - its icon already trails the text)
  ln.bodyOff = (role == 0) ? 0 : findLabelSplit(ln.text);
  if (ln.bodyOff) {
    // Width of the label itself, without the space that follows it.
    uint16_t labelChars = ln.bodyOff;
    while (labelChars && ln.text[labelChars - 1] == ' ') labelChars--;
    ln.labelW = (int16_t)labelChars * charW() + charW() / 2;   // + half-cell gutter
    ln.bodyW  = (int16_t)strlen(ln.text + ln.bodyOff) * charW();
    int16_t window = PANEL_WIDTH - ln.labelW;
    ln.scrolls = ln.bodyW > window;
    ln.x       = ln.scrolls ? 0 : 0;      // body sits right after the label
  } else {
    ln.labelW  = 0;
    ln.bodyW   = ln.pixW;
    ln.scrolls = ln.pixW > PANEL_WIDTH;
    ln.x       = ln.scrolls ? 0 : (PANEL_WIDTH - ln.pixW) / 2;  // center if it fits
  }

  ln.holdUntil = millis() + parkMs;
}

// Rebuild the row list from the weather string + current message.
// The message is split on '|' so the sync can send "NOW: ... | UP NEXT: ..."
// and get two stacked rows.
// Count the non-empty '|'-separated segments in curMessage.
static uint8_t countSegments(void)
{
  uint8_t n = 0;
  const char *p = curMessage;
  while (*p) {
    const char *bar = strchr(p, '|');
    size_t len = bar ? (size_t)(bar - p) : strlen(p);
    char probe[BUF_SIZE];
    if (len > sizeof(probe) - 1) len = sizeof(probe) - 1;
    memcpy(probe, p, len);
    probe[len] = '\0';
    char trimmed[BUF_SIZE];
    trimInto(trimmed, probe, sizeof(trimmed));
    if (trimmed[0]) n++;
    if (!bar) break;
    p = bar + 1;
  }
  return n;
}

void layoutLines(void)
{
  numLines = 0;
  uint8_t cap = rowCap();

  // Clock rows. In mode 2 the clock only appears once the message has gone
  // stale - i.e. the sync stopped pushing, so the PC is probably off and a
  // frozen "NOW: Lunch" at 9pm would be worse than showing the time.
  char clkTime[24] = "", clkDate[24] = "";
  bool haveTime = timeValid();
  bool takeOver = haveTime && clockMode == 2 && messageIsStale();
  bool clockRow = haveTime && clockMode == 1;
  if (takeOver || clockRow) {
    formatClock(clkTime, sizeof(clkTime));
    if (takeOver) formatDate(clkDate, sizeof(clkDate));
  }

  // How many rows the message side wants, so weather can claim a leftover row.
  uint8_t wantRows;
  if (takeOver) wantRows = (clkTime[0] ? 1 : 0) + (clkDate[0] ? 1 : 0);
  else          wantRows = countSegments() + (clkTime[0] ? 1 : 0);

  // At larger text sizes only a row or two fit. The status message matters
  // more than the weather, so weather only gets a row if one is left over.
  if (weatherMsg[0] && wantRows < cap)
    initLine(lines[numLines++], weatherMsg, 0, weatherIcon);

  if (takeOver) {
    if (clkTime[0] && numLines < cap) initLine(lines[numLines++], clkTime, 1, -1);
    if (clkDate[0] && numLines < cap) initLine(lines[numLines++], clkDate, 2, -1);
    return;                       // clock replaces the stale message entirely
  }

  if (clkTime[0] && numLines < cap)
    initLine(lines[numLines++], clkTime, 1, -1);

  // Walk curMessage, splitting on '|'
  uint8_t msgRow = 0;
  const char *p = curMessage;
  while (*p && numLines < cap) {
    const char *bar = strchr(p, '|');
    char part[BUF_SIZE];
    size_t n = bar ? (size_t)(bar - p) : strlen(p);
    if (n > sizeof(part) - 1) n = sizeof(part) - 1;
    memcpy(part, p, n);
    part[n] = '\0';

    // Skip separator-only fragments
    char probe[BUF_SIZE];
    trimInto(probe, part, sizeof(probe));
    if (probe[0]) {
      initLine(lines[numLines++], part, msgRow == 0 ? 1 : 2, -1);
      msgRow++;
    }

    if (!bar) break;
    p = bar + 1;
  }
}

// Draw one row at vertical offset y, honoring color mode.
static void drawLine(const Line &ln, int16_t y)
{
  bool     rainbow   = (colorMode == CM_RAINBOW) && !customColor;
  uint16_t col       = roleColor(ln.role);
  int16_t  bodyStart = ln.labelW;             // 0 when there's no pinned label
  const char *body   = ln.text + ln.bodyOff;

  // A scrolling line is drawn twice (offset by its width + pad) so the text
  // wraps around seamlessly instead of blanking between cycles.
  int reps = ln.scrolls ? 2 : 1;
  for (int r = 0; r < reps; r++) {
    int32_t base = bodyStart + ln.x + (int32_t)r * (ln.bodyW + SCROLL_PAD);

    int32_t cx = base;
    for (size_t i = 0; body[i]; i++) {
      if (cx > -charW() && cx < PANEL_WIDTH) {
        matrix.setTextColor(rainbow
          ? hsv565(huePhase + (uint16_t)(i * 2600), 255, dim(255))
          : col);
        matrix.setCursor(cx, y);
        matrix.write((uint8_t)body[i]);
      }
      cx += charW();
    }

    // Icon trails the text, in its own natural color.
    if (ln.icon >= 0)
      drawIcon(base + ln.textW + ICON_GAP * textSize, y,
               (uint8_t)ln.icon, textSize, rgb565(iconColor[ln.icon]));
  }

  // Pinned label last: blank whatever scrolled underneath it, then draw the
  // label on top. Adafruit_GFX has no clip rect, so this erase is what gives
  // the scrolling body a clean edge to disappear behind.
  if (ln.labelW > 0) {
    matrix.fillRect(0, y, ln.labelW, glyphH(), 0);
    matrix.setTextColor(col);
    matrix.setCursor(0, y);
    for (uint16_t i = 0; i < ln.bodyOff; i++)
      matrix.write((uint8_t)ln.text[i]);
  }
}

void renderFrame(void)
{
  curBright = isNight() ? nightBrightPct : brightnessPct;

  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  matrix.setTextSize(textSize);

  int16_t h     = rowH();
  int16_t total = numLines ? numLines * h - lineGap : 0;
  int16_t y     = (PANEL_HEIGHT - total) / 2;   // vertically center the block
  if (y < 0) y = 0;

  for (uint8_t i = 0; i < numLines; i++, y += h)
    drawLine(lines[i], y);

  matrix.show();
}

void advanceScroll(void)
{
  uint32_t now = millis();
  bool anyScrolling = false;

  for (uint8_t i = 0; i < numLines; i++) {
    Line &ln = lines[i];
    if (!ln.scrolls) continue;          // short lines stay put
    anyScrolling = true;
    if (now < ln.holdUntil) continue;   // brief pause at cycle start

    int32_t span = ln.bodyW + SCROLL_PAD;
    if (scrollDir == DIR_LEFT) {
      if (--ln.x <= -span) { ln.x += span; ln.holdUntil = now + parkMs; }
    } else {
      if (++ln.x >= 0)     { ln.x -= span; ln.holdUntil = now + parkMs; }
    }
  }

  // A pending message swaps in immediately when nothing is mid-scroll;
  // otherwise it waits for the scroll to come back around to the start.
  if (newMessageAvailable) {
    bool atRest = true;
    for (uint8_t i = 0; i < numLines && atRest; i++)
      if (lines[i].scrolls && lines[i].x != 0) atRest = false;
    if (!anyScrolling || atRest) {
      strcpy(curMessage, newMessage);
      newMessageAvailable = false;
      layoutLines();
    }
  }

  huePhase += RAINBOW_STEP;
}

// ============================================================
//  BASE64 ENCODE  (Basic Auth token verification)
// ============================================================
static const char b64chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64encode(const char *in, char *out)
{
  int i = 0, j = 0, len = strlen(in);
  uint8_t a3[3], a4[4];
  while (len--) {
    a3[i++] = *in++;
    if (i == 3) {
      a4[0] = (a3[0] & 0xfc) >> 2;
      a4[1] = ((a3[0] & 0x03) << 4) | ((a3[1] & 0xf0) >> 4);
      a4[2] = ((a3[1] & 0x0f) << 2) | ((a3[2] & 0xc0) >> 6);
      a4[3] =   a3[2] & 0x3f;
      for (int k = 0; k < 4; k++) out[j++] = b64chars[a4[k]];
      i = 0;
    }
  }
  if (i) {
    for (int k = i; k < 3; k++) a3[k] = '\0';
    a4[0] = (a3[0] & 0xfc) >> 2;
    a4[1] = ((a3[0] & 0x03) << 4) | ((a3[1] & 0xf0) >> 4);
    a4[2] = ((a3[1] & 0x0f) << 2) | ((a3[2] & 0xc0) >> 6);
    for (int k = 0; k < i + 1; k++) out[j++] = b64chars[a4[k]];
    while (i++ < 3) out[j++] = '=';
  }
  out[j] = '\0';
}

// authLine is the full "Authorization: Basic <token>" header line (or empty)
bool checkAuth(const char *authLine)
{
  const char *prefix = "Authorization: Basic ";
  if (strncmp(authLine, prefix, 21) != 0) return false;
  const char *token = authLine + 21;

  char credential[68], encoded[128];
  for (uint8_t u = 0; u < numUsers; u++) {
    if (strlen(accounts[u].username) == 0) continue;
    snprintf(credential, sizeof(credential), "%s:%s",
             accounts[u].username, accounts[u].password);
    b64encode(credential, encoded);
    if (strcmp(token, encoded) == 0) return true;
  }
  return false;
}

// ============================================================
//  URL HELPERS
// ============================================================
uint8_t htoi(char c)
{
  c = toupper(c);
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// URL-decode [pStart,pEnd) into dst (at most cap-1 chars + NUL).
void decodeInto(char *dst, size_t cap, const char *pStart, const char *pEnd)
{
  char *psz = dst;
  while (pStart != pEnd && psz < dst + cap - 1) {
    if (*pStart == '%' && isxdigit(*(pStart+1)) && isxdigit(*(pStart+2))) {
      char c = (htoi(*++pStart) << 4);
      c     += htoi(*++pStart);
      *psz++ = c;
      pStart++;
    } else if (*pStart == '+') {
      *psz++ = ' ';
      pStart++;
    } else {
      *psz++ = *pStart++;
    }
  }
  *psz = '\0';
}

static inline void decodeMessage(const char *pStart, const char *pEnd)
{
  decodeInto(newMessage, BUF_SIZE, pStart, pEnd);
}

// ============================================================
//  PARSE PARAMETERS from raw HTTP request
// ============================================================
void getData(const char *buf)
{
  const char *p;

  p = strstr(buf, "/&MSG=");
  if (p) {
    p += 6;
    const char *end = strstr(p, "/&");
    if (end) {
      decodeMessage(p, end);
      newMessageAvailable = (strlen(newMessage) != 0);
      lastMsgMs = millis();          // liveness stamp for the clock fallback
      PRINT("\nNew Msg: ", newMessage);
    }
  }

  p = strstr(buf, "/&SD=");
  if (p) { p += 5; scrollDir = (*p == 'R') ? DIR_RIGHT : DIR_LEFT; markSettingsDirty(); }

  p = strstr(buf, "/&SP=");
  if (p) { scrollSpeed = constrain((int16_t)atoi(p + 5), 5, 200); markSettingsDirty(); }

  // Text size: /&TS=1..4  (GFX classic font scale)
  p = strstr(buf, "/&TS=");
  if (p) {
    uint8_t ts = constrain((int16_t)atoi(p + 5), TEXT_SIZE_MIN, TEXT_SIZE_MAX);
    if (ts != textSize) { textSize = ts; layoutLines(); }
    markSettingsDirty();   // geometry changed
  }

  // Line spacing: /&LG=0..6 blank pixels between rows
  p = strstr(buf, "/&LG=");
  if (p) {
    uint8_t lg = constrain((int16_t)atoi(p + 5), 0, LINE_GAP_MAX);
    if (lg != lineGap) { lineGap = lg; layoutLines(); }
    markSettingsDirty();
  }

  // Park time: /&PK=0..10000 ms a row sits still before scrolling again
  p = strstr(buf, "/&PK=");
  if (p) { parkMs = constrain((int32_t)atol(p + 5), 0, PARK_MS_MAX); markSettingsDirty(); }

  // Sticky labels: /&SK=1 pins "NOW:"/"NEXT:", /&SK=0 scrolls the whole row
  p = strstr(buf, "/&SK=");
  if (p) {
    bool sk = (*(p + 5) == '1');
    if (sk != stickyLabels) { stickyLabels = sk; layoutLines(); }
    markSettingsDirty();
  }

  // Clock: /&CL=0 off, 1 always a row, 2 only when the message goes stale
  p = strstr(buf, "/&CL=");
  if (p) {
    uint8_t cl = constrain((int16_t)atoi(p + 5), 0, 2);
    if (cl != clockMode) { clockMode = cl; layoutLines(); }
    markSettingsDirty();
  }

  // Night dimming: /&ND=0|1, /&NB=level, /&NS=start hour, /&NE=end hour
  p = strstr(buf, "/&ND=");
  if (p) { nightDim = (*(p + 5) == '1'); markSettingsDirty(); }

  p = strstr(buf, "/&NB=");
  if (p) { nightBrightPct = constrain((int16_t)atoi(p + 5), 1, 100); markSettingsDirty(); }

  p = strstr(buf, "/&NS=");
  if (p) { nightStartHr = constrain((int16_t)atoi(p + 5), 0, 23); markSettingsDirty(); }

  p = strstr(buf, "/&NE=");
  if (p) { nightEndHr = constrain((int16_t)atoi(p + 5), 0, 23); markSettingsDirty(); }

  // Presence sensing: /&PR=0|1 enable, /&PD= wake range mm, /&PH= hold seconds
  p = strstr(buf, "/&PR=");
  if (p) { presenceEnabled = (*(p + 5) == '1'); markSettingsDirty(); }

  p = strstr(buf, "/&PD=");
  if (p) { presenceRangeMm = constrain((int32_t)atol(p + 5), 0, 8000); markSettingsDirty(); }

  p = strstr(buf, "/&PH=");
  if (p) { presenceHoldSec = constrain((int32_t)atol(p + 5), 5, 3600); markSettingsDirty(); }

  // Wi-Fi modem sleep between beacons - saves power, costs a little latency
  p = strstr(buf, "/&WP=");
  if (p) { wifiPowerSave = (*(p + 5) == '1'); applyWifiPowerSave(); markSettingsDirty(); }

  // Weather line: /&WX=<text>/&   (empty value clears it)
  p = strstr(buf, "/&WX=");
  if (p) {
    p += 5;
    const char *end = strstr(p, "/&");
    if (end) {
      char raw[WX_SIZE];
      decodeInto(raw, sizeof(raw), p, end);
      strncpy(weatherMsg, raw, sizeof(weatherMsg) - 1);
      weatherMsg[sizeof(weatherMsg) - 1] = '\0';
      layoutLines();
      PRINT("\nWeather: ", weatherMsg);
    }
  }

  p = strstr(buf, "/&BR=");
  if (p) { brightnessPct = constrain((int16_t)atoi(p + 5), 1, 100); markSettingsDirty(); }

  // Color theme: /&TH=0..N  (see the themes[] table)
  p = strstr(buf, "/&TH=");
  if (p) {
    int16_t t = atoi(p + 5);
    if (t >= 0 && t < (int16_t)NUM_THEMES) {
      themeIdx    = (uint8_t)t;
      customColor = false;               // theme wins over any /&CO= override
      colorMode   = themes[themeIdx].rainbow ? CM_RAINBOW : CM_SOLID;
      markSettingsDirty();
    }
  }

  // Flat color override as 6 hex digits: /&CO=RRGGBB  (API back-compat)
  p = strstr(buf, "/&CO=");
  if (p) {
    p += 5;
    if (isxdigit(p[0]) && isxdigit(p[1]) && isxdigit(p[2]) &&
        isxdigit(p[3]) && isxdigit(p[4]) && isxdigit(p[5])) {
      customColorRGB = ((uint32_t)((htoi(p[0]) << 4) | htoi(p[1])) << 16) |
                       ((uint32_t)((htoi(p[2]) << 4) | htoi(p[3])) <<  8) |
                        (uint32_t)((htoi(p[4]) << 4) | htoi(p[5]));
      customColor = true;
      colorMode   = CM_SOLID;
      markSettingsDirty();
    }
  }

  // Weather icon: /&WI=0..6, or -1/empty for none
  p = strstr(buf, "/&WI=");
  if (p) {
    int16_t ic = atoi(p + 5);
    weatherIcon = (ic >= 0 && ic < (int16_t)NUM_ICONS) ? (int8_t)ic : -1;
    layoutLines();
  }

  // Color mode: /&CM=S (solid) or /&CM=R (rainbow)
  p = strstr(buf, "/&CM=");
  if (p) { p += 5; colorMode = (*p == 'R') ? CM_RAINBOW : CM_SOLID; }

  // Update check: /&CHK=1  -> respond with JSON version comparison
  p = strstr(buf, "/&CHK=");
  if (p) checkRequested = true;
}

// ============================================================
//  UPDATE CHECK  (fetch version.txt from GitHub over HTTPS)
// ============================================================
// Returns true if the dotted version 'remote' is newer than 'local'.
bool versionNewer(const char *remote, const char *local)
{
  int r[4] = {0,0,0,0}, l[4] = {0,0,0,0};
  sscanf(remote, "%d.%d.%d.%d", &r[0], &r[1], &r[2], &r[3]);
  sscanf(local,  "%d.%d.%d.%d", &l[0], &l[1], &l[2], &l[3]);
  for (int i = 0; i < 4; i++)
    if (r[i] != l[i]) return r[i] > l[i];
  return false;
}

// Fetch the latest published version string into 'out'.
// Returns true on success (HTTP 200 with a non-empty body).
bool fetchLatestVersion(char *out, size_t len)
{
  out[0] = '\0';
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure secure;
  secure.setInsecure();               // skip cert validation (hobby update check)

  HTTPClient https;
  https.setTimeout(6000);
  if (!https.begin(secure, VERSION_URL)) return false;

  int code = https.GET();
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    String body = https.getString();
    body.trim();
    if (body.length() && body.length() < len) {
      strncpy(out, body.c_str(), len - 1);
      out[len - 1] = '\0';
      ok = true;
    }
  }
  PRINT("\nUpdate check HTTP ", code);
  https.end();
  return ok;
}

// ============================================================
//  WIFI STATE MACHINE
// ============================================================
void handleWiFi(void)
{
  static enum { S_IDLE, S_WAIT_CONN, S_READ, S_EXTRACT, S_RESPONSE, S_DISCONN }
    state = S_IDLE;

  // Read line-by-line so large browser headers never overflow a buffer.
  // We only keep the two lines we actually need:
  //   requestLine - "GET /&MSG=hello/& HTTP/1.1" (URL params)
  //   authLine    - "Authorization: Basic xxxxx"
  static char requestLine[512];
  static char authLine[256];
  static char lineBuf[512];
  static uint16_t lineIdx = 0;
  static bool requestCaptured = false;

  static WiFiClient client;
  static uint32_t timeStart;

  // Handle BLANK immediately
  if (strcmp(newMessage, "BLANK") == 0) {
    curMessage[0]       = '\0';
    newMessage[0]       = '\0';
    newMessageAvailable = false;
    numLines            = 0;        // drop all rows (weather included)
    matrix.fillScreen(0);
    matrix.show();
  }

  switch (state)
  {
  case S_IDLE:
    requestLine[0] = authLine[0] = lineBuf[0] = '\0';
    lineIdx = 0;
    requestCaptured = false;
    checkRequested  = false;   // reset per request; getData re-sets if /&CHK= present
    state = S_WAIT_CONN;
    break;

  case S_WAIT_CONN:
  {
    client = server.available();
    if (!client || !client.connected()) break;
    PRINT("\nClient: ", client.remoteIP());
    timeStart = millis();
    state     = S_READ;
  }
  break;

  case S_READ:
    while (client.available()) {
      char c = client.read();

      if (c == '\n') {
        // Strip trailing \r
        if (lineIdx > 0 && lineBuf[lineIdx-1] == '\r') lineIdx--;
        lineBuf[lineIdx] = '\0';

        if (lineIdx == 0) {
          // Blank line = end of headers
          client.flush();
          state = S_EXTRACT;
          break;
        }

        // First non-empty line is the request line
        if (!requestCaptured) {
          strncpy(requestLine, lineBuf, sizeof(requestLine) - 1);
          requestLine[sizeof(requestLine) - 1] = '\0';
          requestCaptured = true;
          PRINT("\nReq: ", requestLine);
        }
        // Capture Authorization header wherever it falls
        else if (strncmp(lineBuf, "Authorization:", 14) == 0) {
          strncpy(authLine, lineBuf, sizeof(authLine) - 1);
          authLine[sizeof(authLine) - 1] = '\0';
          PRINTS("\nAuth header found");
        }

        lineIdx = 0;
      } else {
        if (lineIdx < sizeof(lineBuf) - 1) lineBuf[lineIdx++] = c;
      }
    }
    if (millis() - timeStart > 1500) state = S_DISCONN;
    break;

  case S_EXTRACT:
    getData(requestLine);
    state = S_RESPONSE;
    break;

  case S_RESPONSE:
    if (!checkAuth(authLine)) {
      client.print("HTTP/1.1 401 Unauthorized\r\n");
      client.print("WWW-Authenticate: Basic realm=\"LED Ticker Control\"\r\n");
      client.print("Content-Type: text/html\r\n\r\n");
      client.print(
        "<html><body style='background:#080808;color:#f0a500;"
        "font-family:monospace;text-align:center;padding-top:60px'>"
        "<h2>&#128274; 401 &mdash; Authentication Required</h2>"
        "<p style='color:#444'>Enter your LED Ticker credentials.</p>"
        "</body></html>"
      );
    } else if (checkRequested) {
      // Update check: fetch version.txt from GitHub and reply with JSON.
      checkRequested = false;
      char latest[32];
      bool ok = fetchLatestVersion(latest, sizeof(latest));
      client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n");
      client.print("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
      client.print("{\"cur\":\"" FW_VERSION "\",");
      if (ok) {
        client.print("\"latest\":\"");
        client.print(latest);
        client.print("\",\"update\":");
        client.print(versionNewer(latest, FW_VERSION) ? "true" : "false");
        client.print("}");
      } else {
        client.print("\"latest\":\"\",\"update\":false,\"err\":\"Could not reach GitHub\"}");
      }
    } else {
      client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n");

      client.print("<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>JOES RGB Matrix Control</title>"
        "<link href=\"https://fonts.googleapis.com/css2?family=VT323&family=Share+Tech+Mono&display=swap\" rel=\"stylesheet\">"
        "<style>"
        ":root{--a:#f0a500;--ad:#7a5200;--ag:#f0a50020;--dk:#080808;--cd:#101010;--br:#1e1e1e;--muted:#444}"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{background:var(--dk);color:var(--a);font-family:'Share Tech Mono',monospace;padding:16px;max-width:560px;margin:0 auto;min-height:100vh}"
        "h1{font-family:'VT323',monospace;font-size:2.8rem;letter-spacing:.2em;text-align:center;text-shadow:0 0 16px var(--a),0 0 32px #f0a50040;padding:20px 0 4px}"
        ".sub{text-align:center;font-size:.72rem;color:var(--ad);letter-spacing:.25em;margin-bottom:18px}"
        ".marquee-wrap{background:#000;border:1px solid var(--ad);border-radius:6px;padding:9px 14px;margin-bottom:20px;overflow:hidden;white-space:nowrap;box-shadow:inset 0 0 20px #00000080}"
        ".marquee-inner{display:inline-block;font-family:'VT323',monospace;font-size:1.5rem;animation:mq 16s linear infinite}"
        "@keyframes mq{0%{transform:translateX(420px)}100%{transform:translateX(-700px)}}"
        ".card{background:var(--cd);border:1px solid var(--br);border-radius:8px;padding:18px;margin-bottom:14px}"
        ".ctitle{font-family:'VT323',monospace;font-size:1.4rem;letter-spacing:.12em;color:var(--a);border-bottom:1px solid var(--br);padding-bottom:8px;margin-bottom:14px;display:flex;align-items:center;gap:8px}"
        ".ctitle .ico{color:var(--ad);font-size:1rem}"
        "input[type=text]{width:100%;background:#000;border:1px solid var(--ad);color:var(--a);font-family:'Share Tech Mono',monospace;font-size:.95rem;padding:10px 12px;border-radius:4px;outline:none;transition:border-color .2s,box-shadow .2s}"
        "input[type=text]:focus{border-color:var(--a);box-shadow:0 0 10px var(--ag)}"
        "input[type=range]{flex:1;accent-color:var(--a);cursor:pointer;height:4px}"
        "input[type=color]{width:44px;height:32px;background:#000;border:1px solid var(--ad);border-radius:4px;cursor:pointer;padding:2px}"
        ".row{display:flex;align-items:center;gap:10px;margin-top:12px}"
        ".btn{background:transparent;border:1px solid var(--a);color:var(--a);font-family:'Share Tech Mono',monospace;font-size:.85rem;padding:8px 18px;border-radius:4px;cursor:pointer;transition:all .15s;white-space:nowrap}"
        ".btn:hover{background:var(--a);color:#000}"
        ".btn.prim{background:var(--a);color:#000;font-weight:bold}"
        ".btn.prim:hover{background:#ffb700;box-shadow:0 0 14px #f0a50050}"
        ".btn.dang{border-color:#c0392b;color:#c0392b}"
        ".btn.dang:hover{background:#c0392b;color:#fff}"
        ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:14px}"
        ".pb{background:transparent;border:1px solid var(--br);color:var(--a);font-family:'Share Tech Mono',monospace;font-size:.75rem;padding:10px 8px;border-radius:4px;cursor:pointer;transition:all .15s;text-align:left;line-height:1.5}"
        ".pb:hover,.pb.sel{border-color:var(--a);background:var(--ag);box-shadow:0 0 8px var(--ag)}"
        ".pb em{display:block;font-style:normal;font-size:1.2rem;margin-bottom:2px}"
        ".cl{min-width:90px;font-size:.78rem;color:var(--muted)}"
        ".cv{min-width:28px;text-align:right;font-size:.85rem;color:var(--a)}"
        ".tg{display:flex;gap:6px;flex:1}"
        ".tb{flex:1;background:transparent;border:1px solid var(--br);color:var(--muted);font-family:'Share Tech Mono',monospace;font-size:.75rem;padding:7px 4px;border-radius:4px;cursor:pointer;transition:all .15s;text-align:center}"
        ".tb.on{border-color:var(--a);color:var(--a);background:var(--ag)}"
        ".tb:hover{border-color:var(--ad);color:var(--a)}"
        ".thg{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px}"
        ".th{background:transparent;border:1px solid var(--br);color:var(--muted);font-family:'Share Tech Mono',monospace;font-size:.68rem;padding:8px 4px;border-radius:4px;cursor:pointer;transition:all .15s;display:flex;flex-direction:column;align-items:center;gap:3px}"
        ".th i{display:block;width:26px;height:4px;border-radius:2px}"
        ".th.on{border-color:var(--a);color:var(--a);background:var(--ag);box-shadow:0 0 8px var(--ag)}"
        ".th:hover{border-color:var(--ad);color:var(--a)}"
        "input[type=number]{width:52px;background:#000;border:1px solid var(--ad);color:var(--a);font-family:'Share Tech Mono',monospace;font-size:.8rem;padding:6px 4px;border-radius:4px;text-align:center;outline:none}"
        "select{width:100%;background:#000;border:1px solid var(--ad);color:var(--a);font-family:'Share Tech Mono',monospace;font-size:.85rem;padding:8px 10px;border-radius:4px;outline:none;margin-top:10px}"
        ".sbar{background:#000;border:1px solid var(--br);border-radius:4px;padding:9px 13px;font-size:.75rem;color:var(--muted);margin-top:4px;display:flex;align-items:center;gap:8px}"
        ".dot{width:7px;height:7px;border-radius:50%;background:var(--a);flex-shrink:0;animation:pl 2s ease-in-out infinite}"
        "@keyframes pl{0%,100%{opacity:1}50%{opacity:.25}}"
        "</style>"
        "<script>"
        "var sp='';"
        "function sMsg(m){var r=new XMLHttpRequest();r.open('GET','/&MSG='+encodeURIComponent(m)+'/&nc='+Math.random(),false);r.send();sts('Sent → '+m);}"
        "function sTxt(){var m=document.getElementById('mi').value.trim();if(m)sMsg(m);}"
        "function blk(){var r=new XMLHttpRequest();r.open('GET','/&MSG=BLANK/&',false);r.send();sts('Display blanked');}"
        "function pWx(v,i){var r=new XMLHttpRequest();r.open('GET','/&WX='+encodeURIComponent(v)+'/&WI='+i+'/&nc='+Math.random(),false);r.send();}"
        "function sWx(){var v=document.getElementById('wi').value.trim();var i=document.getElementById('wic').value;if(v){pWx(v,i);sts('Weather set');}}"
        "function clWx(){document.getElementById('wi').value='';document.getElementById('wic').value='-1';pWx('','-1');sts('Weather cleared');}"
        "function selP(el,v){sp=v;document.querySelectorAll('.pb').forEach(b=>b.classList.remove('sel'));el.classList.add('sel');}"
        "function sndP(){if(sp)sMsg(sp);else sts('Select a preset first');}"
        "function apl(){var s=document.getElementById('sv').value;var b=document.getElementById('bv').value;"
        "var d=document.querySelector('.tb.dir.on');var t=document.querySelector('.th.on');"
        "var z=document.querySelector('.tb.size.on');"
        "var g=document.getElementById('lg').value;var pk=document.getElementById('pk').value;"
        "var k=document.querySelector('.tb.stk.on');"
        "var cl=document.querySelector('.tb.clk.on');var nd=document.querySelector('.tb.nit.on');"
        "var nb=document.getElementById('nb').value;"
        "var pr=document.querySelector('.tb.prs.on');var wp=document.querySelector('.tb.wps.on');"
        "var pd=document.getElementById('pd').value;var ph=document.getElementById('ph').value;"
        "var ns=document.getElementById('ns').value;var ne=document.getElementById('ne').value;"
        "var url='/&SP='+s+'/&BR='+b+'/&LG='+g+'/&PK='+pk;"
        "if(t)url+='/&TH='+t.dataset.v;if(k)url+='/&SK='+k.dataset.v;"
        "if(cl)url+='/&CL='+cl.dataset.v;if(nd)url+='/&ND='+nd.dataset.v;"
        "url+='/&NB='+nb+'/&NS='+ns+'/&NE='+ne+'/&PD='+pd+'/&PH='+ph;"
        "if(pr)url+='/&PR='+pr.dataset.v;if(wp)url+='/&WP='+wp.dataset.v;"
        "if(d)url+='/&SD='+d.dataset.v;if(z)url+='/&TS='+z.dataset.v;"
        "url+='/&nc='+Math.random();"
        "var r=new XMLHttpRequest();r.open('GET',url,false);r.send();sts('Controls applied');}"
        "function tog(cls,el){document.querySelectorAll('.tb.'+cls).forEach(b=>b.classList.remove('on'));el.classList.add('on');}"
        "function togTh(el){document.querySelectorAll('.th').forEach(b=>b.classList.remove('on'));el.classList.add('on');}"
        "function upd(id,v){document.getElementById(id).innerText=v;}"
        "function sts(m){document.getElementById('st').innerText=m;}"
        "function chk(){var m=document.getElementById('fwmsg');m.innerHTML='Contacting GitHub&hellip;';sts('Checking&hellip;');"
        "var r=new XMLHttpRequest();r.open('GET','/&CHK=1/&nc='+Math.random(),true);r.timeout=9000;"
        "r.ontimeout=function(){m.innerHTML='&#9888; Timed out contacting GitHub';sts('Check timed out');};"
        "r.onreadystatechange=function(){if(r.readyState!=4)return;"
        "try{var j=JSON.parse(r.responseText);"
        "if(j.err){m.innerHTML='&#9888; '+j.err;sts('Check failed');}"
        "else if(j.update){m.innerHTML='&#8593; Update available: <b>v'+j.latest+'</b> (installed v'+j.cur+'). "
        "<a href=\"" REPO_URL "\" target=\"_blank\" style=\"color:var(--a)\">Open GitHub</a>';sts('Update available');}"
        "else{m.innerHTML='&#10003; Up to date (v'+j.cur+').';sts('Up to date');}"
        "}catch(e){m.innerHTML='&#9888; Unexpected response';sts('Check error');}};r.send();}"
        "</script></head><body>"

        "<h1>&#9632; JOES RGB TICKER &#9632;</h1>"
        "<p class=\"sub\">MATRIX PORTAL S3 &middot; RGB CONTROLLER v4.0</p>"

        "<div class=\"marquee-wrap\"><span class=\"marquee-inner\" id=\"mqt\">"
        "JOE&rsquo;S TICKER TAPE &nbsp;&mdash;&nbsp; SYSTEM READY &nbsp;&mdash;&nbsp; AWAITING INPUT &nbsp;&mdash;&nbsp; "
        "</span></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#9632;</span>NOW SHOWING</div>"
        "<div id=\"nsmsg\" style=\"font-family:'VT323',monospace;font-size:1.6rem;color:var(--a);"
        "background:#000;border:1px solid var(--ad);border-radius:4px;padding:10px 14px;"
        "letter-spacing:.08em;text-shadow:0 0 10px var(--ag);min-height:2.4rem;word-break:break-all\">&#8212;</div></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#9654;</span>CUSTOM MESSAGE</div>"
        "<input type=\"text\" id=\"mi\" maxlength=\"255\" placeholder=\"Type your message...\""
        " onkeydown=\"if(event.key==='Enter')sTxt()\">"
        "<div class=\"row\">"
        "<button class=\"btn prim\" onclick=\"sTxt()\">SEND</button>"
        "<button class=\"btn dang\" onclick=\"blk()\">BLANK DISPLAY</button>"
        "</div>"
        "<p style=\"font-size:.7rem;color:var(--muted);margin-top:10px;line-height:1.5\">"
        "Use <b>|</b> to split the message across rows, e.g."
        " <span style=\"color:var(--ad)\">NOW: Lunch | UP NEXT: 1:00 PM Review</span>."
        " Rows that fit stay still; only long rows scroll.</p></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#9925;</span>WEATHER ROW</div>"
        "<input type=\"text\" id=\"wi\" maxlength=\"60\" placeholder=\"e.g. 72F Sunny\""
        " onkeydown=\"if(event.key==='Enter')sWx()\">"
        "<select id=\"wic\">"
        "<option value=\"-1\">No icon</option>"
        "<option value=\"0\">&#9728; Sun / Clear</option>"
        "<option value=\"1\">&#9729; Cloud / Overcast</option>"
        "<option value=\"2\">&#9925; Partly cloudy</option>"
        "<option value=\"3\">&#127783; Rain</option>"
        "<option value=\"4\">&#10052; Snow</option>"
        "<option value=\"5\">&#9889; Storm</option>"
        "<option value=\"6\">&#127787; Fog</option>"
        "</select>"
        "<div class=\"row\">"
        "<button class=\"btn prim\" onclick=\"sWx()\">SET WEATHER</button>"
        "<button class=\"btn\" onclick=\"clWx()\">CLEAR</button>"
        "</div>"
        "<p style=\"font-size:.7rem;color:var(--muted);margin-top:10px;line-height:1.5\">"
        "Pinned to the top row. The calendar sync updates this automatically.</p></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#9654;</span>PRESET MESSAGES</div>"
        "<div class=\"grid\">"
        "<button class=\"pb\" onclick=\"selP(this,'In a Meeting!')\"><em>&#128197;</em>IN A MEETING</button>"
        "<button class=\"pb\" onclick=\"selP(this,'Welcome')\"><em>&#128075;</em>WELCOME</button>"
        "<button class=\"pb\" onclick=\"selP(this,'I am Free')\"><em>&#128994;</em>I AM FREE</button>"
        "<button class=\"pb\" onclick=\"selP(this,'I am Hungry :(')\"><em>&#127829;</em>HUNGRY</button>"
        "<button class=\"pb\" onclick=\"selP(this,'Doing Paperwork!')\"><em>&#128336;</em>PAPERWORK</button>"
        "<button class=\"pb\" onclick=\"selP(this,'Do Not Disturb')\"><em>&#128683;</em>DO NOT DISTURB</button>"
        "<button class=\"pb\" onclick=\"selP(this,'On Break')\"><em>&#9749;</em>ON BREAK</button>"
        "<button class=\"pb\" onclick=\"selP(this,'Done For The Day!')\"><em>&#128274;</em>DONE</button>"
        "</div>"
        "<button class=\"btn prim\" onclick=\"sndP()\">SEND PRESET</button></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#9654;</span>DISPLAY CONTROLS</div>"
        "<div class=\"row\"><span class=\"cl\">SPEED</span>"
        "<input type=\"range\" id=\"sv\" min=\"5\" max=\"100\" value=\"35\" oninput=\"upd('sc',this.value)\">"
        "<span class=\"cv\" id=\"sc\">35</span></div>"
        "<div class=\"row\"><span class=\"cl\">LINE GAP</span>"
        "<input type=\"range\" id=\"lg\" min=\"0\" max=\"6\" value=\"4\" oninput=\"upd('lgc',this.value)\">"
        "<span class=\"cv\" id=\"lgc\">4</span></div>"
        "<div class=\"row\"><span class=\"cl\">PARK TIME</span>"
        "<input type=\"range\" id=\"pk\" min=\"0\" max=\"10000\" step=\"250\" value=\"2500\""
        " oninput=\"upd('pkc',(this.value/1000).toFixed(2)+'s')\">"
        "<span class=\"cv\" id=\"pkc\" style=\"min-width:46px\">2.50s</span></div>"
        "<div class=\"row\"><span class=\"cl\">STICKY LABELS</span><div class=\"tg\">"
        "<button class=\"tb stk on\" data-v=\"1\" onclick=\"tog('stk',this)\">PINNED</button>"
        "<button class=\"tb stk\" data-v=\"0\" onclick=\"tog('stk',this)\">SCROLL ALL</button>"
        "</div></div>"
        "<div class=\"row\"><span class=\"cl\">CLOCK</span><div class=\"tg\">"
        "<button class=\"tb clk\" data-v=\"0\" onclick=\"tog('clk',this)\">OFF</button>"
        "<button class=\"tb clk\" data-v=\"1\" onclick=\"tog('clk',this)\">ALWAYS</button>"
        "<button class=\"tb clk on\" data-v=\"2\" onclick=\"tog('clk',this)\">IF IDLE</button>"
        "</div></div>"
        "<div class=\"row\"><span class=\"cl\">NIGHT DIM</span><div class=\"tg\">"
        "<button class=\"tb nit on\" data-v=\"1\" onclick=\"tog('nit',this)\">ON</button>"
        "<button class=\"tb nit\" data-v=\"0\" onclick=\"tog('nit',this)\">OFF</button>"
        "</div></div>"
        "<div class=\"row\"><span class=\"cl\">NIGHT LEVEL</span>"
        "<input type=\"range\" id=\"nb\" min=\"1\" max=\"100\" value=\"8\" oninput=\"upd('nbc',this.value)\">"
        "<span class=\"cv\" id=\"nbc\">8</span></div>"
        "<div class=\"row\"><span class=\"cl\">NIGHT HOURS</span>"
        "<input type=\"number\" id=\"ns\" min=\"0\" max=\"23\" value=\"22\">"
        "<span style=\"color:var(--muted);font-size:.75rem\">to</span>"
        "<input type=\"number\" id=\"ne\" min=\"0\" max=\"23\" value=\"7\">"
        "<span style=\"color:var(--muted);font-size:.72rem\">(24h)</span></div>"
        "<div class=\"row\"><span class=\"cl\">TEXT SIZE</span><div class=\"tg\">"
        "<button class=\"tb size on\" data-v=\"1\" onclick=\"tog('size',this)\">S</button>"
        "<button class=\"tb size\" data-v=\"2\" onclick=\"tog('size',this)\">M</button>"
        "<button class=\"tb size\" data-v=\"3\" onclick=\"tog('size',this)\">L</button>"
        "<button class=\"tb size\" data-v=\"4\" onclick=\"tog('size',this)\">XL</button>"
        "</div></div>"
        "<div class=\"row\"><span class=\"cl\">BRIGHTNESS</span>"
        "<input type=\"range\" id=\"bv\" min=\"5\" max=\"100\" value=\"30\" oninput=\"upd('bc',this.value)\">"
        "<span class=\"cv\" id=\"bc\">30</span></div>"
        "<div class=\"row\"><span class=\"cl\">THEME</span></div>"
        "<div class=\"thg\">"
        "<button class=\"th on\" data-v=\"0\" onclick=\"togTh(this)\">"
        "<i style=\"background:#ffc400\"></i><i style=\"background:#ff5722\"></i>"
        "<i style=\"background:#ff4081\"></i>SUNSET</button>"
        "<button class=\"th\" data-v=\"1\" onclick=\"togTh(this)\">"
        "<i style=\"background:#ffd500\"></i><i style=\"background:#ff8a00\"></i>"
        "<i style=\"background:#ffffff\"></i>AMBER</button>"
        "<button class=\"th\" data-v=\"2\" onclick=\"togTh(this)\">"
        "<i style=\"background:#ccff00\"></i><i style=\"background:#00ff41\"></i>"
        "<i style=\"background:#00ffc8\"></i>MATRIX</button>"
        "<button class=\"th\" data-v=\"3\" onclick=\"togTh(this)\">"
        "<i style=\"background:#ffffff\"></i><i style=\"background:#ff2020\"></i>"
        "<i style=\"background:#2e7bff\"></i>SIREN</button>"
        "<button class=\"th\" data-v=\"4\" onclick=\"togTh(this)\">"
        "<i style=\"background:#ffff00\"></i><i style=\"background:#ff00e0\"></i>"
        "<i style=\"background:#00e5ff\"></i>NEON</button>"
        "<button class=\"th\" data-v=\"5\" onclick=\"togTh(this)\">"
        "<i style=\"background:linear-gradient(90deg,#f00,#ff0,#0f0)\"></i>"
        "<i style=\"background:linear-gradient(90deg,#0f0,#0ff,#00f)\"></i>"
        "<i style=\"background:linear-gradient(90deg,#00f,#f0f,#f00)\"></i>RAINBOW</button>"
        "</div>"
        "<div class=\"row\"><span class=\"cl\">DIRECTION</span><div class=\"tg\">"
        "<button class=\"tb dir on\" data-v=\"L\" onclick=\"tog('dir',this)\">&#8592; LEFT</button>"
        "<button class=\"tb dir\" data-v=\"R\" onclick=\"tog('dir',this)\">RIGHT &#8594;</button>"
        "</div></div>"
        "<div class=\"row\"><button class=\"btn prim\" onclick=\"apl()\">APPLY CONTROLS</button></div></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#128225;</span>PRESENCE (LD2450)</div>"
        "<div class=\"row\"><span class=\"cl\">SENSING</span><div class=\"tg\">"
        "<button class=\"tb prs\" data-v=\"1\" onclick=\"tog('prs',this)\">ON</button>"
        "<button class=\"tb prs on\" data-v=\"0\" onclick=\"tog('prs',this)\">OFF</button>"
        "</div></div>"
        "<div class=\"row\"><span class=\"cl\">WAKE RANGE</span>"
        "<input type=\"range\" id=\"pd\" min=\"0\" max=\"8000\" step=\"250\" value=\"3000\""
        " oninput=\"upd('pdc',this.value==0?'any':(this.value/1000).toFixed(2)+'m')\">"
        "<span class=\"cv\" id=\"pdc\" style=\"min-width:44px\">3.00m</span></div>"
        "<div class=\"row\"><span class=\"cl\">STAY LIT</span>"
        "<input type=\"range\" id=\"ph\" min=\"5\" max=\"600\" step=\"5\" value=\"60\""
        " oninput=\"upd('phc',this.value+'s')\">"
        "<span class=\"cv\" id=\"phc\" style=\"min-width:44px\">60s</span></div>"
        "<div class=\"row\"><span class=\"cl\">WIFI SAVER</span><div class=\"tg\">"
        "<button class=\"tb wps\" data-v=\"1\" onclick=\"tog('wps',this)\">ON</button>"
        "<button class=\"tb wps on\" data-v=\"0\" onclick=\"tog('wps',this)\">OFF</button>"
        "</div></div>"
        "<div id=\"prstat\" style=\"font-size:.75rem;color:var(--muted);margin-top:12px;line-height:1.6\"></div></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#8635;</span>FIRMWARE</div>"
        "<div class=\"row\"><span class=\"cl\">VERSION</span>"
        "<span class=\"cv\" id=\"fwcur\" style=\"min-width:auto\">&#8230;</span>"
        "<button class=\"btn\" onclick=\"chk()\">CHECK FOR UPDATES</button></div>"
        "<div id=\"fwmsg\" style=\"font-size:.78rem;color:var(--muted);margin-top:12px;line-height:1.5\">"
        "Compares this device against the latest version on GitHub.<br>"
        "Also reachable at <b>http://" HOSTNAME ".local/</b></div></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#128274;</span>ACTIVE USERS</div>"
        "<div id=\"ulist\" style=\"font-size:.8rem;color:var(--muted);line-height:2\">Loading...</div></div>"

        "<div class=\"sbar\"><div class=\"dot\"></div><span id=\"st\">READY</span></div>"
        "</body></html>"
      );

      // Inject live state
      client.print("<script>");
      client.print("var cm=\"");
      for (char *cp = curMessage; *cp; cp++) {
        if (*cp == '"' || *cp == '\\') client.print("\\");
        client.print(*cp);
      }
      client.print("\";");
      client.print("var el=document.getElementById('nsmsg');");
      client.print("if(el)el.innerText=cm.trim()||'(blank)';");
      client.print("var mq=document.getElementById('mqt');");
      client.print("if(mq&&cm.trim())mq.innerText=mq.innerText+' --- NOW SHOWING: '+cm.trim()+' --- ';");
      client.print("var ul=document.getElementById('ulist');if(ul){ul.innerHTML='");
      for (uint8_t u = 0; u < numUsers; u++) {
        if (strlen(accounts[u].username) == 0) continue;
        client.print("<span style=\\'color:#f0a500;margin-right:14px\\'>&#128100; ");
        client.print(accounts[u].username);
        client.print("</span>");
      }
      client.print("';}");
      client.print("var fc=document.getElementById('fwcur');if(fc)fc.innerText='v" FW_VERSION "';");

      // Reflect the device's live settings in the controls
      client.print("var wt=\"");
      for (char *cp = weatherMsg; *cp; cp++) {
        if (*cp == '"' || *cp == '\\') client.print("\\");
        client.print(*cp);
      }
      client.print("\";var wf=document.getElementById('wi');if(wf)wf.value=wt;");
      client.print("var ws=document.getElementById('wic');if(ws)ws.value='");
      client.print((int)weatherIcon);
      client.print("';");
      client.print("var tb=document.querySelector('.th[data-v=\"");
      client.print(themeIdx);
      client.print("\"]');if(tb)togTh(tb);");
      client.print("var zb=document.querySelector('.tb.size[data-v=\"");
      client.print(textSize);
      client.print("\"]');if(zb)tog('size',zb);");
      client.print("var sv=document.getElementById('sv');if(sv){sv.value=");
      client.print(scrollSpeed);
      client.print(";upd('sc',sv.value);}");
      client.print("var bv=document.getElementById('bv');if(bv){bv.value=");
      client.print(brightnessPct);
      client.print(";upd('bc',bv.value);}");
      client.print("var lv=document.getElementById('lg');if(lv){lv.value=");
      client.print(lineGap);
      client.print(";upd('lgc',lv.value);}");
      client.print("var pv=document.getElementById('pk');if(pv){pv.value=");
      client.print(parkMs);
      client.print(";upd('pkc',(pv.value/1000).toFixed(2)+'s');}");
      client.print("var kb=document.querySelector('.tb.stk[data-v=\"");
      client.print(stickyLabels ? 1 : 0);
      client.print("\"]');if(kb)tog('stk',kb);");
      client.print("var cb=document.querySelector('.tb.clk[data-v=\"");
      client.print(clockMode);
      client.print("\"]');if(cb)tog('clk',cb);");
      client.print("var nn=document.querySelector('.tb.nit[data-v=\"");
      client.print(nightDim ? 1 : 0);
      client.print("\"]');if(nn)tog('nit',nn);");
      client.print("var nbv=document.getElementById('nb');if(nbv){nbv.value=");
      client.print(nightBrightPct);
      client.print(";upd('nbc',nbv.value);}");
      client.print("var nsv=document.getElementById('ns');if(nsv)nsv.value=");
      client.print(nightStartHr);
      client.print(";");
      client.print("var nev=document.getElementById('ne');if(nev)nev.value=");
      client.print(nightEndHr);
      client.print(";");
      client.print("var pb=document.querySelector('.tb.prs[data-v=\"");
      client.print(presenceEnabled ? 1 : 0);
      client.print("\"]');if(pb)tog('prs',pb);");
      client.print("var wb=document.querySelector('.tb.wps[data-v=\"");
      client.print(wifiPowerSave ? 1 : 0);
      client.print("\"]');if(wb)tog('wps',wb);");
      client.print("var pdv=document.getElementById('pd');if(pdv){pdv.value=");
      client.print(presenceRangeMm);
      client.print(";upd('pdc',pdv.value==0?'any':(pdv.value/1000).toFixed(2)+'m');}");
      client.print("var phv=document.getElementById('ph');if(phv){phv.value=");
      client.print(presenceHoldSec);
      client.print(";upd('phc',phv.value+'s');}");

      // Live radar status: radarFrames == 0 means the sensor never spoke,
      // which is the signature of a wiring or baud-rate problem.
      client.print("var ps=document.getElementById('prstat');if(ps)ps.innerHTML='");
      if (radarFrames == 0) {
        client.print("&#9888; No LD2450 data yet - check wiring (sensor TX to board RX/GPIO8) and 5V power.");
      } else {
        client.print("Radar OK, ");
        client.print(radarFrames);
        client.print(" frames &middot; ");
        client.print(targetSeen ? "&#128100; person detected" : "no one detected");
        if (lastTargetMm) { client.print(" &middot; nearest "); client.print(lastTargetMm); client.print("mm"); }
      }
      // Always report panel power, so the sleep/wake path is observable even
      // when the radar is silent.
      client.print("<br>Panel: ");
      client.print(displayOn ? "<b>ON</b>" : "<b>ASLEEP</b> (matrix.stop)");
      client.print(", sensing ");
      client.print(presenceEnabled ? "enabled" : "disabled");
      client.print("';");
      client.print("</script>");
    }

    state = S_DISCONN;
    break;

  case S_DISCONN:
    client.flush();
    client.stop();
    state = S_IDLE;
    break;

  default:
    state = S_IDLE;
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup()
{
  Serial.begin(57600);
  PRINTS("\n[RGB Matrix Ticker Controller]");

  ProtomatterStatus status = matrix.begin();
  PRINT("\nProtomatter begin() status: ", (int)status);
  if (status != PROTOMATTER_OK) {
    PRINTS("\nMatrix init FAILED - check panel wiring / pin mapping");
    // Halt: nothing sensible to do without a display.
    for (;;) delay(1000);
  }

  curMessage[0] = newMessage[0] = '\0';
  loadSettings();

  strcpy(curMessage, "Starting up...");
  lastMsgMs = millis();
  layoutLines();
  renderFrame();

  PRINT("\nConnecting to ", ssid);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    PRINT("\n", WiFi.status());
    delay(500);
  }
  PRINTS("\nWiFi connected");
  PRINT("\nIP: ", WiFi.localIP());

  // Reachable by name, so a changed DHCP lease doesn't strand the UI.
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    PRINTS("\nmDNS: http://" HOSTNAME ".local/");
  } else {
    PRINTS("\nmDNS start failed (IP still works)");
  }

  applyWifiPowerSave();

  // LD2450 presence radar on the header's TX/RX pins.
  Serial1.begin(LD2450_BAUD, SERIAL_8N1, LD2450_RX_PIN, LD2450_TX_PIN);
  PRINTS("\nLD2450 UART started on RX=8 TX=18 @256000");

  // Clock for the stale-message fallback and night dimming.
  configTzTime(TZ_INFO, NTP_1, NTP_2);

  server.begin();
  PRINTS("\nHTTP server running on port 80");
}

// ============================================================
//  LOOP
// ============================================================
void loop()
{
  handleWiFi();
  pollPresence();
  updateDisplayPower();

  // Debounced settings write: one NVS commit after the changes settle.
  if (settingsDirty && (int32_t)(millis() - settingsDueMs) >= 0)
    saveSettings();

  // Once a minute, refresh the clock row / re-evaluate staleness and night
  // dimming. Cheap, and it keeps a still display correct without scrolling.
  static int lastMin = -1;
  if (timeValid()) {
    struct tm tmv;
    if (getLocalTime(&tmv, 0) && tmv.tm_min != lastMin) {
      lastMin = tmv.tm_min;
      if (displayOn) { layoutLines(); renderFrame(); }
    }
  }

  if (millis() - lastStep >= scrollSpeed) {
    lastStep = millis();

    // Only animate when something actually moves: a scrolling line, a
    // pending message swap, or rainbow mode (whose colors always cycle).
    bool needsFrame = newMessageAvailable || (colorMode == CM_RAINBOW);
    for (uint8_t i = 0; i < numLines && !needsFrame; i++)
      if (lines[i].scrolls) needsFrame = true;

    if (needsFrame && displayOn) {
      advanceScroll();
      renderFrame();
    }
  }
}

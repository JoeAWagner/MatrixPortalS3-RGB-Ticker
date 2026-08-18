
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
#define FW_VERSION   "1.3.0"
#define VERSION_URL  "https://raw.githubusercontent.com/JoeAWagner/MatrixPortalS3-RGB-Ticker/main/version.txt"
#define REPO_URL     "https://github.com/JoeAWagner/MatrixPortalS3-RGB-Ticker"

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
#define LINE_GAP      0            // extra pixels between rows
#define SCROLL_PAD   12            // blank pixels between scroll repeats
#define END_PAUSE_MS 1200          // pause at the start of a scroll cycle

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
enum ScrollDir  { DIR_LEFT, DIR_RIGHT };
enum ColorMode  { CM_SOLID, CM_RAINBOW };

uint16_t   scrollSpeed   = 25;      // ms between 1px steps (lower = faster)
ScrollDir  scrollDir     = DIR_LEFT;
ColorMode  colorMode     = CM_SOLID;

uint8_t    txtR = 0xF0, txtG = 0xA5, txtB = 0x00;  // solid color (amber)
uint8_t    brightnessPct = 60;      // 0-100 (RGB panels are very bright)

#define RAINBOW_STEP  700           // hue advance per scroll step
uint16_t   huePhase    = 0;

#define BUF_SIZE 512
char curMessage[BUF_SIZE];
char newMessage[BUF_SIZE];
bool newMessageAvailable = false;
bool checkRequested      = false;   // web UI asked for a GitHub update check

#define WX_SIZE 64
char weatherMsg[WX_SIZE] = "";      // top-row weather, pushed via /&WX=

// One display row. Short lines sit still; only overflowing lines scroll.
struct Line {
  char     text[BUF_SIZE];
  int16_t  pixW;        // rendered width in pixels
  bool     scrolls;     // true when pixW > PANEL_WIDTH
  int32_t  x;           // current x offset (scrolling lines only)
  uint32_t holdUntil;   // pause before a scroll cycle restarts
};

Line     lines[MAX_LINES];
uint8_t  numLines = 0;
uint32_t lastStep = 0;

// ============================================================
//  COLOR HELPERS
// ============================================================
// Scale an 8-bit channel by the global brightness percentage.
static inline uint8_t dim(uint8_t v) { return (uint16_t)v * brightnessPct / 100; }

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

static void initLine(Line &ln, const char *text)
{
  trimInto(ln.text, text, sizeof(ln.text));
  ln.pixW      = (int16_t)strlen(ln.text) * charW();
  ln.scrolls   = ln.pixW > PANEL_WIDTH;
  ln.x         = ln.scrolls ? 0 : (PANEL_WIDTH - ln.pixW) / 2;  // center if it fits
  ln.holdUntil = millis() + END_PAUSE_MS;
}

// Rebuild the row list from the weather string + current message.
// The message is split on '|' so the sync can send "NOW: ... | UP NEXT: ..."
// and get two stacked rows.
void layoutLines(void)
{
  numLines = 0;
  uint8_t rowCap = PANEL_HEIGHT / (glyphH() + LINE_GAP);
  if (rowCap > MAX_LINES) rowCap = MAX_LINES;

  if (weatherMsg[0] && numLines < rowCap)
    initLine(lines[numLines++], weatherMsg);

  // Walk curMessage, splitting on '|'
  const char *p = curMessage;
  while (*p && numLines < rowCap) {
    const char *bar = strchr(p, '|');
    char part[BUF_SIZE];
    size_t n = bar ? (size_t)(bar - p) : strlen(p);
    if (n > sizeof(part) - 1) n = sizeof(part) - 1;
    memcpy(part, p, n);
    part[n] = '\0';

    // Skip separator-only fragments
    char probe[BUF_SIZE];
    trimInto(probe, part, sizeof(probe));
    if (probe[0]) initLine(lines[numLines++], part);

    if (!bar) break;
    p = bar + 1;
  }
}

// Draw one row at vertical offset y, honoring color mode.
static void drawLine(const Line &ln, int16_t y)
{
  uint16_t solid = matrix.color565(dim(txtR), dim(txtG), dim(txtB));

  // A scrolling line is drawn twice (offset by its width + pad) so the text
  // wraps around seamlessly instead of blanking between cycles.
  int reps = ln.scrolls ? 2 : 1;
  for (int r = 0; r < reps; r++) {
    int32_t base = ln.x + (int32_t)r * (ln.pixW + SCROLL_PAD);
    if (colorMode == CM_RAINBOW) {
      int32_t cx = base;
      for (size_t i = 0; ln.text[i]; i++) {
        if (cx > -charW() && cx < PANEL_WIDTH) {
          matrix.setTextColor(hsv565(huePhase + (uint16_t)(i * 2600), 255, dim(255)));
          matrix.setCursor(cx, y);
          matrix.write((uint8_t)ln.text[i]);
        }
        cx += charW();
      }
    } else {
      matrix.setTextColor(solid);
      matrix.setCursor(base, y);
      matrix.print(ln.text);
    }
  }
}

void renderFrame(void)
{
  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  matrix.setTextSize(textSize);

  int16_t rowH  = glyphH() + LINE_GAP;
  int16_t total = numLines * rowH - LINE_GAP;
  int16_t y     = (PANEL_HEIGHT - total) / 2;   // vertically center the block
  if (y < 0) y = 0;

  for (uint8_t i = 0; i < numLines; i++, y += rowH)
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

    int32_t span = ln.pixW + SCROLL_PAD;
    if (scrollDir == DIR_LEFT) {
      if (--ln.x <= -span) { ln.x += span; ln.holdUntil = now + END_PAUSE_MS; }
    } else {
      if (++ln.x >= 0)     { ln.x -= span; ln.holdUntil = now + END_PAUSE_MS; }
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
      PRINT("\nNew Msg: ", newMessage);
    }
  }

  p = strstr(buf, "/&SD=");
  if (p) { p += 5; scrollDir = (*p == 'R') ? DIR_RIGHT : DIR_LEFT; }

  p = strstr(buf, "/&SP=");
  if (p) { scrollSpeed = constrain((int16_t)atoi(p + 5), 5, 200); }

  // Text size: /&TS=1..4  (GFX classic font scale)
  p = strstr(buf, "/&TS=");
  if (p) {
    uint8_t ts = constrain((int16_t)atoi(p + 5), TEXT_SIZE_MIN, TEXT_SIZE_MAX);
    if (ts != textSize) { textSize = ts; layoutLines(); }   // geometry changed
  }

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
  if (p) { brightnessPct = constrain((int16_t)atoi(p + 5), 0, 100); }

  // Solid text color as 6 hex digits: /&CO=RRGGBB
  p = strstr(buf, "/&CO=");
  if (p) {
    p += 5;
    if (isxdigit(p[0]) && isxdigit(p[1]) && isxdigit(p[2]) &&
        isxdigit(p[3]) && isxdigit(p[4]) && isxdigit(p[5])) {
      txtR = (htoi(p[0]) << 4) | htoi(p[1]);
      txtG = (htoi(p[2]) << 4) | htoi(p[3]);
      txtB = (htoi(p[4]) << 4) | htoi(p[5]);
    }
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
        ".sbar{background:#000;border:1px solid var(--br);border-radius:4px;padding:9px 13px;font-size:.75rem;color:var(--muted);margin-top:4px;display:flex;align-items:center;gap:8px}"
        ".dot{width:7px;height:7px;border-radius:50%;background:var(--a);flex-shrink:0;animation:pl 2s ease-in-out infinite}"
        "@keyframes pl{0%,100%{opacity:1}50%{opacity:.25}}"
        "</style>"
        "<script>"
        "var sp='';"
        "function sMsg(m){var r=new XMLHttpRequest();r.open('GET','/&MSG='+encodeURIComponent(m)+'/&nc='+Math.random(),false);r.send();sts('Sent → '+m);}"
        "function sTxt(){var m=document.getElementById('mi').value.trim();if(m)sMsg(m);}"
        "function blk(){var r=new XMLHttpRequest();r.open('GET','/&MSG=BLANK/&',false);r.send();sts('Display blanked');}"
        "function pWx(v){var r=new XMLHttpRequest();r.open('GET','/&WX='+encodeURIComponent(v)+'/&nc='+Math.random(),false);r.send();}"
        "function sWx(){var v=document.getElementById('wi').value.trim();if(v){pWx(v);sts('Weather set');}}"
        "function clWx(){document.getElementById('wi').value='';pWx('');sts('Weather cleared');}"
        "function selP(el,v){sp=v;document.querySelectorAll('.pb').forEach(b=>b.classList.remove('sel'));el.classList.add('sel');}"
        "function sndP(){if(sp)sMsg(sp);else sts('Select a preset first');}"
        "function apl(){var s=document.getElementById('sv').value;var b=document.getElementById('bv').value;"
        "var c=document.getElementById('cp').value.substring(1);"
        "var d=document.querySelector('.tb.dir.on');var m=document.querySelector('.tb.mode.on');"
        "var z=document.querySelector('.tb.size.on');"
        "var url='/&SP='+s+'/&BR='+b+'/&CO='+c;"
        "if(d)url+='/&SD='+d.dataset.v;if(m)url+='/&CM='+m.dataset.v;if(z)url+='/&TS='+z.dataset.v;"
        "url+='/&nc='+Math.random();"
        "var r=new XMLHttpRequest();r.open('GET',url,false);r.send();sts('Controls applied');}"
        "function tog(cls,el){document.querySelectorAll('.tb.'+cls).forEach(b=>b.classList.remove('on'));el.classList.add('on');}"
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
        "<input type=\"range\" id=\"sv\" min=\"5\" max=\"100\" value=\"25\" oninput=\"upd('sc',this.value)\">"
        "<span class=\"cv\" id=\"sc\">25</span></div>"
        "<div class=\"row\"><span class=\"cl\">TEXT SIZE</span><div class=\"tg\">"
        "<button class=\"tb size on\" data-v=\"1\" onclick=\"tog('size',this)\">S</button>"
        "<button class=\"tb size\" data-v=\"2\" onclick=\"tog('size',this)\">M</button>"
        "<button class=\"tb size\" data-v=\"3\" onclick=\"tog('size',this)\">L</button>"
        "<button class=\"tb size\" data-v=\"4\" onclick=\"tog('size',this)\">XL</button>"
        "</div></div>"
        "<div class=\"row\"><span class=\"cl\">BRIGHTNESS</span>"
        "<input type=\"range\" id=\"bv\" min=\"5\" max=\"100\" value=\"60\" oninput=\"upd('bc',this.value)\">"
        "<span class=\"cv\" id=\"bc\">60</span></div>"
        "<div class=\"row\"><span class=\"cl\">TEXT COLOR</span>"
        "<input type=\"color\" id=\"cp\" value=\"#f0a500\">"
        "<span class=\"cv\" style=\"min-width:auto;color:var(--muted);font-size:.72rem\">solid mode</span></div>"
        "<div class=\"row\"><span class=\"cl\">COLOR MODE</span><div class=\"tg\">"
        "<button class=\"tb mode on\" data-v=\"S\" onclick=\"tog('mode',this)\">SOLID</button>"
        "<button class=\"tb mode\" data-v=\"R\" onclick=\"tog('mode',this)\">&#127752; RAINBOW</button>"
        "</div></div>"
        "<div class=\"row\"><span class=\"cl\">DIRECTION</span><div class=\"tg\">"
        "<button class=\"tb dir on\" data-v=\"L\" onclick=\"tog('dir',this)\">&#8592; LEFT</button>"
        "<button class=\"tb dir\" data-v=\"R\" onclick=\"tog('dir',this)\">RIGHT &#8594;</button>"
        "</div></div>"
        "<div class=\"row\"><button class=\"btn prim\" onclick=\"apl()\">APPLY CONTROLS</button></div></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#8635;</span>FIRMWARE</div>"
        "<div class=\"row\"><span class=\"cl\">VERSION</span>"
        "<span class=\"cv\" id=\"fwcur\" style=\"min-width:auto\">&#8230;</span>"
        "<button class=\"btn\" onclick=\"chk()\">CHECK FOR UPDATES</button></div>"
        "<div id=\"fwmsg\" style=\"font-size:.78rem;color:var(--muted);margin-top:12px;line-height:1.5\">"
        "Compares this device against the latest version on GitHub.</div></div>"

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
  strcpy(curMessage, "Starting up...");
  layoutLines();
  renderFrame();

  PRINT("\nConnecting to ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    PRINT("\n", WiFi.status());
    delay(500);
  }
  PRINTS("\nWiFi connected");
  PRINT("\nIP: ", WiFi.localIP());

  server.begin();
  PRINTS("\nHTTP server running on port 80");
}

// ============================================================
//  LOOP
// ============================================================
void loop()
{
  handleWiFi();

  if (millis() - lastStep >= scrollSpeed) {
    lastStep = millis();

    // Only animate when something actually moves: a scrolling line, a
    // pending message swap, or rainbow mode (whose colors always cycle).
    bool needsFrame = newMessageAvailable || (colorMode == CM_RAINBOW);
    for (uint8_t i = 0; i < numLines && !needsFrame; i++)
      if (lines[i].scrolls) needsFrame = true;

    if (needsFrame) {
      advanceScroll();
      renderFrame();
    }
  }
}

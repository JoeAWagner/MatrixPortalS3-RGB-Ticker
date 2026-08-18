
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
#include <Adafruit_Protomatter.h>

// Credentials live in a separate, gitignored file. Copy
// arduino_secrets.h.example to arduino_secrets.h and fill it in.
#include "arduino_secrets.h"

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
#define TEXT_SIZE   3                       // GFX classic font scale
#define CHAR_W      (6 * TEXT_SIZE)          // cell width  (6px * size)
#define GLYPH_H     (8 * TEXT_SIZE)          // cell height (8px * size)
#define TEXT_Y      ((PANEL_HEIGHT - GLYPH_H) / 2)  // vertical centering

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

int32_t   scrollX  = PANEL_WIDTH;   // current pixel position of text start
uint32_t  lastStep = 0;

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
void renderFrame(void)
{
  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  matrix.setTextSize(TEXT_SIZE);

  if (curMessage[0] != '\0') {
    if (colorMode == CM_RAINBOW) {
      int32_t cx = scrollX;
      for (size_t i = 0; curMessage[i]; i++) {
        uint16_t hue = huePhase + (uint16_t)(i * 2600);
        matrix.setTextColor(hsv565(hue, 255, dim(255)));
        matrix.setCursor(cx, TEXT_Y);
        matrix.write((uint8_t)curMessage[i]);
        cx += CHAR_W;
      }
    } else {
      matrix.setTextColor(matrix.color565(dim(txtR), dim(txtG), dim(txtB)));
      matrix.setCursor(scrollX, TEXT_Y);
      matrix.print(curMessage);
    }
  }

  matrix.show();
}

void advanceScroll(void)
{
  int32_t textPixW = (int32_t)strlen(curMessage) * CHAR_W;

  if (scrollDir == DIR_LEFT) {
    scrollX--;
    if (scrollX < -textPixW) {          // fully off the left edge
      scrollX = PANEL_WIDTH;            // re-enter from the right
      if (newMessageAvailable) { strcpy(curMessage, newMessage); newMessageAvailable = false; }
    }
  } else {
    scrollX++;
    if (scrollX > PANEL_WIDTH) {         // fully off the right edge
      scrollX = -textPixW;             // re-enter from the left
      if (newMessageAvailable) { strcpy(curMessage, newMessage); newMessageAvailable = false; }
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

void decodeMessage(const char *pStart, const char *pEnd)
{
  char *psz = newMessage;
  while (pStart != pEnd && psz < newMessage + BUF_SIZE - 1) {
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
    matrix.fillScreen(0);
    matrix.show();
  }

  switch (state)
  {
  case S_IDLE:
    requestLine[0] = authLine[0] = lineBuf[0] = '\0';
    lineIdx = 0;
    requestCaptured = false;
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
        "function selP(el,v){sp=v;document.querySelectorAll('.pb').forEach(b=>b.classList.remove('sel'));el.classList.add('sel');}"
        "function sndP(){if(sp)sMsg(sp);else sts('Select a preset first');}"
        "function apl(){var s=document.getElementById('sv').value;var b=document.getElementById('bv').value;"
        "var c=document.getElementById('cp').value.substring(1);"
        "var d=document.querySelector('.tb.dir.on');var m=document.querySelector('.tb.mode.on');"
        "var url='/&SP='+s+'/&BR='+b+'/&CO='+c;"
        "if(d)url+='/&SD='+d.dataset.v;if(m)url+='/&CM='+m.dataset.v;"
        "url+='/&nc='+Math.random();"
        "var r=new XMLHttpRequest();r.open('GET',url,false);r.send();sts('Controls applied');}"
        "function tog(cls,el){document.querySelectorAll('.tb.'+cls).forEach(b=>b.classList.remove('on'));el.classList.add('on');}"
        "function upd(id,v){document.getElementById(id).innerText=v;}"
        "function sts(m){document.getElementById('st').innerText=m;}"
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
        "</div></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#9654;</span>PRESET MESSAGES</div>"
        "<div class=\"grid\">"
        "<button class=\"pb\" onclick=\"selP(this,'        In a Meeting!')\"><em>&#128197;</em>IN A MEETING</button>"
        "<button class=\"pb\" onclick=\"selP(this,'          Welcome')\"><em>&#128075;</em>WELCOME</button>"
        "<button class=\"pb\" onclick=\"selP(this,'         I am Free')\"><em>&#128994;</em>I AM FREE</button>"
        "<button class=\"pb\" onclick=\"selP(this,'       I am Hungry :(')\"><em>&#127829;</em>HUNGRY</button>"
        "<button class=\"pb\" onclick=\"selP(this,'     Doing Paperwork!')\"><em>&#128336;</em>PAPERWORK</button>"
        "<button class=\"pb\" onclick=\"selP(this,'       Do Not Disturb')\"><em>&#128683;</em>DO NOT DISTURB</button>"
        "<button class=\"pb\" onclick=\"selP(this,'          On Break')\"><em>&#9749;</em>ON BREAK</button>"
        "<button class=\"pb\" onclick=\"selP(this,'    Done For The Day!')\"><em>&#128274;</em>DONE</button>"
        "</div>"
        "<button class=\"btn prim\" onclick=\"sndP()\">SEND PRESET</button></div>"

        "<div class=\"card\"><div class=\"ctitle\"><span class=\"ico\">&#9654;</span>DISPLAY CONTROLS</div>"
        "<div class=\"row\"><span class=\"cl\">SPEED</span>"
        "<input type=\"range\" id=\"sv\" min=\"5\" max=\"100\" value=\"25\" oninput=\"upd('sc',this.value)\">"
        "<span class=\"cv\" id=\"sc\">25</span></div>"
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
  strcpy(curMessage, "       In A Meeting!");
  scrollX = PANEL_WIDTH;
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
    advanceScroll();
    renderFrame();
  }
}

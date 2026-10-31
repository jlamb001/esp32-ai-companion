/*
  AI companion firmware for the LAFVIN ESP32-S3 board.

  Say "wake up" (or hold the BOOT button and talk). The board streams its microphone over WiFi
  to companion_server.py on your PC, which listens for the wake word, then Whisper hears you,
  Ollama answers and Piper speaks. The board plays the reply and shows the matching face (mood
  tags from the reply), with the mouth moving to the sound. After about 25 seconds of quiet it
  falls asleep again (sleepy face); say "go to sleep" to put it down sooner.

  SETUP
  1. Copy secrets.h.example to secrets.h (same folder) and fill in your WiFi and your PC's IP.
  2. Arduino IDE > Library Manager: install "WebSockets" by Markus Sattler,
     and "GFX Library for Arduino" (already installed for the face test).
  3. Board settings as before: ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash 16MB,
     USB CDC On Boot: Disabled, UART USB-C port. ESP32 core 3.x.
  4. On the PC:  python companion_server.py   (voice mode, no --text)
     The PC and the board must be on the same WiFi network (2.4 GHz).
  5. Upload this, open the Serial Monitor (115200) to watch what's happening.

  The board shows "connecting..." until it reaches the server.
*/
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WebSocketsClient.h>

// WiFi name/password and your PC's address live in secrets.h (copy secrets.h.example to
// secrets.h and edit it). secrets.h is git-ignored so your password is never published.
#include "secrets.h"

// ---- pins ----
#define TFT_MOSI 40
#define TFT_SCK  41
#define TFT_DC   39
#define TFT_CS   47
#define TFT_RST  4
#define TFT_BL   42
#define BL_ACTIVE_HIGH 1
#define TFT_ROTATION   1
#define TFT_INVERT     true

#define I2C_SDA 1
#define I2C_SCL 2
#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x41
#define I2S_MCLK 38
#define I2S_BCLK 14
#define I2S_WS   13
#define I2S_DOUT 45
#define I2S_DIN  12
#define PA_EN    48
#define BTN      0            // BOOT button, pressed = LOW

// ---- audio tuning ----
#define SAMPLE_RATE 16000
#define SPK_VOLUME  170       // codec output volume 0..255
#define MIC_GAIN    10        // 0..14, 3 dB per step
#define MIC_DIGITAL 2         // extra digital gain on what is sent to the PC
#define MAX_RECORD_MS 25000

// ============ face ============
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *panel = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, TFT_INVERT, 240, 320);
Arduino_Canvas *gfx = new Arduino_Canvas(320, 240, panel);

uint16_t BG, EYE, MOUTH, DIM, WHITE, GRAY, RED;

struct Mood {
  const char *name;
  float openL, openR;
  int8_t tilt;
  bool happyEyes;
  int8_t smile;
  int8_t mouthOpen;
  int8_t gx, gy;
  int8_t mouthW;
  int8_t mouthX;
  int8_t lid;
  bool brows;
  int8_t browL, browR;
  int8_t browTilt;
  uint8_t fx;
  float talkScale;
};

const Mood MOODS[] = {
  {"neutral",     1.0f,  1.0f,  0, false,   0,   0,   0,   0, 50,  0,  0, false,  0,   0,  0,   0, 1.0f},
  {"happy",       1.0f,  1.0f,  0, true,   14,   0,   0,   0, 70,  0,  0, true,  -8,  -8,   0,   0, 1.0f},
  {"excited",     1.2f,  1.2f,  0, false,  18,  24,   0,   0, 84,  0,  0, true, -16, -16,   0,   1, 1.0f},
  {"curious",     1.15f, 0.85f, 0, false,   0,   0,  12,  -6, 40,  0,  0, true, -18,  -4,   0,   0, 1.0f},
  {"thinking",    0.9f,  0.9f,  0, false,  -2,   0,  22, -14, 30, 16,  0, true,  -4, -18,  -5,   2, 0.6f},
  {"surprised",   1.35f, 1.35f, 0, false,   0,  26,   0,   0, 26,  0,  0, true, -22, -22,   0,   0, 0.5f},
  {"confused",    1.2f,  0.8f,  0, false,  -6,   0,  -8,   0, 56,  0,  0, true, -14,   2,   4,   0, 1.0f},
  {"sad",         0.9f,  0.9f,  1, false, -14,   0,   0,   6, 50,  0,  0, true,  -4,  -4,  10,   0, 1.0f},
  {"sleepy",      0.8f,  0.8f,  0, false,   0,   8,   0,  10, 30,  0, 55, true,   6,   6,   3,   3, 0.0f},
};
const int NMOODS = sizeof(MOODS) / sizeof(MOODS[0]);
enum { M_NEUTRAL, M_HAPPY, M_EXCITED, M_CURIOUS, M_THINKING, M_SURPRISED, M_CONFUSED, M_SAD, M_SLEEPY };

const int EYE_W = 64, EYE_H0 = 76;

void thickLine(int x0, int y0, int x1, int y1, int r, uint16_t c) {
  const int N = 24;
  for (int i = 0; i <= N; i++)
    gfx->fillCircle(x0 + (x1 - x0) * i / N, y0 + (y1 - y0) * i / N, r, c);
}

void drawBrow(int cx, int cy, const Mood &m, bool left) {
  if (!m.brows) return;
  float open = left ? m.openL : m.openR;
  int off = left ? m.browL : m.browR;
  int top = m.happyEyes ? cy - 22 : cy - (int)(EYE_H0 * open / 2);
  int by = top - 14 + off;
  int innerX = cx + (left ? 24 : -24), outerX = cx + (left ? -24 : 24);
  thickLine(innerX, by - m.browTilt, outerX, by + m.browTilt, 4, EYE);
}

void drawEye(int cx, int cy, float open, bool left, const Mood &m, float blink) {
  int h = (int)(EYE_H0 * open * blink);
  if (h < 6) h = 6;
  cx += m.gx;
  cy += m.gy;
  if (m.happyEyes && blink > 0.5f) {
    gfx->fillCircle(cx, cy + 14, 32, EYE);
    gfx->fillCircle(cx, cy + 32, 32, BG);
    return;
  }
  gfx->fillRoundRect(cx - EYE_W / 2, cy - h / 2, EYE_W, h, 22, EYE);
  if (m.lid) {
    int cover = h * m.lid / 100;
    gfx->fillRect(cx - EYE_W / 2 - 2, cy - h / 2 - 2, EYE_W + 4, cover + 2, BG);
  }
  if (m.tilt) {
    int dir = left ? -1 : 1;
    int outerX = cx + dir * (EYE_W / 2 + 2), innerX = cx - dir * (EYE_W / 2);
    int topY = cy - h / 2 - 2;
    gfx->fillTriangle(innerX, topY, outerX, topY, outerX, topY + 28, BG);
  }
  if (m.fx == 1 && h > 30) {
    gfx->fillCircle(cx + 12, cy - h / 4, 7, WHITE);
    gfx->fillCircle(cx - 8, cy + h / 5, 3, WHITE);
  }
}

void drawMouth(const Mood &m, int talk, int cyShift, uint32_t now) {
  const int cx = 160 + m.mouthX, cy = 188 + cyShift;
  int w = m.mouthW;
  int open = m.mouthOpen + talk;
  if (m.fx == 3) open = m.mouthOpen + (int)(5 * sinf(now / 700.0f));
  if (open > 3) {
    int r = (open / 2 < 14) ? open / 2 : 14;
    gfx->fillRoundRect(cx - w / 2, cy - open / 2, w, open, r, MOUTH);
  } else {
    for (int x = -w / 2; x <= w / 2; x += 2) {
      float t = (float)x / (w / 2.0f);
      gfx->fillCircle(cx + x, cy + (int)(m.smile * (1.0f - t * t)), 3, MOUTH);
    }
  }
}

void drawSubtitle(const char *s, uint16_t color) {
  gfx->setTextSize(1);
  gfx->setTextColor(color);
  int n = strlen(s), pos = 0, line = 0;
  while (pos < n && line < 3) {
    int len = min(52, n - pos);
    if (pos + len < n) {
      int b = len;
      while (b > 20 && s[pos + b] != ' ') b--;
      if (s[pos + b] == ' ') len = b;
    }
    char tmp[56];
    memcpy(tmp, s + pos, len);
    tmp[len] = 0;
    gfx->setCursor(6, 212 + line * 10);
    gfx->print(tmp);
    pos += len;
    while (pos < n && s[pos] == ' ') pos++;
    line++;
  }
}

// ============ shared state (audio/network task <-> display loop) ============
enum { ST_OFFLINE, ST_IDLE, ST_LISTEN, ST_THINK, ST_SPEAK, ST_ASLEEP };
volatile int gState = ST_OFFLINE;
volatile uint32_t gSpeakEnd = 0;     // when the last reply finished playing (mic stays muted briefly after)
volatile int gMood = M_NEUTRAL;
volatile int gTalkTarget = 0;
volatile uint32_t gActivity = 0;
char gText[200] = "";
volatile uint32_t gTextVer = 0;

void setText(const char *s) {
  strncpy(gText, s, sizeof(gText) - 1);
  gText[sizeof(gText) - 1] = 0;
  gTextVer++;
}

int effectiveMood() {
  switch (gState) {
    case ST_LISTEN: return M_CURIOUS;
    case ST_THINK:  return M_THINKING;
    case ST_OFFLINE: return M_NEUTRAL;
    case ST_ASLEEP: return M_SLEEPY;
    default: break;
  }
  if (gState == ST_IDLE) {
    if (millis() - gActivity > 7000) return M_NEUTRAL;
  }
  return gMood;
}

// ============ codecs ============
I2SClass i2s;

bool wr(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool speakerCodecInit() {
  bool ok = true;
  const uint8_t seq[][2] = {
    {0x44,0x08},{0x44,0x08},{0x01,0x30},{0x02,0x00},{0x03,0x10},{0x16,0x24},{0x04,0x10},
    {0x05,0x00},{0x0B,0x00},{0x0C,0x00},{0x10,0x1F},{0x11,0x7F},{0x00,0x80},{0x01,0x3F},
    {0x02,0x00},{0x03,0x10},{0x04,0x10},{0x05,0x00},{0x06,0x03},{0x07,0x00},{0x08,0xFF},
    {0x09,0x0C},{0x0A,0x0C},{0x0D,0x01},{0x0E,0x02},{0x12,0x00},{0x13,0x10},{0x1C,0x6A},
    {0x37,0x08},{0x31,0x00},{0x32,SPK_VOLUME}
  };
  for (auto &s : seq) ok &= wr(ES8311_ADDR, s[0], s[1]);
  return ok;
}

bool micAdcInit() {
  bool ok = true;
  const uint8_t seq[][2] = {
    {0x00,0xFF},{0x00,0x41},{0x01,0x3F},{0x06,0x00},{0x07,0x20},{0x08,0x10},{0x09,0x30},
    {0x0A,0x30},{0x20,0x0A},{0x21,0x2A},{0x22,0x0A},{0x23,0x2A},{0x02,0x81},{0x04,0x01},
    {0x05,0x00},{0x11,0x60},{0x12,0x00},{0x40,0x42},{0x41,0x70},{0x42,0x70},
    {0x43,(uint8_t)(0x10 | MIC_GAIN)},{0x44,(uint8_t)(0x10 | MIC_GAIN)},{0x45,0x00},{0x46,0x00},
    {0x4B,0x00},{0x4C,0xFF},{0x01,0x00},{0x00,0x71},{0x00,0x41}
  };
  for (auto &s : seq) ok &= wr(ES7210_ADDR, s[0], s[1]);
  return ok;
}

// ============ network / audio task ============
WebSocketsClient ws;
bool recording = false;
uint32_t recStart = 0;

// tiny JSON helpers (the server's messages are flat and simple)
bool jsonStr(const char *j, const char *key, char *out, size_t n) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(j, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ') p++;
  if (*p != '"') return false;
  p++;
  size_t o = 0;
  while (*p && *p != '"' && o < n - 1) {
    if (*p == '\\') {
      p++;
      if (*p == 'u') {                       // \uXXXX: keep plain apostrophes/quotes, else '?'
        char hex[5] = {p[1], p[2], p[3], p[4], 0};
        long cp = strtol(hex, nullptr, 16);
        char c = '?';
        if (cp == 0x2018 || cp == 0x2019) c = '\'';
        else if (cp == 0x201C || cp == 0x201D) c = '"';
        else if (cp == 0x2013 || cp == 0x2014) c = '-';
        else if (cp >= 32 && cp < 127) c = (char)cp;
        out[o++] = c;
        p += 5;
        continue;
      }
      out[o++] = (*p == 'n') ? ' ' : *p;
      p++;
      continue;
    }
    out[o++] = *p++;
  }
  out[o] = 0;
  return true;
}

void setPA(bool on) { digitalWrite(PA_EN, on ? HIGH : LOW); }

void playSilence(int ms) {
  static int16_t z[256 * 2];
  int frames = SAMPLE_RATE * ms / 1000;
  while (frames > 0) {
    int n = min(256, frames);
    i2s.write((uint8_t *)z, n * 4);
    frames -= n;
  }
}

void handleBinary(uint8_t *data, size_t len) {
  static int16_t st[256 * 2];
  const int16_t *mono = (const int16_t *)data;
  size_t total = len / 2, done = 0;
  int peak = 0;
  while (done < total) {
    size_t n = min((size_t)256, total - done);
    for (size_t i = 0; i < n; i++) {
      int16_t s = mono[done + i];
      st[2 * i] = st[2 * i + 1] = s;
      int a = abs((int)s);
      if (a > peak) peak = a;
    }
    gTalkTarget = constrain(peak / 700, 0, 16);
    i2s.write((uint8_t *)st, n * 4);
    done += n;
  }
}

void onText(const char *msg) {
  char type[24] = "", val[200] = "";
  if (!jsonStr(msg, "type", type, sizeof(type))) return;
  if (!strcmp(type, "state")) {
    jsonStr(msg, "value", val, sizeof(val));
    if (!strcmp(val, "listening") && gState != ST_SPEAK) gState = ST_LISTEN;
    else if (!strcmp(val, "thinking")) gState = ST_THINK;
    else if (!strcmp(val, "idle") && gState != ST_SPEAK) { gState = ST_IDLE; gActivity = millis(); }
    else if (!strcmp(val, "asleep") && gState != ST_SPEAK) { gState = ST_ASLEEP; gActivity = millis(); }
  } else if (!strcmp(type, "mood")) {
    jsonStr(msg, "value", val, sizeof(val));
    for (int i = 0; i < NMOODS; i++) if (!strcmp(val, MOODS[i].name)) gMood = i;
  } else if (!strcmp(type, "heard")) {
    char t[190];
    if (jsonStr(msg, "text", t, sizeof(t))) {
      snprintf(val, sizeof(val), "you: %s", t);
      setText(val);
    }
    Serial.printf("heard: %s\n", t);
  } else if (!strcmp(type, "reply")) {
    if (jsonStr(msg, "text", val, sizeof(val))) setText(val);
    Serial.printf("reply: %s\n", val);
  } else if (!strcmp(type, "audio_start")) {
    gState = ST_SPEAK;
    setPA(true);
    delay(30);
  } else if (!strcmp(type, "audio_end")) {
    playSilence(200);
    setPA(false);
    gTalkTarget = 0;
    gState = ST_IDLE;
    gActivity = millis();
    gSpeakEnd = millis();
  } else if (!strcmp(type, "error")) {
    if (jsonStr(msg, "text", val, sizeof(val))) setText(val);
    Serial.printf("server error: %s\n", val);
  }
}

void onWs(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("connected to server");
      gState = ST_IDLE;
      gActivity = millis();
      setText("");
      break;
    case WStype_DISCONNECTED:
      if (gState != ST_OFFLINE) Serial.println("disconnected from server");
      gState = ST_OFFLINE;
      recording = false;
      setPA(false);
      break;
    case WStype_TEXT:
      onText((const char *)payload);
      break;
    case WStype_BIN:
      if (gState == ST_SPEAK) handleBinary(payload, length);
      break;
    default: break;
  }
}

void startRecording() {
  uint8_t junk[512];
  for (int i = 0; i < 20; i++) i2s.readBytes((char *)junk, sizeof(junk));   // drop stale audio
  ws.sendTXT("{\"type\":\"start\"}");
  recording = true;
  recStart = millis();
  gState = ST_LISTEN;
  gActivity = millis();
  Serial.println("recording...");
}

void stopRecording() {
  ws.sendTXT("{\"type\":\"end\"}");
  recording = false;
  gState = ST_THINK;
  Serial.println("sent, waiting for reply");
}

void streamChunk() {
  static int16_t stereo[512 * 2];
  static int16_t mono[512];
  size_t bytes = i2s.readBytes((char *)stereo, sizeof(stereo));
  int n = bytes / 4;
  if (n <= 0) return;
  for (int i = 0; i < n; i++) {
    int32_t v = ((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2 * MIC_DIGITAL;
    mono[i] = (int16_t)constrain(v, -32768, 32767);
  }
  ws.sendBIN((uint8_t *)mono, n * 2);
}

void netTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("connecting to WiFi '%s'", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi ok, my IP %s, server %s:%d\n", WiFi.localIP().toString().c_str(), SERVER_HOST, SERVER_PORT);
  ws.begin(SERVER_HOST, SERVER_PORT, "/");
  ws.onEvent(onWs);
  ws.setReconnectInterval(3000);

  for (;;) {
    ws.loop();
    bool pressed = digitalRead(BTN) == LOW;
    static bool wasStreaming = false;
    if (!recording) {
      int st = gState;
      bool canStream = ws.isConnected() && millis() - gSpeakEnd > 500 &&
                       (st == ST_IDLE || st == ST_LISTEN || st == ST_ASLEEP);
      if (pressed && ws.isConnected() && (st == ST_IDLE || st == ST_LISTEN || st == ST_ASLEEP)) {
        wasStreaming = false;
        startRecording();                     // BOOT held = push to talk
      } else if (canStream) {
        if (!wasStreaming) {                  // drop stale audio when the mic restarts
          uint8_t junk[512];
          for (int i = 0; i < 20; i++) i2s.readBytes((char *)junk, sizeof(junk));
          wasStreaming = true;
        }
        streamChunk();                        // always listening: the PC listens for "wake up"
      } else {
        wasStreaming = false;
        delay(2);
      }
    } else {
      if (!pressed || millis() - recStart > MAX_RECORD_MS || !ws.isConnected()) {
        if (ws.isConnected()) stopRecording();
        else recording = false;
      } else {
        streamChunk();
      }
    }
  }
}

// ============ setup / display loop ============
int smTalk = 0;
uint32_t nextBlink = 2000;
uint32_t seenTextVer = 0xFFFFFFFF;
char localText[200] = "";

void setup() {
  Serial.begin(115200);
  pinMode(BTN, INPUT_PULLUP);
  pinMode(PA_EN, OUTPUT);
  setPA(false);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, BL_ACTIVE_HIGH ? LOW : HIGH);

  if (!gfx->begin(40000000)) {
    Serial.println("display init failed (is PSRAM set to OPI PSRAM?)");
    while (true) delay(1000);
  }
  BG = gfx->color565(0, 0, 0);
  EYE = gfx->color565(80, 220, 255);
  MOUTH = gfx->color565(255, 190, 90);
  DIM = gfx->color565(90, 90, 90);
  GRAY = gfx->color565(200, 200, 200);
  WHITE = gfx->color565(255, 255, 255);
  RED = gfx->color565(255, 60, 60);
  gfx->fillScreen(BG);
  gfx->flush();
  digitalWrite(TFT_BL, BL_ACTIVE_HIGH ? HIGH : LOW);

  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S failed to start (need ESP32 core 3.x)");
    while (true) delay(1000);
  }
  delay(100);
  Serial.println(speakerCodecInit() ? "speaker codec ok" : "speaker codec write errors");
  Serial.println(micAdcInit() ? "mic ADC ok" : "mic ADC write errors");

  gActivity = millis();
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 2, nullptr, 0);
}

void loop() {
  uint32_t now = millis();
  int mi = effectiveMood();
  const Mood &m = MOODS[mi];

  float blink = 1.0f;
  float blinkMs = (m.fx == 3) ? 450.0f : 160.0f;
  if (now >= nextBlink) {
    float p = (now - nextBlink) / blinkMs;
    if (p >= 1.0f) nextBlink = now + random(2200, 5000);
    else blink = fabsf(1.0f - 2.0f * p);
  }

  int wanderX = (int)(3 * sinf(now / 1100.0f));
  int breathY = (int)(2 * sinf(now / 900.0f));
  int bounce = (m.fx == 1) ? -(int)(fabsf(sinf(now / 130.0f)) * 10) : 0;

  int target = (gState == ST_SPEAK) ? gTalkTarget : 0;
  smTalk = (smTalk * 6 + target * 4) / 10;
  int talk = (int)(smTalk * m.talkScale);

  gfx->fillScreen(BG);
  int ey = 92 + breathY + bounce;
  Mood g = m;
  g.gx = m.gx + wanderX;
  drawEye(100, ey, m.openL, true, g, blink);
  drawEye(220, ey, m.openR, false, g, blink);
  drawBrow(100, ey, m, true);
  drawBrow(220, ey, m, false);
  drawMouth(m, talk, bounce, now);

  if (m.fx == 2) {
    int dots = (now / 450) % 4;
    if (dots >= 1) gfx->fillCircle(262, 40, 4, WHITE);
    if (dots >= 2) gfx->fillCircle(280, 30, 6, WHITE);
    if (dots >= 3) gfx->fillCircle(302, 18, 8, WHITE);
  }
  if (m.fx == 3) {
    for (int k = 0; k < 3; k++) {
      float p = fmodf((now % 3600) / 3600.0f + k / 3.0f, 1.0f);
      gfx->setTextColor(DIM);
      gfx->setTextSize(1 + (int)(p * 3));
      gfx->setCursor(236 + (int)(p * 44), 70 - (int)(p * 56));
      gfx->print("z");
    }
  }

  // recording dot
  if (gState == ST_LISTEN) gfx->fillCircle(16, 16, 7, RED);

  // bottom line: status or text
  if (seenTextVer != gTextVer) {
    memcpy(localText, gText, sizeof(localText));
    seenTextVer = gTextVer;
  }
  switch (gState) {
    case ST_OFFLINE: drawSubtitle(WiFi.status() == WL_CONNECTED ? "connecting to server..." : "connecting to WiFi...", DIM); break;
    case ST_LISTEN:  drawSubtitle("listening...", GRAY); break;
    case ST_THINK:   drawSubtitle("thinking...", GRAY); break;
    case ST_ASLEEP:  drawSubtitle("zzz... say \"wake up\" (or hold BOOT)", DIM); break;
    default:
      if (localText[0]) drawSubtitle(localText, GRAY);
      else drawSubtitle("hold BOOT to talk", DIM);
  }

  gfx->flush();
  delay(10);
}

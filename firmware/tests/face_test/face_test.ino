/*
  Step 2: the face (version 2: eyebrows, better sleepy / thinking / excited).
  Draws an animated robot face on the TFT. It cycles through the same nine moods the
  server uses (neutral, happy, excited, curious, thinking, surprised, confused, sad,
  sleepy), blinks, looks around, and talks (mouth moves) during the second half of
  each mood.

  New in this version:
    - eyebrows on most moods (raised, lowered, worried, uneven)
    - excited:  bouncing, wide sparkling eyes, big open smile
    - thinking: one brow raised, eyes up and to the side, "..." thought bubble
    - sleepy:   heavy flat eyelids, slow yawning mouth, floating Z's

  Needs the library "GFX Library for Arduino" by moononournation
  (Arduino IDE: Library Manager, search "GFX Library for Arduino", Install).

  Arduino IDE settings (same as the I2C scan):
    Board: ESP32S3 Dev Module   PSRAM: OPI PSRAM   Flash Size: 16MB
    USB CDC On Boot: Disabled   Port: the UART USB-C port

  If the screen is blank:        change BL_ACTIVE_HIGH to 0
  If the face is upside down or
  mirrored:                      change TFT_ROTATION to 3 (or try 0 or 2)
  If colors look inverted:       change TFT_INVERT to false
  If it says "display init failed" in the Serial Monitor: PSRAM is not set to OPI PSRAM.
*/
#include <Arduino_GFX_Library.h>

// ---- pins from the kit's board code ----
#define TFT_MOSI 40
#define TFT_SCK  41
#define TFT_DC   39
#define TFT_CS   47
#define TFT_RST  4
#define TFT_BL   42
#define PA_EN    48        // speaker amplifier enable: held LOW so the speaker stays quiet

#define BL_ACTIVE_HIGH 1
#define TFT_ROTATION   1
#define TFT_INVERT     true

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *panel = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, TFT_INVERT, 240, 320);
Arduino_Canvas *gfx = new Arduino_Canvas(320, 240, panel);   // draw off-screen, then flush: no flicker

uint16_t BG, EYE, MOUTH, DIM, WHITE;

struct Mood {
  const char *name;
  float openL, openR;   // eye height, 1.0 = normal
  int8_t tilt;          // 1 = drooping outer lids (sad)
  bool happyEyes;       // ^ ^ shaped eyes
  int8_t smile;         // mouth curve in px: positive smile, negative frown
  int8_t mouthOpen;     // open-mouth height in px (0 = closed line)
  int8_t gx, gy;        // where the eyes look
  int8_t mouthW;        // mouth width in px
  int8_t mouthX;        // mouth sideways shift in px
  int8_t lid;           // % of the eye covered by a flat upper eyelid (sleepy)
  bool brows;           // draw eyebrows?
  int8_t browL, browR;  // vertical offset in px, negative = raised
  int8_t browTilt;      // + inner ends raised (worried), - inner ends lowered (stern)
  uint8_t fx;           // 0 none, 1 excited, 2 thinking, 3 sleepy
  float talkScale;      // how much the mouth flaps while talking
};

const Mood MOODS[] = {
  //  name        openL  openR tilt happy smile mOpen gx  gy  mW  mX lid brows bL   bR  bTilt fx talk
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

const int EYE_W = 64, EYE_H0 = 76;

void thickLine(int x0, int y0, int x1, int y1, int r, uint16_t c) {
  const int N = 24;
  for (int i = 0; i <= N; i++) {
    int x = x0 + (x1 - x0) * i / N;
    int y = y0 + (y1 - y0) * i / N;
    gfx->fillCircle(x, y, r, c);
  }
}

void drawBrow(int cx, int cy, const Mood &m, bool left) {
  if (!m.brows) return;
  float open = left ? m.openL : m.openR;
  int off = left ? m.browL : m.browR;
  int top = m.happyEyes ? cy - 22 : cy - (int)(EYE_H0 * open / 2);
  int by = top - 14 + off;
  int innerX = cx + (left ? 24 : -24);      // the end nearest the nose
  int outerX = cx + (left ? -24 : 24);
  int yi = by - m.browTilt;                 // positive tilt raises the inner end
  int yo = by + m.browTilt;
  thickLine(innerX, yi, outerX, yo, 4, EYE);
}

void drawEye(int cx, int cy, float open, bool left, const Mood &m, float blink) {
  int h = (int)(EYE_H0 * open * blink);
  if (h < 6) h = 6;
  cx += m.gx;                                // where the eyes are looking
  cy += m.gy;
  if (m.happyEyes && blink > 0.5f) {
    // crescent: a filled circle with a background circle punched out below it
    gfx->fillCircle(cx, cy + 14, 32, EYE);
    gfx->fillCircle(cx, cy + 32, 32, BG);
    return;
  }
  gfx->fillRoundRect(cx - EYE_W / 2, cy - h / 2, EYE_W, h, 22, EYE);
  if (m.lid) {                               // heavy flat eyelid (sleepy)
    int cover = h * m.lid / 100;
    gfx->fillRect(cx - EYE_W / 2 - 2, cy - h / 2 - 2, EYE_W + 4, cover + 2, BG);
  }
  if (m.tilt) {                              // drooping outer corner (sad)
    int dir = left ? -1 : 1;
    int outerX = cx + dir * (EYE_W / 2 + 2);
    int innerX = cx - dir * (EYE_W / 2);
    int topY = cy - h / 2 - 2;
    gfx->fillTriangle(innerX, topY, outerX, topY, outerX, topY + 28, BG);
  }
  if (m.fx == 1 && h > 30) {                 // sparkle (excited)
    gfx->fillCircle(cx + 12, cy - h / 4, 7, WHITE);
    gfx->fillCircle(cx - 8, cy + h / 5, 3, WHITE);
  }
}

void drawMouth(const Mood &m, int talk, int cyShift, uint32_t now) {
  const int cx = 160 + m.mouthX, cy = 188 + cyShift;
  int w = m.mouthW;
  int open = m.mouthOpen + talk;
  if (m.fx == 3) open = m.mouthOpen + (int)(5 * sinf(now / 700.0f));   // slow yawn
  if (open > 3) {
    int r = (open / 2 < 14) ? open / 2 : 14;
    gfx->fillRoundRect(cx - w / 2, cy - open / 2, w, open, r, MOUTH);
  } else {
    for (int x = -w / 2; x <= w / 2; x += 2) {
      float t = (float)x / (w / 2.0f);
      int y = cy + (int)(m.smile * (1.0f - t * t));
      gfx->fillCircle(cx + x, y, 3, MOUTH);
    }
  }
}

int moodIdx = 0;
uint32_t moodStart = 0, nextBlink = 2000;

void setup() {
  Serial.begin(115200);
  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, LOW);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, BL_ACTIVE_HIGH ? LOW : HIGH);   // off until the first frame is ready

  if (!gfx->begin(40000000)) {
    Serial.println("display init failed (is PSRAM set to OPI PSRAM?)");
    while (true) delay(1000);
  }
  BG = gfx->color565(0, 0, 0);
  EYE = gfx->color565(80, 220, 255);
  MOUTH = gfx->color565(255, 190, 90);
  DIM = gfx->color565(90, 90, 90);
  WHITE = gfx->color565(255, 255, 255);
  gfx->fillScreen(BG);
  gfx->flush();
  digitalWrite(TFT_BL, BL_ACTIVE_HIGH ? HIGH : LOW);
  moodStart = millis();
  Serial.printf("mood: %s\n", MOODS[0].name);
}

void loop() {
  uint32_t now = millis();
  uint32_t elapsed = now - moodStart;

  if (elapsed > 3500) {
    moodIdx = (moodIdx + 1) % NMOODS;
    moodStart = now;
    elapsed = 0;
    Serial.printf("mood: %s\n", MOODS[moodIdx].name);
  }
  const Mood &m = MOODS[moodIdx];

  // blink (sleepy blinks slowly)
  float blink = 1.0f;
  float blinkMs = (m.fx == 3) ? 450.0f : 160.0f;
  if (now >= nextBlink) {
    float p = (now - nextBlink) / blinkMs;
    if (p >= 1.0f) {
      nextBlink = now + random(2200, 5000);
    } else {
      blink = fabsf(1.0f - 2.0f * p);
    }
  }

  // idle motion: tiny gaze wander and breathing
  int wanderX = (int)(3 * sinf(now / 1100.0f));
  int breathY = (int)(2 * sinf(now / 900.0f));
  int bounce = (m.fx == 1) ? -(int)(fabsf(sinf(now / 130.0f)) * 10) : 0;   // excited bounce

  // talking: mouth flaps during the second part of each mood
  int talk = 0;
  if (elapsed > 1500) talk = (int)((0.5f + 0.5f * sinf(now / 70.0f)) * 16 * m.talkScale);

  gfx->fillScreen(BG);
  int ey = 92 + breathY + bounce;
  Mood g = m;
  g.gx = m.gx + wanderX;
  drawEye(100, ey, m.openL, true, g, blink);
  drawEye(220, ey, m.openR, false, g, blink);
  drawBrow(100, ey, m, true);
  drawBrow(220, ey, m, false);
  drawMouth(m, talk, bounce, now);

  // thinking: "..." bubble builds up in the top right
  if (m.fx == 2) {
    int dots = (now / 450) % 4;            // 0..3 dots
    if (dots >= 1) gfx->fillCircle(262, 40, 4, WHITE);
    if (dots >= 2) gfx->fillCircle(280, 30, 6, WHITE);
    if (dots >= 3) gfx->fillCircle(302, 18, 8, WHITE);
  }
  // sleepy: Z's float up and to the right, growing as they rise
  if (m.fx == 3) {
    for (int k = 0; k < 3; k++) {
      float p = fmodf((now % 3600) / 3600.0f + k / 3.0f, 1.0f);
      gfx->setTextColor(DIM);
      gfx->setTextSize(1 + (int)(p * 3));
      gfx->setCursor(236 + (int)(p * 44), 70 - (int)(p * 56));
      gfx->print("z");
    }
  }

  gfx->setTextSize(2);
  gfx->setTextColor(DIM);
  gfx->setCursor(8, 218);
  gfx->print(m.name);
  gfx->flush();
  delay(10);
}

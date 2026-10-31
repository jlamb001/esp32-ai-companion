/*
  Step 4: microphone test for the LAFVIN ESP32-S3 board.
  Loop: short beep -> records 4 seconds from the two microphones -> plays it back.
  Also prints a loudness bar to the Serial Monitor (115200) while recording, so you can
  see whether the mics hear anything even if playback is quiet.

  Chips: ES7210 mic ADC at 0x41, ES8311 speaker codec at 0x18 (init copied from speaker_test).
  Same pins and board settings as the speaker test. Needs ESP32 core 3.x.

  Tuning:  MIC_GAIN  (0..14, each step = 3 dB; 10 = 30 dB) if recordings are too quiet or distorted
           PLAY_GAIN (digital multiplier on playback) and VOLUME (speaker codec volume)
  The ES7210 setup is written from the chip's register sequence and has not been run on your
  board. If the level bar stays flat, tell me what the Serial Monitor shows.
*/
#include <Wire.h>
#include <ESP_I2S.h>

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

#define SAMPLE_RATE 16000
#define VOLUME      170
#define MIC_GAIN    10      // 0..14
#define PLAY_GAIN   2       // digital boost on playback
#define REC_SECONDS 4

I2SClass i2s;
int16_t *rec = nullptr;                    // stereo frames: L, R, L, R ...
const size_t REC_FRAMES = (size_t)SAMPLE_RATE * REC_SECONDS;

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
    {0x37,0x08},{0x31,0x00},{0x32,VOLUME}
  };
  for (auto &s : seq) ok &= wr(ES8311_ADDR, s[0], s[1]);
  return ok;
}

bool micAdcInit() {
  bool ok = true;
  const uint8_t seq[][2] = {
    {0x00,0xFF},            // reset
    {0x00,0x41},
    {0x01,0x3F},            // all clocks off while configuring
    {0x06,0x00},
    {0x07,0x20},            // OSR
    {0x08,0x10},            // slave mode
    {0x09,0x30},
    {0x0A,0x30},
    {0x20,0x0A},
    {0x21,0x2A},
    {0x22,0x0A},
    {0x23,0x2A},
    {0x02,0x81},            // MCLK = 256 x LRCK
    {0x04,0x01},            // LRCK divider high
    {0x05,0x00},            // LRCK divider low
    {0x11,0x60},            // 16-bit I2S
    {0x12,0x00},            // no TDM
    {0x40,0x42},            // analog power
    {0x41,0x70},
    {0x42,0x70},
    {0x43,(uint8_t)(0x10 | MIC_GAIN)},   // mic 1 gain on
    {0x44,(uint8_t)(0x10 | MIC_GAIN)},   // mic 2 gain on
    {0x45,0x00},
    {0x46,0x00},
    {0x4B,0x00},            // power mic 1+2 bias
    {0x4C,0xFF},            // mic 3+4 off
    {0x01,0x00},            // turn ALL ADC clocks on (this was the bug: they stayed gated off)
    {0x00,0x71},
    {0x00,0x41}             // start
  };
  for (auto &s : seq) ok &= wr(ES7210_ADDR, s[0], s[1]);
  return ok;
}

void playTone(float freq, int ms) {
  static int16_t buf[256 * 2];
  int frames = SAMPLE_RATE * ms / 1000, done = 0;
  float phase = 0, step = 2.0f * PI * freq / SAMPLE_RATE;
  while (done < frames) {
    int n = min(256, frames - done);
    for (int i = 0; i < n; i++) {
      int pos = done + i, fade = SAMPLE_RATE / 100;
      float env = 1.0f;
      if (pos < fade) env = (float)pos / fade;
      if (frames - pos < fade) env = (float)(frames - pos) / fade;
      int16_t s = (int16_t)(8000 * env * sinf(phase));
      phase += step;
      buf[2 * i] = buf[2 * i + 1] = s;
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nmic test");

  rec = (int16_t *)ps_malloc(REC_FRAMES * 4);
  if (!rec) {
    Serial.println("not enough PSRAM (set PSRAM: OPI PSRAM)");
    while (true) delay(1000);
  }

  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, LOW);
  Wire.begin(I2C_SDA, I2C_SCL, 100000);

  i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S failed to start (need ESP32 core 3.x)");
    while (true) delay(1000);
  }
  delay(100);   // clocks are now running; the chips need them before configuration

  Serial.println(speakerCodecInit() ? "speaker codec ok" : "speaker codec write errors");
  Serial.println(micAdcInit() ? "mic ADC ok" : "mic ADC write errors (check address 0x41)");
  digitalWrite(PA_EN, HIGH);
}

void loop() {
  Serial.println("\nget ready...");
  delay(800);
  playTone(880, 150);                   // beep = start talking
  delay(100);

  // throw away anything buffered before the beep
  uint8_t junk[512];
  for (int i = 0; i < 20; i++) i2s.readBytes((char *)junk, sizeof(junk));

  Serial.println("RECORDING - talk now");
  size_t got = 0;
  int16_t peak = 0;
  uint32_t lastPrint = millis();
  while (got < REC_FRAMES) {
    size_t want = min((size_t)256, REC_FRAMES - got);
    size_t bytes = i2s.readBytes((char *)(rec + got * 2), want * 4);
    size_t frames = bytes / 4;
    for (size_t i = 0; i < frames * 2; i++) {
      int16_t v = abs(rec[got * 2 + i]);
      if (v > peak) peak = v;
    }
    got += frames;
    if (millis() - lastPrint > 200) {      // level bar
      lastPrint = millis();
      int bars = peak * 40 / 32767;
      char line[48];
      for (int i = 0; i < 40; i++) line[i] = i < bars ? '#' : '.';
      line[40] = 0;
      Serial.printf("%s %5d\n", line, peak);
      peak = 0;
    }
  }

  Serial.println("playing back");
  playTone(660, 120);
  for (size_t i = 0; i < REC_FRAMES * 2; i++) {          // digital boost, clipped
    int32_t v = (int32_t)rec[i] * PLAY_GAIN;
    rec[i] = (int16_t)constrain(v, -32768, 32767);
  }
  i2s.write((uint8_t *)rec, REC_FRAMES * 4);
  delay(1500);
}

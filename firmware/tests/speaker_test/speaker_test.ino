/*
  Step 3: speaker test for the LAFVIN ESP32-S3 board.
  Starts the ES8311 audio codec over I2C, streams audio over I2S, enables the amplifier
  and plays a short three-note chime every 3 seconds. Open the Serial Monitor (115200)
  to see what it's doing.

  Pins (from the kit's board code):
    I2C  SDA=1  SCL=2          ES8311 at 0x18
    I2S  MCLK=38 BCLK=14 WS=13 DOUT(to speaker)=45 DIN(from mics)=12
    PA_EN (amplifier on) = GPIO48

  Needs the Arduino ESP32 core 3.x (Boards Manager: "esp32 by Espressif", version 3.0 or newer),
  because it uses the built-in ESP_I2S library. Same board settings as before:
    ESP32S3 Dev Module, PSRAM: OPI PSRAM, Flash 16MB, USB CDC On Boot: Disabled.

  If it is silent:  try VOLUME higher (max 255), or flip PA_ACTIVE_HIGH to 0.
  If it is too loud: lower VOLUME. Start low.
  I wrote the codec setup from the ES8311 chip's register sequence; I haven't run it on
  your board, so tell me what the Serial Monitor prints if it doesn't make a sound.
*/
#include <Wire.h>
#include <ESP_I2S.h>

#define I2C_SDA 1
#define I2C_SCL 2
#define ES8311_ADDR 0x18

#define I2S_MCLK 38
#define I2S_BCLK 14
#define I2S_WS   13
#define I2S_DOUT 45
#define I2S_DIN  12

#define PA_EN 48
#define PA_ACTIVE_HIGH 1

#define SAMPLE_RATE 16000
#define VOLUME 170          // codec DAC volume 0..255 (191 = 0 dB). Start moderate.
#define AMPLITUDE 9000      // digital loudness of the tone, max 32767

I2SClass i2s;

bool codecWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool codecInit() {
  bool ok = true;
  // reset and basic power-up
  ok &= codecWrite(0x44, 0x08);
  ok &= codecWrite(0x44, 0x08);
  ok &= codecWrite(0x01, 0x30);
  ok &= codecWrite(0x02, 0x00);
  ok &= codecWrite(0x03, 0x10);
  ok &= codecWrite(0x16, 0x24);
  ok &= codecWrite(0x04, 0x10);
  ok &= codecWrite(0x05, 0x00);
  ok &= codecWrite(0x0B, 0x00);
  ok &= codecWrite(0x0C, 0x00);
  ok &= codecWrite(0x10, 0x1F);
  ok &= codecWrite(0x11, 0x7F);
  ok &= codecWrite(0x00, 0x80);   // power on, chip is the I2S slave
  // clocks: MCLK from the MCLK pin, 256 x sample rate (16 kHz -> 4.096 MHz)
  ok &= codecWrite(0x01, 0x3F);
  ok &= codecWrite(0x02, 0x00);   // pre-divider 1, multiplier 1
  ok &= codecWrite(0x03, 0x10);
  ok &= codecWrite(0x04, 0x10);
  ok &= codecWrite(0x05, 0x00);
  ok &= codecWrite(0x06, 0x03);   // BCLK divider
  ok &= codecWrite(0x07, 0x00);   // LRCK divider high
  ok &= codecWrite(0x08, 0xFF);   // LRCK divider low
  // 16-bit I2S format
  ok &= codecWrite(0x09, 0x0C);
  ok &= codecWrite(0x0A, 0x0C);
  // analog / DAC path
  ok &= codecWrite(0x0D, 0x01);   // power up analog circuitry
  ok &= codecWrite(0x0E, 0x02);
  ok &= codecWrite(0x12, 0x00);   // power up DAC
  ok &= codecWrite(0x13, 0x10);   // enable output to the amplifier
  ok &= codecWrite(0x1C, 0x6A);
  ok &= codecWrite(0x37, 0x08);
  ok &= codecWrite(0x31, 0x00);   // un-mute DAC
  ok &= codecWrite(0x32, VOLUME); // DAC volume
  return ok;
}

void playTone(float freq, int ms) {
  const int frames = SAMPLE_RATE * ms / 1000;
  static int16_t buf[512 * 2];
  int done = 0;
  float phase = 0;
  const float step = 2.0f * PI * freq / SAMPLE_RATE;
  while (done < frames) {
    int n = min(512, frames - done);
    for (int i = 0; i < n; i++) {
      // short fade in/out so there are no clicks
      int pos = done + i;
      float env = 1.0f;
      int fade = SAMPLE_RATE / 100;   // 10 ms
      if (pos < fade) env = (float)pos / fade;
      if (frames - pos < fade) env = (float)(frames - pos) / fade;
      int16_t s = (int16_t)(AMPLITUDE * env * sinf(phase));
      phase += step;
      buf[2 * i] = s;       // left
      buf[2 * i + 1] = s;   // right
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

void silence(int ms) {
  static int16_t z[512 * 2];
  int frames = SAMPLE_RATE * ms / 1000;
  while (frames > 0) {
    int n = min(512, frames);
    i2s.write((uint8_t *)z, n * 4);
    frames -= n;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nspeaker test");

  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, PA_ACTIVE_HIGH ? LOW : HIGH);   // amp off while setting up

  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("ES8311 not found at 0x18 (run the I2C scan again)");
    while (true) delay(1000);
  }

  // the codec needs MCLK running before its registers are set, so start I2S first
  i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S failed to start (need ESP32 core 3.x)");
    while (true) delay(1000);
  }
  silence(100);

  Serial.println(codecInit() ? "codec configured" : "codec write errors (check wiring / address)");
  silence(100);
  digitalWrite(PA_EN, PA_ACTIVE_HIGH ? HIGH : LOW);   // amp on
  Serial.println("amplifier on, playing chime every 3 seconds");
}

void loop() {
  Serial.println("chime");
  playTone(523.25f, 180);   // C5
  silence(40);
  playTone(659.25f, 180);   // E5
  silence(40);
  playTone(783.99f, 320);   // G5
  silence(3000);
}

/*
  Step 1 hardware check for the LAFVIN ESP32-S3 board.
  Scans the I2C bus used by the audio chips and prints what it finds.

  Pins (from the kit's config.h):  SDA = GPIO1, SCL = GPIO2
  Expected devices (7-bit addresses):
    0x18  ES8311 audio codec  (speaker output)
    0x41  ES7210 audio ADC    (microphones; config.h lists it as 0x82 in 8-bit form)

  Arduino IDE settings:
    Board: ESP32S3 Dev Module
    PSRAM: OPI PSRAM        Flash Size: 16MB
    USB CDC On Boot: Disabled   (use the UART USB-C port)
    Serial Monitor: 115200 baud
*/
#include <Wire.h>

#define I2C_SDA 1
#define I2C_SCL 2

const char* nameFor(uint8_t a) {
  if (a == 0x18) return "ES8311 audio codec (expected)";
  if (a >= 0x40 && a <= 0x43) return "ES7210 microphone ADC (expected at 0x41)";
  return "unknown";
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  Serial.println("\nI2C scan on SDA=GPIO1, SCL=GPIO2");
}

void loop() {
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X  %s\n", addr, nameFor(addr));
      found++;
    }
  }
  if (found == 0) Serial.println("  nothing found: check that the audio board is seated in its socket");
  Serial.printf("done, %d device(s)\n\n", found);
  delay(5000);
}

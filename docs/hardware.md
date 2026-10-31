# Hardware notes

Board: **LAFVIN AIoT Starter Kit, ESP32-S3** (ESP32-S3 N16R8: 16 MB flash, 8 MB PSRAM) with its
2.0" 240x320 ST7789 TFT and audio module. Pins below come from the kit's own `config.h` /
board source and were checked on real hardware with `firmware/tests/i2c_scan`.

| Function | Pin / address |
|---|---|
| TFT MOSI / SCLK | GPIO 40 / 41 |
| TFT DC / CS / RST | GPIO 39 / 47 / 4 |
| TFT backlight | GPIO 42 (polarity: see `BL_ACTIVE_HIGH`) |
| TFT | SPI mode 0, 40 MHz, colors inverted, rotation 1 (320x240) |
| I2S MCLK / BCLK / WS | GPIO 38 / 14 / 13 |
| I2S data in (mics) / out (speaker) | GPIO 12 / 45 |
| Audio I2C SDA / SCL | GPIO 1 / 2 |
| ES8311 speaker codec | I2C 0x18 |
| ES7210 mic ADC (two mics) | I2C 0x41 (0x82 in 8-bit form) |
| Speaker amplifier enable (PA_EN) | GPIO 48 (high = on) |
| BOOT button (push-to-talk) | GPIO 0 |

Pins used by the kit's optional demo parts (RGB LED, DHT11, servo, relay, ...) can overlap with
the above if those parts are plugged in. This project does not use them.

## Arduino IDE settings
- Board: **ESP32S3 Dev Module** (ESP32 core **3.x**; the audio code uses the built-in `ESP_I2S`)
- PSRAM: **OPI PSRAM**, Flash size: **16MB**
- USB CDC On Boot: **Disabled**, upload through the **UART** USB-C port (CP210x driver on Windows)
- To force download mode: hold BOOT, tap RESET.

## Libraries
- *GFX Library for Arduino* (moononournation)
- *WebSockets* (Markus Sattler)

## Bring-up order
Run the sketches in `firmware/tests/` first: `i2c_scan` (are both audio chips found?),
`face_test`, `speaker_test`, `mic_test`. Then the main `firmware/companion` sketch.

## Other boards
Other ESP32-S3 boards with an ES8311 + ES7210 audio path and an ST7789 display should work after
editing the pin `#define`s at the top of each sketch. Untested.

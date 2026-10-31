# ESP32-S3 AI Companion

A little desk companion with a face, a voice and ears. It runs on an ESP32-S3 board with a
screen, speaker and microphones, and talks to **local** AI models on your own PC. No cloud
service, no API key, no subscription.

Say **"wake up"** and chat. The board shows an animated face whose expression matches what the
AI is feeling, and its mouth moves while it speaks.

```
 ESP32-S3 board                         your PC (same WiFi)
 ┌────────────────────┐   mic audio    ┌─────────────────────────────────┐
 │ mics  ────────────────────────────▶ │ wake word + voice detection     │
 │                    │   WebSocket    │ Whisper   speech -> text        │
 │ face (TFT)         │                │ Ollama    the AI that answers   │
 │ speaker ◀────────────────────────── │ Piper     text -> speech        │
 └────────────────────┘  voice + mood  └─────────────────────────────────┘
```

## Features
- Animated face with nine moods (neutral, happy, excited, curious, thinking, surprised,
  confused, sad, sleepy), eyebrows, blinking, breathing, and a mouth that follows the voice
- Wake word ("wake up"), conversation without repeating it, falls asleep after ~25 s of quiet,
  or on "go to sleep"
- Hold the BOOT button for push-to-talk instead
- Everything local: faster-whisper, Ollama, Piper
- Conversations printed in the terminal and saved to daily text files
- The AI picks its own expression with a mood tag in each reply

## What you need
- **LAFVIN AIoT Starter Kit ESP32-S3** (ESP32-S3 N16R8, 2.0" ST7789 TFT, ES8311 speaker codec,
  ES7210 two-mic ADC). Pin map and notes: [docs/hardware.md](docs/hardware.md)
- A PC (Windows, macOS or Linux) on the same 2.4 GHz WiFi, with [Ollama](https://ollama.com),
  Python 3.10+ and roughly 4 GB of free RAM. A GPU helps but is not required.
- Arduino IDE with the **ESP32 core 3.x**, plus the libraries *GFX Library for Arduino* and
  *WebSockets* (Markus Sattler)

## Quick start

```bash
git clone https://github.com/jlamb001/esp32-ai-companion.git
cd esp32-ai-companion
```

### 1. Server (PC)
```bash
cd server
pip install -r requirements.txt
ollama pull llama3.2:3b                       # or any model; set MODEL in companion_server.py
python -m piper.download_voices en_US-lessac-medium    # saves the voice in the current folder
```
If you use a different voice, set `PIPER_VOICE` (and `PIPER_RATE` if it isn't 22050 Hz) at the
top of `companion_server.py`.

Try the AI without any hardware:
```bash
python companion_server.py --text
```
Then start the real server:
```bash
python companion_server.py
```
Allow Python through the firewall on private networks if Windows asks.
Find your PC's IP (`ipconfig` on Windows, `ip a` on Linux) for the next step.

### 2. Board
1. Copy `firmware/companion/secrets.h.example` to `firmware/companion/secrets.h` and fill in your
   WiFi name, password and the PC's IP. (`secrets.h` is git-ignored.)
2. Open `firmware/companion/companion.ino` in Arduino IDE, set the board options from
   [docs/hardware.md](docs/hardware.md), and upload.
3. Open the Serial Monitor at 115200 baud. The screen shows "connecting..." until it reaches
   the server, then goes sleepy. Say **"wake up"**.

First time on a new board? Run the sketches in `firmware/tests/` in order:
`i2c_scan` -> `face_test` -> `speaker_test` -> `mic_test`.

## Using it
- "Wake up" wakes it. "Wake up, what time is it" works in one breath.
- It keeps listening for about 25 s after each exchange; "go to sleep" or "good night" sleeps it.
- Hold BOOT to push-to-talk at any time.
- Conversations are saved in `server/conversations/YYYY-MM-DD.txt`. It does not remember past
  conversations after the server restarts.

## Tuning
All near the top of `server/companion_server.py` unless noted.

| Problem | Try |
|---|---|
| Never wakes | Watch the terminal: `(asleep, heard: ...)` shows what Whisper heard. Add variants to `WAKE_PHRASES`. |
| Wakes on noise / TV | Raise `VAD_MIN_RMS` |
| Cuts you off mid-sentence | Raise `VAD_END_FRAMES` (25 = 0.8 s) |
| Mic too quiet / mishears you | `MIC_GAIN`, `MIC_DIGITAL` in `companion.ino` |
| Speaker too quiet / loud | `SPK_VOLUME` in `companion.ino` |
| Slow replies | A smaller `MODEL`, `STT_MODEL = "tiny.en"`, or a "medium" Piper voice |
| Different personality | Edit `SYSTEM_PROMPT` |
| Blank screen | `BL_ACTIVE_HIGH` 1 -> 0 |
| Face upside down / mirrored | `TFT_ROTATION` (try 3, 0 or 2) |
| Wrong colors | `TFT_INVERT` |

More: [docs/protocol.md](docs/protocol.md) describes the WebSocket messages if you want to build
a different client.

## Privacy
Everything runs on your own network and computer. Note that the board streams microphone audio to
your PC whenever it is on and not speaking, and the PC transcribes speech in order to listen for
the wake word. The saved conversation files are plain text.

## Status
Developed and run on the LAFVIN ESP32-S3 AIoT Starter Kit. Other hardware is untested.
Pull requests and issues welcome.

## Credits
[Arduino_GFX](https://github.com/moononournation/Arduino_GFX),
[arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets),
[faster-whisper](https://github.com/SYSTRAN/faster-whisper),
[Piper](https://github.com/OHF-Voice/piper1-gpl) (check the license of the voice you download),
[Ollama](https://ollama.com).

## License
MIT, see [LICENSE](LICENSE).

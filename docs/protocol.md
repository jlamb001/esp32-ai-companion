# Board <-> server protocol

WebSocket on port 8765. Audio is PCM16 mono, 16 kHz, little-endian, in binary frames.

**Board -> server**
- binary frames: microphone audio, streamed continuously while the board is not speaking
- `{"type":"start"}` ... binary ... `{"type":"end"}`: push-to-talk (BOOT button), bypasses the wake word

**Server -> board**
- `{"type":"state","value":"asleep|idle|listening|thinking"}`
- `{"type":"heard","text":"..."}`   what was understood
- `{"type":"mood","value":"neutral|happy|excited|curious|thinking|surprised|confused|sad|sleepy"}`
- `{"type":"reply","text":"..."}`   text shown on screen
- `{"type":"audio_start","rate":16000}`, binary PCM16 frames, `{"type":"audio_end"}`
- `{"type":"error","text":"..."}`

The language model is told to start every reply with one mood tag such as `[happy]`; the server
strips it and sends it as the `mood` message.

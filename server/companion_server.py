#!/usr/bin/env python3
"""
Companion server: talks to a local Ollama model (free, runs on your own computer).

Two modes:
  python companion_server.py --text   Terminal chat, no audio. Use this first to test
                                      Ollama and the mood tags.
  python companion_server.py          WebSocket server for the ESP32-S3 board (port 8765).

Install:
  pip install requests websockets numpy faster-whisper piper-tts
  ollama pull llama3.2:3b             (or any model; check with `ollama list`)
  Download a Piper voice (.onnx + .onnx.json), e.g. en_US-lessac-medium, and set PIPER_VOICE.

Board protocol (WebSocket, JSON text frames + binary audio frames):
  board -> server : {"type":"start"}, then binary PCM16 mono 16 kHz frames, then {"type":"end"}
  server -> board : {"type":"state","value":"listening|thinking|idle"}
                    {"type":"heard","text":"..."}        what it understood
                    {"type":"mood","value":"happy"}      drives the face
                    {"type":"reply","text":"..."}        text for the screen
                    {"type":"audio_start","rate":22050}, binary PCM16 mono frames, {"type":"audio_end"}
                    {"type":"error","text":"..."}
"""
import argparse
import asyncio
import json
import re
import collections
import subprocess
import time

import requests

# ---------------- CONFIG ----------------
OLLAMA_URL = "http://localhost:11434"
MODEL = "llama3.2:3b"                 # small = fast replies. Change to a model you have.
HOST, PORT = "0.0.0.0", 8765          # board connects to ws://<this computer's LAN IP>:8765

STT_MODEL = "base.en"                 # faster-whisper model: tiny.en / base.en / small.en
PIPER_BIN = "piper"
PIPER_VOICE = "en_US-ryan-high.onnx"
PIPER_RATE = 22050                    # sample rate of the Piper voice (medium voices: 22050)
LOG_DIR = "conversations"             # saved next to this script, one .txt file per day
OUT_RATE = 16000                     # rate sent to the board (the board plays everything at 16 kHz)

# ---- wake word and listening ----
WAKE_PHRASES = ("wake up", "wakeup")
SLEEP_PHRASES = ("go to sleep", "go back to sleep", "good night", "goodnight")
AWAKE_SECONDS = 25                    # stays awake this long after the last thing said
MAX_UTTERANCE_S = 15
POST_REPLY_IGNORE_S = 0.7             # ignore the room right after speaking (echo)
VAD_FRAME = 512                       # samples per analysis frame (32 ms at 16 kHz)
VAD_MIN_RMS = 300                     # louder than this counts as speech (raise if it hears noise)
VAD_RATIO = 3.0                       # ...and this many times louder than the room's background
VAD_END_FRAMES = 25                   # this many quiet frames (~0.8 s) end a sentence
VAD_MIN_VOICED_FRAMES = 6             # ignore blips shorter than ~0.2 s
VAD_PREROLL_FRAMES = 8                # keep a little audio from before speech started
JUNK = {"you", "thank you", "thanks", "thanks for watching", "bye", "the", "uh", "um",
        "hmm", "mm", "oh", "ah", "huh"}   # things Whisper imagines in noise

MOODS = ["neutral", "happy", "excited", "curious", "thinking",
         "surprised", "confused", "sad", "sleepy"]
MAX_HISTORY = 12                      # messages remembered per conversation
MIN_RECORD_S, MAX_RECORD_S = 0.3, 30
CHUNK = 4096                          # bytes per audio frame sent to the board

SYSTEM_PROMPT = (
    "You are a small friendly desk companion with a face on a little screen. "
    "Your replies are spoken aloud, so answer in one to three short, natural sentences. "
    "No lists, no markdown, no emojis. "
    "Begin every reply with exactly one mood tag in square brackets that matches how you feel, "
    "chosen from: " + ", ".join(MOODS) + ". Example: [happy] Good to see you again!"
)
# ----------------------------------------

_TAG = re.compile(r"\[(\w+)\]")
_EMOJI = re.compile("[\U0001F300-\U0001FAFF\u2600-\u27BF]")


def check_ollama():
    try:
        r = requests.get(OLLAMA_URL + "/api/tags", timeout=5)
        names = [m["name"] for m in r.json().get("models", [])]
    except Exception as e:
        print("Can't reach Ollama at", OLLAMA_URL, "-", e)
        print("Start it with:  ollama serve")
        return False
    if MODEL not in names:
        print(f"Model '{MODEL}' not found. Installed models: {', '.join(names) or 'none'}")
        print("Either `ollama pull " + MODEL + "` or change MODEL at the top of this file.")
        return False
    return True


def split_mood(raw):
    """Pull the [mood] tag off the front; clean the text so it sounds right when spoken."""
    raw = raw.strip()
    m = _TAG.match(raw)
    mood = m.group(1).lower() if m and m.group(1).lower() in MOODS else "neutral"
    text = _TAG.sub("", raw)
    text = _EMOJI.sub("", text)
    text = re.sub(r"[*_`#]", "", text).strip()
    return mood, text or "..."


def log_turn(heard, mood, reply):
    """Show the exchange in the terminal and append it to conversations/YYYY-MM-DD.txt."""
    import datetime
    import os
    now = datetime.datetime.now()
    print(f"[{now:%H:%M:%S}] you      > {heard}")
    print(f"[{now:%H:%M:%S}] companion [{mood}] > {reply}\n")
    try:
        folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), LOG_DIR)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, f"{now:%Y-%m-%d}.txt"), "a", encoding="utf-8") as f:
            f.write(f"[{now:%H:%M:%S}] You: {heard}\n")
            f.write(f"[{now:%H:%M:%S}] Companion ({mood}): {reply}\n\n")
    except OSError as e:
        print("couldn't save conversation:", e)


def ask_ollama(history, user_text):
    history.append({"role": "user", "content": user_text})
    del history[:-MAX_HISTORY]
    messages = [{"role": "system", "content": SYSTEM_PROMPT}] + history
    r = requests.post(
        OLLAMA_URL + "/api/chat",
        json={"model": MODEL, "messages": messages, "stream": False,
              "options": {"num_predict": 160}},
        timeout=180,
    )
    r.raise_for_status()
    raw = r.json()["message"]["content"]
    history.append({"role": "assistant", "content": raw})
    return split_mood(raw)


# ---------- speech to text (local) ----------
_stt = None


def transcribe(pcm):
    global _stt
    import numpy as np
    if _stt is None:
        from faster_whisper import WhisperModel
        _stt = WhisperModel(STT_MODEL, device="cpu", compute_type="int8")
    audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    segments, _ = _stt.transcribe(audio, language="en", vad_filter=True)
    return " ".join(s.text.strip() for s in segments).strip()


# ---------- text to speech (local, Piper) ----------
def synthesize(text):
    import os
    voice = PIPER_VOICE
    if not os.path.isabs(voice) and not os.path.exists(voice):
        alt = os.path.join(os.path.dirname(os.path.abspath(__file__)), voice)
        if os.path.exists(alt):
            voice = alt                  # found next to this script
    p = subprocess.run(
        [PIPER_BIN, "--model", voice, "--output_raw"],
        input=text.encode("utf-8"), capture_output=True, timeout=90,
    )
    if p.returncode != 0:
        raise RuntimeError("piper failed: " + p.stderr.decode(errors="ignore")[-200:])
    return resample(p.stdout, PIPER_RATE, OUT_RATE)   # raw PCM16 mono at OUT_RATE


def resample(pcm, src, dst):
    if src == dst or not pcm:
        return pcm
    import numpy as np
    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    n = int(len(x) * dst / src)
    y = np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x)
    return y.astype(np.int16).tobytes()


# ---------- board connection ----------
async def send(ws, obj):
    await ws.send(json.dumps(obj))


async def speak(ws, mood, text):
    """Say `text` on the board with the given face."""
    audio = await asyncio.to_thread(synthesize, text)
    await send(ws, {"type": "mood", "value": mood})
    await send(ws, {"type": "reply", "text": text})
    await send(ws, {"type": "audio_start", "rate": OUT_RATE})
    for i in range(0, len(audio), CHUNK):
        await ws.send(audio[i:i + CHUNK])
        await asyncio.sleep(0)           # let the connection breathe
    await send(ws, {"type": "audio_end"})


def find_phrase(norm, phrases, max_words_before=2):
    """Return (start, end) of the first phrase that appears near the start of `norm`."""
    for p in phrases:
        i = norm.find(p)
        if i >= 0 and len(norm[:i].split()) <= max_words_before:
            return i, i + len(p)
    return None


class Session:
    """One connected board: voice activity detection, wake word, conversation."""

    def __init__(self, ws):
        self.ws = ws
        self.history = []
        self.awake = False
        self.awake_until = 0.0
        self.busy = False
        self.ignore_until = 0.0
        self.tasks = set()
        # push-to-talk (BOOT button)
        self.ptt = False
        self.ptt_buf = bytearray()
        # voice activity detection
        self.pending = bytearray()
        self.preroll = collections.deque(maxlen=VAD_PREROLL_FRAMES)
        self.noise = 150.0
        self.loud_run = 0
        self.in_speech = False
        self.utt = bytearray()
        self.silence_frames = 0
        self.voiced = 0

    # ----- helpers -----
    def touch(self):
        self.awake_until = time.monotonic() + AWAKE_SECONDS

    def reset_vad(self):
        self.pending.clear()
        self.preroll.clear()
        self.utt = bytearray()
        self.in_speech = False
        self.loud_run = 0
        self.silence_frames = 0
        self.voiced = 0

    def launch(self, coro):
        self.busy = True
        t = asyncio.create_task(coro)
        self.tasks.add(t)
        t.add_done_callback(self.tasks.discard)

    async def final_state(self):
        await send(self.ws, {"type": "state", "value": "idle" if self.awake else "asleep"})

    async def finish(self):
        self.reset_vad()
        self.ignore_until = time.monotonic() + POST_REPLY_IGNORE_S
        if self.awake:
            self.touch()
        self.busy = False
        try:
            await self.final_state()
        except Exception:
            pass

    # ----- audio in -----
    async def feed(self, data):
        if self.ptt:
            if len(self.ptt_buf) < MAX_RECORD_S * 16000 * 2:
                self.ptt_buf.extend(data)
            return
        if self.busy or time.monotonic() < self.ignore_until:
            return
        self.pending.extend(data)
        n = VAD_FRAME * 2
        while len(self.pending) >= n and not self.busy:
            frame = bytes(self.pending[:n])
            del self.pending[:n]
            await self.vad_frame(frame)
        if self.awake and not self.in_speech and not self.busy and time.monotonic() > self.awake_until:
            self.awake = False
            print("(going back to sleep)")
            await self.final_state()

    async def vad_frame(self, frame):
        import numpy as np
        x = np.frombuffer(frame, dtype=np.int16).astype(np.float32)
        rms = float(np.sqrt(np.mean(x * x))) if len(x) else 0.0
        thr = max(VAD_MIN_RMS, self.noise * VAD_RATIO)
        loud = rms > thr
        if not self.in_speech:
            self.noise = 0.98 * self.noise + 0.02 * min(rms, thr)   # follow the room's background level
            self.preroll.append(frame)
            self.loud_run = self.loud_run + 1 if loud else 0
            if self.loud_run >= 2:
                self.in_speech = True
                self.utt = bytearray(b"".join(self.preroll))
                self.preroll.clear()
                self.silence_frames = 0
                self.voiced = 2
                if self.awake:
                    self.touch()
                    await send(self.ws, {"type": "state", "value": "listening"})
            return
        self.utt.extend(frame)
        if loud:
            self.silence_frames = 0
            self.voiced += 1
        else:
            self.silence_frames += 1
        too_long = len(self.utt) > MAX_UTTERANCE_S * 16000 * 2
        if self.silence_frames >= VAD_END_FRAMES or too_long:
            pcm, voiced = bytes(self.utt), self.voiced
            self.reset_vad()
            if voiced >= VAD_MIN_VOICED_FRAMES:
                self.launch(self.handle_utterance(pcm))
            elif self.awake:
                await self.final_state()

    # ----- what to do with what was heard -----
    async def handle_utterance(self, pcm):
        try:
            if self.awake:
                await send(self.ws, {"type": "state", "value": "thinking"})
            heard = await asyncio.to_thread(transcribe, pcm)
            await self.route(heard)
        except Exception as e:
            await self.report_error(e)
        finally:
            await self.finish()

    async def report_error(self, e):
        print("error:", e)
        try:
            await send(self.ws, {"type": "mood", "value": "confused"})
            await send(self.ws, {"type": "error", "text": str(e)[:120]})
        except Exception:
            pass

    async def route(self, heard):
        text = heard.strip()
        norm = " ".join(re.sub(r"[^a-z' ]", " ", text.lower()).split())
        if not norm or norm in JUNK:
            return
        if not self.awake:
            hit = find_phrase(norm, WAKE_PHRASES)
            if hit is None:
                print(f"(asleep, heard: {text})")
                return
            self.awake = True
            print("wake word heard")
            rest = norm[hit[1]:].strip()
            if len(rest.split()) >= 2:
                await self.converse(rest)           # "wake up, what time is it"
            else:
                await self.say("happy", "I'm awake! What's up?", text)
            return
        if find_phrase(norm, SLEEP_PHRASES, 3) is not None:
            await self.say("sleepy", "Okay, good night.", text)
            self.awake = False
            return
        await self.converse(text)

    async def say(self, mood, reply, heard):
        await send(self.ws, {"type": "heard", "text": heard})
        log_turn(heard, mood, reply)
        await speak(self.ws, mood, reply)

    async def converse(self, user_text):
        await send(self.ws, {"type": "heard", "text": user_text})
        mood, text = await asyncio.to_thread(ask_ollama, self.history, user_text)
        log_turn(user_text, mood, text)
        await speak(self.ws, mood, text)

    # ----- push-to-talk (BOOT button) -----
    async def ptt_start(self):
        self.reset_vad()
        self.ptt_buf.clear()
        self.ptt = True
        await send(self.ws, {"type": "state", "value": "listening"})

    async def ptt_end(self):
        if not self.ptt:
            return
        self.ptt = False
        pcm = bytes(self.ptt_buf)
        self.ptt_buf.clear()
        if len(pcm) < MIN_RECORD_S * 16000 * 2:
            await self.final_state()
            return
        self.launch(self.handle_ptt(pcm))

    async def handle_ptt(self, pcm):
        try:
            self.awake = True
            await send(self.ws, {"type": "state", "value": "thinking"})
            heard = await asyncio.to_thread(transcribe, pcm)
            if heard.strip():
                await self.converse(heard.strip())
        except Exception as e:
            await self.report_error(e)
        finally:
            await self.finish()


async def handler(ws):
    print("board connected - say \"wake up\" (or hold the BOOT button)")
    s = Session(ws)
    try:
        await send(ws, {"type": "state", "value": "asleep"})
        async for msg in ws:
            if isinstance(msg, bytes):
                await s.feed(msg)
                continue
            try:
                kind = json.loads(msg).get("type")
            except ValueError:
                continue
            if kind == "start":
                await s.ptt_start()
            elif kind == "end":
                await s.ptt_end()
    finally:
        print("board disconnected")


async def serve():
    import websockets
    async with websockets.serve(handler, HOST, PORT, max_size=2 ** 22):
        print(f"Listening on ws://{HOST}:{PORT}  (model: {MODEL})")
        await asyncio.Future()


def text_mode():
    print(f"Chatting with {MODEL}. Ctrl+C to quit.\n")
    history = []
    while True:
        try:
            user = input("you > ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not user:
            continue
        try:
            mood, text = ask_ollama(history, user)
        except Exception as e:
            print("error:", e)
            continue
        log_turn(user, mood, text)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", action="store_true", help="terminal chat, no audio")
    args = ap.parse_args()
    if not check_ollama():
        raise SystemExit(1)
    if args.text:
        text_mode()
    else:
        asyncio.run(serve())

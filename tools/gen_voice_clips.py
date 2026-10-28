# -*- coding: utf-8 -*-
"""Generate the mouth's vocabulary: one small PCM clip per word segment.

Why segments and not whole sentences:
  the brain (312K pin-LLM) only emits action symbols, so every spoken reply is
  a templated sentence -- "<number> 号脚 已打开", "跑马灯启动", "没听懂".  Recording
  ~23 words covers all of them, while recording every sentence would need
  thousands of clips.  Chinese numerals compose cleanly (三十八 = 三 十 八), so
  ten digit clips plus "十" cover every pin the board has.

Output: main/voice_clips.bin, read by mouth.c at runtime.
  header: "VCL1" | u32 count | u32 rate | u32 reserved
  index : count x { u32 offset_samples, u32 n_samples }   (relative to pcm base)
  pcm   : int16 LE, mono, `rate` Hz

The clips live in flash (embedded into the app image by EMBED_FILES), so they
cost no RAM and no extra partition.

Usage (the venv has edge-tts + miniaudio):
  E:/esp32-tools/tts-venv/Scripts/python.exe tools/gen_voice_clips.py
"""
import asyncio
import hashlib
import json
import struct
import sys
from pathlib import Path

import miniaudio
import numpy as np

VOICE = "zh-CN-XiaoxiaoNeural"
RATE = 24000            # edge-tts native rate: no resampling, no aliasing
RATE_ARG = "+15%"       # brisk assistant, not a sleepy one
PEAK = 0.85             # per-clip peak normalisation: segments stay even
HEAD_MS = 12            # keep a little air before the first phoneme
TAIL_MS = 30            # enough to finish the final consonant

PROJECT = Path(__file__).resolve().parent.parent
OUT = PROJECT / "main" / "voice_clips.bin"
CACHE = Path("E:/esp32-tools/tts-cache")
MANIFEST = PROJECT / "tools" / "voice_clips_manifest.json"

# id order is the firmware's enum (main/voice_clips.h) -- keep them in sync.
CLIPS = [
    ("ZERO", "零"), ("ONE", "一"), ("TWO", "二"), ("THREE", "三"), ("FOUR", "四"),
    ("FIVE", "五"), ("SIX", "六"), ("SEVEN", "七"), ("EIGHT", "八"), ("NINE", "九"),
    ("TEN", "十"),
    ("HAOJIAO", "号脚"),
    ("OPENED", "已打开"),
    ("CLOSED", "已关闭"),
    ("ALL_ON", "已全部打开"),
    ("ALL_OFF", "已全部关闭"),
    ("BLINK", "开始闪烁"),
    ("CHASE", "跑马灯启动"),
    ("STOPPED", "已停止"),
    ("OK", "好的"),
    ("HERE", "我在"),
    ("NOPE", "没听懂"),
    ("BADPIN", "这个引脚不能用"),
]


async def synth(text: str, mp3: Path) -> None:
    import edge_tts
    if mp3.exists() and mp3.stat().st_size > 1000:
        return                      # cache: reruns work offline
    for attempt in range(3):
        try:
            await edge_tts.Communicate(text, VOICE, rate=RATE_ARG).save(str(mp3))
            if mp3.exists() and mp3.stat().st_size > 1000:
                return
        except Exception as exc:                     # noqa: BLE001 - network
            if attempt == 2:
                raise
            print("  retry after %s" % exc)
            await asyncio.sleep(1.5)


def decode(mp3: Path) -> np.ndarray:
    """mp3 -> int16 mono at RATE.  miniaudio resamples to the rate we ask for;
    asking for the native 24 kHz means it does not resample at all."""
    d = miniaudio.decode_file(str(mp3), output_format=miniaudio.SampleFormat.SIGNED16,
                              nchannels=1, sample_rate=RATE)
    return np.frombuffer(bytes(d.samples), dtype=np.int16).astype(np.float32)


def trim_normalise(x: np.ndarray) -> np.ndarray:
    if x.size == 0:
        return x
    peak = float(np.abs(x).max())
    if peak <= 0:
        return x
    x = x / peak                                   # now -1.0 .. +1.0
    loud = np.flatnonzero(np.abs(x) > 0.02)        # -34 dBFS relative to peak
    if loud.size:
        a = max(0, loud[0] - RATE * HEAD_MS // 1000)
        b = min(x.size, loud[-1] + RATE * TAIL_MS // 1000)
        x = x[a:b]
    return x * (PEAK * 32767.0)


def main() -> int:
    CACHE.mkdir(parents=True, exist_ok=True)
    print("voice: %s  rate: %d Hz  clips: %d" % (VOICE, RATE, len(CLIPS)))

    for i, (name, text) in enumerate(CLIPS):
        key = hashlib.md5((VOICE + RATE_ARG + text).encode("utf-8")).hexdigest()[:10]
        mp3 = CACHE / ("%02d_%s_%s.mp3" % (i, name, key))
        print("  [%2d/%d] %-8s %s" % (i + 1, len(CLIPS), name, text))
        asyncio.run(synth(text, mp3))

    pcm_parts, index, manifest = [], [], []
    offset = 0
    for name, text in CLIPS:
        key = hashlib.md5((VOICE + RATE_ARG + text).encode("utf-8")).hexdigest()[:10]
        mp3 = CACHE / ("%02d_%s_%s.mp3" % (len(index), name, key))
        y = trim_normalise(decode(mp3))
        n = y.size
        pcm_parts.append(y.astype(np.int16))
        index.append((offset, n))
        manifest.append({"id": name, "text": text, "offset": offset, "samples": n,
                         "ms": round(n * 1000.0 / RATE, 1)})
        offset += n

    pcm = np.concatenate(pcm_parts) if pcm_parts else np.zeros(0, np.int16)
    header = b"VCL1" + struct.pack("<III", len(CLIPS), RATE, 0)
    body = b"".join(struct.pack("<II", o, n) for o, n in index)
    OUT.write_bytes(header + body + pcm.tobytes())
    MANIFEST.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")

    total_ms = sum(m["ms"] for m in manifest)
    print("\nwrote %s  (%.1f KB, %.1f s of audio in %d clips)"
          % (OUT, OUT.stat().st_size / 1024.0, total_ms / 1000.0, len(CLIPS)))
    print("manifest: %s" % MANIFEST)
    return 0


if __name__ == "__main__":
    sys.exit(main())

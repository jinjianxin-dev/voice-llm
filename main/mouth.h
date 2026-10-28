/*
 * mouth.h -- the third organ: I2S TX to the MAX98357.
 *
 * The mouth is deliberately *not* part of the detect task.  Playback takes
 * 0.3-1.5 s and the detect task must keep draining the AFE in that time, or the
 * mic ringbuffer overruns and the next command is lost.  So:
 *
 *   detect task:  cmd -> llmrt -> gpio -> mouth_say_seq()  (enqueue, never blocks)
 *   mouth task :  dequeue -> I2S TX in 20 ms chunks        (never touches the mic)
 *
 * Half duplex by design: while the speaker is talking, `mouth_gate_closed()` is
 * true and the detect task stops feeding the recogniser.  Without that the
 * board hears its own "八号脚已打开", MultiNet matches it and the lamp toggles
 * forever -- the classic speaker-to-mic feedback loop.  The gate stays closed
 * for MOUTH_TAIL_MS after the last sample to let the room echo die down.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "voice_clips.h"

#define MOUTH_SEQ_MAX 8       /* longest reply: "二十一 号脚 已打开" is 3 */

int mouth_init(void);

/* Loud line-by-line proof that I2S TX actually drains: plays two short tones
 * and reports bytes/second.  Called once from app_main, synchronously. */
void mouth_selftest(void);

/* Queue up to MOUTH_SEQ_MAX segments.  Returns false when the queue is full
 * (the caller is a real-time task; dropping a reply beats blocking it). */
bool mouth_say_seq(const uint8_t *ids, int n);
bool mouth_say(voice_clip_t id);

/* True while a reply is playing or still queued. */
bool mouth_speaking(void);

/* True while the ear must stay deaf: speaking, or within the echo tail. */
bool mouth_gate_closed(void);

/* One-line counters for the REPL: bytes written, errors, current state. */
void mouth_stats(char *out, int n);

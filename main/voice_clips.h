/*
 * voice_clips.h -- the mouth's vocabulary, baked into the firmware.
 *
 * The brain is a 312K-parameter model that emits action symbols, not prose, so
 * every spoken reply is a template: "<number> 号脚 已打开", "跑马灯启动",
 * "没听懂".  Recording the ~23 word segments that those templates are built from
 * costs 559 KB of flash and no RAM (the blob stays memory-mapped), and gives a
 * reply that is intelligible on the first try -- unlike an on-chip TTS, which
 * would cost ~2 MB, a new partition and a few hundred ms of synthesis before
 * the first syllable.
 *
 * Regenerate with:  tools/gen_voice_clips.py   (order must match this enum)
 */
#pragma once

#include <stdint.h>

typedef enum {
    VC_ZERO = 0, VC_ONE, VC_TWO, VC_THREE, VC_FOUR, VC_FIVE, VC_SIX,
    VC_SEVEN, VC_EIGHT, VC_NINE, VC_TEN,
    VC_HAOJIAO,       /* 号脚 */
    VC_OPENED,        /* 已打开 */
    VC_CLOSED,        /* 已关闭 */
    VC_ALL_ON,        /* 已全部打开 */
    VC_ALL_OFF,       /* 已全部关闭 */
    VC_BLINK,         /* 开始闪烁 */
    VC_CHASE,         /* 跑马灯启动 */
    VC_STOPPED,       /* 已停止 */
    VC_OK,            /* 好的 */
    VC_HERE,          /* 我在 */
    VC_NOPE,          /* 没听懂 */
    VC_BADPIN,        /* 这个引脚不能用 */
    VC_COUNT
} voice_clip_t;

typedef struct {
    const int16_t *pcm;     /* mono, `rate` Hz */
    uint32_t       n_samples;
    uint32_t       rate;
} voice_clip_ref_t;

/* Validate the embedded blob. Returns 0 on success, -1 if the image does not
 * match this header (stale .bin next to a newer enum, or a partial flash). */
int clips_init(void);

const voice_clip_ref_t *clips_get(voice_clip_t id);
const char             *clips_name(voice_clip_t id);

/* Total samples across all clips -- a single number the boot log can print so
 * a silent mouth is distinguishable from an empty vocabulary. */
uint32_t clips_total_samples(void);

/* Sample rate of every clip (they are generated at one rate). */
uint32_t clips_rate(void);

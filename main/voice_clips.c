/*
 * voice_clips.c -- parse the blob that tools/gen_voice_clips.py writes.
 *
 * The blob is embedded with EMBED_FILES (see CMakeLists.txt) so it lands in
 * .rodata and stays in flash: `clips_get()` hands out a pointer straight into
 * the memory-mapped region, and playback streams from there.  Nothing is copied
 * to RAM -- 559 KB of words would not fit next to the AFE, MultiNet and the KV
 * cache, and there is no reason to move it.
 *
 * Layout (little-endian):
 *   +0   'V''C''L''1'
 *   +4   u32 count
 *   +8   u32 rate (Hz)
 *   +12  u32 reserved (0)
 *   +16  count x { u32 offset_samples, u32 n_samples }   offset from pcm base
 *   pcm  int16 mono, `rate` Hz
 */
#include "voice_clips.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"

extern const uint8_t voice_clips_bin_start[] asm("_binary_voice_clips_bin_start");
extern const uint8_t voice_clips_bin_end[]   asm("_binary_voice_clips_bin_end");

#define HDR_BYTES 16

typedef struct {
    uint32_t offset;
    uint32_t n_samples;
} index_entry_t;

static uint32_t s_rate = 0;
static uint32_t s_total = 0;
static voice_clip_ref_t s_refs[VC_COUNT];

static const char *const k_names[VC_COUNT] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
    "nine", "ten", "haojiao", "opened", "closed", "all_on", "all_off",
    "blink", "chase", "stopped", "ok", "here", "nope", "badpin",
};

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int clips_init(void) {
    size_t bytes = (size_t)(voice_clips_bin_end - voice_clips_bin_start);
    if (bytes < HDR_BYTES + VC_COUNT * 8) {
        printf("clips: blob too small (%u bytes) -- rebuild voice_clips.bin\n",
               (unsigned)bytes);
        return -1;
    }
    if (memcmp(voice_clips_bin_start, "VCL1", 4) != 0) {
        printf("clips: bad magic -- voice_clips.bin is not a clip blob\n");
        return -1;
    }

    uint32_t count = rd32(voice_clips_bin_start + 4);
    uint32_t rate  = rd32(voice_clips_bin_start + 8);
    if (count != (uint32_t)VC_COUNT) {
        printf("clips: %u clips in the blob, %d in the enum -- regenerate\n",
               (unsigned)count, (int)VC_COUNT);
        return -1;
    }
    if (rate < 8000 || rate > 48000) {
        printf("clips: implausible rate %u Hz\n", (unsigned)rate);
        return -1;
    }

    const uint8_t *index = voice_clips_bin_start + HDR_BYTES;
    const uint8_t *pcm   = index + (size_t)count * 8;
    size_t pcm_bytes = (size_t)(voice_clips_bin_end - pcm);
    if (((uintptr_t)pcm & 1) != 0) {
        printf("clips: pcm base is odd -- a 16-bit load would fault\n");
        return -1;
    }

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = rd32(index + i * 8);
        uint32_t n   = rd32(index + i * 8 + 4);
        if (n == 0 || off < total || (size_t)(off + n) * 2 > pcm_bytes) {
            printf("clips: entry %u out of bounds (off %u n %u of %u bytes)\n",
                   (unsigned)i, (unsigned)off, (unsigned)n, (unsigned)pcm_bytes);
            return -1;
        }
        total = off + n;
        s_refs[i].pcm       = (const int16_t *)(pcm + (size_t)off * 2);
        s_refs[i].n_samples = n;
        s_refs[i].rate      = rate;
    }

    s_rate  = rate;
    s_total = total;
    return 0;
}

const voice_clip_ref_t *clips_get(voice_clip_t id) {
    if ((int)id < 0 || id >= VC_COUNT) return NULL;
    return &s_refs[id];
}

const char *clips_name(voice_clip_t id) {
    if ((int)id < 0 || id >= VC_COUNT) return "?";
    return k_names[id];
}

uint32_t clips_total_samples(void) { return s_total; }
uint32_t clips_rate(void) { return s_rate; }

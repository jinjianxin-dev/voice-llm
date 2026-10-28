/*
 * mouth.c -- I2S TX to the MAX98357, on its own task and its own I2S port.
 *
 * Port allocation (the two halves of the conversation never share hardware):
 *   I2S_NUM_0  RX  BCLK 5  WS 6  DIN 7    INMP441   (the ear, main.c)
 *   I2S_NUM_1  TX  BCLK 15 WS 16 DOUT 18  MAX98357  (the mouth, here)
 *
 * Format: 16-bit stereo, mono duplicated into both slots.  That is exactly the
 * configuration the standalone speaker_test sketch proved on this board; the
 * MAX98357 takes the left slot by default (SD floating) so the duplicate costs
 * two bytes per frame and removes one way to be wrong.
 *
 * History that matters here: on this board, in Arduino, the IDF i2s_std TX path
 * once stalled its DMA after ~4 KB and the legacy driver was the one that
 * actually drained.  That was inside the Arduino wrapper (a second channel on
 * the same port, so writes went to a channel nobody clocked).  In plain IDF we
 * get the port exclusively, so i2s_std is used -- but the throughput is still
 * measured and printed at boot (mouth_selftest), because "bytes written" is not
 * the same claim as "sound came out".
 */
#include "mouth.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define PIN_BCLK  GPIO_NUM_15
#define PIN_WS    GPIO_NUM_16
#define PIN_DOUT  GPIO_NUM_18

#define CHUNK_FRAMES    480                 /* 20 ms at 24 kHz */
#define BYTES_PER_FRAME 4                   /* 2 ch x 16 bit    */
#define CHUNK_BYTES     (CHUNK_FRAMES * BYTES_PER_FRAME)

#define QUEUE_LEN       4
#define GAP_MS          40                  /* between words of one reply */
#define DRAIN_MS        60                  /* let the DMA finish the last chunk */
#define MOUTH_TAIL_MS   300                 /* ear stays deaf this long after */

#define TASK_STACK      4096
#define TASK_PRIO       4                   /* below detect(5): recognition first */

typedef struct {
    uint8_t n;
    uint8_t id[MOUTH_SEQ_MAX];
} mouth_item_t;

static i2s_chan_handle_t s_tx = NULL;
static QueueHandle_t     s_q = NULL;
static int16_t          *s_buf = NULL;      /* CHUNK_FRAMES x 2 interleaved */
static uint32_t          s_rate = 0;

static volatile bool  s_playing = false;
static volatile int64_t s_quiet_until_us = 0;

static uint32_t  s_bytes = 0;
static uint32_t  s_errors = 0;
static esp_err_t s_last_err = ESP_OK;

/* ------------------------------------------------------------------ */

static int tx_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    /* auto_clear: an underrun outputs silence instead of repeating the last
     * buffer forever (which on a speaker is a stuck DC value / click). */
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        printf("mouth: i2s_new_channel failed: %s\n", esp_err_to_name(err));
        return -1;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        printf("mouth: init_std_mode failed: %s\n", esp_err_to_name(err));
        return -1;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        printf("mouth: channel_enable failed: %s\n", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

/* Push `n` mono samples out as stereo frames.  Returns false on the first
 * short/failed write -- the caller stops the reply rather than replaying
 * half a word at the wrong time. */
static bool tx_mono(const int16_t *pcm, size_t n) {
    while (n > 0) {
        size_t take = (n > CHUNK_FRAMES) ? CHUNK_FRAMES : n;
        for (size_t i = 0; i < take; i++) {
            s_buf[2 * i]     = pcm[i];
            s_buf[2 * i + 1] = pcm[i];
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_buf, take * BYTES_PER_FRAME,
                                          &written, pdMS_TO_TICKS(500));
        s_bytes += (uint32_t)written;
        if (err != ESP_OK || written != take * BYTES_PER_FRAME) {
            s_errors++;
            s_last_err = err;
            return false;
        }
        pcm += take;
        n -= take;
    }
    return true;
}

static bool tx_silence(int ms) {
    static const int16_t zero = 0;
    int n = (int)((int64_t)s_rate * ms / 1000);
    while (n > 0) {
        int take = (n > CHUNK_FRAMES) ? CHUNK_FRAMES : n;
        for (int i = 0; i < take; i++) {
            s_buf[2 * i]     = zero;
            s_buf[2 * i + 1] = zero;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_buf, (size_t)take * BYTES_PER_FRAME,
                                          &written, pdMS_TO_TICKS(500));
        s_bytes += (uint32_t)written;
        if (err != ESP_OK || written != (size_t)take * BYTES_PER_FRAME) {
            s_errors++;
            s_last_err = err;
            return false;
        }
        n -= take;
    }
    return true;
}

static void mouth_mark_idle(void) {
    s_playing = false;
    s_quiet_until_us = esp_timer_get_time() + (int64_t)MOUTH_TAIL_MS * 1000;
}

static void mouth_task(void *arg) {
    (void)arg;
    mouth_item_t it;
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;

        s_playing = true;
        for (int i = 0; i < it.n; i++) {
            const voice_clip_ref_t *c = clips_get((voice_clip_t)it.id[i]);
            if (!c) continue;
            if (!tx_mono(c->pcm, c->n_samples)) {
                printf("mouth: write failed (%s) after %u bytes\n",
                       esp_err_to_name(s_last_err), (unsigned)s_bytes);
                break;
            }
            if (i + 1 < it.n) tx_silence(GAP_MS);
        }
        /* The DMA still holds up to a chunk; telling the ear it is over now
         * would let the tail of the last word be recognised as a command. */
        vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
        mouth_mark_idle();
    }
}

/* ------------------------------------------------------------------ */

void mouth_selftest(void) {
    /* Two tones through the real path (same buffer, same i2s_channel_write).
     * A silent speaker with a clean log means wiring or amp, not code -- this
     * is the shortcut that costs the least time when the mouth goes quiet. */
    printf("mouth: self-test 880/1320 Hz ...\n");
    s_playing = true;
    uint32_t before = s_bytes;
    int64_t t0 = esp_timer_get_time();

    for (int round = 0; round < 2; round++) {
        double f = round ? 1320.0 : 880.0;
        static int16_t tone[CHUNK_FRAMES];
        for (int i = 0; i < CHUNK_FRAMES; i++)
            tone[i] = (int16_t)(sin(2.0 * M_PI * f * i / (double)s_rate) * 0.55 * 32767.0);
        tx_mono(tone, CHUNK_FRAMES);
        tx_mono(tone, CHUNK_FRAMES);
        tx_silence(60);
    }

    uint32_t dt_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    uint32_t sent = s_bytes - before;
    double kbps = dt_ms ? (sent / 1024.0) * 1000.0 / dt_ms : 0.0;
    double want = s_rate * BYTES_PER_FRAME / 1024.0;

    vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
    mouth_mark_idle();

    printf("mouth: %u bytes in %u ms = %.0f KB/s (wall clock wants %.0f)\n",
           (unsigned)sent, (unsigned)dt_ms, kbps, want);
    if (kbps < want * 0.7) {
        printf("mouth: TX is SLOW -- DMA may not be draining (see README: legacy "
               "driver fallback). Sound may be absent or stuttering.\n");
    } else {
        printf("mouth: I2S TX drains at full speed -- if you heard nothing, check "
               "MAX98357 VIN/GND/GAIN and the speaker wires.\n");
    }
}

int mouth_init(void) {
    if (clips_init() != 0) return -1;
    s_rate = clips_rate();
    if (!s_rate) return -1;

    s_buf = (int16_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buf) {
        printf("mouth: no memory for the TX buffer\n");
        return -1;
    }

    if (tx_init() != 0) return -1;

    s_q = xQueueCreate(QUEUE_LEN, sizeof(mouth_item_t));
    if (!s_q) {
        printf("mouth: no memory for the queue\n");
        return -1;
    }

    printf("mouth: %d clips, %.1f s, %u Hz, %u KB in flash\n",
           (int)VC_COUNT, (double)clips_total_samples() / s_rate, (unsigned)s_rate,
           (unsigned)((uint32_t)clips_total_samples() * 2 / 1024));

    BaseType_t ok = xTaskCreatePinnedToCore(&mouth_task, "mouth", TASK_STACK, NULL,
                                            TASK_PRIO, NULL, 0);
    if (ok != pdPASS) {
        printf("mouth: task create failed\n");
        return -1;
    }
    return 0;
}

bool mouth_say_seq(const uint8_t *ids, int n) {
    if (!s_q || !ids || n <= 0) return false;
    if (n > MOUTH_SEQ_MAX) n = MOUTH_SEQ_MAX;

    mouth_item_t it;
    it.n = (uint8_t)n;
    for (int i = 0; i < n; i++) it.id[i] = ids[i];

    /* Zero timeout: the detect task must never wait on the loudspeaker. */
    if (xQueueSend(s_q, &it, 0) != pdTRUE) {
        printf("mouth: queue full, reply dropped\n");
        return false;
    }
    return true;
}

bool mouth_say(voice_clip_t id) {
    uint8_t one = (uint8_t)id;
    return mouth_say_seq(&one, 1);
}

bool mouth_speaking(void) {
    if (s_playing) return true;
    return s_q && uxQueueMessagesWaiting(s_q) > 0;
}

bool mouth_gate_closed(void) {
    if (mouth_speaking()) return true;
    return esp_timer_get_time() < s_quiet_until_us;
}

void mouth_stats(char *out, int n) {
    snprintf(out, (size_t)n,
             "mouth: %s  bytes=%u errors=%u last=%s queue=%u",
             mouth_speaking() ? "speaking" : "idle", (unsigned)s_bytes,
             (unsigned)s_errors, esp_err_to_name(s_last_err),
             (unsigned)(s_q ? uxQueueMessagesWaiting(s_q) : 0));
}

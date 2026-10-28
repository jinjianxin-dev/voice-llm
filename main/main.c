/*
 * main.c -- voice front-end for the esp32-gpio-llm, all on-chip.
 *
 *   INMP441 (I2S, 16 kHz) -> AFE -> WakeNet "nihaoxiaozhi" -> MultiNet7 cn
 *     -> command_id -> English text -> llm_runtime (the original 312K model)
 *     -> Command -> gpio_execute
 *
 * The LLM stays the brain: voice only supplies the text that the typed REPL
 * used to supply, so every refusal, range check and alias resolution is the
 * same code the host gates verified.
 *
 * Serial REPL still works: type "turn on pin 4" or "alias desk lamp = 4".
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "model_path.h"

/* command.h carries the shared enums (CmdAction, CmdLevel, Verdict) this file
 * needs to turn an outcome into words, plus a handful of static helpers that
 * only llm_runtime.c uses. Suppress the "defined but not used" noise rather
 * than mirroring the enum values here, where they could drift. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "command.h"
#pragma GCC diagnostic pop

#include "llm_runtime.h"
#include "mouth.h"
#include "voice_clips.h"

#define I2S_BCLK  GPIO_NUM_5
#define I2S_WS    GPIO_NUM_6
#define I2S_DIN   GPIO_NUM_7
#define SAMPLE_RATE 16000

static const char *TAG = "voice_llm";

/* Defined below (see "Speaking the result"); the detect task needs it. */
static void schedule_reply(const llmrt_outcome_t *r);

/* ------------------------------------------------------------------ */
/* Voice command table: one MultiNet command id -> an English utterance
 * the LLM already understands. Pin-numbered phrases are generated from
 * this list; the spoken form uses Chinese numerals because MultiNet
 * refuses Arabic digits in command strings. */

typedef struct {
    int pin;
    const char *cn;          /* spoken numeral: "四", "二十一", ... */
    const char *py;          /* tone-less pinyin: mn7_cn only accepts pinyin */
} pin_name_t;

static const pin_name_t PINS[] = {
    { 1, "一", "yi" }, { 2, "二", "er" }, { 3, "三", "san" }, { 4, "四", "si" },
    { 5, "五", "wu" }, { 6, "六", "liu" }, { 7, "七", "qi" }, { 8, "八", "ba" },
    { 9, "九", "jiu" }, { 10, "十", "shi" }, { 11, "十一", "shi yi" },
    { 12, "十二", "shi er" }, { 13, "十三", "shi san" }, { 14, "十四", "shi si" },
    { 15, "十五", "shi wu" }, { 16, "十六", "shi liu" }, { 17, "十七", "shi qi" },
    { 18, "十八", "shi ba" }, { 21, "二十一", "er shi yi" },
    { 38, "三十八", "san shi ba" }, { 39, "三十九", "san shi jiu" },
    { 40, "四十", "si shi" }, { 41, "四十一", "si shi yi" },
    { 42, "四十二", "si shi er" }, { 48, "四十八", "si shi ba" },
};
#define N_PINS (sizeof(PINS) / sizeof(PINS[0]))

typedef struct {
    int id;
    char phrase[32];         /* what you say  (registered with MultiNet) */
    char text[96];           /* what the LLM hears (English utterance)   */
} voice_cmd_t;

#define MAX_CMDS 128
static voice_cmd_t g_cmds[MAX_CMDS];
static int g_n_cmds = 0;

static void add_cmd(int id, const char *phrase, const char *text) {
    if (g_n_cmds >= MAX_CMDS) return;
    g_cmds[g_n_cmds].id = id;
    snprintf(g_cmds[g_n_cmds].phrase, sizeof(g_cmds[0].phrase), "%s", phrase);
    snprintf(g_cmds[g_n_cmds].text, sizeof(g_cmds[0].text), "%s", text);
    g_n_cmds++;
}

static const voice_cmd_t *find_cmd(int id) {
    for (int i = 0; i < g_n_cmds; i++) if (g_cmds[i].id == id) return &g_cmds[i];
    return NULL;
}

static void build_command_table(void) {
    int id = 1;
    for (int i = 0; i < (int)N_PINS; i++) {
        char ph[40], tx[80];   /* zh UTF-8: the chase line alone is ~55 bytes */
        /* mn7_cn takes tone-less pinyin syllables only ("da kai kong tiao");
         * raw Chinese characters are refused at esp_mn_commands_add().
         * The LLM was retrained on Chinese (cmd-zh-s1), so the mapped text
         * is Chinese too -- phrases verified against the zh model on the
         * host REPL. Note: the C parser has no "和" connector; lists must
         * join with "、". */
        snprintf(ph, sizeof(ph), "da kai %s yin jiao", PINS[i].py);
        snprintf(tx, sizeof(tx), "打开%d号脚", PINS[i].pin);
        add_cmd(id++, ph, tx);
        snprintf(ph, sizeof(ph), "guan bi %s yin jiao", PINS[i].py);
        snprintf(tx, sizeof(tx), "关闭%d号脚", PINS[i].pin);
        add_cmd(id++, ph, tx);
        /* blink uses the fixed 500ms x3 defaults; type a custom interval in
         * the serial REPL ("让8号脚闪烁，每1000毫秒，闪5次") for anything else */
        snprintf(ph, sizeof(ph), "shan shuo %s yin jiao", PINS[i].py);
        snprintf(tx, sizeof(tx), "让%d号脚闪烁，每500毫秒，闪3次", PINS[i].pin);
        add_cmd(id++, ph, tx);
    }
    /* A few human phrases on top of the pin grammar. "台灯" resolves through
     * the alias table (seeded to pin 1 at boot -- the user's first LED), so it
     * exercises the LLM's name path rather than bypassing it. */
    add_cmd(id++, "da kai tai deng",   "打开台灯");
    add_cmd(id++, "guan bi tai deng",  "关闭台灯");
    add_cmd(id++, "shan shuo tai deng","让台灯闪烁，每500毫秒，闪3次");
    add_cmd(id++, "liu shui deng",     "让1号脚、2号脚、8号脚跑马灯，每500毫秒");
    add_cmd(id++, "quan bu guan bi",   "把所有灯都关掉");
    add_cmd(id++, "ting zhi",          "停止");
}

static void register_commands(esp_mn_iface_t *multinet, model_iface_data_t *model_data) {
    esp_mn_commands_clear();
    for (int i = 0; i < g_n_cmds; i++) {
        esp_err_t err = esp_mn_commands_add(g_cmds[i].id, g_cmds[i].phrase);
        if (err != ESP_OK) printf("command refused: %s\n", g_cmds[i].phrase);
    }
    esp_mn_error_t *errs = esp_mn_commands_update();
    if (errs) {
        printf("some commands failed to parse\n");
    } else {
        printf("registered %d voice commands\n", g_n_cmds);
    }
    (void)multinet; (void)model_data;
    esp_mn_active_commands_print();
}

/* ------------------------------------------------------------------ */
/* I2S RX from the INMP441: 32-bit mono frames, L/R tied to GND (left). */

static i2s_chan_handle_t s_rx = NULL;

static int i2s_mic_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws   = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    return 0;
}

/* ------------------------------------------------------------------ */
/* AFE + wake + multinet pipeline. */

static srmodel_list_t *s_models = NULL;
static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static volatile int s_task_flag = 0;

/* feed task: I2S -> AFE, pinned to core 0 */
static void feed_task(void *arg) {
    (void)arg;
    int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);   /* samples per channel */
    int nch = s_afe_handle->get_feed_channel_num(s_afe_data);   /* 1 for input_format "M" */
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t) * nch, MALLOC_CAP_SPIRAM);
    int32_t *raw = heap_caps_malloc(2 * chunk * sizeof(int32_t), MALLOC_CAP_DMA);
    assert(buf && raw);

    int chunks = 0;                 /* mic-level debug: peak amplitude / sec */
    int peak = 0;
    long long dc_sum = 0;           /* debug: DC offset + zero-crossing rate */
    long dc_n = 0;
    int crossings = 0;
    int prev = 0;
    int snap[12];                   /* 12 decimated samples from the first chunk */
    int snap_n = -1;

    /* The S3 std RX driver delivers both slots (stereo frames) even when
     * configured mono at 32-bit width: real audio in slot 0, zeros in slot 1.
     * Decimate to true 16 kHz mono by taking every even-index sample. */
    while (s_task_flag) {
        size_t br = 0;
        i2s_channel_read(s_rx, raw, 2 * chunk * sizeof(int32_t), &br, portMAX_DELAY);
        int got = br / sizeof(int32_t) / 2;      /* stereo frames -> mono samples */
        if (got > chunk) got = chunk;
        if (snap_n < 0) {
            for (int i = 0; i < 12 && i < got; i++) snap[i] = (int)(raw[2 * i] >> 14);
            snap_n = got < 12 ? got : 12;
        }
        for (int i = 0; i < got; i++) {
            int32_t v = raw[2 * i] >> 14;        /* left slot only */
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            dc_sum += v; dc_n++;
            if ((v < 0) != (prev < 0)) crossings++;
            prev = (int)v;
            buf[i] = (int16_t)v;
        }
        for (int i = got; i < chunk; i++) buf[i] = 0;
        s_afe_handle->feed(s_afe_data, buf);

        chunks++;
        if (chunks >= 30) {         /* ~1 s */
            printf("[mic peak %d dc %+ld zcr %.3f] raw:",
                   peak, (long)(dc_sum / (dc_n ? dc_n : 1)),
                   dc_n ? (float)crossings / dc_n : 0.0f);
            for (int i = 0; i < snap_n; i++) printf(" %d", snap[i]);
            printf("\n");
            peak = 0; chunks = 0; dc_sum = 0; dc_n = 0; crossings = 0; snap_n = -1;
        }
    }
    free(buf); free(raw);
    vTaskDelete(NULL);
}

/* detect task: AFE fetch -> wakenet -> multinet -> LLM -> GPIO, core 1 */
static void detect_task(void *arg) {
    (void)arg;
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        printf("no Chinese multinet model in the srmodel partition\n");
        vTaskDelete(NULL);
        return;
    }
    printf("multinet model: %s\n", mn_name);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 8000);
    register_commands(multinet, model_data);

    int wakeup = 0;
    printf("------------listening: say \"ni hao xiao zhi\"------------\n");
    int last_vad = -1;   /* debug: print VAD transitions + what AFE hears */
    int ear_gated = 0;   /* 1 while the mouth owns the room (half duplex) */

    while (s_task_flag) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            printf("fetch error!\n");
            break;
        }

        /* Half duplex, and not out of politeness: the MAX98357 is 20 cm from
         * the INMP441 with no AEC (the AFE runs "M", one mic, no reference
         * channel). While a reply is playing, the board would hear its own
         * "打开八号脚" and MultiNet would match it -- the lamp toggles forever.
         * So the ear goes deaf for the reply plus MOUTH_TAIL_MS of room echo.
         * Anything said in that window is lost; the log says so. */
        int gated = mouth_gate_closed() ? 1 : 0;
        if (gated != ear_gated) {
            ear_gated = gated;
            multinet->clean(model_data);   /* drop any half-eaten utterance */
            printf(gated ? "[ear muted: speaking]\n" : "[ear live]\n");
        }
        if (gated) continue;

        if ((int)res->vad_state != last_vad) {
            last_vad = (int)res->vad_state;
            printf("[vad %s vol %.1f dBFS]\n",
                   (int)res->vad_state == (int)AFE_VAD_SPEECH ? "SPEECH" : "silence",
                   res->data_volume);
        }
        if (res->wakeup_state != WAKENET_NO_DETECT) {
            printf("[wakenet state %d]\n", (int)res->wakeup_state);
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            printf(">> wake word detected\n");
            multinet->clean(model_data);
            wakeup = 1;
            /* A person says "我在" when you call their name. Wait for it to
             * finish before speaking: the command window opens after it. */
            mouth_say(VC_HERE);
            continue;
        }

        if (!wakeup) continue;

        esp_mn_state_t st = multinet->detect(model_data, res->data);
        if (st == ESP_MN_STATE_DETECTING) continue;

        esp_mn_results_t *r = multinet->get_results(model_data);
        if (st == ESP_MN_STATE_DETECTED && r && r->num > 0) {
            const voice_cmd_t *vc = find_cmd(r->command_id[0]);
            printf(">> heard: %s  (prob %.2f)\n",
                   vc ? vc->phrase : "?", r->prob[0]);
            if (vc) {
                char msg[160];
                uint32_t ms = 0;
                llmrt_outcome_t outc;
                llmrt_run_text_ex(vc->text, msg, sizeof(msg), &ms, &outc);
                printf(">> %s\n", msg);
                schedule_reply(&outc);
            }
            multinet->clean(model_data);        /* continuous mode: next command without re-wake */
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            printf(">> (no command, back to sleep)\n");
            s_afe_handle->enable_wakenet(s_afe_data);
            wakeup = 0;
        }
    }
    multinet->destroy(model_data);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Speaking the result.
 *
 * The 312K model emits <set> <pin> 8 <high>, never a sentence, so the reply is
 * assembled from word clips -- the same way the model assembles symbols.
 * Numbers compose in Chinese (三十八 = 三 十 八), so ten digit clips cover all
 * 25 usable pins.  Anything longer than one pin is summarised with "好的":
 * reading out "一号脚、二号脚、八号脚已打开" takes four seconds and tells the user
 * nothing the LEDs have not already shown. */

static const uint8_t k_digit[10] = {
    VC_ZERO, VC_ONE, VC_TWO, VC_THREE, VC_FOUR,
    VC_FIVE, VC_SIX, VC_SEVEN, VC_EIGHT, VC_NINE,
};

static int push_number(int v, uint8_t *seq, int k) {
    if (v < 0 || k >= MOUTH_SEQ_MAX) return k;
    if (v <= 10) { seq[k++] = k_digit[v]; return k; }
    if (v < 20) {
        seq[k++] = VC_TEN;
        if (k < MOUTH_SEQ_MAX) seq[k++] = k_digit[v - 10];
        return k;
    }
    if (k < MOUTH_SEQ_MAX) seq[k++] = k_digit[v / 10];
    if (k < MOUTH_SEQ_MAX) seq[k++] = VC_TEN;
    if (v % 10 && k < MOUTH_SEQ_MAX) seq[k++] = k_digit[v % 10];
    return k;
}

static void schedule_reply(const llmrt_outcome_t *r) {
    uint8_t seq[MOUTH_SEQ_MAX];
    int k = 0;

    if (!r->executed) {
        /* "这个引脚不能用" only when the pin itself was the problem; every other
         * refusal is the model not understanding, which is a different thing
         * to tell the user and a different thing to go fix. */
        seq[k++] = (r->verdict == VERDICT_BAD_PIN) ? VC_BADPIN : VC_NOPE;
        mouth_say_seq(seq, k);
        return;
    }

    switch (r->action) {
    case ACT_SET:
        if (r->all) {
            seq[k++] = (r->level == LVL_LOW) ? VC_ALL_OFF : VC_ALL_ON;
        } else if (r->n_pins == 1 && r->level != LVL_TOGGLE) {
            k = push_number(r->pins[0], seq, k);
            if (k < MOUTH_SEQ_MAX) seq[k++] = VC_HAOJIAO;
            if (k < MOUTH_SEQ_MAX)
                seq[k++] = (r->level == LVL_HIGH) ? VC_OPENED : VC_CLOSED;
        } else {
            seq[k++] = VC_OK;          /* several pins, or a toggle: no number */
        }
        break;

    case ACT_BLINK:
        if (r->n_pins == 1) {
            k = push_number(r->pins[0], seq, k);
            if (k < MOUTH_SEQ_MAX) seq[k++] = VC_HAOJIAO;
        }
        if (k < MOUTH_SEQ_MAX) seq[k++] = VC_BLINK;
        break;

    case ACT_SEQ:   seq[k++] = VC_CHASE;   break;
    case ACT_STOP:  seq[k++] = VC_STOPPED; break;
    default:        seq[k++] = VC_OK;      break;
    }
    mouth_say_seq(seq, k);
}

/* ------------------------------------------------------------------ */
/* Serial REPL: same lines the espcontrol sketch took, plus "alias".   */

/* Boot progress, reported by a heartbeat so a silent console can be told
 * apart from a hung init: whichever step the heartbeat shows is where we
 * are (or got stuck). */
static volatile int s_step = 0;
static volatile int s_interactive = 0;


static void repl_task(void *arg) {
    (void)arg;
    char line[256];
    printf("\n> ");
    while (s_task_flag) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s_interactive = 1;   /* host is talking to us; stop the heartbeat */
        size_t n = strcspn(line, "\r\n");
        line[n] = 0;
        if (!line[0]) { printf("> "); continue; }

        /* Mouth diagnostics, typed, so a silent speaker can be checked without
         * saying anything: "vt" = say it, "vt 21" = say 21, "vm" = counters. */
        if (strncmp(line, "vm", 2) == 0) {
            char s[128];
            mouth_stats(s, sizeof(s));
            printf("%s\n> ", s);
            continue;
        }
        if (strncmp(line, "vt", 2) == 0) {
            int pin = atoi(line + 2);
            if (pin <= 0) pin = 8;
            uint8_t seq[MOUTH_SEQ_MAX];
            int k = push_number(pin, seq, 0);
            if (k < MOUTH_SEQ_MAX) seq[k++] = VC_HAOJIAO;
            if (k < MOUTH_SEQ_MAX) seq[k++] = VC_OPENED;
            printf("speaking: %d 号脚 已打开\n", pin);
            mouth_say_seq(seq, k);
            printf("> ");
            continue;
        }

        char msg[160];
        uint32_t ms = 0;
        llmrt_outcome_t outc;
        llmrt_run_text_ex(line, msg, sizeof(msg), &ms, &outc);
        printf("%s\n> ", msg);
        /* Typed commands speak too: it is the only way to check the reply
         * wording without saying anything out loud. */
        schedule_reply(&outc);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

static void heartbeat_task(void *arg) {
    (void)arg;
    int n = 0;
    while (!s_interactive) {
        printf("[alive %d s] step=%d heap=%u\n", n += 2, s_step,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    /* Unbuffered stdout: a short banner must reach the host immediately,
     * not sit in a 4 KB buffer until something fills it. */
    setvbuf(stdout, NULL, _IONBF, 0);

    xTaskCreatePinnedToCore(&heartbeat_task, "hb", 3 * 1024, NULL, 2, NULL, 0);

    printf("\nvoice_llm: offline voice control for the pin LLM\n");
    s_step = 1;
    printf("[step] 1/7 nvs\n");
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    if (llmrt_init() != 0) {
        printf("LLM init failed -- halting\n");
        while (1) vTaskDelay(1000);
    }
    s_step = 2;
    printf("[step] 2/7 llm mapped + seeded\n");
    vTaskDelay(pdMS_TO_TICKS(300));

    /* The mouth comes up before the ear: its self-test is the fastest way to
     * tell "no sound because the channel is dead" from "no sound because the
     * amp is unplugged", and it costs half a second of boot. */
    if (mouth_init() != 0) {
        printf("mouth init failed -- continuing mute (ears and brain still work)\n");
    } else {
        mouth_selftest();
    }
    s_step = 3;
    printf("[step] 3/7 mouth\n");
    vTaskDelay(pdMS_TO_TICKS(300));

    i2s_mic_init();
    s_step = 4;
    printf("[step] 4/7 i2s mic\n");
    vTaskDelay(pdMS_TO_TICKS(300));

    s_models = esp_srmodel_init("model");   /* esp-sr owns the "model" name */
    if (!s_models) {
        printf("no 'model' partition or no srmodels flashed at 0x5f0000\n");
        while (1) vTaskDelay(1000);
    }
    s_step = 5;
    printf("[step] 5/7 srmodel partition\n");
    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    printf("wakenet model: %s\n", wn_name ? wn_name : "(none)");
    vTaskDelay(pdMS_TO_TICKS(300));

    afe_config_t *afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    s_afe_handle = esp_afe_handle_from_config(afe_config);
    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    s_step = 6;
    printf("[step] 6/7 afe created\n");
    vTaskDelay(pdMS_TO_TICKS(300));

    build_command_table();
    s_step = 7;
    printf("[step] 7/7 tasks\n");

    s_task_flag = 1;
    /* Unicore build (CONFIG_FREERTOS_UNICORE=y): core 1 does not exist.
     * Pinning a task to core 1 trips the xTaskCreatePinnedToCore assert and
     * reboot-loops the chip, so every task runs on core 0. */
    xTaskCreatePinnedToCore(&feed_task,   "feed",   8 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_task, "detect", 8 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&repl_task,   "repl",   8 * 1024, NULL, 3, NULL, 0);
}

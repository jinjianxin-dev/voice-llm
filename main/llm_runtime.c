/*
 * llm_runtime.c -- espcontrol.ino ported to ESP-IDF, unchanged in spirit.
 *
 * The original file's comments explain the two traps this port must also
 * avoid, so they are kept here:
 *
 * 1. BPE_INIT, never a field list. C zero-fills the members you leave out,
 *    and tok_decode with vocab==0 refuses every id.
 * 2. Refuse, do not guess. A parse failure or an unknown name is reported
 *    by name/value, never swapped for the nearest thing that exists.
 */

#include "llm_runtime.h"

#include <string.h>
#include <stdio.h>

#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "llm.h"
#include "tokenizer.h"
#include "command.h"
#include "alias.h"
#include "gpio_control.h"
#include "bpe.h"

#define MAX_IDS  256
#define MAX_NEW  40

static Model model;
static Run  *run;                    /* ~200 KB of KV cache: PSRAM, not the stack */
static Bpe   bpe = BPE_INIT;         /* see the comment above: never spell the fields out */
static const void *model_base = NULL;

static AliasTable aliases;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ */
/* Alias persistence: the Arduino sketch used Preferences (NVS under  */
/* the hood); here we talk to NVS directly. Same flat-blob format.    */
/* ------------------------------------------------------------------ */

static void alias_load(void) {
    alias_init(&aliases);
    nvs_handle_t h;
    if (nvs_open("espcontrol", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = 0;
    if (nvs_get_blob(h, "aliases", NULL, &n) == ESP_OK && n == sizeof(AliasTable)) {
        nvs_get_blob(h, "aliases", &aliases, &n);
    }
    nvs_close(h);
    if (aliases.n < 0 || aliases.n > ALIAS_MAX) alias_init(&aliases);
}

static void alias_save(void) {
    nvs_handle_t h;
    if (nvs_open("espcontrol", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "aliases", &aliases, sizeof(AliasTable));
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/* Model mapping: subtype 0x40, label "llmmodel" -- same offset, size and
 * subtype as the original sketch's "model" partition, renamed because
 * esp-sr's build system claims the "model" name for srmodels.bin.     */

static int map_model(void) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "llmmodel");
    if (!part) {
        printf("no 'llmmodel' partition -- check partitions.csv\n");
        return -1;
    }
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &model_base, &handle);
    if (err != ESP_OK) {
        printf("mmap failed: %d\n", err);
        return -1;
    }
    int rc = llm_load(&model, model_base);
    if (rc == -1) { printf("bad magic -- is model.bin flashed?\n"); return -1; }
    if (rc == -2) { printf("model exceeds the compile-time ceilings in llm.h\n"); return -1; }
    return 0;
}

int llmrt_init(void) {
    run = (Run *)heap_caps_malloc(sizeof(Run), MALLOC_CAP_SPIRAM);
    if (!run) run = (Run *)malloc(sizeof(Run));
    if (!run) { printf("out of memory for the KV cache\n"); return -1; }

    if (map_model() != 0) return -1;

    printf("model: V=%d D=%d L=%d H=%d  (%u KB mapped)\n",
           model.c.vocab, model.c.dim, model.c.n_layers, model.c.n_heads,
           (unsigned)(llm_size(&model) / 1024));

    int n_allow;
    const int *allow = gpio_allowed_pins(&n_allow);
    printf("pins: %d usable (%d..%d)\n", n_allow, allow[0], allow[n_allow - 1]);

    alias_load();
    printf("aliases: %d stored\n", aliases.n);

    /* Seed "台灯" = 4 so the voice phrase 打开台灯 works on first boot.
     * The zh model emits the name exactly as the utterance spells it, so
     * the seed must be the same UTF-8 string the voice mapping sends
     * (main.c: "打开台灯" -- not the English "desk lamp" the EN model
     * used to get). Only seeds when the name is absent: a user-edited
     * table is never overwritten. */
    if (alias_find(&aliases, "台灯") < 0) {
        int lamp[1] = { 1 };
        if (alias_set(&aliases, "台灯", lamp, 1) == 0) {
            alias_save();
            printf("seeded alias: \"台灯\" -> 1\n");
        }
    }
    return 0;
}

/* text -> emitted symbol ids. Mirrors host_verify/repl.c exactly. */
static int generate(const char *text, int *out, int max_out) {
    static int ids[MAX_IDS];
    int n = tok_encode(&bpe, text, ids, MAX_IDS - 1);

    int budget = model.c.seq_len - MAX_NEW - 1;
    if (n > budget) { memmove(ids, ids + (n - budget), budget * sizeof(int)); n = budget; }
    ids[n++] = SYM_GO;

    llm_reset(run);
    for (int i = 0; i < n; i++) llm_forward(&model, run, ids[i]);

    int emitted = 0;
    while (emitted < max_out && emitted < MAX_NEW) {
        int next = llm_argmax(run->logits, model.c.vocab);
        out[emitted++] = next;
        if (next == SYM_END) break;
        llm_forward(&model, run, next);
    }
    return emitted;
}

/* Typed `alias` commands, ported verbatim from espcontrol.ino's
 * handle_alias(). Voice never reaches here -- MultiNet only recognises the
 * phrases we registered -- but the serial REPL keeps the same interface.
 *
 *   alias                      list
 *   alias desk lamp = 4        create or replace
 *   alias del desk lamp        remove
 *
 * Returns true when the line was an alias command and has been dealt with. */
static bool alias_typed(const char *line, char *msg, size_t msg_size) {
    if (strncmp(line, "alias", 5) != 0 || (line[5] && line[5] != ' ')) return false;
    const char *arg = line + 5;
    while (*arg == ' ') arg++;

    if (!*arg) {
        if (!aliases.n) { snprintf(msg, msg_size, "no aliases yet -- try: alias desk lamp = 4"); return true; }
        size_t used = 0;
        for (int i = 0; i < aliases.n; i++) {
            int w = snprintf(msg + used, msg_size - used, "  %-24s ->", aliases.e[i].name);
            if (w < 0 || (size_t)w >= msg_size - used) break;
            used += w;
            for (int k = 0; k < aliases.e[i].n_pins; k++) {
                w = snprintf(msg + used, msg_size - used, " %d", aliases.e[i].pins[k]);
                if (w < 0 || (size_t)w >= msg_size - used) break;
                used += w;
            }
            w = snprintf(msg + used, msg_size - used, "; ");
            if (w < 0 || (size_t)w >= msg_size - used) break;
            used += w;
        }
        return true;
    }

    if (strncmp(arg, "del ", 4) == 0) {
        const char *nm = arg + 4;
        while (*nm == ' ') nm++;
        snprintf(msg, msg_size, "%s", alias_remove(&aliases, nm) == 0 ? "removed" : "no such alias");
        alias_save();
        return true;
    }

    const char *eq = strchr(arg, '=');
    if (!eq) { snprintf(msg, msg_size, "usage: alias <name> = <pin> [pin ...]"); return true; }

    char nm[ALIAS_NAME_BUF];
    int len = (int)(eq - arg);
    while (len > 0 && arg[len - 1] == ' ') len--;
    if (len <= 0 || len >= (int)sizeof(nm)) { snprintf(msg, msg_size, "bad name"); return true; }
    memcpy(nm, arg, (size_t)len);
    nm[len] = 0;

    int pins[ALIAS_MAX_PINS], np = 0;
    for (const char *p = eq + 1; *p && np < ALIAS_MAX_PINS; ) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if (*p < '0' || *p > '9') { snprintf(msg, msg_size, "pins must be numbers"); return true; }
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        pins[np++] = v;
    }
    if (!np) { snprintf(msg, msg_size, "give at least one pin"); return true; }

    /* Checked here so a name cannot be bound to a pin the board does not have. */
    int n_allow;
    const int *allow = gpio_allowed_pins(&n_allow);
    for (int i = 0; i < np; i++) {
        bool ok = false;
        for (int j = 0; j < n_allow; j++) if (allow[j] == pins[i]) { ok = true; break; }
        if (!ok) { snprintf(msg, msg_size, "pin %d is not a GPIO on this board", pins[i]); return true; }
    }

    if (alias_set(&aliases, nm, pins, np) != 0) {
        snprintf(msg, msg_size, "table is full (%d entries)", ALIAS_MAX);
        return true;
    }
    alias_save();
    size_t used = (size_t)snprintf(msg, msg_size, "\"%s\" ->", nm);
    for (int i = 0; i < np && used < msg_size; i++) {
        int w = snprintf(msg + used, msg_size - used, " %d", pins[i]);
        if (w < 0 || (size_t)w >= msg_size - used) break;
        used += w;
    }
    return true;
}

int llmrt_run_text(const char *text, char *msg, size_t msg_size, uint32_t *ms) {
    return llmrt_run_text_ex(text, msg, msg_size, ms, NULL);
}

int llmrt_run_text_ex(const char *text, char *msg, size_t msg_size, uint32_t *ms,
                      llmrt_outcome_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->action = -1;
        out->verdict = -1;
    }
    msg[0] = 0;
    if (!text || !text[0]) { snprintf(msg, msg_size, "empty input"); return -1; }
    if (alias_typed(text, msg, msg_size)) {
        if (out) { out->executed = 1; out->action = ACT_ALIAS; out->verdict = 0; }
        return 0;
    }

    uint32_t t0 = now_ms();
    int sym[MAX_NEW];                 /* not `out`: that is the outcome pointer */
    int n = generate(text, sym, MAX_NEW);
    uint32_t took = now_ms() - t0;
    if (ms) *ms = took;

    Command c;
    if (cmd_parse(sym, n, &bpe, &c) != 0) {
        snprintf(msg, msg_size, "refused: I don't understand that   (%d symbols)", n);
        return -1;
    }

    char buf[128];

    /* Alias creation is a typed command; voice never reaches here because
     * MultiNet only recognises the phrases we registered. Keep the typed
     * path honest anyway: range-check before the table is touched. */
    if (c.action == ACT_ALIAS) {
        int n_allow;
        const int *allow = gpio_allowed_pins(&n_allow);
        Verdict rv = cmd_range_check(&c, allow, n_allow, buf, sizeof(buf));
        if (rv != VERDICT_EXECUTE) {
            if (out) { out->action = ACT_ALIAS; out->verdict = rv; }
            snprintf(msg, msg_size, "refused: %s   (%lums)", buf, (unsigned long)took);
            return -1;
        }
        if (alias_apply(&c, &aliases, buf, sizeof(buf)) != 0) {
            if (out) { out->action = ACT_ALIAS; out->verdict = VERDICT_UNKNOWN; }
            snprintf(msg, msg_size, "refused: %s   (%lums)", buf, (unsigned long)took);
            return -1;
        }
        alias_save();
        if (out) { out->executed = 1; out->action = ACT_ALIAS; out->verdict = rv; }
        snprintf(msg, msg_size, "%s   (%lums)", buf, (unsigned long)took);
        return 0;
    }

    /* Resolution first: an unknown name is not a bad pin. */
    Verdict v = alias_resolve(&c, &aliases, buf, sizeof(buf));
    if (v != VERDICT_EXECUTE) {
        if (out) { out->action = c.action; out->verdict = v; }
        snprintf(msg, msg_size, "refused: %s   (%lums)", buf, (unsigned long)took);
        return -1;
    }

    v = gpio_execute(&c, buf, sizeof(buf));
    if (out) {
        out->executed = (v == VERDICT_EXECUTE);
        out->action   = c.action;
        out->level    = c.level;
        out->all      = c.all;
        out->verdict  = v;
        out->n_pins   = c.n_pins;
        for (int i = 0; i < c.n_pins && i < 16; i++) out->pins[i] = c.pins[i];
    }
    snprintf(msg, msg_size, "%s%s   (%lums)",
             v == VERDICT_EXECUTE ? "" : "refused: ", buf, (unsigned long)took);
    return v == VERDICT_EXECUTE ? 0 : -1;
}

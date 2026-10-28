#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The esp32-gpio-llm runtime, ported from the espcontrol Arduino sketch to
 * plain ESP-IDF. Same pipeline, same guarantees:
 *
 *   text -> tok_encode -> 312K transformer (flash-mapped weights)
 *        -> emitted symbols -> Command -> alias_resolve -> gpio_execute
 *
 * The model itself is NOT linked in: it is memory-mapped from the "model"
 * partition (subtype 0x40) at boot, exactly like the original sketch.
 */

/** Map the model partition, load the BPE tables and the alias table.
 *  Returns 0 on success; on failure the caller should not call llmrt_run. */
int llmrt_init(void);

/**
 * What the utterance turned into, for callers that must *speak* the result.
 * The mouth cannot read the English log line, and parsing it back would couple
 * the two: this struct is the same Command the GPIO layer executed.
 */
typedef struct {
    int executed;      /* 1 = the command ran; 0 = refused */
    int action;        /* CmdAction; -1 when the symbols did not parse at all */
    int level;         /* CmdLevel (LVL_HIGH / LVL_LOW / LVL_TOGGLE) */
    int all;           /* the <all> target was used ("把所有灯都关掉") */
    int n_pins;        /* pins after alias resolution */
    int pins[16];      /* CMD_MAX_PINS; only the first n_pins are meaningful */
    int verdict;       /* Verdict; -1 when the model's symbols did not parse */
} llmrt_outcome_t;

/**
 * Run one utterance through the model and (on a valid parse) the GPIO layer.
 * Writes a human-readable result into msg -- what happened, or why it was
 * refused -- and returns 0 when a command was executed, -1 on refusal.
 * *ms receives how long the model took.
 */
int llmrt_run_text(const char *text, char *msg, size_t msg_size, uint32_t *ms);

/** Same, plus the outcome. Pass out = NULL if you only want the text. */
int llmrt_run_text_ex(const char *text, char *msg, size_t msg_size, uint32_t *ms,
                      llmrt_outcome_t *out);

#ifdef __cplusplus
}
#endif

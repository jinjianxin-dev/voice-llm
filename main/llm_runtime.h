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
 * Run one utterance through the model and (on a valid parse) the GPIO layer.
 * Writes a human-readable result into msg -- what happened, or why it was
 * refused -- and returns 0 when a command was executed, -1 on refusal.
 * *ms receives how long the model took.
 */
int llmrt_run_text(const char *text, char *msg, size_t msg_size, uint32_t *ms);

#ifdef __cplusplus
}
#endif

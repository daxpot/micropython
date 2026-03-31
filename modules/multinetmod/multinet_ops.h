/*
 * MultiNet Operations - Feed Mode Implementation
 *
 * Manages MultiNet model lifecycle, custom command registration via pinyin,
 * and provides a streaming detect interface with internal PCM buffering.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MULTINET_OPS_H
#define MULTINET_OPS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct multinet_ops multinet_ops_t;

/* Detection result */
typedef struct {
    int command_id;
    const char *phrase;
    float prob;
} multinet_result_t;

/* Create MultiNet instance. Loads model from flash partition.
 * Returns NULL on failure. */
multinet_ops_t *multinet_ops_create(void);

/* Destroy instance and free all resources. */
void multinet_ops_destroy(multinet_ops_t *ops);

/* Clear all registered commands. */
int multinet_ops_clear_commands(multinet_ops_t *ops);

/* Add a command. pinyin is space-separated (e.g. "xiao yu tong xue").
 * Returns 0 on success, -1 on error. */
int multinet_ops_add_command(multinet_ops_t *ops, int command_id,
                             const char *pinyin);

/* Remove a command by pinyin string. Returns 0 on success. */
int multinet_ops_remove_command(multinet_ops_t *ops, const char *pinyin);

/* Compile commands into FST language model. Must be called after
 * add/remove/clear before detect() will work. Returns 0 on success. */
int multinet_ops_update(multinet_ops_t *ops);

/* Feed PCM samples and run detection.
 * Returns true if a command was detected (fills result).
 * Returns false if no detection yet. */
bool multinet_ops_detect(multinet_ops_t *ops, const int16_t *samples,
                         int num_samples, multinet_result_t *result);

/* Reset internal accumulation buffer. */
void multinet_ops_clear(multinet_ops_t *ops);

/* Set detection threshold (0.01 - 0.99, lower = more sensitive). */
int multinet_ops_set_threshold(multinet_ops_t *ops, float threshold);

/* Query model parameters. */
int multinet_ops_get_chunk_samples(multinet_ops_t *ops);
int multinet_ops_get_sample_rate(multinet_ops_t *ops);

/* Check if commands have been registered and updated. */
bool multinet_ops_is_ready(multinet_ops_t *ops);

#endif /* MULTINET_OPS_H */

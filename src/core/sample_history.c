#include "dab/core/sample_history.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void dab_sample_history_write(dab_sample_history_t *history,
                                     uint32_t position,
                                     const int16_t *src,
                                     uint32_t sample_count) {
    const uint32_t first_count = sample_count < (history->capacity_samples - position)
                                     ? sample_count
                                     : (history->capacity_samples - position);
    const uint32_t second_count = sample_count - first_count;
    const size_t first_bytes = sizeof(*history->iq) * 2u * first_count;
    const size_t second_bytes = sizeof(*history->iq) * 2u * second_count;

    memcpy(history->iq + (2u * position), src, first_bytes);
    memcpy(history->iq + (2u * (position + history->capacity_samples)), src, first_bytes);
    if (second_count != 0u) {
        src += 2u * first_count;
        memcpy(history->iq, src, second_bytes);
        memcpy(history->iq + (2u * history->capacity_samples), src, second_bytes);
    }
}

static void dab_sample_history_reset_from_block(dab_sample_history_t *history,
                                                const dab_sdr_block_view_t *block) {
    const uint32_t keep = block->sample_count > history->capacity_samples
                              ? history->capacity_samples
                              : block->sample_count;
    const uint32_t skip = block->sample_count - keep;

    history->sample_count = 0u;
    history->write_pos = 0u;
    history->start_sample_index = block->sample_index + skip;
    if (keep == 0u) {
        return;
    }

    dab_sample_history_write(history, 0u, block->iq_interleaved + (2u * skip), keep);
    history->sample_count = keep;
    history->write_pos = keep == history->capacity_samples ? 0u : keep;
}

int dab_sample_history_init(dab_sample_history_t *history, uint32_t capacity_samples) {
    if (history == 0 || capacity_samples == 0u ||
        (size_t)capacity_samples > (SIZE_MAX / (sizeof(int16_t) * 4u))) {
        return -1;
    }

    *history = (dab_sample_history_t){0};
    history->iq = (int16_t *)malloc(sizeof(*history->iq) * 4u * (size_t)capacity_samples);
    if (history->iq == 0) {
        return -1;
    }
    history->capacity_samples = capacity_samples;
    return 0;
}

void dab_sample_history_destroy(dab_sample_history_t *history) {
    if (history == 0) {
        return;
    }
    free(history->iq);
    *history = (dab_sample_history_t){0};
}

void dab_sample_history_clear(dab_sample_history_t *history) {
    if (history == 0) {
        return;
    }
    history->sample_count = 0u;
    history->write_pos = 0u;
    history->start_sample_index = 0u;
}

int dab_sample_history_append(dab_sample_history_t *history, const dab_sdr_block_view_t *block) {
    uint64_t expected_next;
    uint32_t drop;

    if (history == 0 || block == 0 || history->iq == 0 || block->iq_interleaved == 0 ||
        history->capacity_samples == 0u) {
        return -1;
    }
    if (block->sample_count == 0u) {
        return 0;
    }
    if (history->sample_count == 0u || block->sample_count > history->capacity_samples) {
        dab_sample_history_reset_from_block(history, block);
        return 0;
    }

    expected_next = history->start_sample_index + history->sample_count;
    if (expected_next < history->start_sample_index || block->sample_index != expected_next) {
        dab_sample_history_reset_from_block(history, block);
        return 0;
    }

    if (history->sample_count > history->capacity_samples - block->sample_count) {
        drop = history->sample_count - (history->capacity_samples - block->sample_count);
        history->sample_count -= drop;
        history->start_sample_index += drop;
    }

    dab_sample_history_write(history, history->write_pos, block->iq_interleaved, block->sample_count);
    history->write_pos += block->sample_count;
    if (history->write_pos >= history->capacity_samples) {
        history->write_pos -= history->capacity_samples;
    }
    history->sample_count += block->sample_count;
    return 0;
}

dab_sdr_block_view_t dab_sample_history_view(const dab_sample_history_t *history) {
    dab_sdr_block_view_t view = {0};
    uint32_t start_pos;

    if (history == 0 || history->iq == 0 || history->capacity_samples == 0u ||
        history->sample_count == 0u) {
        return view;
    }

    start_pos = history->write_pos >= history->sample_count
                    ? history->write_pos - history->sample_count
                    : history->capacity_samples - (history->sample_count - history->write_pos);
    view.iq_interleaved = history->iq + (2u * start_pos);
    view.sample_count = history->sample_count;
    view.sample_index = history->start_sample_index;
    view.timestamp_ns = 0u;
    return view;
}

int dab_sample_history_contains_range(const dab_sample_history_t *history,
                                      uint64_t start_sample,
                                      uint64_t end_sample) {
    uint64_t history_end;

    if (history == 0 || history->sample_count == 0u || end_sample < start_sample) {
        return 0;
    }
    history_end = history->start_sample_index + history->sample_count;
    if (history_end < history->start_sample_index) {
        return 0;
    }
    return start_sample >= history->start_sample_index && end_sample <= history_end;
}

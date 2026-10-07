#ifndef DAB_RECEIVE_AUDIO_INTERNAL_H
#define DAB_RECEIVE_AUDIO_INTERNAL_H

#include <stdint.h>

enum {
    DAB_RECEIVE_AUDIO_ASC_CAPACITY = 8u
};

typedef struct {
    uint8_t valid;
    uint32_t service_id;
    uint64_t frequency_hz;
    uint32_t sample_rate;
    uint32_t channels;
    uint8_t asc[DAB_RECEIVE_AUDIO_ASC_CAPACITY];
    uint32_t asc_length;
} dab_receive_audio_format_state_t;

/* Returns 1 when the format changed, 0 when unchanged, and -1 on invalid input. */
int dab_receive_audio_format_update(dab_receive_audio_format_state_t *state,
                                    uint32_t service_id,
                                    uint64_t frequency_hz,
                                    uint32_t sample_rate,
                                    uint32_t channels,
                                    const uint8_t *asc,
                                    uint32_t asc_length);

#endif

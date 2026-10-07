#ifndef DAB_RECEIVE_SNAPSHOT_INTERNAL_H
#define DAB_RECEIVE_SNAPSHOT_INTERNAL_H

#include <stdint.h>

#include "dab/core/fic.h"
#include "dab/core/msc.h"
#include "dab/core/sync.h"
#include "dab/receive.h"

void dab_receive_snapshot_build(dab_receive_snapshot_t *snapshot,
                                uint64_t sequence,
                                uint64_t timestamp_ns,
                                uint64_t frequency_hz,
                                uint32_t sample_rate_hz,
                                uint64_t blocks_received,
                                uint32_t dropped_blocks,
                                const dab_sync_t *sync,
                                const dab_fic_state_t *fic,
                                const dab_msc_state_t *msc,
                                const dab_msc_dabplus_debug_t *msc_audio);

#endif

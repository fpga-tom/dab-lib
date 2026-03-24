#include "dab/core/receiver.h"

#include <stdlib.h>

static uint32_t dab_receiver_queue_capacity(const dab_receiver_config_t *config) {
    return config->queue_capacity == 0u ? DAB_DEFAULT_QUEUE_CAPACITY : config->queue_capacity;
}

int dab_receiver_init(dab_receiver_t *receiver, const dab_receiver_config_t *config) {
    uint32_t block_count;
    uint32_t queue_capacity;

    if (receiver == 0 || config == 0) {
        return -1;
    }

    *receiver = (dab_receiver_t){0};
    receiver->config = *config;

    queue_capacity = dab_receiver_queue_capacity(config);
    block_count = config->block_count == 0u ? queue_capacity : config->block_count;

    receiver->blocks = (dab_iq_block_t *)calloc(block_count, sizeof(*receiver->blocks));
    receiver->free_storage = (void **)calloc(block_count, sizeof(*receiver->free_storage));
    receiver->filled_storage = (void **)calloc(queue_capacity, sizeof(*receiver->filled_storage));

    if (receiver->blocks == 0 || receiver->free_storage == 0 || receiver->filled_storage == 0) {
        dab_receiver_shutdown(receiver);
        return -1;
    }

    if (dab_iq_block_pool_init(&receiver->iq_pool, receiver->blocks, receiver->free_storage, block_count) != 0) {
        dab_receiver_shutdown(receiver);
        return -1;
    }

    if (dab_pc_queue_init(&receiver->filled_iq, receiver->filled_storage, queue_capacity) != 0) {
        dab_receiver_shutdown(receiver);
        return -1;
    }

    return 0;
}

void dab_receiver_shutdown(dab_receiver_t *receiver) {
    if (receiver == 0) {
        return;
    }

    if (receiver->filled_storage != 0) {
        dab_pc_queue_stop(&receiver->filled_iq);
        dab_pc_queue_destroy(&receiver->filled_iq);
    }
    if (receiver->free_storage != 0) {
        dab_pc_queue_stop(&receiver->iq_pool.free_blocks);
        dab_iq_block_pool_destroy(&receiver->iq_pool);
    }

    free(receiver->filled_storage);
    free(receiver->free_storage);
    free(receiver->blocks);

    *receiver = (dab_receiver_t){0};
}

#ifndef DAB_CORE_PC_QUEUE_H
#define DAB_CORE_PC_QUEUE_H

#include <stdint.h>

#include "dab/os/os_cond.h"
#include "dab/os/os_mutex.h"

typedef struct {
    void **items;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t high_watermark;
    int stop;
    dab_mutex_t mutex;
    dab_cond_t not_empty;
    dab_cond_t not_full;
} dab_pc_queue_t;

enum {
    DAB_PC_QUEUE_OK = 0,
    DAB_PC_QUEUE_ERROR = -1,
    DAB_PC_QUEUE_TIMEOUT = 1
};

int dab_pc_queue_init(dab_pc_queue_t *queue, void **storage, uint32_t capacity);
void dab_pc_queue_destroy(dab_pc_queue_t *queue);
int dab_pc_queue_push(dab_pc_queue_t *queue, void *item);
int dab_pc_queue_pop(dab_pc_queue_t *queue, void **item);
int dab_pc_queue_pop_timeout(dab_pc_queue_t *queue, void **item, uint32_t timeout_ms);
int dab_pc_queue_try_push(dab_pc_queue_t *queue, void *item);
int dab_pc_queue_try_pop(dab_pc_queue_t *queue, void **item);
void dab_pc_queue_stop(dab_pc_queue_t *queue);
void dab_pc_queue_usage(dab_pc_queue_t *queue,
                        uint32_t *count,
                        uint32_t *high_watermark,
                        uint32_t *capacity);

#endif

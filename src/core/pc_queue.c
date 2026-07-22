#include "dab/core/pc_queue.h"

#include <limits.h>

#include "dab/os/os_time.h"

int dab_pc_queue_init(dab_pc_queue_t *queue, void **storage, uint32_t capacity) {
    if (queue == 0 || storage == 0 || capacity == 0u) {
        return -1;
    }

    queue->items = storage;
    queue->capacity = capacity;
    queue->head = 0u;
    queue->tail = 0u;
    queue->count = 0u;
    queue->high_watermark = 0u;
    queue->stop = 0;

    if (dab_mutex_init(&queue->mutex) != 0) {
        return -1;
    }
    if (dab_cond_init(&queue->not_empty) != 0) {
        dab_mutex_destroy(&queue->mutex);
        return -1;
    }
    if (dab_cond_init(&queue->not_full) != 0) {
        dab_cond_destroy(&queue->not_empty);
        dab_mutex_destroy(&queue->mutex);
        return -1;
    }

    return 0;
}

void dab_pc_queue_destroy(dab_pc_queue_t *queue) {
    if (queue == 0) {
        return;
    }
    dab_cond_destroy(&queue->not_full);
    dab_cond_destroy(&queue->not_empty);
    dab_mutex_destroy(&queue->mutex);
}

static int dab_pc_queue_push_locked(dab_pc_queue_t *queue, void *item) {
    if (queue->count == queue->capacity) {
        return -1;
    }
    queue->items[queue->tail] = item;
    queue->tail = (queue->tail + 1u) % queue->capacity;
    queue->count++;
    if (queue->count > queue->high_watermark) {
        queue->high_watermark = queue->count;
    }
    dab_cond_signal(&queue->not_empty);
    return 0;
}

static int dab_pc_queue_pop_locked(dab_pc_queue_t *queue, void **item) {
    if (queue->count == 0u) {
        return -1;
    }
    *item = queue->items[queue->head];
    queue->head = (queue->head + 1u) % queue->capacity;
    queue->count--;
    dab_cond_signal(&queue->not_full);
    return 0;
}

int dab_pc_queue_push(dab_pc_queue_t *queue, void *item) {
    int rc = 0;
    dab_mutex_lock(&queue->mutex);
    while (!queue->stop && queue->count == queue->capacity) {
        dab_cond_wait(&queue->not_full, &queue->mutex);
    }
    if (queue->stop) {
        rc = -1;
    } else {
        rc = dab_pc_queue_push_locked(queue, item);
    }
    dab_mutex_unlock(&queue->mutex);
    return rc;
}

int dab_pc_queue_pop(dab_pc_queue_t *queue, void **item) {
    int rc = 0;
    dab_mutex_lock(&queue->mutex);
    while (!queue->stop && queue->count == 0u) {
        dab_cond_wait(&queue->not_empty, &queue->mutex);
    }
    if (queue->count == 0u && queue->stop) {
        rc = -1;
    } else {
        rc = dab_pc_queue_pop_locked(queue, item);
    }
    dab_mutex_unlock(&queue->mutex);
    return rc;
}

int dab_pc_queue_try_push(dab_pc_queue_t *queue, void *item) {
    int rc = 0;
    dab_mutex_lock(&queue->mutex);
    if (queue->stop) {
        rc = -1;
    } else {
        rc = dab_pc_queue_push_locked(queue, item);
    }
    dab_mutex_unlock(&queue->mutex);
    return rc;
}

int dab_pc_queue_try_pop(dab_pc_queue_t *queue, void **item) {
    int rc = 0;
    dab_mutex_lock(&queue->mutex);
    rc = dab_pc_queue_pop_locked(queue, item);
    dab_mutex_unlock(&queue->mutex);
    return rc;
}

void dab_pc_queue_stop(dab_pc_queue_t *queue) {
    dab_mutex_lock(&queue->mutex);
    queue->stop = 1;
    dab_cond_broadcast(&queue->not_empty);
    dab_cond_broadcast(&queue->not_full);
    dab_mutex_unlock(&queue->mutex);
}

int dab_pc_queue_pop_timeout(dab_pc_queue_t *queue, void **item, uint32_t timeout_ms) {
    const uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ull;
    const uint64_t started_ns = dab_time_monotonic_ns();
    const uint64_t deadline_ns = UINT64_MAX - started_ns < timeout_ns
        ? UINT64_MAX
        : started_ns + timeout_ns;
    int rc = DAB_PC_QUEUE_OK;

    dab_mutex_lock(&queue->mutex);
    while (!queue->stop && queue->count == 0u) {
        const uint64_t now_ns = dab_time_monotonic_ns();
        uint64_t remaining_ns;
        uint64_t remaining_ms;
        int wait_rc;

        if (now_ns >= deadline_ns) {
            rc = DAB_PC_QUEUE_TIMEOUT;
            break;
        }
        remaining_ns = deadline_ns - now_ns;
        remaining_ms = (remaining_ns + 999999ull) / 1000000ull;
        if (remaining_ms > UINT_MAX) {
            remaining_ms = UINT_MAX;
        }
        wait_rc = dab_cond_wait_timeout(&queue->not_empty,
                                        &queue->mutex,
                                        (uint32_t)remaining_ms);
        if (wait_rc < 0) {
            rc = DAB_PC_QUEUE_ERROR;
            break;
        }
        if (wait_rc == 1 && queue->count == 0u) {
            rc = DAB_PC_QUEUE_TIMEOUT;
            break;
        }
    }
    if (queue->count != 0u) {
        rc = dab_pc_queue_pop_locked(queue, item);
    } else if (queue->stop) {
        rc = DAB_PC_QUEUE_ERROR;
    }
    dab_mutex_unlock(&queue->mutex);
    return rc;
}

void dab_pc_queue_usage(dab_pc_queue_t *queue,
                        uint32_t *count,
                        uint32_t *high_watermark,
                        uint32_t *capacity) {
    if (queue == 0) {
        return;
    }
    dab_mutex_lock(&queue->mutex);
    if (count != 0) {
        *count = queue->count;
    }
    if (high_watermark != 0) {
        *high_watermark = queue->high_watermark;
    }
    if (capacity != 0) {
        *capacity = queue->capacity;
    }
    dab_mutex_unlock(&queue->mutex);
}

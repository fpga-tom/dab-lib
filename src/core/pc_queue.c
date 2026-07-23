#include "dab/core/pc_queue.h"

int dab_pc_queue_init(dab_pc_queue_t *queue, void **storage, uint32_t capacity) {
    if (queue == 0 || storage == 0 || capacity == 0u) {
        return -1;
    }

    queue->items = storage;
    queue->capacity = capacity;
    queue->head = 0u;
    queue->tail = 0u;
    queue->count = 0u;
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

int dab_pc_queue_pop_timeout(dab_pc_queue_t *queue, void **item, uint32_t timeout_ms) {
    int rc = 0;

    dab_mutex_lock(&queue->mutex);
    while (!queue->stop && queue->count == 0u) {
        rc = dab_cond_wait_timeout(&queue->not_empty, &queue->mutex, timeout_ms);
        if (rc != 0) {
            break;
        }
    }
    if (queue->count != 0u) {
        rc = dab_pc_queue_pop_locked(queue, item);
    } else if (queue->stop || rc < 0) {
        rc = -1;
    } else {
        rc = 1;
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

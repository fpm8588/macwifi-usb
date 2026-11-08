// rxq.c — see rxq.h. Original code (MIT).

#include "rxq.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

typedef struct Item {
    struct Item *next;
    size_t n;
    int err;                 // nonzero: an error marker, no data
    uint8_t data[];
} Item;

struct RxQ {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Item *head, *tail;
    size_t items, bytes, max_items, max_bytes, max_depth;
    unsigned long pushed, popped, dropped, truncated;
};

RxQ *rxq_new(size_t max_items, size_t max_bytes) {
    RxQ *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->max_items = max_items;
    q->max_bytes = max_bytes;
    return q;
}

void rxq_free(RxQ *q) {
    if (!q) return;
    for (Item *i = q->head; i;) { Item *n = i->next; free(i); i = n; }
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
    free(q);
}

static void append(RxQ *q, Item *it) {
    it->next = NULL;
    if (q->tail) q->tail->next = it;
    else q->head = it;
    q->tail = it;
    q->items++;
    q->bytes += it->n;
    if (q->items > q->max_depth) q->max_depth = q->items;
    pthread_cond_signal(&q->cv);
}

int rxq_push(RxQ *q, const uint8_t *data, size_t n) {
    pthread_mutex_lock(&q->mu);
    if (q->items >= q->max_items || q->bytes + n > q->max_bytes) {
        q->dropped++;
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    pthread_mutex_unlock(&q->mu);
    Item *it = malloc(sizeof *it + n);          // outside the lock
    if (!it) {
        pthread_mutex_lock(&q->mu);
        q->dropped++;
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    it->n = n;
    it->err = 0;
    memcpy(it->data, data, n);
    pthread_mutex_lock(&q->mu);
    q->pushed++;
    append(q, it);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void rxq_push_error(RxQ *q, int err) {
    Item *it = malloc(sizeof *it);
    pthread_mutex_lock(&q->mu);
    if (it) {
        it->n = 0;
        it->err = err ? err : -1;
        append(q, it);
    }
    pthread_mutex_unlock(&q->mu);
}

long rxq_pop(RxQ *q, uint8_t *buf, size_t cap, int timeout_ms, int *err) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&q->mu);
    while (!q->head) {
        if (pthread_cond_timedwait(&q->cv, &q->mu, &ts) == ETIMEDOUT && !q->head) {
            pthread_mutex_unlock(&q->mu);
            return 0;
        }
    }
    Item *it = q->head;
    if (it->err) {                                  // leave the marker: every later pop reports it too
        if (err) *err = it->err;
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    q->head = it->next;
    if (!q->head) q->tail = NULL;
    q->items--;
    q->bytes -= it->n;
    q->popped++;
    size_t n = it->n;
    if (n > cap) { n = cap; q->truncated++; }
    pthread_mutex_unlock(&q->mu);
    memcpy(buf, it->data, n);
    free(it);
    return (long)n;
}

void rxq_stats(RxQ *q, RxQStats *s) {
    pthread_mutex_lock(&q->mu);
    s->pushed = q->pushed;
    s->popped = q->popped;
    s->dropped = q->dropped;
    s->truncated = q->truncated;
    s->depth = q->items;
    s->max_depth = q->max_depth;
    pthread_mutex_unlock(&q->mu);
}

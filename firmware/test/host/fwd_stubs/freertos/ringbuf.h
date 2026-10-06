#pragma once
// A bounded queue of byte items with the FreeRTOS no-split ring buffer's
// calls. Space is counted as item bytes plus an 8-byte header each.
#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#define RINGBUF_TYPE_NOSPLIT 0
typedef struct rb_item { struct rb_item *next; size_t len; char data[]; } rb_item_t;
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    rb_item_t *head, *tail;
    size_t used, cap;
} *RingbufHandle_t;
static inline RingbufHandle_t xRingbufferCreate(size_t cap, int type)
{
    (void)type;
    RingbufHandle_t r = calloc(1, sizeof(*r));
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->c, NULL);
    r->cap = cap;
    return r;
}
static inline BaseType_t xRingbufferSend(RingbufHandle_t r, const void *p, size_t len, TickType_t t)
{
    (void)t;
    pthread_mutex_lock(&r->m);
    if (r->used + len + 8 > r->cap) {
        pthread_mutex_unlock(&r->m);
        return pdFALSE;
    }
    rb_item_t *it = malloc(sizeof(*it) + len);
    it->next = NULL;
    it->len = len;
    memcpy(it->data, p, len);
    if (r->tail) r->tail->next = it; else r->head = it;
    r->tail = it;
    r->used += len + 8;
    pthread_cond_signal(&r->c);
    pthread_mutex_unlock(&r->m);
    return pdTRUE;
}
static inline void *xRingbufferReceive(RingbufHandle_t r, size_t *len, TickType_t ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&r->m);
    while (!r->head) {
        if (pthread_cond_timedwait(&r->c, &r->m, &ts) == ETIMEDOUT) break;
    }
    rb_item_t *it = r->head;
    if (it) {
        r->head = it->next;
        if (!r->head) r->tail = NULL;
        r->used -= it->len + 8;
        *len = it->len;
    }
    pthread_mutex_unlock(&r->m);
    return it ? it->data : NULL;
}
static inline void vRingbufferReturnItem(RingbufHandle_t r, void *p)
{
    (void)r;
    free((char *)p - offsetof(rb_item_t, data));
}

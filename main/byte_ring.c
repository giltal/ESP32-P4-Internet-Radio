#include "byte_ring.h"

#include <string.h>
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"

#define EV_DATA   BIT0
#define EV_SPACE  BIT1

struct byte_ring {
    uint8_t *buf;
    size_t size;
    size_t head;   /* write position */
    size_t tail;   /* read position */
    size_t fill;
    uint64_t total_in, total_out;
    SemaphoreHandle_t lock;
    EventGroupHandle_t ev;
};

byte_ring_t *byte_ring_create(size_t size)
{
    byte_ring_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->buf) {
        r->buf = malloc(size);
    }
    r->lock = xSemaphoreCreateMutex();
    r->ev = xEventGroupCreate();
    if (!r->buf || !r->lock || !r->ev) {
        free(r->buf);
        free(r);
        return NULL;
    }
    r->size = size;
    return r;
}

size_t byte_ring_write(byte_ring_t *r, const uint8_t *data, size_t len, TickType_t timeout)
{
    size_t done = 0;
    TickType_t start = xTaskGetTickCount();
    while (done < len) {
        xSemaphoreTake(r->lock, portMAX_DELAY);
        size_t space = r->size - r->fill;
        size_t n = len - done < space ? len - done : space;
        if (n) {
            size_t first = r->size - r->head < n ? r->size - r->head : n;
            memcpy(r->buf + r->head, data + done, first);
            memcpy(r->buf, data + done + first, n - first);
            r->head = (r->head + n) % r->size;
            r->fill += n;
            r->total_in += n;
            done += n;
        } else {
            xEventGroupClearBits(r->ev, EV_SPACE);   /* full: wait for the reader */
        }
        xSemaphoreGive(r->lock);
        if (n) {
            xEventGroupSetBits(r->ev, EV_DATA);
            continue;
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            break;
        }
        xEventGroupWaitBits(r->ev, EV_SPACE, pdTRUE, pdFALSE, timeout - elapsed);
    }
    return done;
}

size_t byte_ring_read(byte_ring_t *r, uint8_t *data, size_t len, TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        xSemaphoreTake(r->lock, portMAX_DELAY);
        size_t n = len < r->fill ? len : r->fill;
        if (n) {
            size_t first = r->size - r->tail < n ? r->size - r->tail : n;
            memcpy(data, r->buf + r->tail, first);
            memcpy(data + first, r->buf, n - first);
            r->tail = (r->tail + n) % r->size;
            r->fill -= n;
            r->total_out += n;
        } else {
            xEventGroupClearBits(r->ev, EV_DATA);    /* empty: wait for the writer */
        }
        xSemaphoreGive(r->lock);
        if (n) {
            xEventGroupSetBits(r->ev, EV_SPACE);
            return n;
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return 0;
        }
        xEventGroupWaitBits(r->ev, EV_DATA, pdFALSE, pdFALSE, timeout - elapsed);
    }
}

size_t byte_ring_filled(byte_ring_t *r)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t f = r->fill;
    xSemaphoreGive(r->lock);
    return f;
}

size_t byte_ring_size(byte_ring_t *r)
{
    return r->size;
}

void byte_ring_reset(byte_ring_t *r)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    r->head = r->tail = r->fill = 0;
    r->total_out = r->total_in;
    xEventGroupClearBits(r->ev, EV_DATA);
    xSemaphoreGive(r->lock);
    xEventGroupSetBits(r->ev, EV_SPACE);
}

size_t byte_ring_drop(byte_ring_t *r, size_t len)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    size_t n = len < r->fill ? len : r->fill;
    r->tail = (r->tail + n) % r->size;
    r->fill -= n;
    r->total_out += n;
    xSemaphoreGive(r->lock);
    if (n) {
        xEventGroupSetBits(r->ev, EV_SPACE);
    }
    return n;
}

void byte_ring_totals(byte_ring_t *r, uint64_t *in, uint64_t *out)
{
    xSemaphoreTake(r->lock, portMAX_DELAY);
    *in = r->total_in;
    *out = r->total_out;
    xSemaphoreGive(r->lock);
}

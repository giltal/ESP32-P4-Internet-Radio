/*
 * Single-producer / single-consumer byte ring buffer in PSRAM.
 * Unlike a FreeRTOS stream buffer it can be reset at any time, which is
 * what we need when switching stations.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

typedef struct byte_ring byte_ring_t;

byte_ring_t *byte_ring_create(size_t size);
size_t byte_ring_write(byte_ring_t *r, const uint8_t *data, size_t len, TickType_t timeout);
size_t byte_ring_read(byte_ring_t *r, uint8_t *data, size_t len, TickType_t timeout);
size_t byte_ring_filled(byte_ring_t *r);
size_t byte_ring_size(byte_ring_t *r);
void byte_ring_reset(byte_ring_t *r);
/** Discard up to len of the oldest bytes (FIFO overflow while paused); returns how many. */
size_t byte_ring_drop(byte_ring_t *r, size_t len);
/** Running totals of bytes written and consumed (read or dropped) since creation. */
void byte_ring_totals(byte_ring_t *r, uint64_t *in, uint64_t *out);

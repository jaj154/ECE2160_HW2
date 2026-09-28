#include "io_unit.h"
#include "sensor_hw.h"

void io_unit_init(raw_ring_t *ring)
{
    ring->head = 0;
    ring->count = 0;
    sensor_hw_init();
}

void io_unit_sample(raw_ring_t *ring, uint32_t tick)
{
    /* Overwrite-oldest policy: a real I/O unit favors freshness over
     * completeness when the consumer falls behind, the same tradeoff
     * a junior dev would make against a fixed-size hardware FIFO. */
    raw_sample_t s;
    s.tick = tick;
    s.value_c = sensor_hw_read_raw(tick);
    s.is_outlier = 0;

    ring->buf[ring->head] = s;
    ring->head = (ring->head + 1u) % RAW_RING_SIZE;

    if (ring->count < RAW_RING_SIZE) {
        ring->count++;
    }
}

uint32_t io_unit_drain(raw_ring_t *ring, raw_sample_t *out, uint32_t max_out)
{
    uint32_t n = ring->count < max_out ? ring->count : max_out;
    uint32_t start = (ring->head + RAW_RING_SIZE - ring->count) % RAW_RING_SIZE;

    for (uint32_t i = 0; i < n; i++) {
        out[i] = ring->buf[(start + i) % RAW_RING_SIZE];
    }
    ring->count -= n;
    return n;
}

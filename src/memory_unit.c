#include "memory_unit.h"
#include <math.h>

void memory_unit_init(summary_ring_t *ring)
{
    ring->head = 0;
    ring->count = 0;
}

void memory_unit_process(summary_ring_t *ring, raw_sample_t *samples, uint32_t n)
{
    if (n == 0) return;

    /* Pass 1: rough mean, used only to detect outliers. A junior-dev
     * shortcut - a running mean/variance would avoid the two-pass
     * cost on real hardware, but this stays simple and readable. */
    float sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        sum += samples[i].value_c;
    }
    float rough_mean = sum / (float)n;

    /* Pass 2: reject samples too far from the rough mean, accumulate
     * stats over the survivors only. */
    float min_c = 1e9f, max_c = -1e9f;
    float kept_sum = 0.0f, kept_sq_sum = 0.0f;
    uint16_t kept = 0;

    for (uint32_t i = 0; i < n; i++) {
        float v = samples[i].value_c;
        if (fabsf(v - rough_mean) > OUTLIER_THRESHOLD_C) {
            samples[i].is_outlier = 1;
            continue;
        }
        samples[i].is_outlier = 0;
        if (v < min_c) min_c = v;
        if (v > max_c) max_c = v;
        kept_sum += v;
        kept_sq_sum += v * v;
        kept++;
    }

    summary_record_t rec;
    rec.tick_start   = samples[0].tick;
    rec.tick_end     = samples[n - 1].tick;
    rec.samples_in   = (uint16_t)n;
    rec.samples_kept = kept;

    if (kept > 0) {
        rec.mean_c = kept_sum / (float)kept;
        float variance = (kept_sq_sum / (float)kept) - (rec.mean_c * rec.mean_c);
        rec.stddev_c = variance > 0.0f ? sqrtf(variance) : 0.0f;
        rec.min_c = min_c;
        rec.max_c = max_c;
    } else {
        /* Degenerate window (everything looked like an outlier) -
         * fall back to the rough mean so downstream code never sees
         * uninitialized/garbage stats. */
        rec.mean_c   = rough_mean;
        rec.stddev_c = 0.0f;
        rec.min_c    = rough_mean;
        rec.max_c    = rough_mean;
    }

    ring->buf[ring->head] = rec;
    ring->head = (ring->head + 1u) % SUMMARY_RING_SIZE;
    if (ring->count < SUMMARY_RING_SIZE) {
        ring->count++;
    }
}

uint32_t memory_unit_drain(summary_ring_t *ring, summary_record_t *out, uint32_t max_out)
{
    uint32_t n = ring->count < max_out ? ring->count : max_out;
    uint32_t start = (ring->head + SUMMARY_RING_SIZE - ring->count) % SUMMARY_RING_SIZE;

    for (uint32_t i = 0; i < n; i++) {
        out[i] = ring->buf[(start + i) % SUMMARY_RING_SIZE];
    }
    ring->count -= n;
    return n;
}

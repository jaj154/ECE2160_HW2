#ifndef PIPELINE_WORKLOAD_H
#define PIPELINE_WORKLOAD_H

#include <stdint.h>
#include "pipeline_types.h"
#include "instrumentation.h"

#define MEMORY_WINDOW 16u   /* raw samples ingested per summarization batch */

typedef struct {
    double       wall_seconds;
    uint32_t     total_ticks;
    uint32_t     summaries_produced;
    uint32_t     predictions_made;
    uint64_t     total_samples_in;
    uint64_t     total_samples_kept;
    prediction_t last_prediction;
    int          have_prediction;

    stage_stats_t io_stats;
    stage_stats_t mem_stats;
    stage_stats_t compute_stats;
} pipeline_result_t;

/* Runs the exact same tick loop that was in the original main.c.
 * Re-initializes the sensor (fixed seed) and both rings on every call,
 * so successive calls across trials all see an identical workload.
 * If verbose != 0, prints the original per-prediction printf lines. */
void pipeline_run(uint32_t total_ticks, pipeline_result_t *result, int verbose);

const char *hvac_state_str(hvac_state_t s);

#endif /* PIPELINE_WORKLOAD_H */

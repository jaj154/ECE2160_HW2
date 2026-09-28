#include <stdio.h>
#include <stdint.h>

#include "pipeline_types.h"
#include "pipeline_workload.h"

#define TOTAL_TICKS   4096u

int main(void)
{
    pipeline_result_t result;

    printf("=== Critter Pipeline Simulation (bare-metal desktop stand-in) ===\n");
    printf("Ticks: %u | Memory window: %u samples | Predict window: %u summaries\n\n",
           TOTAL_TICKS, MEMORY_WINDOW, PREDICT_WINDOW);

    pipeline_run(TOTAL_TICKS, &result, /*verbose=*/1);

    printf("\n=== Pipeline Stage Timing (wall clock: %.4f s) ===\n", result.wall_seconds);
    stage_stats_report(&result.io_stats, result.wall_seconds);
    stage_stats_report(&result.mem_stats, result.wall_seconds);
    stage_stats_report(&result.compute_stats, result.wall_seconds);

    printf("\n=== Memory Footprint (why the memory unit exists) ===\n");
    size_t raw_bytes  = (size_t)TOTAL_TICKS * sizeof(raw_sample_t);
    size_t summ_bytes = (size_t)result.summaries_produced * sizeof(summary_record_t);
    printf("  raw samples ingested   : %u  (%zu bytes if all retained)\n", TOTAL_TICKS, raw_bytes);
    printf("  summary records stored : %u  (%zu bytes)\n", result.summaries_produced, summ_bytes);
    if (summ_bytes > 0) {
        printf("  compression ratio      : %.1fx\n", (double)raw_bytes / (double)summ_bytes);
    }

    printf("\n=== Outlier Rejection (memory unit) ===\n");
    uint64_t rejected = result.total_samples_in - result.total_samples_kept;
    double reject_pct = result.total_samples_in ?
        100.0 * (double)rejected / (double)result.total_samples_in : 0.0;
    printf("  samples in   : %llu\n", (unsigned long long)result.total_samples_in);
    printf("  samples kept : %llu\n", (unsigned long long)result.total_samples_kept);
    printf("  rejected     : %llu (%.2f%%)\n", (unsigned long long)rejected, reject_pct);

    printf("\n=== Predictions ===\n");
    printf("  predictions produced : %u\n", result.predictions_made);

    return 0;
}

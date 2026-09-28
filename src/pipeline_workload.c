#include "pipeline_workload.h"
#include "io_unit.h"
#include "memory_unit.h"
#include "compute_unit.h"
#include "sensor_hw.h"
#include <stdio.h>

const char *hvac_state_str(hvac_state_t s)
{
    switch (s) {
        case HVAC_STATE_NOMINAL:       return "NOMINAL";
        case HVAC_STATE_UNDERCOOLING:  return "UNDERCOOLING (rising trend)";
        case HVAC_STATE_OVERCOOLING:   return "OVERCOOLING (falling trend)";
        case HVAC_STATE_SHORT_CYCLING: return "SHORT-CYCLING (oscillation)";
        default:                       return "UNKNOWN";
    }
}

void pipeline_run(uint32_t total_ticks, pipeline_result_t *result, int verbose)
{
    raw_ring_t     raw_ring;
    summary_ring_t summary_ring;

    raw_sample_t     raw_batch[MEMORY_WINDOW];
    summary_record_t predict_batch[PREDICT_WINDOW];

    stage_stats_init(&result->io_stats, "io_unit");
    stage_stats_init(&result->mem_stats, "memory_unit");
    stage_stats_init(&result->compute_stats, "compute_unit");

    sensor_hw_init();               /* fixed seed -> identical dataset every call */
    io_unit_init(&raw_ring);
    memory_unit_init(&summary_ring);

    result->total_ticks         = total_ticks;
    result->summaries_produced  = 0;
    result->predictions_made    = 0;
    result->total_samples_in    = 0;
    result->total_samples_kept  = 0;
    result->have_prediction     = 0;

    double t_start = now_ns();

    for (uint32_t tick = 0; tick < total_ticks; tick++) {

        /* --- I/O unit: high-rate sampling, runs every tick ------------ */
        double t0 = now_ns();
        io_unit_sample(&raw_ring, tick);
        stage_stats_record(&result->io_stats, now_ns() - t0);

        /* --- Memory unit: fires once a full batch has accumulated ----- */
        if ((tick + 1u) % MEMORY_WINDOW == 0u) {
            uint32_t drained = io_unit_drain(&raw_ring, raw_batch, MEMORY_WINDOW);

            double t1 = now_ns();
            memory_unit_process(&summary_ring, raw_batch, drained);
            stage_stats_record(&result->mem_stats, now_ns() - t1);

            result->summaries_produced++;
            result->total_samples_in += drained;
            for (uint32_t i = 0; i < drained; i++) {
                if (!raw_batch[i].is_outlier) result->total_samples_kept++;
            }

            /* --- Compute unit: fires once enough summaries exist ----- */
            if (result->summaries_produced % PREDICT_WINDOW == 0u) {
                uint32_t sdrained = memory_unit_drain(&summary_ring, predict_batch, PREDICT_WINDOW);

                prediction_t pred;
                double t2 = now_ns();
                compute_unit_predict(predict_batch, sdrained, &pred);
                stage_stats_record(&result->compute_stats, now_ns() - t2);

                result->predictions_made++;
                result->last_prediction = pred;
                result->have_prediction = 1;

                if (verbose) {
                    printf("[tick %5u] trend=%+7.4f C/tick  projected=%6.2fC  state=%s\n",
                           pred.tick, pred.trend_slope_c_per_tick,
                           pred.projected_temp_c, hvac_state_str(pred.state));
                }
            }
        }
    }

    double t_end = now_ns();
    result->wall_seconds = (t_end - t_start) / 1e9;
}

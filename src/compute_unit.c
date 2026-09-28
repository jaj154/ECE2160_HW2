#include "compute_unit.h"

void compute_unit_predict(const summary_record_t *records, uint32_t n, prediction_t *out)
{
    if (n == 0) {
        out->tick = 0;
        out->trend_slope_c_per_tick = 0.0f;
        out->projected_temp_c = 0.0f;
        out->state = HVAC_STATE_NOMINAL;
        return;
    }

    /* Least-squares linear regression: mean_c vs. tick_start,
     * recentered on the first tick in the window for numerical
     * stability. */
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
    double x0 = (double)records[0].tick_start;

    for (uint32_t i = 0; i < n; i++) {
        double x = (double)records[i].tick_start - x0;
        double y = (double)records[i].mean_c;
        sum_x  += x;
        sum_y  += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    double denom = ((double)n * sum_xx) - (sum_x * sum_x);
    double slope = 0.0;
    double intercept = sum_y / (double)n;

    if (denom != 0.0) {
        slope = (((double)n * sum_xy) - (sum_x * sum_y)) / denom;
        intercept = (sum_y - slope * sum_x) / (double)n;
    }

    /* Oscillation detection: count sign changes between successive
     * summary means. Frequent flips indicate the HVAC is
     * short-cycling rather than trending steadily. */
    uint32_t sign_changes = 0;
    int last_sign = 0;
    for (uint32_t i = 1; i < n; i++) {
        float delta = records[i].mean_c - records[i - 1].mean_c;
        int sign = (delta > 0.05f) ? 1 : (delta < -0.05f) ? -1 : 0;
        if (sign != 0 && last_sign != 0 && sign != last_sign) {
            sign_changes++;
        }
        if (sign != 0) last_sign = sign;
    }

    double last_x = (double)records[n - 1].tick_start - x0;
    double projected = intercept + slope * (last_x + PROJECTION_TICKS);

    hvac_state_t state;
    if (sign_changes >= OSCILLATION_SIGN_CHANGES) {
        state = HVAC_STATE_SHORT_CYCLING;
    } else if (slope > RISING_THRESHOLD_C_PER_TICK) {
        state = HVAC_STATE_UNDERCOOLING;
    } else if (slope < FALLING_THRESHOLD_C_PER_TICK) {
        state = HVAC_STATE_OVERCOOLING;
    } else {
        state = HVAC_STATE_NOMINAL;
    }

    out->tick = records[n - 1].tick_end;
    out->trend_slope_c_per_tick = (float)slope;
    out->projected_temp_c = (float)projected;
    out->state = state;
}

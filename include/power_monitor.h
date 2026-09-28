#ifndef POWER_MONITOR_H
#define POWER_MONITOR_H

/* Background sampler around `vcgencmd pmic_read_adc`, the mechanism
 * described in the assignment's Power Consumption Hints. The Pi 5's
 * PMIC exposes voltage+current for ~12 internal rails (VDD_CORE,
 * 1V8_SYS, DDR_VDD2, ... ); summing V*I across the rails that have
 * both a matching current and voltage entry gives an estimate of
 * total board power (this excludes downstream 5V/USB/HAT/NVMe draw,
 * which is not visible to the PMIC -- see docs/power_model_notes.md).
 *
 * VDD_CORE is reported separately since it's the rail most directly
 * affected by the clock-speed / core-count / governor experiments. */

typedef struct {
    int    vcgencmd_available;
    long   sample_count;
    double elapsed_seconds;

    double avg_core_power_w;       /* VDD_CORE rail only                 */
    double avg_summed_rails_w;     /* sum of all paired rails (raw PMIC) */
    double avg_corrected_total_w;  /* summed_rails scaled by the linear
                                     * calibration in docs/power_model_notes.md
                                     * (jfikar/RPi5-power); unit-specific,
                                     * treat as an estimate, not ground truth */
    double energy_joules;          /* avg_corrected_total_w * elapsed_seconds */
} power_summary_t;

typedef struct power_monitor power_monitor_t;

/* Starts a background sampling thread polling vcgencmd every
 * interval_ms. Returns NULL on failure to start the thread (caller
 * should still be able to proceed with vcgencmd_available=0). */
power_monitor_t *power_monitor_start(long interval_ms);

/* Stops the thread and fills *out with the accumulated summary. */
void power_monitor_stop(power_monitor_t *pm, power_summary_t *out);

#endif /* POWER_MONITOR_H */

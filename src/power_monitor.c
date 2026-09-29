#define _POSIX_C_SOURCE 200809L
#include "power_monitor.h"
#include "pmic_read.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Empirical linear correction from jfikar/RPi5-power (README, measured
 * against a USB-C power meter on one specific Pi 5 unit):
 *   real_consumption_W = pmic_output_W * 1.1451 + 0.5879
 * This is a per-unit calibration, not a universal constant. The
 * assignment's Hint 1/2 explicitly suggest cross-checking against a
 * real meter -- if the team has one, recalibrate these two constants
 * for your specific board and note it in the paper. */
#define RPI5_POWER_CAL_SLOPE     1.1451
#define RPI5_POWER_CAL_INTERCEPT 0.5879

struct power_monitor {
    pthread_t       thread;
    volatile int    running;
    long            interval_ms;

    pthread_mutex_t lock;
    long            sample_count;
    double          sum_core_power_w;
    double          sum_total_power_w;
    int             vcgencmd_available; /* -1 unknown, 0 no, 1 yes */

    /* In-run cpu0 frequency accumulators (sampled alongside power). */
    long            freq_sample_count;
    double          sum_freq_khz;
    long            min_freq_khz;
    long            max_freq_khz;

    struct timespec t_start;
};

/* Reads cpu0's current scaling frequency (kHz) from sysfs. Returns -1
 * if unavailable (e.g. non-Pi/no cpufreq). */
static long read_cpu0_freq_khz(void)
{
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (!f) return -1;
    long khz = -1;
    if (fscanf(f, "%ld", &khz) != 1) khz = -1;
    fclose(f);
    return khz;
}

/* Sampling now delegates to the pmic_read backend (fast hwmon direct
 * reads where available, vcgencmd fallback otherwise), initialized once
 * by power_monitor_start via pmic_init(). */
static int sample_once(double *out_core_power, double *out_total_power)
{
    return pmic_sample(out_core_power, out_total_power);
}

static void *monitor_thread_fn(void *arg)
{
    power_monitor_t *pm = (power_monitor_t *)arg;
    struct timespec ts;
    ts.tv_sec  = pm->interval_ms / 1000;
    ts.tv_nsec = (pm->interval_ms % 1000) * 1000000L;

    while (pm->running) {
        double core_p, total_p;
        int rc = sample_once(&core_p, &total_p);
        long freq = read_cpu0_freq_khz();   /* sampled DURING load */

        pthread_mutex_lock(&pm->lock);
        if (pm->vcgencmd_available < 0) {
            pm->vcgencmd_available = (rc == 0) ? 1 : 0;
        }
        if (rc == 0) {
            pm->sample_count++;
            pm->sum_core_power_w  += core_p;
            pm->sum_total_power_w += total_p;
        }
        if (freq > 0) {
            if (pm->freq_sample_count == 0) {
                pm->min_freq_khz = freq;
                pm->max_freq_khz = freq;
            } else {
                if (freq < pm->min_freq_khz) pm->min_freq_khz = freq;
                if (freq > pm->max_freq_khz) pm->max_freq_khz = freq;
            }
            pm->freq_sample_count++;
            pm->sum_freq_khz += (double)freq;
        }
        pthread_mutex_unlock(&pm->lock);

        nanosleep(&ts, NULL);
    }
    return NULL;
}

power_monitor_t *power_monitor_start(long interval_ms)
{
    power_monitor_t *pm = calloc(1, sizeof(power_monitor_t));
    if (!pm) return NULL;

    pmic_init();   /* select fast hwmon path or vcgencmd fallback (idempotent) */

    pm->interval_ms = (interval_ms > 0) ? interval_ms : 200;
    pm->running = 1;
    pm->vcgencmd_available = -1;
    pthread_mutex_init(&pm->lock, NULL);
    clock_gettime(CLOCK_MONOTONIC, &pm->t_start);

    if (pthread_create(&pm->thread, NULL, monitor_thread_fn, pm) != 0) {
        free(pm);
        return NULL;
    }
    return pm;
}

void power_monitor_stop(power_monitor_t *pm, power_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!pm) {
        out->vcgencmd_available = 0;
        return;
    }

    pm->running = 0;
    pthread_join(pm->thread, NULL);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - pm->t_start.tv_sec) +
                      (t_end.tv_nsec - pm->t_start.tv_nsec) / 1e9;

    pthread_mutex_lock(&pm->lock);
    out->vcgencmd_available = (pm->vcgencmd_available == 1);
    out->sample_count       = pm->sample_count;
    out->elapsed_seconds    = elapsed;

    if (pm->sample_count > 0) {
        out->avg_core_power_w   = pm->sum_core_power_w  / (double)pm->sample_count;
        out->avg_summed_rails_w = pm->sum_total_power_w / (double)pm->sample_count;
        out->avg_corrected_total_w =
            out->avg_summed_rails_w * RPI5_POWER_CAL_SLOPE + RPI5_POWER_CAL_INTERCEPT;
        out->energy_joules = out->avg_corrected_total_w * elapsed;
    }

    out->freq_sample_count = pm->freq_sample_count;
    if (pm->freq_sample_count > 0) {
        out->avg_freq_khz = pm->sum_freq_khz / (double)pm->freq_sample_count;
        out->min_freq_khz = pm->min_freq_khz;
        out->max_freq_khz = pm->max_freq_khz;
    }
    pthread_mutex_unlock(&pm->lock);

    pthread_mutex_destroy(&pm->lock);
    free(pm);
}

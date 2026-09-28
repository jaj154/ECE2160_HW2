#define _POSIX_C_SOURCE 200809L
#include "power_monitor.h"
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

#define MAX_RAILS 24

typedef struct {
    char   base[24];
    double volt;
    double curr;
    int    have_volt;
    int    have_curr;
} rail_t;

struct power_monitor {
    pthread_t       thread;
    volatile int    running;
    long            interval_ms;

    pthread_mutex_t lock;
    long            sample_count;
    double          sum_core_power_w;
    double          sum_total_power_w;
    int             vcgencmd_available; /* -1 unknown, 0 no, 1 yes */

    struct timespec t_start;
};

static void strip_suffix(char *name, const char *suffix)
{
    size_t nlen = strlen(name), slen = strlen(suffix);
    if (nlen > slen && strcmp(name + nlen - slen, suffix) == 0) {
        name[nlen - slen] = '\0';
    }
}

static rail_t *find_or_add_rail(rail_t *rails, int *count, const char *base)
{
    for (int i = 0; i < *count; i++) {
        if (strcmp(rails[i].base, base) == 0) return &rails[i];
    }
    if (*count >= MAX_RAILS) return NULL;
    rail_t *r = &rails[*count];
    memset(r, 0, sizeof(*r));
    strncpy(r->base, base, sizeof(r->base) - 1);
    (*count)++;
    return r;
}

/* Parses one full `vcgencmd pmic_read_adc` invocation. Returns 0 on
 * success (command ran and produced at least one parsable line), -1
 * if the command couldn't be run at all (no vcgencmd on this system). */
static int sample_once(double *out_core_power, double *out_total_power)
{
    FILE *fp = popen("vcgencmd pmic_read_adc 2>/dev/null", "r");
    if (!fp) return -1;

    rail_t rails[MAX_RAILS];
    int rail_count = 0;
    char line[128];
    int any_line = 0;

    while (fgets(line, sizeof(line), fp)) {
        char name[32], rest[64];
        if (sscanf(line, "%31s %63s", name, rest) != 2) continue;
        char *eq = strchr(rest, '=');
        if (!eq) continue;
        double val = atof(eq + 1);

        int is_current = (strstr(name, "_A") == name + strlen(name) - 2);
        int is_volt    = (strstr(name, "_V") == name + strlen(name) - 2);
        if (!is_current && !is_volt) continue;

        char base[32];
        strncpy(base, name, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        strip_suffix(base, is_current ? "_A" : "_V");

        rail_t *r = find_or_add_rail(rails, &rail_count, base);
        if (!r) continue;
        if (is_current) { r->curr = val; r->have_curr = 1; }
        else            { r->volt = val; r->have_volt = 1; }
        any_line = 1;
    }
    int status = pclose(fp);

    if (!any_line || status != 0) return -1;

    double total = 0.0, core = 0.0;
    for (int i = 0; i < rail_count; i++) {
        if (rails[i].have_volt && rails[i].have_curr) {
            double p = rails[i].volt * rails[i].curr;
            total += p;
            if (strcmp(rails[i].base, "VDD_CORE") == 0) core = p;
        }
    }
    *out_core_power = core;
    *out_total_power = total;
    return 0;
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

        pthread_mutex_lock(&pm->lock);
        if (pm->vcgencmd_available < 0) {
            pm->vcgencmd_available = (rc == 0) ? 1 : 0;
        }
        if (rc == 0) {
            pm->sample_count++;
            pm->sum_core_power_w  += core_p;
            pm->sum_total_power_w += total_p;
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
    pthread_mutex_unlock(&pm->lock);

    pthread_mutex_destroy(&pm->lock);
    free(pm);
}

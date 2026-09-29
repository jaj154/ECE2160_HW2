/* run_experiments.c  (v3: refined measurement methodology)
 *
 * The test-case driver for the assignment. Sweeps the three "option 1"
 * device-configuration knobs -- clock speed, active core count, and
 * cpufreq governor -- against the full Critter pipeline (io_unit ->
 * memory_unit -> compute_unit) and measures power via the Pi 5 PMIC.
 *
 * --- What changed from v2 -> v3 (measurement refinements) ----------
 * v2 produced one usable headline result (min clock speed COSTS ~22%
 * more energy per unit work, not less) but three measurement
 * artifacts muddied the smaller effects. v3 fixes the artifacts so the
 * evaluation approach -- not just the numbers -- is defensible:
 *
 *   1. SETTLE BEFORE CALIBRATE. v2 timed its calibration pass
 *      immediately after applying a config (e.g. right after offlining
 *      cores or switching governor), while the system was still
 *      transitioning. That skewed the iteration count and gave the
 *      core-count trials short, low-sample windows. v3 sleeps
 *      SETTLE_MS after applying config, and does a warm-up pass whose
 *      timing is discarded, before calibrating.
 *
 *   2. FREQUENCY SAMPLED DURING LOAD, NOT ONCE BEFORE IT. v2 read
 *      scaling_cur_freq a single time, before the sustained load, so
 *      demand-based governors (ondemand/schedutil) read stale-low.
 *      v3 samples frequency on the same schedule as power, throughout
 *      the run, and reports avg/min/max. (Implemented in
 *      power_monitor.c.)
 *
 *   3. REPEATED TRIALS + VARIANCE. v2 ran each config once, so there
 *      was no way to tell a real 5% effect from 5% run-to-run noise --
 *      which is exactly why the core-count numbers zig-zagged in sign.
 *      v3 repeats each config REPEATS times and reports mean and
 *      sample standard deviation for power and energy, so the paper
 *      can show error bars and state effects with confidence rather
 *      than asserting them.
 *
 * These are precision refinements, not outcome tuning: "clearer" is
 * defined as more samples / tighter variance / labels that match
 * observed behavior, all decided independently of which option wins.
 * The v2 headline (min-clock energy penalty) is EXPECTED to survive,
 * because it is physically grounded (E proportional to V^2 for fixed
 * work; clamping f via performance+min=max does not drop V
 * proportionally) rather than a measurement artifact.
 * -------------------------------------------------------------------
 *
 * Every trial runs the IDENTICAL synthetic workload (fixed RNG seed in
 * sensor_hw.c), so any difference between configs is attributable to
 * the config, not the data.
 *
 * Output: human-readable report to stdout AND results.csv (one row per
 * config, carrying the across-repeats mean and stddev).
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#include "pipeline_workload.h"
#include "power_config.h"
#include "power_monitor.h"

#define TOTAL_TICKS_PER_CALL   4096u
#define POWER_SAMPLE_MS         150   /* vcgencmd + freq poll interval          */
#define TARGET_TRIAL_SECONDS    3.0   /* sustained-load window per repeat        */
#define SETTLE_MS               400   /* wait after applying config before timing*/
#define REPEATS                   5   /* repeats per config, for mean +/- stddev */
#define MIN_ITERATIONS           20u
#define MAX_ITERATIONS       200000u

typedef enum { T_BASELINE, T_CLOCK, T_CORES, T_GOVERNOR } trial_kind_t;

typedef struct {
    trial_kind_t kind;
    char         label[80];
    long         freq_khz;
    int          core_n;
    core_mode_t  core_mode;
    const char  *governor;
} trial_spec_t;

/* One repeat's aggregated outcome. */
typedef struct {
    uint32_t     iterations;
    double       total_wall_seconds;
    uint32_t     predictions_made;
    uint64_t     total_samples_in;
    uint64_t     total_samples_kept;
    prediction_t last_prediction;
    int          have_prediction;

    int          power_ok;
    long         power_sample_count;
    double       avg_core_power_w;
    double       avg_corrected_total_w;
    double       energy_joules_window;
    double       energy_per_prediction_mj;

    long         freq_sample_count;
    double       avg_freq_khz;
    long         min_freq_khz;
    long         max_freq_khz;
} repeat_result_t;

/* Mean/stddev accumulator over repeats. */
typedef struct {
    int    n;
    double sum;
    double sum_sq;
} stat_acc_t;

static void acc_add(stat_acc_t *a, double x) { a->n++; a->sum += x; a->sum_sq += x * x; }
static double acc_mean(const stat_acc_t *a) { return a->n ? a->sum / a->n : 0.0; }
static double acc_stddev(const stat_acc_t *a)
{
    if (a->n < 2) return 0.0;
    double m = acc_mean(a);
    double var = (a->sum_sq / a->n) - (m * m);
    return var > 0.0 ? sqrt(var) : 0.0;
}

static FILE *g_csv = NULL;

static void csv_header(void)
{
    g_csv = fopen("results.csv", "w");
    if (!g_csv) return;
    fprintf(g_csv,
        "trial,category,param,requested_ok,repeats,"
        "avg_freq_khz_inrun_mean,avg_freq_khz_inrun_min,avg_freq_khz_inrun_max,"
        "seconds_per_call_mean,seconds_per_call_std,"
        "core_power_w_mean,core_power_w_std,"
        "corrected_total_w_mean,corrected_total_w_std,"
        "energy_per_prediction_mj_mean,energy_per_prediction_mj_std,"
        "power_sample_count_mean,reject_pct,predictions_per_call,correctness_match\n");
}

static int apply_trial(const trial_spec_t *t)
{
    switch (t->kind) {
        case T_BASELINE:
            return 1;
        case T_CLOCK: {
            int rc = pc_set_freq_khz_all(t->freq_khz);
            long cur = 0;
            pc_get_cur_freq_khz(0, &cur);
            return (rc == 0) && (cur > 0) &&
                   (cur >= t->freq_khz * 0.90) && (cur <= t->freq_khz * 1.10);
        }
        case T_CORES:
            return (pc_set_core_count(t->core_n, t->core_mode) == 0);
        case T_GOVERNOR: {
            int rc = pc_set_governor_all(t->governor);
            char g[64] = "";
            pc_get_governor(0, g, sizeof(g));
            return (rc == 0) && (strcmp(g, t->governor) == 0);
        }
    }
    return 0;
}

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Runs one repeat: warm-up (discarded) -> calibrate -> sustained load
 * with power+freq monitoring. Assumes config is already applied AND the
 * system has been given SETTLE_MS to stabilize by the caller. */
static void run_one_repeat(repeat_result_t *rr)
{
    memset(rr, 0, sizeof(*rr));

    /* Warm-up pass: discarded. Pulls code/data into cache and lets any
     * just-applied DVFS/governor transition finish reacting to load,
     * so the calibration timing below reflects steady state. */
    pipeline_result_t warm;
    pipeline_run(TOTAL_TICKS_PER_CALL, &warm, /*verbose=*/0);

    /* Calibration pass: now measures steady-state per-call time. */
    pipeline_result_t calib;
    pipeline_run(TOTAL_TICKS_PER_CALL, &calib, /*verbose=*/0);

    uint32_t iterations = MIN_ITERATIONS;
    if (calib.wall_seconds > 1e-9) {
        double needed = TARGET_TRIAL_SECONDS / calib.wall_seconds;
        if (needed > (double)MAX_ITERATIONS) needed = (double)MAX_ITERATIONS;
        if (needed > (double)MIN_ITERATIONS) iterations = (uint32_t)needed;
    }

    power_monitor_t *pm = power_monitor_start(POWER_SAMPLE_MS);

    for (uint32_t i = 0; i < iterations; i++) {
        pipeline_result_t r;
        pipeline_run(TOTAL_TICKS_PER_CALL, &r, /*verbose=*/0);
        rr->iterations++;
        rr->total_wall_seconds += r.wall_seconds;
        rr->predictions_made   += r.predictions_made;
        rr->total_samples_in   += r.total_samples_in;
        rr->total_samples_kept += r.total_samples_kept;
        if (r.have_prediction) { rr->last_prediction = r.last_prediction; rr->have_prediction = 1; }
    }

    power_summary_t psum;
    power_monitor_stop(pm, &psum);

    rr->power_ok = psum.vcgencmd_available;
    rr->power_sample_count    = psum.sample_count;
    rr->avg_core_power_w      = psum.avg_core_power_w;
    rr->avg_corrected_total_w = psum.avg_corrected_total_w;
    rr->energy_joules_window  = psum.energy_joules;
    rr->energy_per_prediction_mj = (rr->predictions_made > 0) ?
        (psum.energy_joules / (double)rr->predictions_made) * 1000.0 : 0.0;

    rr->freq_sample_count = psum.freq_sample_count;
    rr->avg_freq_khz      = psum.avg_freq_khz;
    rr->min_freq_khz      = psum.min_freq_khz;
    rr->max_freq_khz      = psum.max_freq_khz;
}

static void run_config(int trial_no, const char *category, const char *param,
                        const trial_spec_t *t, const power_config_snapshot_t *baseline,
                        const prediction_t *golden, int have_golden)
{
    pc_restore(baseline);
    pc_restore_cores();

    printf("\n---------------------------------------------------------------\n");
    printf("Trial %d [%s]: %s\n", trial_no, category, t->label);

    int applied_ok = apply_trial(t);

    long snap_freq = 0;
    char snap_gov[64] = "n/a";
    pc_get_cur_freq_khz(0, &snap_freq);
    pc_get_governor(0, snap_gov, sizeof(snap_gov));

    printf("  requested change applied : %s\n", applied_ok ? "YES (confirmed on hardware)" :
           "NO (permission denied or unsupported -- rerun with sudo; results below reflect"
           " whatever config was actually in effect)");
    printf("  governor (post-apply)    : %s\n", snap_gov);
    printf("  freq snapshot pre-load   : %ld kHz  (may be stale for demand governors --\n", snap_freq);
    printf("                             see in-run avg below, which is the trustworthy one)\n");

    /* Let the just-applied config settle before any timing. */
    msleep(SETTLE_MS);

    stat_acc_t a_sec = {0}, a_core = {0}, a_corr = {0}, a_epp = {0}, a_psamp = {0};
    stat_acc_t a_freq = {0};
    long freq_min_all = 0, freq_max_all = 0;
    int freq_seen = 0;
    int correctness_match = 1;
    double reject_pct = 0.0;
    double predictions_per_call = 0.0;
    int any_power = 0;

    for (int rep = 0; rep < REPEATS; rep++) {
        repeat_result_t rr;
        run_one_repeat(&rr);

        double sec_per_call = (rr.iterations > 0) ?
            rr.total_wall_seconds / (double)rr.iterations : 0.0;
        acc_add(&a_sec, sec_per_call);

        if (rr.power_ok) {
            any_power = 1;
            acc_add(&a_core, rr.avg_core_power_w);
            acc_add(&a_corr, rr.avg_corrected_total_w);
            acc_add(&a_epp,  rr.energy_per_prediction_mj);
            acc_add(&a_psamp, (double)rr.power_sample_count);
        }
        if (rr.freq_sample_count > 0) {
            acc_add(&a_freq, rr.avg_freq_khz);
            if (!freq_seen) { freq_min_all = rr.min_freq_khz; freq_max_all = rr.max_freq_khz; freq_seen = 1; }
            else {
                if (rr.min_freq_khz < freq_min_all) freq_min_all = rr.min_freq_khz;
                if (rr.max_freq_khz > freq_max_all) freq_max_all = rr.max_freq_khz;
            }
        }

        /* Correctness: last prediction must match the golden reference
         * (baseline's) on every repeat of every config. */
        if (have_golden && rr.have_prediction) {
            if (rr.last_prediction.state != golden->state ||
                fabsf(rr.last_prediction.trend_slope_c_per_tick - golden->trend_slope_c_per_tick) > 1e-4f ||
                fabsf(rr.last_prediction.projected_temp_c - golden->projected_temp_c) > 1e-2f) {
                correctness_match = 0;
            }
        }

        uint64_t rejected = rr.total_samples_in - rr.total_samples_kept;
        reject_pct = rr.total_samples_in ? 100.0 * (double)rejected / (double)rr.total_samples_in : 0.0;
        predictions_per_call = (rr.iterations > 0) ?
            (double)rr.predictions_made / (double)rr.iterations : 0.0;
    }

    printf("  --- results over %d repeats (mean +/- sample stddev) ---\n", REPEATS);
    if (freq_seen) {
        printf("    in-run cpu0 frequency   : %.0f kHz avg  (range %ld..%ld kHz across load)\n",
               acc_mean(&a_freq), freq_min_all, freq_max_all);
    }
    printf("    time per pipeline call  : %.6f +/- %.6f s\n", acc_mean(&a_sec), acc_stddev(&a_sec));
    printf("    predictions per call    : %.1f   reject%%: %.2f%%\n", predictions_per_call, reject_pct);
    if (any_power) {
        printf("    avg VDD_CORE power      : %.4f +/- %.4f W\n", acc_mean(&a_core), acc_stddev(&a_core));
        printf("    avg corrected est power : %.4f +/- %.4f W\n", acc_mean(&a_corr), acc_stddev(&a_corr));
        printf("    energy per prediction   : %.5f +/- %.5f mJ\n", acc_mean(&a_epp), acc_stddev(&a_epp));
        printf("    power samples/repeat    : %.1f\n", acc_mean(&a_psamp));
    } else {
        printf("    power: vcgencmd not available -- N/A (expected off-Pi)\n");
    }
    printf("    correctness vs baseline : %s\n", correctness_match ? "MATCH (100%)" : "MISMATCH (!!)");

    if (g_csv) {
        fprintf(g_csv,
            "%d,%s,%s,%d,%d,"
            "%.0f,%ld,%ld,"
            "%.9f,%.9f,"
            "%.4f,%.4f,"
            "%.4f,%.4f,"
            "%.6f,%.6f,"
            "%.1f,%.2f,%.1f,%s\n",
            trial_no, category, param, applied_ok, REPEATS,
            acc_mean(&a_freq), freq_min_all, freq_max_all,
            acc_mean(&a_sec), acc_stddev(&a_sec),
            acc_mean(&a_core), acc_stddev(&a_core),
            acc_mean(&a_corr), acc_stddev(&a_corr),
            acc_mean(&a_epp), acc_stddev(&a_epp),
            acc_mean(&a_psamp), reject_pct, predictions_per_call,
            correctness_match ? "MATCH" : "MISMATCH");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (geteuid() != 0) {
        fprintf(stderr,
            "WARNING: not running as root. Config changes require root to\n"
            "write sysfs; most trials will report 'requested change applied: NO'.\n"
            "Re-run with: sudo ./run_experiments\n\n");
    }

    printf("=== Critter Power Experiment Harness (v3: refined methodology) ===\n");
    printf("Workload: full pipeline (io->memory->compute), %u ticks/call, looped ~%.1f s\n",
           TOTAL_TICKS_PER_CALL, TARGET_TRIAL_SECONDS);
    printf("per repeat, %d repeats per config. Settle %d ms + warm-up pass before timing.\n",
           REPEATS, SETTLE_MS);
    printf("Frequency sampled DURING load. Identical synthetic data every call (fixed seed).\n");

    power_config_snapshot_t baseline;
    pc_snapshot(&baseline);
    printf("Captured baseline config: cpu0 governor=%s, %d cpu(s) present.\n",
           baseline.governor[0][0] ? baseline.governor[0] : "unknown", baseline.num_cpus_present);

    long avail_min = 0, avail_max = 0;
    int have_range = (pc_get_available_freq_range(&avail_min, &avail_max) == 0);
    long f_max = have_range ? avail_max : 2400000;
    long f_min = have_range ? avail_min : 1500000;
    long f_mid = (f_min + f_max) / 2;

    csv_header();

    /* Establish the golden correctness reference from a single baseline
     * pipeline pass before the sweep, so every later repeat can be
     * checked against it. */
    prediction_t golden; int have_golden = 0;
    {
        pipeline_result_t g;
        pipeline_run(TOTAL_TICKS_PER_CALL, &g, /*verbose=*/0);
        if (g.have_prediction) { golden = g.last_prediction; have_golden = 1; }
    }

    int trial_no = 0;

    trial_spec_t base = { T_BASELINE, "Baseline (system default config, unmodified)", 0, 0, CORE_MODE_AFFINITY, NULL };
    run_config(++trial_no, "baseline", "default", &base, &baseline, &golden, have_golden);

    trial_spec_t clk_max = { T_CLOCK, "", f_max, 0, CORE_MODE_AFFINITY, NULL };
    snprintf(clk_max.label, sizeof(clk_max.label), "Clock speed: max (%ld kHz, performance-pinned)", f_max);
    run_config(++trial_no, "clock", "max", &clk_max, &baseline, &golden, have_golden);

    trial_spec_t clk_mid = { T_CLOCK, "", f_mid, 0, CORE_MODE_AFFINITY, NULL };
    snprintf(clk_mid.label, sizeof(clk_mid.label), "Clock speed: mid (%ld kHz, performance-pinned)", f_mid);
    run_config(++trial_no, "clock", "mid", &clk_mid, &baseline, &golden, have_golden);

    trial_spec_t clk_min = { T_CLOCK, "", f_min, 0, CORE_MODE_AFFINITY, NULL };
    snprintf(clk_min.label, sizeof(clk_min.label), "Clock speed: min (%ld kHz, performance-pinned)", f_min);
    run_config(++trial_no, "clock", "min", &clk_min, &baseline, &golden, have_golden);

    trial_spec_t cores4 = { T_CORES, "Core count: 4 active (all online)", 0, 4, CORE_MODE_OFFLINE, NULL };
    run_config(++trial_no, "cores", "4", &cores4, &baseline, &golden, have_golden);

    trial_spec_t cores2 = { T_CORES, "Core count: 2 active (cores 2-3 offline)", 0, 2, CORE_MODE_OFFLINE, NULL };
    run_config(++trial_no, "cores", "2", &cores2, &baseline, &golden, have_golden);

    trial_spec_t cores1 = { T_CORES, "Core count: 1 active (cores 1-3 offline)", 0, 1, CORE_MODE_OFFLINE, NULL };
    run_config(++trial_no, "cores", "1", &cores1, &baseline, &golden, have_golden);

    trial_spec_t gov_perf = { T_GOVERNOR, "Governor: performance (always max freq)", 0, 0, CORE_MODE_AFFINITY, "performance" };
    run_config(++trial_no, "governor", "performance", &gov_perf, &baseline, &golden, have_golden);

    trial_spec_t gov_sched = { T_GOVERNOR, "Governor: schedutil (default demand-based scaling)", 0, 0, CORE_MODE_AFFINITY, "schedutil" };
    run_config(++trial_no, "governor", "schedutil", &gov_sched, &baseline, &golden, have_golden);

    trial_spec_t gov_power = { T_GOVERNOR, "Governor: powersave (always min freq)", 0, 0, CORE_MODE_AFFINITY, "powersave" };
    run_config(++trial_no, "governor", "powersave", &gov_power, &baseline, &golden, have_golden);

    pc_restore(&baseline);
    pc_restore_cores();
    if (g_csv) fclose(g_csv);

    printf("\n=================================================================\n");
    printf("Done. %d configs x %d repeats written to results.csv.\n", trial_no, REPEATS);
    printf("=================================================================\n");
    return 0;
}

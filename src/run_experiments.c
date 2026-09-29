/* run_experiments.c
 *
 * The test-case driver requested by the assignment. Running this on
 * the Pi 5 (as root, see README.md) executes:
 *   1. a baseline trial at whatever config the system is already at
 *   2. a clock-speed sweep (option 1: reduce clock speed)
 *   3. a core-count sweep (option 1: reduce active cores)
 *   4. a cpufreq governor sweep (option 1, policy-level variant)
 *
 * against all three Critter prototype units (io_unit, memory_unit,
 * compute_unit) driven together as one pipeline, exactly as they run
 * in the shipped main.c.
 *
 * --- FIX (v2) -----------------------------------------------------
 * v1 called pipeline_run() exactly ONCE per trial. The whole pipeline
 * finishes in under ~1-3 ms, but `vcgencmd pmic_read_adc` itself takes
 * on the order of 100-300 ms to fork/exec/read the PMIC over I2C. That
 * meant most trials ended before the power-monitor thread's first
 * sample even returned -> 0 or 1 power samples per trial, and the
 * "elapsed_seconds" the harness measured was dominated by that single
 * vcgencmd call's latency, not by the actual workload.
 *
 * v2 fixes this by calibrating each trial (one throwaway pipeline_run
 * call) to see how long a single pass takes under the CURRENT config,
 * then looping pipeline_run() enough times to keep the workload
 * running for TARGET_TRIAL_SECONDS. This gives the power monitor a
 * real sampling window (dozens of vcgencmd reads instead of 0-1), and
 * makes energy-per-prediction a number built on an actual average
 * rather than one race-condition sample. It also gives governors like
 * `ondemand`/`schedutil` enough sustained load to actually ramp
 * frequency, instead of measuring them mid-ramp on a sub-millisecond
 * burst (which is what happened to Trial 5 in the v1 data: ondemand
 * never left 1.6 GHz because the burst was over before it could react).
 * -------------------------------------------------------------------
 *
 * Every trial still runs the IDENTICAL synthetic workload (fixed RNG
 * seed in sensor_hw.c, same TOTAL_TICKS_PER_CALL every iteration), so
 * any difference in time/power between trials is attributable to the
 * configuration change under test, not to different input data.
 *
 * Output: a human-readable report to stdout AND results.csv in the
 * working directory for dropping straight into a spreadsheet /
 * plotting tool for the paper.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pipeline_workload.h"
#include "power_config.h"
#include "power_monitor.h"

#define TOTAL_TICKS_PER_CALL   4096u
#define POWER_SAMPLE_MS         150   /* vcgencmd poll interval while a trial runs */
#define TARGET_TRIAL_SECONDS    3.0   /* how long each trial should keep the
                                        * workload running, so the power
                                        * monitor gets a real sample window */
#define MIN_ITERATIONS           20u
#define MAX_ITERATIONS        20000u  /* safety cap; should never be hit in
                                        * practice since one pass is ~1-3 ms */

typedef enum { T_BASELINE, T_CLOCK, T_CORES, T_GOVERNOR } trial_kind_t;

typedef struct {
    trial_kind_t kind;
    char         label[80];
    long         freq_khz;     /* T_CLOCK   */
    int          core_n;       /* T_CORES   */
    core_mode_t  core_mode;    /* T_CORES   */
    const char  *governor;     /* T_GOVERNOR */
} trial_spec_t;

/* Aggregated result across every pipeline_run() iteration in one trial. */
typedef struct {
    uint32_t     iterations;
    double       total_wall_seconds;   /* sum of pipeline_run()'s own wall time */
    uint32_t     total_ticks;          /* sum across iterations */
    uint32_t     summaries_produced;   /* sum across iterations */
    uint32_t     predictions_made;     /* sum across iterations */
    uint64_t     total_samples_in;
    uint64_t     total_samples_kept;
    prediction_t last_prediction;      /* from the final iteration */
    int          have_prediction;
    stage_stats_t io_stats;
    stage_stats_t mem_stats;
    stage_stats_t compute_stats;
} agg_result_t;

static FILE *g_csv = NULL;

static void csv_header(void)
{
    g_csv = fopen("results.csv", "w");
    if (!g_csv) return;
    fprintf(g_csv,
        "trial,category,param,requested_ok,measured_freq_khz,measured_governor,"
        "iterations,total_wall_seconds,avg_seconds_per_call,"
        "avg_core_power_w,avg_summed_rails_w,avg_corrected_total_w,"
        "power_sample_count,power_window_seconds,"
        "energy_joules,energy_per_prediction_mj,"
        "total_samples_in,total_samples_kept,reject_pct,"
        "total_summaries_produced,total_predictions_made\n");
}

static void csv_row(int trial_no, const char *category, const char *param, int requested_ok,
                     long measured_freq_khz, const char *measured_gov,
                     const agg_result_t *r, const power_summary_t *p)
{
    if (!g_csv) return;
    uint64_t rejected = r->total_samples_in - r->total_samples_kept;
    double reject_pct = r->total_samples_in ?
        100.0 * (double)rejected / (double)r->total_samples_in : 0.0;
    double energy_per_pred_mj = (r->predictions_made > 0) ?
        (p->energy_joules / (double)r->predictions_made) * 1000.0 : 0.0;
    double avg_sec_per_call = (r->iterations > 0) ?
        (r->total_wall_seconds / (double)r->iterations) : 0.0;

    fprintf(g_csv, "%d,%s,%s,%d,%ld,%s,%u,%.6f,%.9f,%.4f,%.4f,%.4f,%ld,%.4f,%.4f,%.4f,%llu,%llu,%.2f,%u,%u\n",
            trial_no, category, param, requested_ok, measured_freq_khz, measured_gov,
            r->iterations, r->total_wall_seconds, avg_sec_per_call,
            p->avg_core_power_w, p->avg_summed_rails_w, p->avg_corrected_total_w,
            p->sample_count, p->elapsed_seconds,
            p->energy_joules, energy_per_pred_mj,
            (unsigned long long)r->total_samples_in, (unsigned long long)r->total_samples_kept,
            reject_pct, r->summaries_produced, r->predictions_made);
}

/* Applies the config for one trial. Returns 1 if the requested change
 * was actually confirmed on hardware, 0 if it silently failed (e.g.
 * missing root) -- callers should print this, not assume success. */
static int apply_trial(const trial_spec_t *t)
{
    switch (t->kind) {
        case T_BASELINE:
            return 1;
        case T_CLOCK: {
            int rc = pc_set_freq_khz_all(t->freq_khz);
            long cur = 0;
            pc_get_cur_freq_khz(0, &cur);
            /* allow +/-5% slack: hardware steps may not land exactly */
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

/* Runs the pipeline repeatedly under the CURRENT config until at least
 * TARGET_TRIAL_SECONDS of wall time has elapsed (or MAX_ITERATIONS is
 * hit), aggregating results as it goes. This is what gives the power
 * monitor a real sampling window instead of racing a single ~1ms call. */
static void run_workload_for_target_duration(agg_result_t *agg)
{
    memset(agg, 0, sizeof(*agg));
    stage_stats_init(&agg->io_stats, "io_unit");
    stage_stats_init(&agg->mem_stats, "memory_unit");
    stage_stats_init(&agg->compute_stats, "compute_unit");

    /* Calibration pass: measure one call under the current config so
     * we know how many iterations are needed to reach the target
     * duration (faster configs need more iterations, slower configs
     * need fewer -- this keeps every trial's *measurement window*
     * roughly equal even though throughput differs). */
    pipeline_result_t calib;
    pipeline_run(TOTAL_TICKS_PER_CALL, &calib, /*verbose=*/0);

    uint32_t iterations = MIN_ITERATIONS;
    if (calib.wall_seconds > 1e-9) {
        double needed = TARGET_TRIAL_SECONDS / calib.wall_seconds;
        if (needed > (double)MAX_ITERATIONS) needed = (double)MAX_ITERATIONS;
        if (needed > (double)MIN_ITERATIONS) iterations = (uint32_t)needed;
    }

    /* Fold the calibration pass itself into the aggregate so it isn't wasted. */
    agg->iterations = 1;
    agg->total_wall_seconds  += calib.wall_seconds;
    agg->total_ticks         += calib.total_ticks;
    agg->summaries_produced  += calib.summaries_produced;
    agg->predictions_made    += calib.predictions_made;
    agg->total_samples_in    += calib.total_samples_in;
    agg->total_samples_kept  += calib.total_samples_kept;
    if (calib.have_prediction) { agg->last_prediction = calib.last_prediction; agg->have_prediction = 1; }
    stage_stats_merge(&agg->io_stats, &calib.io_stats);
    stage_stats_merge(&agg->mem_stats, &calib.mem_stats);
    stage_stats_merge(&agg->compute_stats, &calib.compute_stats);

    for (uint32_t i = 1; i < iterations; i++) {
        pipeline_result_t r;
        pipeline_run(TOTAL_TICKS_PER_CALL, &r, /*verbose=*/0);

        agg->iterations++;
        agg->total_wall_seconds  += r.wall_seconds;
        agg->total_ticks         += r.total_ticks;
        agg->summaries_produced  += r.summaries_produced;
        agg->predictions_made    += r.predictions_made;
        agg->total_samples_in    += r.total_samples_in;
        agg->total_samples_kept  += r.total_samples_kept;
        if (r.have_prediction) { agg->last_prediction = r.last_prediction; agg->have_prediction = 1; }
        stage_stats_merge(&agg->io_stats, &r.io_stats);
        stage_stats_merge(&agg->mem_stats, &r.mem_stats);
        stage_stats_merge(&agg->compute_stats, &r.compute_stats);
    }
}

static void run_one_trial(int trial_no, const char *category, const char *param,
                           const trial_spec_t *t, const power_config_snapshot_t *baseline)
{
    /* Always start every trial from the captured system baseline so
     * trials never "inherit" a previous trial's config. */
    pc_restore(baseline);
    pc_restore_cores();

    printf("\n---------------------------------------------------------------\n");
    printf("Trial %d [%s]: %s\n", trial_no, category, t->label);

    int applied_ok = apply_trial(t);

    long measured_freq = 0;
    char measured_gov[64] = "n/a";
    pc_get_cur_freq_khz(0, &measured_freq);
    pc_get_governor(0, measured_gov, sizeof(measured_gov));

    printf("  requested change applied : %s\n", applied_ok ? "YES (confirmed on hardware)" :
           "NO (permission denied or unsupported -- rerun with sudo; results below reflect"
           " whatever config was actually in effect, see 'measured' fields)");
    printf("  measured cpu0 frequency  : %ld kHz\n", measured_freq);
    printf("  measured cpu0 governor   : %s\n", measured_gov);

    power_monitor_t *pm = power_monitor_start(POWER_SAMPLE_MS);

    agg_result_t agg;
    run_workload_for_target_duration(&agg);

    power_summary_t psum;
    power_monitor_stop(pm, &psum);

    /* --- processing/validation info: proves the pipeline still did
     * the same work correctly under this configuration, every single
     * iteration ------------------------------------------------------ */
    uint64_t rejected = agg.total_samples_in - agg.total_samples_kept;
    double reject_pct = agg.total_samples_in ?
        100.0 * (double)rejected / (double)agg.total_samples_in : 0.0;
    double io_avg_ns, io_pct, mem_avg_ns, mem_pct, comp_avg_ns, comp_pct;
    stage_stats_summary(&agg.io_stats, agg.total_wall_seconds, &io_avg_ns, &io_pct);
    stage_stats_summary(&agg.mem_stats, agg.total_wall_seconds, &mem_avg_ns, &mem_pct);
    stage_stats_summary(&agg.compute_stats, agg.total_wall_seconds, &comp_avg_ns, &comp_pct);

    double avg_sec_per_call = agg.total_wall_seconds / (double)agg.iterations;

    printf("  --- processing validation ---\n");
    printf("    pipeline iterations run : %u  (%.3f s total workload time, %.6f s/call avg)\n",
           agg.iterations, agg.total_wall_seconds, avg_sec_per_call);
    printf("    io_unit avg/call        : %.1f ns  (%.2f%% of workload time)\n", io_avg_ns, io_pct);
    printf("    memory_unit avg/call    : %.1f ns  (%.2f%% of workload time)\n", mem_avg_ns, mem_pct);
    printf("    compute_unit avg/call   : %.1f ns  (%.2f%% of workload time)\n", comp_avg_ns, comp_pct);
    printf("    samples in/kept/rej%%    : %llu / %llu / %.2f%%\n",
           (unsigned long long)agg.total_samples_in,
           (unsigned long long)agg.total_samples_kept, reject_pct);
    printf("    summaries/predictions   : %u / %u  (totals across all iterations)\n",
           agg.summaries_produced, agg.predictions_made);
    if (agg.have_prediction) {
        printf("    last prediction         : trend=%+7.4f C/tick projected=%6.2fC state=%s\n",
               agg.last_prediction.trend_slope_c_per_tick,
               agg.last_prediction.projected_temp_c,
               hvac_state_str(agg.last_prediction.state));
    }

    printf("  --- power ---\n");
    if (psum.vcgencmd_available) {
        double energy_per_pred_mj = (agg.predictions_made > 0) ?
            (psum.energy_joules / (double)agg.predictions_made) * 1000.0 : 0.0;
        printf("    vcgencmd samples        : %ld (over %.2f s sampling window)\n", psum.sample_count, psum.elapsed_seconds);
        printf("    avg VDD_CORE power      : %.4f W\n", psum.avg_core_power_w);
        printf("    avg summed-rail power   : %.4f W  (raw PMIC sum, excludes 5V/USB/HAT/NVMe)\n", psum.avg_summed_rails_w);
        printf("    avg corrected est. power: %.4f W  (linear-calibrated, see docs/power_model_notes.md)\n", psum.avg_corrected_total_w);
        printf("    energy over sampling win: %.4f J\n", psum.energy_joules);
        printf("    energy per prediction   : %.4f mJ  (across %u predictions)\n", energy_per_pred_mj, agg.predictions_made);
        if (psum.sample_count < 5) {
            printf("    NOTE: only %ld power sample(s) captured -- still low. Consider raising\n"
                   "          TARGET_TRIAL_SECONDS or lowering POWER_SAMPLE_MS in run_experiments.c.\n",
                   psum.sample_count);
        }
    } else {
        printf("    vcgencmd not available on this system -- power fields are N/A.\n");
        printf("    (expected on non-Pi hardware; run on the Pi 5 for real numbers)\n");
    }

    csv_row(trial_no, category, param, applied_ok, measured_freq, measured_gov, &agg, &psum);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (geteuid() != 0) {
        fprintf(stderr,
            "WARNING: not running as root. Clock-speed, governor, and\n"
            "core-offline changes require root to write to sysfs; this run\n"
            "will still execute but most trials will likely report\n"
            "'requested change applied: NO'. Re-run with: sudo ./run_experiments\n\n");
    }

    printf("=== Critter Power Experiment Harness (v2: sustained-load sampling) ===\n");
    printf("Workload: full pipeline (io_unit -> memory_unit -> compute_unit), %u ticks/call,\n", TOTAL_TICKS_PER_CALL);
    printf("looped for ~%.1f s per trial so the power monitor gets a real sample window.\n", TARGET_TRIAL_SECONDS);
    printf("Identical synthetic sensor data on every call (fixed RNG seed).\n");

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

    int trial_no = 0;

    /* --- Trial 0: baseline ------------------------------------------- */
    trial_spec_t base = { T_BASELINE, "Baseline (system default config, unmodified)", 0, 0, CORE_MODE_AFFINITY, NULL };
    run_one_trial(++trial_no, "baseline", "default", &base, &baseline);

    /* --- Clock speed sweep -------------------------------------------- */
    trial_spec_t clk_max = { T_CLOCK, "", f_max, 0, CORE_MODE_AFFINITY, NULL };
    snprintf(clk_max.label, sizeof(clk_max.label), "Clock speed: max (%ld kHz, performance-pinned)", f_max);
    run_one_trial(++trial_no, "clock", "max", &clk_max, &baseline);

    trial_spec_t clk_mid = { T_CLOCK, "", f_mid, 0, CORE_MODE_AFFINITY, NULL };
    snprintf(clk_mid.label, sizeof(clk_mid.label), "Clock speed: mid (%ld kHz, performance-pinned)", f_mid);
    run_one_trial(++trial_no, "clock", "mid", &clk_mid, &baseline);

    trial_spec_t clk_min = { T_CLOCK, "", f_min, 0, CORE_MODE_AFFINITY, NULL };
    snprintf(clk_min.label, sizeof(clk_min.label), "Clock speed: min (%ld kHz, performance-pinned)", f_min);
    run_one_trial(++trial_no, "clock", "min", &clk_min, &baseline);

    /* --- Core count sweep (physically offline cores) ------------------ */
    trial_spec_t cores4 = { T_CORES, "Core count: 4 active (all online)", 0, 4, CORE_MODE_OFFLINE, NULL };
    run_one_trial(++trial_no, "cores", "4", &cores4, &baseline);

    trial_spec_t cores2 = { T_CORES, "Core count: 2 active (cores 2-3 offline)", 0, 2, CORE_MODE_OFFLINE, NULL };
    run_one_trial(++trial_no, "cores", "2", &cores2, &baseline);

    trial_spec_t cores1 = { T_CORES, "Core count: 1 active (cores 1-3 offline)", 0, 1, CORE_MODE_OFFLINE, NULL };
    run_one_trial(++trial_no, "cores", "1", &cores1, &baseline);

    /* --- Governor sweep (default max frequency, policy decides) ------- */
    trial_spec_t gov_perf = { T_GOVERNOR, "Governor: performance (always max freq)", 0, 0, CORE_MODE_AFFINITY, "performance" };
    run_one_trial(++trial_no, "governor", "performance", &gov_perf, &baseline);

    trial_spec_t gov_sched = { T_GOVERNOR, "Governor: schedutil (default demand-based scaling)", 0, 0, CORE_MODE_AFFINITY, "schedutil" };
    run_one_trial(++trial_no, "governor", "schedutil", &gov_sched, &baseline);

    trial_spec_t gov_power = { T_GOVERNOR, "Governor: powersave (always min freq)", 0, 0, CORE_MODE_AFFINITY, "powersave" };
    run_one_trial(++trial_no, "governor", "powersave", &gov_power, &baseline);

    /* --- cleanup -------------------------------------------------------- */
    pc_restore(&baseline);
    pc_restore_cores();
    if (g_csv) fclose(g_csv);

    printf("\n=================================================================\n");
    printf("Done. %d trials written to results.csv in the working directory.\n", trial_no);
    printf("=================================================================\n");
    return 0;
}

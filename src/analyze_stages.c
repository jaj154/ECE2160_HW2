/* analyze_stages.c
 *
 * Per-STAGE power isolation. The main sweep and the embedded analysis
 * both drive the three prototype units together as one pipeline, which
 * lets us measure each stage's share of execution TIME but not its
 * share of POWER -- and time-share is not energy-share, because the
 * stages can have different switching activity per unit time.
 *
 * This harness closes that gap. It runs each of the three prototype
 * units in ISOLATION under its own sustained load, measuring the power
 * drawn while only that stage is executing, and subtracts the platform
 * idle floor to isolate each stage's DYNAMIC power. That yields a
 * direct, measured decomposition of dynamic power across the three
 * units -- the number the paper currently can only bound analytically.
 *
 * Each stage is driven at a scale that keeps it busy continuously so
 * the sampler sees steady-state power, and the input to each stage is
 * the deterministic synthetic data the rest of the project uses, so
 * results are comparable to the pipeline runs. Every stage is measured
 * REPEATS times for mean +/- stddev.
 *
 * Output: console report + stage_results.csv.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "pipeline_types.h"
#include "io_unit.h"
#include "memory_unit.h"
#include "compute_unit.h"
#include "sensor_hw.h"
#include "power_monitor.h"
#include "pmic_read.h"

#define POWER_SAMPLE_MS       30    /* fast: hwmon backend permits ms-scale */
#define MEASURE_SECONDS       4.0
#define IDLE_SECONDS          4.0
#define REPEATS               5
#define MEM_WINDOW            16u
#define PRED_WINDOW           PREDICT_WINDOW

typedef struct { int n; double sum, sq; } acc_t;
static void   add(acc_t *a, double x){ a->n++; a->sum+=x; a->sq+=x*x; }
static double mean(const acc_t *a){ return a->n? a->sum/a->n:0.0; }
static double sd(const acc_t *a){ if(a->n<2)return 0; double m=mean(a),v=(a->sq/a->n)-m*m; return v>0?sqrt(v):0; }
static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void   msleep(long ms){ struct timespec t={ms/1000,(ms%1000)*1000000L}; nanosleep(&t,NULL); }

/* ---- isolated stage drivers ----------------------------------------
 * Each returns after ~MEASURE_SECONDS of continuous work on its unit,
 * accumulating a checksum so the compiler cannot optimize the work away
 * and so correctness can be confirmed. */

static volatile uint64_t g_sink;

static void drive_io(void)
{
    raw_ring_t ring; sensor_hw_init(); io_unit_init(&ring);
    raw_sample_t batch[MEM_WINDOW];
    double end = now_s() + MEASURE_SECONDS; uint32_t tick=0; uint64_t cs=0;
    while (now_s() < end) {
        for (uint32_t i=0;i<MEM_WINDOW*64u;i++) io_unit_sample(&ring, tick++);
        uint32_t d = io_unit_drain(&ring, batch, MEM_WINDOW);
        for (uint32_t i=0;i<d;i++) cs += (uint64_t)batch[i].value_c;
    }
    g_sink += cs;
}

static void drive_memory(void)
{
    /* Pre-generate a fixed batch once, then hammer the memory unit's
     * outlier-rejection + summarization path on it repeatedly. */
    raw_sample_t batch[MEM_WINDOW];
    sensor_hw_init();
    for (uint32_t i=0;i<MEM_WINDOW;i++){ batch[i].tick=i; batch[i].value_c=sensor_hw_read_raw(i); batch[i].is_outlier=0; }
    summary_ring_t ring; memory_unit_init(&ring);
    double end = now_s() + MEASURE_SECONDS; uint64_t cs=0;
    while (now_s() < end) {
        for (uint32_t r=0;r<4096u;r++){
            /* reset outlier flags so each pass does the full work */
            for (uint32_t i=0;i<MEM_WINDOW;i++) batch[i].is_outlier=0;
            memory_unit_process(&ring, batch, MEM_WINDOW);
        }
        summary_record_t out[SUMMARY_RING_SIZE];
        uint32_t d = memory_unit_drain(&ring, out, SUMMARY_RING_SIZE);
        for (uint32_t i=0;i<d;i++) cs += (uint64_t)out[i].mean_c;
    }
    g_sink += cs;
}

static void drive_compute(void)
{
    /* Build a fixed window of summaries, then hammer the regression. */
    summary_record_t recs[PRED_WINDOW];
    for (uint32_t i=0;i<PRED_WINDOW;i++){
        recs[i].tick_start=i*MEM_WINDOW; recs[i].tick_end=(i+1)*MEM_WINDOW-1;
        recs[i].samples_in=MEM_WINDOW; recs[i].samples_kept=MEM_WINDOW;
        recs[i].mean_c=22.0f+0.1f*i; recs[i].stddev_c=0.2f;
        recs[i].min_c=21.0f; recs[i].max_c=23.0f;
    }
    double end = now_s() + MEASURE_SECONDS; uint64_t cs=0;
    while (now_s() < end) {
        prediction_t p;
        for (uint32_t r=0;r<4096u;r++){ compute_unit_predict(recs, PRED_WINDOW, &p); cs += (uint64_t)p.projected_temp_c; }
    }
    g_sink += cs;
}

typedef void (*driver_fn)(void);

static void measure_stage(const char *name, driver_fn fn,
                          acc_t *core, acc_t *tot)
{
    for (int r=0;r<REPEATS;r++){
        power_monitor_t *pm = power_monitor_start(POWER_SAMPLE_MS);
        fn();
        power_summary_t s; power_monitor_stop(pm, &s);
        if (s.vcgencmd_available){ add(core,s.avg_core_power_w); add(tot,s.avg_corrected_total_w); }
        printf("  %-8s repeat %d: core=%.4f W total=%.4f W (%ld samples)\n",
               name, r+1, s.avg_core_power_w, s.avg_corrected_total_w, s.sample_count);
    }
}

static void measure_idle(acc_t *core, acc_t *tot)
{
    for (int r=0;r<REPEATS;r++){
        power_monitor_t *pm = power_monitor_start(POWER_SAMPLE_MS);
        double end = now_s()+IDLE_SECONDS; while(now_s()<end) msleep(50);
        power_summary_t s; power_monitor_stop(pm, &s);
        if (s.vcgencmd_available){ add(core,s.avg_core_power_w); add(tot,s.avg_corrected_total_w); }
        printf("  %-8s repeat %d: core=%.4f W total=%.4f W (%ld samples)\n",
               "idle", r+1, s.avg_core_power_w, s.avg_corrected_total_w, s.sample_count);
    }
}

int main(void)
{
    int backend = pmic_init();
    printf("=== Per-Stage Power Isolation ===\n");
    printf("PMIC backend: %s   sample interval: %d ms   %d repeats/stage\n",
           pmic_backend_name(), POWER_SAMPLE_MS, REPEATS);
    printf("Each stage driven in isolation under sustained load; idle floor\n");
    printf("subtracted to isolate per-stage DYNAMIC power.\n\n");

    if (backend == PMIC_BACKEND_NONE)
        printf("WARNING: no PMIC backend available -- power will read N/A (expected off-Pi).\n\n");

    acc_t idle_c={0}, idle_t={0};
    acc_t io_c={0}, io_t={0}, mem_c={0}, mem_t={0}, cmp_c={0}, cmp_t={0};

    printf("--- IDLE floor ---\n");            measure_idle(&idle_c, &idle_t);
    printf("--- I/O stage ---\n");             measure_stage("io",     drive_io,      &io_c,  &io_t);
    printf("--- MEMORY stage ---\n");          measure_stage("memory", drive_memory,  &mem_c, &mem_t);
    printf("--- COMPUTE stage ---\n");         measure_stage("compute",drive_compute, &cmp_c, &cmp_t);

    double idle = mean(&idle_t);
    struct { const char *name; double tot, tot_sd, dyn; } row[3] = {
        { "I/O",     mean(&io_t),  sd(&io_t),  mean(&io_t)  - idle },
        { "Memory",  mean(&mem_t), sd(&mem_t), mean(&mem_t) - idle },
        { "Compute", mean(&cmp_t), sd(&cmp_t), mean(&cmp_t) - idle },
    };
    for (int i=0;i<3;i++) if (row[i].dyn < 0) row[i].dyn = 0;
    double dyn_sum = row[0].dyn + row[1].dyn + row[2].dyn;

    printf("\n=================== PER-STAGE POWER SUMMARY ===================\n");
    printf("Idle floor: %.4f +/- %.4f W\n\n", idle, sd(&idle_t));
    printf("  %-8s %-14s %-16s %-14s\n","stage","total W","dynamic W","%% of dynamic");
    for (int i=0;i<3;i++){
        double pct = dyn_sum>0? 100.0*row[i].dyn/dyn_sum : 0.0;
        printf("  %-8s %6.4f        %6.4f          %5.1f%%\n",
               row[i].name, row[i].tot, row[i].dyn, pct);
    }
    printf("\nThis is a DIRECT measurement of each stage's dynamic power, so it\n");
    printf("replaces the time-share-as-energy-share assumption with data.\n");

    FILE *f = fopen("stage_results.csv","w");
    if (f){
        fprintf(f,"backend,idle_w,io_total_w,io_total_sd,io_dyn_w,mem_total_w,mem_total_sd,mem_dyn_w,"
                  "compute_total_w,compute_total_sd,compute_dyn_w,io_pct_dyn,mem_pct_dyn,compute_pct_dyn\n");
        double p0 = dyn_sum>0?100*row[0].dyn/dyn_sum:0;
        double p1 = dyn_sum>0?100*row[1].dyn/dyn_sum:0;
        double p2 = dyn_sum>0?100*row[2].dyn/dyn_sum:0;
        fprintf(f,"%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,%.1f,%.1f\n",
                pmic_backend_name(), idle,
                row[0].tot, row[0].tot_sd, row[0].dyn,
                row[1].tot, row[1].tot_sd, row[1].dyn,
                row[2].tot, row[2].tot_sd, row[2].dyn, p0, p1, p2);
        fclose(f);
        printf("\nWrote stage_results.csv\n");
    }
    printf("(checksum %llu)\n", (unsigned long long)g_sink);
    return 0;
}

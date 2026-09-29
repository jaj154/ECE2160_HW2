/* analyze_embedded.c
 *
 * A SECOND harness (separate from run_experiments.c) that measures the
 * embedded-systems power aspects the configuration sweep did not:
 *
 *   A. IDLE / SLEEP POWER       -- process blocked (not busy-looping),
 *                                  network left up. Stands in for the
 *                                  static/platform floor of a monitor
 *                                  that is >99% idle in deployment.
 *   B. PLATFORM BASELINE        -- same idle measurement, reported
 *                                  explicitly as the fixed cost that
 *                                  every active number sits on top of.
 *   C. ACTIVE (busy) POWER      -- the pipeline under sustained load,
 *                                  for direct comparison to idle.
 *   D. STATIC vs DYNAMIC SPLIT  -- DERIVED, not directly measured:
 *                                  static ~= idle floor, dynamic ~=
 *                                  active - idle. Labelled as an
 *                                  estimate because a running Linux box
 *                                  cannot cleanly halt all switching.
 *   E. DUTY-CYCLED ENERGY       -- the realistic deployment model:
 *                                  wake -> do ONE pipeline pass -> sleep
 *                                  for the rest of the sample period.
 *                                  Energy per period is modelled from
 *                                  the measured active and idle powers,
 *                                  swept across several duty cycles, to
 *                                  show where idle power starts to
 *                                  dominate the budget.
 *
 * Peripheral states (GUI on/off, Bluetooth on/off) are NOT toggled from
 * here -- flipping the desktop or radios mid-process is a system-level
 * action, so scripts/peripheral_states.sh drives THIS binary once per
 * peripheral configuration instead. Each run prints a PERIPHERAL-STATE
 * line (read from the env var CRITTER_PERIPH_LABEL, or "unspecified")
 * so the driver script can label rows.
 *
 * Output: human-readable console report + embedded_results.csv.
 *
 * All power numbers use the same PMIC path and linear calibration as
 * run_experiments.c (see docs/power_model_notes.md); they are internal-
 * rail estimates and exclude the 5V side, so idle here includes the
 * PMIC-visible platform rails but NOT external USB/NIC draw. Network is
 * left up so the SoC-side radio/controller idle is represented.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#include "pipeline_workload.h"
#include "power_monitor.h"

#define TOTAL_TICKS_PER_CALL   4096u
#define POWER_SAMPLE_MS         150
#define IDLE_MEASURE_SECONDS    5.0   /* how long to sit blocked-idle       */
#define ACTIVE_MEASURE_SECONDS  4.0   /* how long to sustain busy load      */
#define REPEATS                   5

/* Duty cycles to model for the deployment-energy sweep. A real Critter
 * samples slowly, so the interesting region is small duty cycles. */
static const double DUTY_CYCLES[] = { 1.0, 0.50, 0.10, 0.01, 0.001 };
#define NUM_DUTY (sizeof(DUTY_CYCLES)/sizeof(DUTY_CYCLES[0]))

typedef struct { int n; double sum, sum_sq; } stat_acc_t;
static void   acc_add(stat_acc_t *a, double x){ a->n++; a->sum+=x; a->sum_sq+=x*x; }
static double acc_mean(const stat_acc_t *a){ return a->n? a->sum/a->n : 0.0; }
static double acc_std(const stat_acc_t *a){
    if(a->n<2) return 0.0;
    double m=acc_mean(a); double v=(a->sum_sq/a->n)-(m*m);
    return v>0.0? sqrt(v):0.0;
}

static void msleep(long ms){
    struct timespec ts; ts.tv_sec=ms/1000; ts.tv_nsec=(ms%1000)*1000000L;
    nanosleep(&ts,NULL);
}

static double now_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}

/* --- A/B: measure power while the process is BLOCKED (true idle) ------
 * The key distinction from a busy loop: we nanosleep the main thread so
 * the CPU has no work and can drop into its shallow idle states, while
 * the monitor thread still samples the PMIC. This is process-idle, not
 * suspend -- the board stays fully up with the network online. */
static void measure_idle(power_summary_t *out){
    power_monitor_t *pm = power_monitor_start(POWER_SAMPLE_MS);
    double t_end = now_s() + IDLE_MEASURE_SECONDS;
    while (now_s() < t_end) msleep(100);   /* blocked, not spinning */
    power_monitor_stop(pm, out);
}

/* --- C: measure power under sustained pipeline load ------------------ */
static void measure_active(power_summary_t *out, double *avg_call_s,
                            uint64_t *predictions){
    power_monitor_t *pm = power_monitor_start(POWER_SAMPLE_MS);
    double t_end = now_s() + ACTIVE_MEASURE_SECONDS;
    uint64_t iters=0, preds=0; double busy=0.0;
    while (now_s() < t_end){
        pipeline_result_t r;
        pipeline_run(TOTAL_TICKS_PER_CALL, &r, 0);
        iters++; preds += r.predictions_made; busy += r.wall_seconds;
    }
    power_monitor_stop(pm, out);
    *avg_call_s  = iters? busy/(double)iters : 0.0;
    *predictions = preds;
}

int main(void){
    const char *periph = getenv("CRITTER_PERIPH_LABEL");
    if(!periph || !*periph) periph = "unspecified";

    if (geteuid()!=0){
        fprintf(stderr,"NOTE: not root -- PMIC reads still work, but run the\n"
                       "config sweep (run_experiments) as root separately.\n\n");
    }

    printf("=== Critter Embedded Power Analysis ===\n");
    printf("Peripheral state: %s\n", periph);
    printf("Idle window %.1fs, active window %.1fs, %d repeats each.\n",
           IDLE_MEASURE_SECONDS, ACTIVE_MEASURE_SECONDS, REPEATS);
    printf("Network left UP; idle = process blocked (not busy-looping).\n");
    printf("One pipeline pass = %u ticks = 32 predictions.\n", TOTAL_TICKS_PER_CALL);

    /* One pass's wall time and energy, needed for the duty-cycle model. */
    /* ---- repeats: idle ---- */
    stat_acc_t idle_core={0}, idle_tot={0}, active_core={0}, active_tot={0};
    stat_acc_t call_s={0};
    int power_ok = 0;

    printf("\n--- Measuring IDLE (blocked, network up) x%d ---\n", REPEATS);
    for(int i=0;i<REPEATS;i++){
        power_summary_t s; measure_idle(&s);
        if(s.vcgencmd_available){
            power_ok=1;
            acc_add(&idle_core,s.avg_core_power_w);
            acc_add(&idle_tot,s.avg_corrected_total_w);
        }
        printf("  repeat %d: total=%.4f W (core=%.4f W, %ld samples)\n",
               i+1, s.avg_corrected_total_w, s.avg_core_power_w, s.sample_count);
    }

    printf("\n--- Measuring ACTIVE (sustained pipeline) x%d ---\n", REPEATS);
    for(int i=0;i<REPEATS;i++){
        power_summary_t s; double cs; uint64_t pr;
        measure_active(&s,&cs,&pr);
        if(s.vcgencmd_available){
            acc_add(&active_core,s.avg_core_power_w);
            acc_add(&active_tot,s.avg_corrected_total_w);
        }
        acc_add(&call_s, cs);
        printf("  repeat %d: total=%.4f W (core=%.4f W), %.6f s/call, %ld samples\n",
               i+1, s.avg_corrected_total_w, s.avg_core_power_w, cs, s.sample_count);
    }

    double idle_w   = acc_mean(&idle_tot);
    double active_w = acc_mean(&active_tot);
    double call_time= acc_mean(&call_s);

    /* --- D: static vs dynamic split (DERIVED) ------------------------ */
    double static_w  = idle_w;                 /* floor ~= idle          */
    double dynamic_w = active_w - idle_w;       /* switching component    */
    if(dynamic_w < 0) dynamic_w = 0;

    printf("\n=================== EMBEDDED POWER SUMMARY ===================\n");
    printf("Peripheral state: %s\n", periph);
    if(!power_ok){
        printf("vcgencmd/PMIC not available -- power N/A (expected off-Pi).\n");
    } else {
        printf("Idle  (platform floor)  : %.4f +/- %.4f W  (core %.4f W)\n",
               idle_w, acc_std(&idle_tot), acc_mean(&idle_core));
        printf("Active(sustained load)  : %.4f +/- %.4f W  (core %.4f W)\n",
               active_w, acc_std(&active_tot), acc_mean(&active_core));
        printf("  -> STATIC  (derived)  : %.4f W  (~= idle floor)\n", static_w);
        printf("  -> DYNAMIC (derived)  : %.4f W  (active - idle; switching)\n", dynamic_w);
        double dyn_pct = active_w>0? 100.0*dynamic_w/active_w : 0.0;
        printf("  -> dynamic is %.1f%% of active draw; static floor is %.1f%%\n",
               dyn_pct, 100.0-dyn_pct);
        printf("Time per pipeline pass  : %.6f s  (32 predictions)\n", call_time);
        double active_energy_per_pass = active_w * call_time; /* J */
        printf("Active energy per pass  : %.4f J  (%.4f mJ/prediction)\n",
               active_energy_per_pass, active_energy_per_pass/32.0*1000.0);

        /* --- E: duty-cycled energy model ---------------------------- */
        /* Model one sample PERIOD as: run one pass (busy at active_w for
         * call_time), then idle at idle_w for the remainder. Duty cycle
         * d = busy_time / period, so period = call_time / d. Average
         * power over the period is the energy-weighted mix. This is the
         * number that actually matters for a mostly-idle monitor. */
        printf("\n--- DUTY-CYCLED DEPLOYMENT MODEL ---\n");
        printf("(one %uk-tick pass per sample period; idle the rest)\n", TOTAL_TICKS_PER_CALL/1000);
        printf("  %-10s %-12s %-14s %-16s\n","duty","period(s)","avg_power(W)","energy/period(J)");
        for(size_t k=0;k<NUM_DUTY;k++){
            double d = DUTY_CYCLES[k];
            double period = call_time / d;
            double idle_time = period - call_time;
            double e_period = active_w*call_time + idle_w*idle_time;
            double avg_p = e_period / period;
            printf("  %-10.3f %-12.4f %-14.4f %-16.4f\n", d, period, avg_p, e_period);
        }
        printf("As duty -> 0, avg power -> idle floor (%.4f W): idle DOMINATES\n", idle_w);
        printf("the budget, so idle power matters more than any active-config knob.\n");
    }

    /* --- CSV --------------------------------------------------------- */
    FILE *csv = fopen("embedded_results.csv","w");
    if(csv){
        fprintf(csv,"peripheral_state,idle_total_w,idle_total_w_std,idle_core_w,"
                    "active_total_w,active_total_w_std,active_core_w,"
                    "static_w_derived,dynamic_w_derived,dynamic_pct_of_active,"
                    "seconds_per_pass,active_energy_per_pass_j,mj_per_prediction");
        for(size_t k=0;k<NUM_DUTY;k++) fprintf(csv,",avgW_duty_%.3f",DUTY_CYCLES[k]);
        for(size_t k=0;k<NUM_DUTY;k++) fprintf(csv,",Jperiod_duty_%.3f",DUTY_CYCLES[k]);
        fprintf(csv,"\n");

        double dyn_pct = active_w>0? 100.0*dynamic_w/active_w : 0.0;
        double e_pass  = active_w*call_time;
        fprintf(csv,"%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.6f,%.4f,%.4f",
                periph, idle_w, acc_std(&idle_tot), acc_mean(&idle_core),
                active_w, acc_std(&active_tot), acc_mean(&active_core),
                static_w, dynamic_w, dyn_pct, call_time, e_pass, e_pass/32.0*1000.0);
        for(size_t k=0;k<NUM_DUTY;k++){
            double d=DUTY_CYCLES[k]; double period=call_time/d;
            double e=active_w*call_time + idle_w*(period-call_time);
            fprintf(csv,",%.4f", e/period);
        }
        for(size_t k=0;k<NUM_DUTY;k++){
            double d=DUTY_CYCLES[k]; double period=call_time/d;
            double e=active_w*call_time + idle_w*(period-call_time);
            fprintf(csv,",%.4f", e);
        }
        fprintf(csv,"\n");
        fclose(csv);
        printf("\nWrote embedded_results.csv\n");
    }
    printf("=============================================================\n");
    return 0;
}

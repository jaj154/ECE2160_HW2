/* asm_experiment.c
 *
 * Assignment Option 2, actually measured: compares the energy of the
 * integer scaling kernel when the scale-by-constant is performed with a
 * hardware MULTIPLY versus a hand-substituted SHIFT.
 *
 * This binary is linked TWICE by the build (see scripts/run_asm_experiment.sh):
 * once against the multiply variant of kernel_scale() and once against
 * the shift variant. Each resulting binary is run here, driving the
 * kernel in a sustained loop under the same PMIC power monitor used for
 * the configuration sweep, so the two are measured on identical footing.
 *
 * The kernel is called enough times to sustain load for several seconds
 * (so the ~100 ms PMIC read yields many samples), repeated REPEATS times
 * for a mean +/- stddev, exactly mirroring the methodology of the main
 * sweep. A correctness checksum is printed so the two variants can be
 * confirmed to produce identical output.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "asm_kernel.h"
#include "power_monitor.h"

#define N_SAMPLES        4096u
#define TARGET_SECONDS   4.0
#define POWER_SAMPLE_MS  150
#define REPEATS          5

/* label passed in by the build so the CSV/console row is tagged
 * "multiply" or "shift"; defaults to "unspecified". */
#ifndef VARIANT_LABEL
#define VARIANT_LABEL "unspecified"
#endif

typedef struct { int n; double sum, sum_sq; } acc_t;
static void   add(acc_t *a, double x){ a->n++; a->sum+=x; a->sum_sq+=x*x; }
static double mean(const acc_t *a){ return a->n? a->sum/a->n:0.0; }
static double sdev(const acc_t *a){
    if(a->n<2) return 0.0;
    double m=mean(a), v=(a->sum_sq/a->n)-(m*m);
    return v>0?sqrt(v):0.0;
}
static double now_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec + ts.tv_nsec/1e9;
}

int main(void)
{
    static uint32_t samples[N_SAMPLES];
    for (uint32_t i=0;i<N_SAMPLES;i++) samples[i] = i*2654435761u + 12345u;

    printf("=== Assembly Optimization Experiment: variant = %s ===\n", VARIANT_LABEL);
    printf("Kernel: accumulate %u samples scaled by constant 64.\n", N_SAMPLES);
    printf("%d repeats, ~%.1f s sustained load each.\n", REPEATS, TARGET_SECONDS);

    acc_t p_core={0}, p_tot={0}, call_ns={0};
    uint64_t checksum = 0;
    int power_ok = 0;

    for (int rep=0; rep<REPEATS; rep++) {
        /* calibrate: time one kernel call */
        double c0 = now_s();
        checksum = kernel_scale(samples, N_SAMPLES);
        double c1 = now_s();
        double one = c1 - c0;
        if (one < 1e-9) one = 1e-9;
        uint64_t iters = (uint64_t)(TARGET_SECONDS / one);
        if (iters < 1000) iters = 1000;

        power_monitor_t *pm = power_monitor_start(POWER_SAMPLE_MS);
        double t0 = now_s();
        volatile uint64_t sink = 0;
        for (uint64_t k=0;k<iters;k++) sink += kernel_scale(samples, N_SAMPLES);
        double t1 = now_s();
        power_summary_t s; power_monitor_stop(pm, &s);
        (void)sink;

        double per_call_ns = ((t1-t0)/(double)iters)*1e9;
        add(&call_ns, per_call_ns);
        if (s.vcgencmd_available){
            power_ok = 1;
            add(&p_core, s.avg_core_power_w);
            add(&p_tot,  s.avg_corrected_total_w);
        }
        printf("  repeat %d: %llu iters, %.2f ns/call, core=%.4f W, total=%.4f W, %ld samples\n",
               rep+1, (unsigned long long)iters, per_call_ns,
               s.avg_core_power_w, s.avg_corrected_total_w, s.sample_count);
    }

    double call_s = mean(&call_ns) / 1e9;
    double e_per_call_uj = power_ok ? mean(&p_tot) * call_s * 1e6 : 0.0;

    printf("\n--- %s summary (mean +/- stddev over %d repeats) ---\n", VARIANT_LABEL, REPEATS);
    printf("  checksum (correctness)  : %llu\n", (unsigned long long)checksum);
    printf("  time per kernel call    : %.2f +/- %.2f ns\n", mean(&call_ns), sdev(&call_ns));
    if (power_ok){
        printf("  avg core power          : %.4f +/- %.4f W\n", mean(&p_core), sdev(&p_core));
        printf("  avg total power         : %.4f +/- %.4f W\n", mean(&p_tot), sdev(&p_tot));
        printf("  energy per kernel call  : %.4f uJ\n", e_per_call_uj);
    } else {
        printf("  power: vcgencmd unavailable -- N/A (expected off-Pi)\n");
    }

    /* append one row to asm_results.csv */
    FILE *f = fopen("asm_results.csv","a");
    if (f){
        /* write header if file is empty */
        fseek(f,0,SEEK_END);
        if (ftell(f)==0)
            fprintf(f,"variant,checksum,ns_per_call_mean,ns_per_call_std,"
                      "core_w_mean,core_w_std,total_w_mean,total_w_std,energy_per_call_uj\n");
        fprintf(f,"%s,%llu,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                VARIANT_LABEL,(unsigned long long)checksum,
                mean(&call_ns),sdev(&call_ns),mean(&p_core),sdev(&p_core),
                mean(&p_tot),sdev(&p_tot),e_per_call_uj);
        fclose(f);
    }
    return 0;
}

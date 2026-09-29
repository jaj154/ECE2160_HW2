#define _POSIX_C_SOURCE 200809L
#include "instrumentation.h"
#include <time.h>
#include <stdio.h>

double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

void stage_stats_init(stage_stats_t *s, const char *name)
{
    s->name     = name;
    s->calls    = 0;
    s->total_ns = 0.0;
    s->min_ns   = 0.0;
    s->max_ns   = 0.0;
}

void stage_stats_record(stage_stats_t *s, double elapsed_ns)
{
    if (s->calls == 0) {
        s->min_ns = elapsed_ns;
        s->max_ns = elapsed_ns;
    } else {
        if (elapsed_ns < s->min_ns) s->min_ns = elapsed_ns;
        if (elapsed_ns > s->max_ns) s->max_ns = elapsed_ns;
    }
    s->total_ns += elapsed_ns;
    s->calls++;
}

void stage_stats_merge(stage_stats_t *dst, const stage_stats_t *src)
{
    if (src->calls == 0) return;
    if (dst->calls == 0) {
        dst->min_ns = src->min_ns;
        dst->max_ns = src->max_ns;
    } else {
        if (src->min_ns < dst->min_ns) dst->min_ns = src->min_ns;
        if (src->max_ns > dst->max_ns) dst->max_ns = src->max_ns;
    }
    dst->calls    += src->calls;
    dst->total_ns += src->total_ns;
}

void stage_stats_summary(const stage_stats_t *s, double wall_seconds,
                          double *out_avg_ns, double *out_pct_of_wall)
{
    double avg_ns = (s->calls > 0) ? (s->total_ns / (double)s->calls) : 0.0;
    double pct = (wall_seconds > 0.0) ? (100.0 * (s->total_ns / 1e9) / wall_seconds) : 0.0;
    if (out_avg_ns) *out_avg_ns = avg_ns;
    if (out_pct_of_wall) *out_pct_of_wall = pct;
}

void stage_stats_report(const stage_stats_t *s, double wall_seconds)
{
    double avg_ns, pct;
    stage_stats_summary(s, wall_seconds, &avg_ns, &pct);
    printf("  %-14s calls=%-6llu avg=%9.1f ns  min=%9.1f ns  max=%9.1f ns  (%.2f%% of wall)\n",
           s->name, (unsigned long long)s->calls, avg_ns, s->min_ns, s->max_ns, pct);
}

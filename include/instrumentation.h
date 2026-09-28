#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#include <stdint.h>

double now_ns(void);   /* monotonic clock, nanoseconds */

typedef struct {
    const char *name;
    uint64_t    calls;
    double      total_ns;
    double      min_ns;
    double      max_ns;
} stage_stats_t;

void stage_stats_init(stage_stats_t *s, const char *name);
void stage_stats_record(stage_stats_t *s, double elapsed_ns);

/* Human-readable single-line report (used by the standalone main.c). */
void stage_stats_report(const stage_stats_t *s, double wall_seconds);

/* Non-printing accessor (used by the power test harness so it can fold
 * the numbers into its own trial report / CSV row). */
void stage_stats_summary(const stage_stats_t *s, double wall_seconds,
                          double *out_avg_ns, double *out_pct_of_wall);

#endif /* INSTRUMENTATION_H */

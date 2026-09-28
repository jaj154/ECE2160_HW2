#ifndef PIPELINE_TYPES_H
#define PIPELINE_TYPES_H

#include <stdint.h>

/* ---------------------------------------------------------------------
 * NOTE FOR THE TEAM:
 * This header was NOT part of the original three-module upload. It was
 * reconstructed from what io_unit.c / memory_unit.c / compute_unit.c
 * assume about ring sizes, struct layout, and tunable thresholds. If a
 * "real" version of this header already exists elsewhere, diff it
 * against this one before merging -- the ring sizes and thresholds
 * below were chosen to be reasonable for a 4096-tick test run, not
 * pulled from a spec.
 * --------------------------------------------------------------------- */

/* ---- Ring capacities -------------------------------------------------- */
#define RAW_RING_SIZE      64u   /* raw samples buffered by the I/O unit   */
#define SUMMARY_RING_SIZE  32u   /* summary records buffered by memory unit*/
#define PREDICT_WINDOW      8u   /* summaries consumed per prediction      */

/* ---- Memory-unit tuning ----------------------------------------------- */
#define OUTLIER_THRESHOLD_C   3.0f   /* deg C from rough mean = outlier    */

/* ---- Compute-unit tuning ----------------------------------------------- */
#define PROJECTION_TICKS            16.0   /* ticks ahead to project        */
#define OSCILLATION_SIGN_CHANGES     3u    /* sign flips => short-cycling   */
#define RISING_THRESHOLD_C_PER_TICK  0.05  /* slope => undercooling         */
#define FALLING_THRESHOLD_C_PER_TICK -0.05 /* slope => overcooling          */

/* ---- I/O unit types ----------------------------------------------------- */
typedef struct {
    uint32_t tick;
    float    value_c;
    uint8_t  is_outlier;
} raw_sample_t;

typedef struct {
    raw_sample_t buf[RAW_RING_SIZE];
    uint32_t     head;
    uint32_t     count;
} raw_ring_t;

/* ---- Memory unit types --------------------------------------------------- */
typedef struct {
    uint32_t tick_start;
    uint32_t tick_end;
    uint16_t samples_in;
    uint16_t samples_kept;
    float    mean_c;
    float    stddev_c;
    float    min_c;
    float    max_c;
} summary_record_t;

typedef struct {
    summary_record_t buf[SUMMARY_RING_SIZE];
    uint32_t         head;
    uint32_t         count;
} summary_ring_t;

/* ---- Compute unit types --------------------------------------------------- */
typedef enum {
    HVAC_STATE_NOMINAL = 0,
    HVAC_STATE_UNDERCOOLING,
    HVAC_STATE_OVERCOOLING,
    HVAC_STATE_SHORT_CYCLING
} hvac_state_t;

typedef struct {
    uint32_t     tick;
    float        trend_slope_c_per_tick;
    float        projected_temp_c;
    hvac_state_t state;
} prediction_t;

#endif /* PIPELINE_TYPES_H */

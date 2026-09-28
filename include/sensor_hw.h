#ifndef SENSOR_HW_H
#define SENSOR_HW_H

#include <stdint.h>

/* Reconstructed stand-in for real sensor hardware (e.g. a DS18B20 / TMP
 * probe). Produces a deterministic synthetic temperature signal so that
 * EVERY trial in the power experiments sees an IDENTICAL workload/input
 * sequence -- this is essential: if the input data differed between
 * trials, differences in power/time could be attributed to the data
 * instead of the configuration change under test.
 *
 * Determinism is achieved by reseeding a fixed PRNG seed in
 * sensor_hw_init(); as long as sensor_hw_read_raw() is called for
 * ticks 0..N-1 in order after each init(), the sequence is identical
 * run over run. */

void  sensor_hw_init(void);
float sensor_hw_read_raw(uint32_t tick);

#endif /* SENSOR_HW_H */

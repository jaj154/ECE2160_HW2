#include "sensor_hw.h"
#include <stdlib.h>
#include <math.h>

/* Fixed seed => identical synthetic dataset across every trial. Do NOT
 * change this between trials in the same experiment run; the whole
 * point of the harness is that the ONLY thing that changes between
 * trials is the system configuration under test. */
#define SENSOR_HW_FIXED_SEED   424242u

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void sensor_hw_init(void)
{
    srand(SENSOR_HW_FIXED_SEED);
}

float sensor_hw_read_raw(uint32_t tick)
{
    /* Slow diurnal-style drift, representative of a machine-room
     * temperature riding on an HVAC duty cycle. */
    float base = 22.0f + 3.0f * sinf((float)tick * (2.0f * (float)M_PI) / 512.0f);

    /* Small sensor/ADC noise, +/- 0.15 C. */
    float noise = (((float)(rand() % 2001) - 1000.0f) / 1000.0f) * 0.15f;

    float value = base + noise;

    /* Periodic large glitch to exercise the memory unit's outlier
     * rejection path (e.g. a bad ADC read or a door opening near the
     * sensor). */
    if ((tick % 137u) == 0u) {
        value += (rand() % 2) ? 8.0f : -8.0f;
    }

    return value;
}

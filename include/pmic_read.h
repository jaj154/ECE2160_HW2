#ifndef PMIC_READ_H
#define PMIC_READ_H

/* Backend-agnostic PMIC power reader. pmic_init() probes for the
 * fastest available interface (direct hwmon sysfs reads in C) and
 * falls back to vcgencmd if the hwmon rails are not exposed. */

enum {
    PMIC_BACKEND_NONE = 0,
    PMIC_BACKEND_HWMON,     /* fast: direct sysfs reads, ms-scale polling */
    PMIC_BACKEND_VCGENCMD   /* slow fallback: fork vcgencmd per sample     */
};

/* Probe and select a backend. Returns one of the PMIC_BACKEND_* codes.
 * Call once before sampling. */
int pmic_init(void);

/* Human-readable name of the selected backend, for reporting. */
const char *pmic_backend_name(void);

/* Take one sample. Fills VDD_CORE power and summed-rail power in watts.
 * Returns 0 on success, -1 if unavailable. */
int pmic_sample(double *core_w, double *total_w);

#endif /* PMIC_READ_H */

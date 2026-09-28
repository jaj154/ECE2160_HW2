#ifndef POWER_CONFIG_H
#define POWER_CONFIG_H

/* Runtime control of the three "option (1)" device-configuration knobs
 * named in the assignment: clock speed, cpufreq governor, and active
 * core count. Everything here is done through Linux sysfs at runtime
 * (no reboot, no /boot/firmware/config.txt edits) so a single process
 * can sweep configurations in one run.
 *
 * Writing to these sysfs nodes requires root. If not run as root, the
 * write functions return -1 and the caller should report the requested
 * value as "NOT APPLIED (permission denied)" rather than silently
 * assuming it took effect -- this matters for result validity. */

typedef enum {
    CORE_MODE_AFFINITY, /* restrict THIS process to N cores (no root needed
                          * beyond what the process already has; other
                          * cores stay online and can still idle/park) */
    CORE_MODE_OFFLINE   /* physically hotplug cores offline system-wide
                          * (requires root; also removes their idle/leakage
                          * draw, which is the more realistic "reduce core
                          * count to save power" scenario) */
} core_mode_t;

typedef struct {
    int  num_cpus_present;
    char governor[8][64];
    long min_khz[8];
    long max_khz[8];
    int  online[8];
    int  captured;
} power_config_snapshot_t;

int  pc_num_cpus(void);

/* Capture/restore full baseline state so every trial starts identical. */
int  pc_snapshot(power_config_snapshot_t *snap);
int  pc_restore(const power_config_snapshot_t *snap);

/* Clock speed control: pins every online core to as close to khz as the
 * hardware's available frequency table allows. Returns 0 on success. */
int  pc_set_freq_khz_all(long khz);
int  pc_get_available_freq_range(long *min_khz, long *max_khz);
int  pc_get_cur_freq_khz(int cpu, long *out_khz);

/* cpufreq governor control ("performance", "schedutil", "ondemand",
 * "powersave", "conservative" -- availability is kernel/board dependent). */
int  pc_set_governor_all(const char *governor);
int  pc_get_governor(int cpu, char *out, int out_len);

/* Active core count control. n must be >= 1. cpu0 is never offlined. */
int  pc_set_core_count(int n, core_mode_t mode);
int  pc_restore_cores(void); /* bring all present cpus back online + clear affinity */

#endif /* POWER_CONFIG_H */

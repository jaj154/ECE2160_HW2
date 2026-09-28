#define _GNU_SOURCE
#include "power_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>

#define MAX_CPUS 8

static int write_sysfs(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(value);
    size_t written = fwrite(value, 1, len, f);
    fclose(f);
    return (written == len) ? 0 : -1;
}

static int read_sysfs_str(const char *path, char *out, int out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, out_len, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return 0;
}

static int read_sysfs_long(const char *path, long *out)
{
    char buf[64];
    if (read_sysfs_str(path, buf, sizeof(buf)) != 0) return -1;
    *out = strtol(buf, NULL, 10);
    return 0;
}

int pc_num_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n < 1) n = 1;
    if (n > MAX_CPUS) n = MAX_CPUS;
    return (int)n;
}

int pc_get_available_freq_range(long *min_khz, long *max_khz)
{
    char path_min[128], path_max[128];
    snprintf(path_min, sizeof(path_min), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
    snprintf(path_max, sizeof(path_max), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (read_sysfs_long(path_min, min_khz) != 0) return -1;
    if (read_sysfs_long(path_max, max_khz) != 0) return -1;
    return 0;
}

int pc_get_cur_freq_khz(int cpu, long *out_khz)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    return read_sysfs_long(path, out_khz);
}

int pc_get_governor(int cpu, char *out, int out_len)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
    return read_sysfs_str(path, out, out_len);
}

static int governor_supported(int cpu, const char *governor)
{
    char path[128], list[512];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_available_governors", cpu);
    if (read_sysfs_str(path, list, sizeof(list)) != 0) return 0;
    return (strstr(list, governor) != NULL);
}

int pc_set_governor_all(const char *governor)
{
    int n = pc_num_cpus();
    int ok = 0, fail = 0;
    for (int c = 0; c < n; c++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", c);
        if (write_sysfs(path, governor) == 0) ok++; else fail++;
    }
    return (fail == 0 && ok > 0) ? 0 : -1;
}

int pc_set_freq_khz_all(long khz)
{
    int n = pc_num_cpus();
    int all_ok = 1;

    /* Preferred path: "userspace" governor + exact scaling_setspeed.
     * Fallback: "performance" governor with min==max==khz clamp, which
     * pins the frequency on kernels that dropped the userspace
     * governor (common on recent Raspberry Pi OS kernels). */
    int use_userspace = governor_supported(0, "userspace");

    char khz_str[32];
    snprintf(khz_str, sizeof(khz_str), "%ld", khz);

    for (int c = 0; c < n; c++) {
        char p_gov[128], p_min[128], p_max[128], p_speed[128];
        snprintf(p_gov,   sizeof(p_gov),   "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", c);
        snprintf(p_min,   sizeof(p_min),   "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", c);
        snprintf(p_max,   sizeof(p_max),   "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c);
        snprintf(p_speed, sizeof(p_speed), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed", c);

        if (use_userspace) {
            if (write_sysfs(p_gov, "userspace") != 0) { all_ok = 0; continue; }
            if (write_sysfs(p_speed, khz_str) != 0) all_ok = 0;
        } else {
            if (write_sysfs(p_gov, "performance") != 0) { all_ok = 0; continue; }
            /* order matters: raise max before/with min to avoid a
             * min>max transient rejection */
            if (write_sysfs(p_max, khz_str) != 0) all_ok = 0;
            if (write_sysfs(p_min, khz_str) != 0) all_ok = 0;
        }
    }
    return all_ok ? 0 : -1;
}

int pc_snapshot(power_config_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->num_cpus_present = pc_num_cpus();
    for (int c = 0; c < snap->num_cpus_present; c++) {
        pc_get_governor(c, snap->governor[c], sizeof(snap->governor[c]));

        char p_min[128], p_max[128], p_online[128];
        snprintf(p_min, sizeof(p_min), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", c);
        snprintf(p_max, sizeof(p_max), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c);
        read_sysfs_long(p_min, &snap->min_khz[c]);
        read_sysfs_long(p_max, &snap->max_khz[c]);

        if (c == 0) {
            snap->online[c] = 1; /* cpu0 has no "online" control file */
        } else {
            snprintf(p_online, sizeof(p_online), "/sys/devices/system/cpu/cpu%d/online", c);
            char buf[8] = "1";
            read_sysfs_str(p_online, buf, sizeof(buf));
            snap->online[c] = (buf[0] == '1');
        }
    }
    snap->captured = 1;
    return 0;
}

int pc_restore(const power_config_snapshot_t *snap)
{
    if (!snap->captured) return -1;
    int fail = 0;

    /* Bring cores back online first, then restore freq/governor. */
    for (int c = 1; c < snap->num_cpus_present; c++) {
        if (snap->online[c]) {
            char p_online[128];
            snprintf(p_online, sizeof(p_online), "/sys/devices/system/cpu/cpu%d/online", c);
            write_sysfs(p_online, "1");
        }
    }
    /* Clear any affinity restriction placed on this process. */
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int c = 0; c < snap->num_cpus_present; c++) CPU_SET(c, &all);
    sched_setaffinity(0, sizeof(all), &all);

    for (int c = 0; c < snap->num_cpus_present; c++) {
        char p_gov[128], p_min[128], p_max[128];
        snprintf(p_gov, sizeof(p_gov), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", c);
        snprintf(p_min, sizeof(p_min), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", c);
        snprintf(p_max, sizeof(p_max), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c);

        char minbuf[32], maxbuf[32];
        snprintf(maxbuf, sizeof(maxbuf), "%ld", snap->max_khz[c]);
        snprintf(minbuf, sizeof(minbuf), "%ld", snap->min_khz[c]);
        if (write_sysfs(p_max, maxbuf) != 0) fail = 1;
        if (write_sysfs(p_min, minbuf) != 0) fail = 1;
        if (snap->governor[c][0] && write_sysfs(p_gov, snap->governor[c]) != 0) fail = 1;
    }
    return fail ? -1 : 0;
}

int pc_set_core_count(int n, core_mode_t mode)
{
    int total = pc_num_cpus();
    if (n < 1) n = 1;
    if (n > total) n = total;

    if (mode == CORE_MODE_AFFINITY) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 0; c < n; c++) CPU_SET(c, &set);
        return (sched_setaffinity(0, sizeof(set), &set) == 0) ? 0 : -1;
    }

    /* CORE_MODE_OFFLINE: keep cpu0..n-1 online, offline the rest.
     * cpu0 is never offlined (most ARM SMP kernels refuse anyway). */
    int fail = 0;
    for (int c = 1; c < total; c++) {
        char p_online[128];
        snprintf(p_online, sizeof(p_online), "/sys/devices/system/cpu/cpu%d/online", c);
        const char *want = (c < n) ? "1" : "0";
        if (write_sysfs(p_online, want) != 0) fail = 1;
    }
    return fail ? -1 : 0;
}

int pc_restore_cores(void)
{
    int total = pc_num_cpus();
    int fail = 0;
    for (int c = 1; c < total; c++) {
        char p_online[128];
        snprintf(p_online, sizeof(p_online), "/sys/devices/system/cpu/cpu%d/online", c);
        if (write_sysfs(p_online, "1") != 0) fail = 1;
    }
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int c = 0; c < total; c++) CPU_SET(c, &all);
    sched_setaffinity(0, sizeof(all), &all);
    return fail ? -1 : 0;
}

# Critter power experiments

Code for evaluating power-consumption options against the Critter's
three prototype units (`io_unit`, `memory_unit`, `compute_unit`) on a
Raspberry Pi 5.

## What's here

```
include/               all headers
src/
  io_unit.c             <- unmodified, as provided
  memory_unit.c         <- unmodified, as provided
  compute_unit.c        <- unmodified, as provided
  sensor_hw.c           <- NEW: deterministic synthetic sensor (see below)
  instrumentation.c     <- NEW: timing/stats helpers referenced by main.c
  pipeline_workload.c   <- NEW: your main.c's tick loop, extracted into
                             pipeline_run() so it can be called repeatedly
                             by the test harness across trials
  main.c                <- slimmed down; behavior/output unchanged from
                             the original main.c you provided
  power_config.c        <- NEW: clock speed / governor / core-count control
  power_monitor.c       <- NEW: vcgencmd pmic_read_adc background sampler
  run_experiments.c     <- NEW: the test-case driver (the main deliverable)
docs/power_model_notes.md   background formulas + citations (not the paper)
Makefile
```

### Files that had to be reconstructed

The uploaded `main.c` includes several headers and calls several
functions that weren't provided: `pipeline_types.h`, `io_unit.h`,
`memory_unit.h`, `compute_unit.h`, `instrumentation.h`, `sensor_hw.h`,
and the implementations `sensor_hw.c` / `instrumentation.c`. These
were written to match exactly what the three provided `.c` files
assume (ring sizes, struct fields, threshold constants). **If your
team has the real versions of these files, replace the ones here and
rebuild** — the logic in `io_unit.c` / `memory_unit.c` /
`compute_unit.c` was not altered, so it should drop in cleanly as long
as struct field names match.

## Building

```
make            # builds bin/critter_main and bin/run_experiments
make clean
```

Tested to compile clean on a normal Linux box (this sandbox has
neither Pi hardware nor `vcgencmd`, so power fields correctly report
N/A here — that's expected, not a bug). On the Pi 5 with `vcgencmd`
present it will report real numbers.

## Running

```
./bin/critter_main          # original single-run pipeline report
sudo ./bin/run_experiments  # the config-sweep power suite (clock/cores/governor)
./bin/analyze_embedded      # embedded power aspects (idle, duty-cycle, static/dynamic)
./bin/analyze_stages        # NEW: per-stage DIRECT power isolation (io/memory/compute)
sudo ./scripts/peripheral_states.sh    # runs analyze_embedded per GUI/BT state
sudo ./scripts/run_asm_experiment.sh   # NEW: assembly multiply-vs-shift, measured
```

### Fast PMIC backend (pmic_read.c)

Power sampling now goes through `pmic_read`, which probes for the Pi 5
PMIC rails in the kernel hwmon sysfs tree and reads them directly in C
(open/read/close, no subprocess). This removes the ~100 ms per-sample
cost of forking `vcgencmd`, allowing millisecond-scale polling and much
tighter error bars. If the hwmon rails are not exposed on your firmware,
it automatically falls back to the original `vcgencmd` parse, so results
remain valid either way. Each harness prints which backend it selected
(`hwmon-direct` or `vcgencmd`); note it in the paper, since the sampling
rate differs between them.

### bin/analyze_stages — per-stage power isolation

Drives each of the three prototype units in isolation under sustained
load and measures the power drawn while only that stage runs, then
subtracts the idle floor to isolate each stage's dynamic power. This
produces a DIRECT measured decomposition of dynamic power across the
three units, replacing the time-share-as-energy-share assumption with
data. Writes `stage_results.csv`. No root required.

### scripts/run_asm_experiment.sh — assembly optimization, measured

Compiles the integer scaling kernel with `gcc -S`, locates the hardware
multiply, hand-edits it into a shift (arch-aware: `imul`→`sal` on x86,
`mul`→`lsl` on the Pi's aarch64), builds both variants, and measures
each under the PMIC monitor. Confirms identical checksums (correctness
preserved) and writes `asm_results.csv` with a multiply-vs-shift energy
comparison. This is the direct experimental evaluation of assignment
Option 2.

### bin/analyze_embedded — embedded power aspects

A second harness that measures what the config sweep did not: the
platform power floor and how it dominates a mostly-idle deployment.
Reports (console + `embedded_results.csv`):
- **Idle power** — measured with the process *blocked* (nanosleep,
  not busy-looping) so the CPU can drop into shallow idle states, with
  the network left up. This is the realistic platform floor.
- **Active power** — the pipeline under sustained load, for contrast.
- **Static vs dynamic split** — *derived* (not directly measurable on a
  running Linux box): static ≈ idle floor, dynamic ≈ active − idle.
  Labelled as an estimate throughout.
- **Duty-cycled energy model** — the key deployment result: models one
  pipeline pass per sample period followed by idle for the remainder,
  swept across duty cycles from 100% down to 0.1%, showing average
  power collapsing toward the idle floor as the duty cycle shrinks.
  This is what demonstrates that idle power, not active-config choice,
  governs a real monitor's energy budget.

Doesn't need root (PMIC reads work unprivileged), but the process-idle
figure is cleanest on an otherwise-quiet machine.

### scripts/peripheral_states.sh — GUI/Bluetooth comparison

Toggling the desktop GUI and Bluetooth are whole-board, system-level
actions, so they're driven from a script that runs `analyze_embedded`
once per state (as-found, GUI-off/BT-on, GUI-off/BT-off, GUI-on/BT-off)
and concatenates the per-state rows into `embedded_results_all.csv`.

**Run it over SSH, not from the desktop** — stopping the display
manager will drop a local session. The script captures your original
GUI/BT state and restores it on exit (including Ctrl-C) via a trap.

**`run_experiments` needs `sudo`.** Writing to
`/sys/devices/system/cpu/.../scaling_governor`,
`scaling_min/max_freq`, `scaling_setspeed`, and `.../online` all
require root. Without root the program still runs to completion (it
never crashes on a failed write) but every trial after baseline will
print `requested change applied: NO` and you'll effectively be
re-measuring the same baseline config ten times — still useful as a
sanity check that the harness's own overhead/workload is consistent,
but not useful as the actual power comparison.

**Runtime (v3): expect ~3-4 minutes.** The harness now runs each of
the 10 configs `REPEATS` times (default 5), each repeat sustaining
load for ~`TARGET_TRIAL_SECONDS` (default 3s), plus a settle delay and
warm-up pass per config. That's roughly 10 × 5 × 3s ≈ 150s of load
plus overhead. If you want a faster smoke test, lower `REPEATS` and
`TARGET_TRIAL_SECONDS` at the top of `src/run_experiments.c`.

### v3 measurement refinements

Three changes over v2, all aimed at measurement precision (not at
changing which option "wins"):
- **Settle-before-calibrate**: after applying a config, the harness
  waits `SETTLE_MS` and runs a discarded warm-up pass before timing,
  so calibration reflects steady state rather than a mid-transition
  moment (this is what previously gave the core-count trials short,
  low-sample windows).
- **In-run frequency sampling**: cpu0 frequency is now sampled on the
  same schedule as power, *during* the sustained load, and reported as
  avg/min/max — instead of a single pre-load snapshot that read stale
  for demand-based governors (`ondemand`/`schedutil`). The pre-load
  snapshot is still printed but explicitly flagged as the untrustworthy
  one.
- **Repeated trials + variance**: each config is measured `REPEATS`
  times and `results.csv` carries the mean *and* sample standard
  deviation for time, power, and energy — so a real effect can be told
  apart from run-to-run noise (and the paper can show error bars).

Each run produces:
- a console report per trial (setting changed → confirmed hardware
  state → processing-validation numbers → power numbers)
- `results.csv` in the working directory, one row per trial, ready to
  drop into a spreadsheet for the paper's tables/charts

## Trials run by `run_experiments`

| # | Category | Trials |
|---|----------|--------|
| 1 | baseline | whatever the system's default config already is |
| 2-4 | clock speed | max / mid / min available frequency, pinned via `performance` governor + min=max clamp (or `userspace` governor if supported) |
| 5-7 | core count | 4 / 2 / 1 active cores, extra cores **physically offlined** via sysfs (not just CPU-affinity-restricted) so idle-core leakage is actually removed |
| 8-10 | governor | `performance` / `schedutil` / `powersave`, default frequency range, policy decides |

Every trial calls the identical `pipeline_run()` over the identical
4096-tick synthetic dataset (fixed RNG seed in `sensor_hw.c`), so any
difference in timing or power between trials is attributable to the
configuration change, not to different input data. The harness
restores the captured baseline config before *and* after every trial
so trials never inherit leftover state from the previous one.

## Power measurement

Uses `vcgencmd pmic_read_adc` (background thread, polled every 200 ms
during each trial), per the assignment's Hint 1/2. Reports:
- `avg_core_power_w` — the `VDD_CORE` rail alone (most directly tied
  to the three knobs under test)
- `avg_summed_rails_w` — sum of all ~12 PMIC rails (raw estimate,
  excludes 5V/USB/HAT/NVMe draw — the PMIC can't see that)
- `avg_corrected_total_w` — the above scaled by a linear calibration
  from a third-party USB-C-meter comparison (jfikar/RPi5-power); this
  constant is unit-specific — recalibrate with your own meter if you
  have one and want tighter numbers
- `energy_joules` / `energy_per_prediction_mj` — the normalized metric
  that actually lets you compare "less power but slower" trials fairly

Full formulas/citations for *why* each knob should move power the way
it does: see `docs/power_model_notes.md`.

## Known limitations / things to check on real hardware

- `pc_set_freq_khz_all()` tries `userspace` governor first, falls back
  to `performance` + min=max clamp if `userspace` isn't in
  `scaling_available_governors` (common on recent Raspberry Pi OS
  kernels). Confirm which path your kernel took by checking the
  printed "measured cpu0 governor" line per trial.
- Core-offline mode never offlines `cpu0` (most ARM SMP kernels refuse
  this anyway).
- If `vcgencmd` isn't on the PATH or the PMIC ADC isn't exposed on
  your specific Pi 5 firmware version, `avg_*_power_w` will be 0 and
  `vcgencmd_available` will report false — the harness still runs and
  the timing/correctness numbers are still valid.

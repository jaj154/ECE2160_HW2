# Power model reference notes

Background reference for the three configuration knobs under test
(clock speed, active core count, cpufreq governor). This is **not**
the workshop paper — it's the formulaic grounding to pull from when
writing the paper's analysis section, with sources for anything that
doesn't reduce to a clean closed-form equation.

## 1. Clock speed (DVFS)

The standard CMOS dynamic-power model:

```
P_dynamic = A * C_L * V^2 * f
```

where `A` is the switching activity factor, `C_L` the load
capacitance, `V` the supply voltage, and `f` the clock frequency.
Total power is dynamic + leakage: `P_avg = P_dynamic + P_leakage`.

- Source: A. Chandrakasan and R. Brodersen, "Minimizing Power
  Consumption in Digital CMOS Circuits," *Proceedings of the IEEE*,
  vol. 83, no. 4, pp. 498–523, 1995. (Peer-reviewed; this is the
  textbook derivation cited by nearly every later power-modeling
  paper, including the ones below.)
- Restated with the same notation in: S. Pagani et al., *Energy
  Efficient Computing Systems: Architectures, Abstractions and
  Modeling to Techniques and Standards*, arXiv:2007.09976 (survey,
  see Sec. 2.2 for `P_avg = P_dynamic + P_leakage`).

**Why this matters for the clock-speed sweep:** on real hardware, `V`
and `f` aren't independent — DVFS lowers `V` as `f` drops. The
alpha-power law relates them as `f ∝ V^(α_c - 1)`, i.e.
`V = k_V * f^(1/(α_c-1))`. Substituting into `P = A*C*V^2*f` shows
power collapses *faster* than linearly with frequency once you
account for the accompanying voltage drop — this is the whole reason
DVFS-based clock reduction is expected to outperform a naive "just
run fewer instructions per second at the same voltage" intuition. If
the harness's `pc_set_freq_khz_all()` pins frequency without the
kernel also lowering voltage (some boards clamp voltage to the
highest pinned point's requirement), the team should expect to see a
noticeably smaller-than-`V^2` power drop and should say so explicitly
in the paper rather than assume ideal DVFS behavior.
- Source: Y. Xie et al. (alpha-power MOSFET model application),
  restated in the same arXiv:2411.19854 system model (Sec. II-A) used
  for a multi-step-update energy paper — cites the classical
  alpha-power law (T. Sakurai and A. R. Newton, "Alpha-power law
  MOSFET model...," *IEEE JSSC*, 1990) for `f ∝ V^(α_c-1)`.

**Energy, not just power:** for a fixed amount of *work* `W` (here,
one full 4096-tick pipeline pass), execution time scales roughly as
`T ∝ 1/f`, so `E = P * T ∝ (C*V^2*f) * (W/f) = C*V^2*W` — energy for a
fixed task is independent of frequency at constant voltage, and drops
with `V^2` when voltage is allowed to scale down with frequency. This
is why the harness reports **energy per prediction** (mJ), not just
average watts: a lower-power trial that takes proportionally longer
is not automatically a win, and a lower-frequency trial that also
drops voltage should show a net energy improvement per unit of work,
not just lower instantaneous wattage.

## 2. Active core count

There isn't a single universally-cited closed-form equation for
"power vs. active core count" the way there is for `P = ACV²f`,
because it depends on how much of the chip is shared/uncore vs.
per-core, and on leakage-vs-dynamic balance at the process node in
question. The reasonable model to use, and the one implicit in the
harness's design:

```
P_total(N) ≈ N * P_core_active + P_uncore + (n_total - N) * P_core_idle_leakage
```

i.e. total power is roughly linear in the number of *active* cores,
plus a fixed shared/uncore term, plus leakage from any cores that are
merely idle rather than physically offlined. This is why the harness
implements core-count reduction as `CORE_MODE_OFFLINE` (actually
hotplugging cores offline via sysfs) rather than only CPU affinity —
affinity alone leaves the unused cores idle-but-leaking, which masks
the very effect being measured.

- Source (for the leakage-vs-active-core tradeoff and the argument
  that per-core power does not scale cleanly with count at modern
  process nodes — the "dark silicon" result): H. Esmaeilzadeh, E.
  Blem, R. St. Amant, K. Sankaralingam, D. Burger, "Dark Silicon and
  the End of Multicore Scaling," *Proc. 38th Int'l Symposium on
  Computer Architecture (ISCA '11)*, ACM, pp. 365–376, 2011.
  (Peer-reviewed; directly analyzes the power/performance tradeoff of
  varying active core count under a fixed power budget, which is the
  same regime this experiment probes at a much smaller scale.)

## 3. cpufreq governor

Governors don't add a new physical mechanism — each one is just a
different control policy over the same `P = ACV²f` relationship,
choosing *when* to move along the frequency/voltage curve:

- `performance`: pins `f` at max at all times → highest instantaneous
  power, shortest run time. Useful as the upper bound of the sweep.
- `powersave`: pins `f` at min at all times → lowest instantaneous
  power, longest run time. Lower bound of the sweep.
- `schedutil` / `ondemand`: dynamically tracks load, so the pipeline
  workload's actual power/energy under "normal" operation should fall
  between the two pinned extremes. This is the governor the Critter
  would realistically ship with.

Because a governor changes *when* voltage/frequency transitions
happen rather than the underlying equation, the same `E = P*T`
reasoning from Sec. 1 applies — the interesting number to report is
energy per prediction under each governor, not raw average watts,
since `schedutil` will spend some ticks at low frequency and some
at high frequency depending on load.

- Source (authoritative but not peer-reviewed — cite as a primary/
  technical source, not a research paper): Linux kernel documentation,
  "CPU Frequency and Voltage Scaling Code in the Linux(TM) Kernel,"
  `Documentation/admin-guide/pm/cpufreq.rst`, torvalds/linux, kernel.org.
- Peer-reviewed evaluation of governor energy impact under real
  workloads (good citation for the "how much does governor choice
  actually matter in practice" argument): V. Spiliopoulos, S.
  Kaxiras, G. Keramidas, "Green governors: A framework for
  continuously adaptive DVFS," *2011 International Green Computing
  Conference and Workshops*, IEEE, 2011.

## 4. Power measurement methodology (Raspberry Pi 5 specifics)

The Pi 5's PMIC exposes voltage and current for ~12 internal power
rails via `vcgencmd pmic_read_adc` (rail names such as `VDD_CORE_A`/
`VDD_CORE_V`, `1V8_SYS_A`/`_V`, `DDR_VDD2_A`/`_V`, etc.). Summing
`V*I` across every rail that has both a current and a voltage entry
gives an estimate of total board power — but this excludes anything
drawn over the 5V rail downstream of the PMIC (USB peripherals, HATs,
NVMe HAT, etc.), since the Pi 5's PMIC does not directly meter the
5V input current.

- Source: RPi5-power (J. Fikar), <https://github.com/jfikar/RPi5-power>.
  Also empirically derives a linear correction against a USB-C power
  meter: `real_consumption_W = pmic_output_W * 1.1451 + 0.5879`
  (measured on one specific unit — **recalibrate this on the team's
  own board with a USB-C meter if precision matters**; treat the
  constants in `power_monitor.c` as a starting estimate, not ground
  truth).
- Source: Raspberry Pi Forums, "Onboard voltage/current monitoring on
  Pi 5," <https://forums.raspberrypi.com/viewtopic.php?t=367244> —
  discusses the same PMIC ADC interface and its coverage limitations.

`VDD_CORE` specifically is reported alongside the summed-rail total
in every trial because it's the rail most directly driven by the
Arm core voltage regulator — the one the clock-speed, core-count, and
governor experiments are actually expected to move. The summed-rail
and corrected totals are broader system estimates and will move less
sharply, since they include comparatively fixed-consumption rails
(DDR, I/O) that aren't targeted by any of the three experiments.

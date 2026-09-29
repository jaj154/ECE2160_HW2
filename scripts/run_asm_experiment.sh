#!/usr/bin/env bash
# run_asm_experiment.sh
#
# Assignment Option 2, end to end, on whatever architecture the Pi is:
#   1. compile the scaling kernel to assembly with `gcc -S`
#   2. show the target instruction (a hardware multiply)
#   3. hand-edit that one instruction into a shift (done here with sed so
#      it is reproducible, but the edit is a single-instruction swap of
#      exactly the kind the assignment describes)
#   4. build two binaries -- multiply variant and shift variant
#   5. run each under the PMIC power monitor and record energy
#
# The Pi 5 is aarch64, so the multiply is `mul` and the shift is `lsl`.
# On x86 dev boxes it is `imul` -> `sal`. The script handles both.
#
# Run from the repo root:  sudo ./scripts/run_asm_experiment.sh
# (sudo not strictly required for PMIC reads, but keeps parity with the
#  other experiments and avoids scheduler interference.)

set -eu
INC=include
CC=${CC:-gcc}
ARCH=$(uname -m)
rm -f asm_results.csv

echo "############################################################"
echo "# Assembly-level optimization experiment  (arch: $ARCH)"
echo "############################################################"

# ---- 1. force a real hardware multiply in the baseline kernel ----------
# An inline-asm barrier makes the compiler treat the factor as an opaque
# runtime value it cannot constant-fold into a shift. A plain `volatile`
# is enough on x86 but the aarch64 optimizer sees through it and emits a
# shift anyway; the barrier forces a genuine `mul` (aarch64) / `imul`
# (x86) on both.
cat > /tmp/kernel_mul.c << 'EOF'
#include "asm_kernel.h"
uint64_t kernel_scale(const uint32_t *samples, uint32_t n)
{
    uint64_t factor = 64u;
    __asm__ volatile("" : "+r"(factor));   /* opaque: prevents strength reduction */
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++)
        acc += (uint64_t)samples[i] * factor;
    return acc;
}
EOF

echo ">>> Step 1: gcc -S  (generating assembly for the multiply kernel)"
$CC -S -O1 -I$INC /tmp/kernel_mul.c -o kernel_mul.s

echo ">>> Step 2: locating the multiply instruction in kernel_mul.s"
if [[ "$ARCH" == aarch64* || "$ARCH" == arm* ]]; then
    MUL_RE='\bmul\b'
    grep -nE "$MUL_RE" kernel_mul.s || { echo "no mul found (compiler may have strength-reduced); aborting"; exit 1; }
else
    MUL_RE='\bimul'
    grep -nE "$MUL_RE" kernel_mul.s || { echo "no imul found; aborting"; exit 1; }
fi

# ---- 3. hand-edit: multiply -> shift -----------------------------------
echo ">>> Step 3: substituting the multiply with a shift (2^6 = 64)"
cp kernel_mul.s kernel_shift.s
if [[ "$ARCH" == aarch64* || "$ARCH" == arm* ]]; then
    # aarch64: `mul Xd, Xa, Xb` -> `lsl Xd, X?, #6`, where X? is whichever
    # of Xa/Xb is NOT the factor register. The factor (64) is loaded with a
    # `mov X?, #64` (or w-reg) shortly before the mul; we find that register
    # so we shift the SAMPLE operand, not the constant. If we cannot
    # identify it, we fall back to assuming the factor is the last operand
    # (the common layout) and rely on the Step-6 checksum guard to catch a
    # wrong guess.
    line=$(grep -nE '\bmul\b' kernel_shift.s | head -1 | cut -d: -f1)
    orig=$(sed -n "${line}p" kernel_shift.s)
    d=$(echo "$orig" | sed -E 's/.*mul[[:space:]]+([xw][0-9]+),[[:space:]]*([xw][0-9]+),[[:space:]]*([xw][0-9]+).*/\1/')
    a=$(echo "$orig" | sed -E 's/.*mul[[:space:]]+([xw][0-9]+),[[:space:]]*([xw][0-9]+),[[:space:]]*([xw][0-9]+).*/\2/')
    b=$(echo "$orig" | sed -E 's/.*mul[[:space:]]+([xw][0-9]+),[[:space:]]*([xw][0-9]+),[[:space:]]*([xw][0-9]+).*/\3/')

    # Which of a/b holds the constant 64? Look backwards for `mov <reg>, #64`.
    # Compare on the numeric register index so x3/w3 match.
    aidx=$(echo "$a" | tr -dc '0-9')
    bidx=$(echo "$b" | tr -dc '0-9')
    factreg=$(awk -v L="$line" '
        NR<L && /mov[[:space:]]+[xw][0-9]+,[[:space:]]*#64\b/ {
            match($0,/[xw][0-9]+/); r=substr($0,RSTART,RLENGTH); gsub(/[xw]/,"",r); last=r
        } END{print last}' kernel_shift.s)

    if [[ "$factreg" == "$bidx" ]]; then
        sample="$a"    # factor is b, so shift a
    elif [[ "$factreg" == "$aidx" ]]; then
        sample="$b"    # factor is a, so shift b
    else
        sample="$a"    # unknown: assume factor is last operand (b); guard will verify
    fi
    sed -i "${line}s|.*|\tlsl\t${d}, ${sample}, #6\t// HAND-EDIT: multiply-by-64 -> shift-left-6|" kernel_shift.s
else
    # x86-64: `imulq %rsi, %rdx` -> `salq $6, %rdx`
    sed -i -E 's/\timulq\t(%[a-z0-9]+), (%[a-z0-9]+)/\tsalq\t$6, \2\t# HAND-EDIT: multiply-by-64 -> shift-left-6/' kernel_shift.s
fi

echo ">>> Diff of the one-instruction change:"
diff kernel_mul.s kernel_shift.s || true

# ---- 4. build the two binaries -----------------------------------------
echo ">>> Step 4: building multiply and shift binaries"
OBJS="bin/power_monitor.o bin/pmic_read.o"
# ensure power_monitor.o + pmic_read.o exist (built by the main Makefile)
if [[ ! -f bin/power_monitor.o || ! -f bin/pmic_read.o ]]; then make bin/power_monitor.o bin/pmic_read.o >/dev/null 2>&1 || make >/dev/null 2>&1; fi

$CC -O1 -I$INC -DVARIANT_LABEL='"multiply"' src/asm_experiment.c kernel_mul.s  $OBJS -o bin/asm_multiply -lm -lpthread
$CC -O1 -I$INC -DVARIANT_LABEL='"shift"'    src/asm_experiment.c kernel_shift.s $OBJS -o bin/asm_shift    -lm -lpthread

# ---- 5. run both --------------------------------------------------------
echo ">>> Step 5: measuring MULTIPLY variant"
./bin/asm_multiply
echo ""
echo ">>> Step 5: measuring SHIFT variant"
./bin/asm_shift

# ---- 6. correctness guard ----------------------------------------------
# The shift is only a valid substitution if it computes the SAME result as
# the multiply. On aarch64 the register the barrier introduces can land in
# either mul operand, so a naive edit could shift the wrong register. We
# verify the two variants' checksums match before trusting any timing/power
# comparison; if they differ, the hand-edit was wrong and the run is void.
echo ""
echo ">>> Step 6: verifying correctness (checksums must match)"
cm=$(grep -E '^multiply,' asm_results.csv | cut -d, -f2)
cs=$(grep -E '^shift,'    asm_results.csv | cut -d, -f2)
if [[ -n "$cm" && -n "$cs" && "$cm" == "$cs" ]]; then
    echo "    OK: checksums match ($cm) -- shift is a valid substitution."
else
    echo "    ERROR: checksum mismatch (multiply=$cm shift=$cs)."
    echo "    The assembly edit shifted the wrong operand. Paste kernel_shift.s"
    echo "    (the 'lsl' line) back and it can be corrected by hand."
fi

echo ""
echo ">>> Combined results in asm_results.csv:"
cat asm_results.csv
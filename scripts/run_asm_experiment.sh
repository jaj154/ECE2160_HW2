#!/usr/bin/env bash
# run_asm_experiment.sh
#
# Assignment Option 2, end to end: compare the energy of a scale-by-64
# operation implemented with a hardware MULTIPLY versus an explicit
# SHIFT, on whatever architecture/word-size the Pi is running.
#
# Rather than hand-edit a multi-instruction 64-bit multiply (which on a
# 32-bit build becomes umull+mla and is fragile to rewrite), we compile
# TWO C implementations -- one that forces a real multiply, one that
# uses an explicit <<6 -- and:
#   1. use `gcc -S` to show which instruction each version emits
#   2. build and measure both under the PMIC power monitor
#   3. verify both produce identical checksums (correctness)
#
# This satisfies the assignment's `gcc -S` requirement (we inspect the
# generated assembly) while producing a robust, measured comparison.
#
# Run from the repo root:  sudo bash scripts/run_asm_experiment.sh

set -eu
INC=include
CC=${CC:-gcc}
ARCH=$(uname -m)
BITS=$(getconf LONG_BIT 2>/dev/null || echo "?")
rm -f asm_results.csv

echo "############################################################"
echo "# Assembly-level optimization experiment"
echo "# arch: $ARCH   word size: ${BITS}-bit"
echo "############################################################"

# ---- 1. gcc -S: show the instruction each variant compiles to ----------
echo ">>> Step 1: gcc -S  (inspecting emitted instructions)"
$CC -S -O2 -I$INC src/asm_kernel.c -o asm_kernel.s

echo ">>> MULTIPLY variant (kernel_scale_mul) emits:"
mul_ins=$(sed -n '/kernel_scale_mul:/,/\.size.*kernel_scale_mul/p' asm_kernel.s | grep -iE '(umull|umlal|imul|madd|\bmla\b|\bmul)' || true)
if [[ -n "$mul_ins" ]]; then echo "$mul_ins" | sed 's/^/      /'; else echo "      (no multiply mnemonic found)"; fi

echo ">>> SHIFT variant (kernel_scale_shift) emits:"
shift_ins=$(sed -n '/kernel_scale_shift:/,/\.size.*kernel_scale_shift/p' asm_kernel.s | grep -iE '(lsl|lsr|asr|shl|sal|shr|orr)' || true)
if [[ -n "$shift_ins" ]]; then echo "$shift_ins" | sed 's/^/      /'; else echo "      (no shift mnemonic found)"; fi

echo ">>> (full listing saved in asm_kernel.s for the paper)"

# ---- 2. build both measurement binaries --------------------------------
echo ">>> Step 2: building multiply and shift binaries"
OBJS="bin/power_monitor.o bin/pmic_read.o"
if [[ ! -f bin/power_monitor.o || ! -f bin/pmic_read.o ]]; then
    make bin/power_monitor.o bin/pmic_read.o >/dev/null 2>&1 || make >/dev/null 2>&1
fi

$CC -O2 -I$INC -DVARIANT_LABEL='"multiply"' \
    src/asm_experiment.c src/asm_kernel.c $OBJS -o bin/asm_multiply -lm -lpthread
$CC -O2 -I$INC -DVARIANT_LABEL='"shift"' -DVARIANT_IS_SHIFT \
    src/asm_experiment.c src/asm_kernel.c $OBJS -o bin/asm_shift -lm -lpthread

# ---- 3. measure both ---------------------------------------------------
echo ">>> Step 3: measuring MULTIPLY variant"
./bin/asm_multiply
echo ""
echo ">>> Step 3: measuring SHIFT variant"
./bin/asm_shift

# ---- 4. correctness guard ----------------------------------------------
echo ""
echo ">>> Step 4: verifying correctness (checksums must match)"
cm=$(grep -E '^multiply,' asm_results.csv | cut -d, -f2)
cs=$(grep -E '^shift,'    asm_results.csv | cut -d, -f2)
if [[ -n "$cm" && -n "$cs" && "$cm" == "$cs" ]]; then
    echo "    OK: checksums match ($cm) -- both variants compute the same result."
else
    echo "    ERROR: checksum mismatch (multiply=$cm shift=$cs)."
fi

echo ""
echo ">>> Combined results in asm_results.csv:"
cat asm_results.csv

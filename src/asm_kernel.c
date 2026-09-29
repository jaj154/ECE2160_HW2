/* asm_kernel.c
 *
 * A small, self-contained integer kernel used ONLY for the assembly-
 * level optimization experiment (assignment Option 2). It is
 * deliberately representative of the kind of fixed-point scaling a
 * production embedded build of the memory unit would use instead of
 * floating point: raw ADC-style samples are accumulated and rescaled
 * by a constant factor.
 *
 * The operation of interest is the multiply-by-constant on the marked
 * line. When the constant is a power of two, an optimizing compiler is
 * free to emit a shift instead of a hardware multiply -- but to make
 * the experiment explicit and controllable (rather than at the mercy
 * of -O2 heuristics), this file is compiled at -O0 with `gcc -S`, and
 * the multiply instruction in the emitted assembly is replaced by a
 * shift by hand. The two objects are then linked into separate
 * binaries and measured under the same PMIC harness.
 *
 * kernel_scale() is marked noinline and lives in its own translation
 * unit so the target instruction is easy to locate in the .s file and
 * is not inlined away.
 */

#include "asm_kernel.h"

/* SCALE_SHIFT: multiply by 2^SCALE_SHIFT. Kept as a literal multiply in
 * C source; the whole point of the experiment is what the toolchain and
 * the hand edit do with it at the instruction level. */
#define SCALE_FACTOR 64u   /* == 1 << 6 */

__attribute__((noinline))
uint64_t kernel_scale(const uint32_t *samples, uint32_t n)
{
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        /* >>> TARGET OPERATION <<<
         * multiply-by-constant; becomes `imul`/`mul` at -O0, hand-edited
         * to a shift in the modified assembly variant. */
        uint64_t scaled = (uint64_t)samples[i] * SCALE_FACTOR;
        acc += scaled;
    }
    return acc;
}

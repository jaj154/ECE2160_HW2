/* asm_kernel.c
 *
 * Two implementations of the same integer scaling operation for the
 * assembly-level optimization experiment (assignment Option 2):
 *
 *   kernel_scale_mul()   -- scales each sample by a runtime-opaque
 *                           factor of 64, which the compiler must
 *                           implement as a hardware MULTIPLY (it cannot
 *                           prove the value is a power of two).
 *   kernel_scale_shift() -- scales by an explicit left-shift of 6,
 *                           i.e. the "more power efficient" instruction
 *                           the assignment suggests substituting in.
 *
 * Both compute an identical 64-bit result, so any difference measured
 * between them is purely the cost of MUL versus SHIFT on this platform.
 * Compiling this file with `gcc -S` (see scripts/run_asm_experiment.sh)
 * shows exactly which instruction each version emits -- on a Pi 5 the
 * multiply becomes `umull`/`mla` (32-bit build) or `mul` (64-bit), and
 * the shift becomes a shift-and-combine sequence. This makes the
 * comparison explicit at the instruction level without requiring a
 * fragile hand-edit of a multi-register multiply.
 */

#include "asm_kernel.h"

__attribute__((noinline))
uint64_t kernel_scale_mul(const uint32_t *samples, uint32_t n)
{
    uint64_t factor = 64u;
    __asm__ volatile("" : "+r"(factor));   /* opaque: forces a real multiply */
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++)
        acc += (uint64_t)samples[i] * factor;
    return acc;
}

__attribute__((noinline))
uint64_t kernel_scale_shift(const uint32_t *samples, uint32_t n)
{
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++)
        acc += (uint64_t)samples[i] << 6;   /* explicit shift == x64 */
    return acc;
}

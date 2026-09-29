#ifndef ASM_KERNEL_H
#define ASM_KERNEL_H

#include <stdint.h>

/* Accumulate n samples, each scaled by a constant power-of-two factor.
 * Defined in asm_kernel.c; the multiply-by-constant inside is the
 * target of the assembly-level shift-vs-multiply experiment. */
uint64_t kernel_scale(const uint32_t *samples, uint32_t n);

#endif /* ASM_KERNEL_H */

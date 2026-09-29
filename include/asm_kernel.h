#ifndef ASM_KERNEL_H
#define ASM_KERNEL_H

#include <stdint.h>

/* Two variants of the same scale-by-64 accumulation. mul uses a
 * runtime-opaque hardware multiply; shift uses an explicit <<6. Both
 * return an identical 64-bit result; the experiment measures the
 * energy difference between the two instruction choices. */
uint64_t kernel_scale_mul(const uint32_t *samples, uint32_t n);
uint64_t kernel_scale_shift(const uint32_t *samples, uint32_t n);

#endif /* ASM_KERNEL_H */

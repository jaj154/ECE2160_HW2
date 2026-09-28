#ifndef IO_UNIT_H
#define IO_UNIT_H

#include "pipeline_types.h"

void     io_unit_init(raw_ring_t *ring);
void     io_unit_sample(raw_ring_t *ring, uint32_t tick);
uint32_t io_unit_drain(raw_ring_t *ring, raw_sample_t *out, uint32_t max_out);

#endif /* IO_UNIT_H */

#ifndef MEMORY_UNIT_H
#define MEMORY_UNIT_H

#include "pipeline_types.h"

void     memory_unit_init(summary_ring_t *ring);
void     memory_unit_process(summary_ring_t *ring, raw_sample_t *samples, uint32_t n);
uint32_t memory_unit_drain(summary_ring_t *ring, summary_record_t *out, uint32_t max_out);

#endif /* MEMORY_UNIT_H */

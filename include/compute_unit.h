#ifndef COMPUTE_UNIT_H
#define COMPUTE_UNIT_H

#include "pipeline_types.h"

void compute_unit_predict(const summary_record_t *records, uint32_t n, prediction_t *out);

#endif /* COMPUTE_UNIT_H */

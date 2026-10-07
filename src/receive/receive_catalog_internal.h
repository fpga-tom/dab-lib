#ifndef DAB_RECEIVE_CATALOG_INTERNAL_H
#define DAB_RECEIVE_CATALOG_INTERNAL_H

#include <stdint.h>

#include "dab/core/fic.h"
#include "dab/receive.h"

void dab_receive_catalog_build(dab_receive_catalog_t *catalog,
                               uint64_t sequence,
                               uint64_t timestamp_ns,
                               uint64_t frequency_hz,
                               const dab_fic_database_t *database);

int dab_receive_catalog_content_equal(const dab_receive_catalog_t *left,
                                      const dab_receive_catalog_t *right);

int dab_receive_catalog_is_regression(const dab_receive_catalog_t *current,
                                      const dab_receive_catalog_t *candidate);

#endif

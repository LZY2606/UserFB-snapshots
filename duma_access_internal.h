#ifndef DUMA_ACCESS_INTERNAL_H
#define DUMA_ACCESS_INTERNAL_H

#include "duma.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the real allocator while holding DUMA's allocator lock. */
void *duma_access_adapter_allocated(uint64_t request_start,
                                    uint64_t request_size,
                                    uint32_t source,
                                    uint64_t *identity_out);
void duma_access_adapter_freed(void *identity_slot_token);
void duma_access_adapter_reallocated(void *old_token,
                                     void *new_token,
                                     uint64_t new_identity);
duma_access_status duma_access_adapter_ref_from_token(void *identity_token,
                                                      duma_access_ref **out_ref);
duma_access_status duma_access_adapter_real_capture(
    duma_access_ref *ref,
    uint64_t address,
    uint64_t size,
    uint32_t op,
    const duma_access_model_segment *physical,
    size_t physical_count,
    duma_access_event **out_event);

/* Raw storage for the allocator adapter's temporary segment array. */
void *duma_access_raw_alloc(size_t size);
void duma_access_raw_free(void *pointer);
int duma_access_append_diagnostic_segments(
    duma_access_model_segment *segments,
    size_t capacity,
    size_t *count,
    uint64_t query_start,
    uint64_t query_end);

#ifdef __cplusplus
}
#endif

#endif

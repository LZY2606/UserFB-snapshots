/*
 * duma_access.c - offline, integer-addressable access diagnostics for DUMA.
 *
 * This module deliberately does not know the layout of struct _DUMA_Slot and
 * does not walk DUMA's allocList.  The DUMA adapter passes trusted facts that
 * it collected while holding the allocator lock.  The integer-model entry point
 * feeds the same interpreter with caller-owned arrays.
 *
 * Diagnostic storage comes directly from mmap/MAP_ANON (or VirtualAlloc on
 * Windows), never from malloc/realloc/free and therefore never recursively
 * enters the allocation path being observed.
 */

#define DUMA_SKIP_SETUP 1
#include "duma_access_internal.h"

#include <pthread.h>
#include <string.h>
#include <stdarg.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define DA_REF_MAGIC   0x44415246u /* "DARF" */
#define DA_EVT_MAGIC   0x44414556u /* "DAEV" */
#define DA_ID_MAGIC    0x44414944u /* "DAID" */

typedef struct duma_access_identity
{
    uint32_t magic;
    unsigned refs;
    uint64_t identity;
    uint64_t request_start;
    uint64_t request_end;
    uint32_t source;
    uint32_t state;
    uint64_t transition_sequence;
    uint64_t successor;
} da_identity;

struct duma_access_ref
{
    uint32_t magic;
    da_identity *id;
};

struct duma_access_event
{
    uint32_t magic;
    duma_access_report report;
};

typedef struct da_raw_header da_raw_header;
struct da_raw_header
{
    size_t map_size;
    da_raw_header *next;
    uint64_t user_start;
    uint64_t user_size;
};

typedef union da_raw_block
{
    da_raw_header header;
    long double align_max;
} da_raw_block;

static pthread_mutex_t da_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t da_raw_lock = PTHREAD_MUTEX_INITIALIZER;
static da_raw_header *da_raw_list = 0;
static int da_unavailable = 0;
static uint64_t da_next_identity = 1;
static uint64_t da_sequence = 1;
static uint64_t da_live_identities = 0;
static uint64_t da_live_refs = 0;
static uint64_t da_live_events = 0;
static uint64_t da_mapped_bytes = 0;

/* ------------------------------------------------------------------ */
/* Raw storage                                                         */
/* ------------------------------------------------------------------ */

static size_t da_page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
#else
    long n = sysconf(_SC_PAGESIZE);
    return n > 0 ? (size_t)n : 16384u;
#endif
}

void *duma_access_raw_alloc(size_t size)
{
    size_t header_size = sizeof(da_raw_block);
    size_t page = da_page_size();
    size_t total = header_size + size;
    size_t map_size;
    da_raw_block *block;
    da_raw_header *header;
    void *mapping;
    void *user;

    if (size && total < size)
        return 0;
    map_size = (total + page - 1) & ~(page - 1);
    if (map_size < total)
        return 0;

#if defined(_WIN32)
    mapping = VirtualAlloc(0, map_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mapping)
        return 0;
#else
    mapping = mmap(0, map_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return 0;
#endif

    block = (da_raw_block *)mapping;
    header = &block->header;
    user = (char *)mapping + header_size;
    header->map_size = map_size;
    header->next = 0;
    header->user_start = (uint64_t)(uintptr_t)user;
    header->user_size = (uint64_t)size;

    pthread_mutex_lock(&da_raw_lock);
    header->next = da_raw_list;
    da_raw_list = header;
    pthread_mutex_unlock(&da_raw_lock);

    __sync_add_and_fetch(&da_mapped_bytes, map_size);

    return user;
}

void duma_access_raw_free(void *pointer)
{
    da_raw_block *block;
    da_raw_header *header, **cursor;
    size_t map_size;
    void *mapping;

    if (!pointer)
        return;

    block = (da_raw_block *)((char *)pointer - sizeof(da_raw_block));
    header = &block->header;
    map_size = header->map_size;
    mapping = block;

    pthread_mutex_lock(&da_raw_lock);
    cursor = &da_raw_list;
    while (*cursor && *cursor != header)
        cursor = &(*cursor)->next;
    if (*cursor == header)
        *cursor = header->next;
    pthread_mutex_unlock(&da_raw_lock);

    __sync_sub_and_fetch(&da_mapped_bytes, map_size);

#if defined(_WIN32)
    VirtualFree(mapping, 0, MEM_RELEASE);
#else
    munmap(mapping, map_size);
#endif
}

int duma_access_append_diagnostic_segments(
    duma_access_model_segment *segments,
    size_t capacity,
    size_t *count,
    uint64_t query_start,
    uint64_t query_end)
{
    da_raw_header *header;
    int result = 0;

    if (!segments || !count)
        return -1;

    pthread_mutex_lock(&da_raw_lock);
    for (header = da_raw_list; header; header = header->next)
    {
        uint64_t start = header->user_start;
        uint64_t end = start + header->user_size;
        if (header->user_size == 0)
            continue;
        if (start >= query_end || end <= query_start)
            continue;
        if (*count >= capacity)
        {
            result = -1;
            break;
        }
        segments[*count].start = start;
        segments[*count].end = end;
        segments[*count].kind = DUMA_ACCESS_SEG_DIAGNOSTIC;
        segments[*count].protection = DUMA_ACCESS_PROT_READ |
                                      DUMA_ACCESS_PROT_WRITE;
        segments[*count].owner = 0;
        segments[*count].request_start = 0;
        segments[*count].request_end = 0;
        ++*count;
    }
    pthread_mutex_unlock(&da_raw_lock);
    return result;
}

static void da_latch_unavailable(void)
{
    pthread_mutex_lock(&da_lock);
    da_unavailable = 1;
    pthread_mutex_unlock(&da_lock);
}

static int da_is_unavailable(void)
{
    int value;
    pthread_mutex_lock(&da_lock);
    value = da_unavailable;
    pthread_mutex_unlock(&da_lock);
    return value;
}

/* ------------------------------------------------------------------ */
/* Identity lifetime                                                   */
/* ------------------------------------------------------------------ */

static void da_identity_get_locked(da_identity *id)
{
    if (id && id->magic == DA_ID_MAGIC)
        ++id->refs;
}

static void da_identity_put_locked(da_identity *id)
{
    if (!id || id->magic != DA_ID_MAGIC)
        return;
    if (id->refs == 0)
        return;
    --id->refs;
    if (id->refs == 0)
    {
        id->magic = 0;
        --da_live_identities;
        duma_access_raw_free(id);
    }
}

void *duma_access_adapter_allocated(uint64_t request_start,
                                    uint64_t request_size,
                                    uint32_t source,
                                    uint64_t *identity_out)
{
    da_identity *id;

    if (identity_out)
        *identity_out = 0;
    if (da_is_unavailable())
        return 0;

    id = (da_identity *)duma_access_raw_alloc(sizeof(*id));
    if (!id)
    {
        da_latch_unavailable();
        return 0;
    }

    pthread_mutex_lock(&da_lock);
    memset(id, 0, sizeof(*id));
    id->magic = DA_ID_MAGIC;
    id->refs = 1; /* the allocator slot */
    id->identity = da_next_identity++;
    id->request_start = request_start;
    id->request_end = request_start + request_size;
    id->source = source;
    id->state = DUMA_ACCESS_LIVE;
    ++da_live_identities;
    if (identity_out)
        *identity_out = id->identity;
    pthread_mutex_unlock(&da_lock);

    return id;
}

duma_access_status duma_access_adapter_ref_from_token(void *token,
                                                      duma_access_ref **out_ref)
{
    da_identity *id = (da_identity *)token;
    duma_access_ref *ref;

    if (!out_ref)
        return DUMA_ACCESS_EINVAL;
    *out_ref = 0;
    if (!id || id->magic != DA_ID_MAGIC)
        return DUMA_ACCESS_ESTATE;
    if (da_is_unavailable())
        return DUMA_ACCESS_EUNAVAILABLE;

    ref = (duma_access_ref *)duma_access_raw_alloc(sizeof(*ref));
    if (!ref)
    {
        da_latch_unavailable();
        return DUMA_ACCESS_ENOMEM;
    }

    pthread_mutex_lock(&da_lock);
    if (id->magic != DA_ID_MAGIC)
    {
        pthread_mutex_unlock(&da_lock);
        duma_access_raw_free(ref);
        return DUMA_ACCESS_ESTATE;
    }
    da_identity_get_locked(id);
    ref->magic = DA_REF_MAGIC;
    ref->id = id;
    ++da_live_refs;
    pthread_mutex_unlock(&da_lock);

    *out_ref = ref;
    return DUMA_ACCESS_OK;
}

void duma_access_adapter_freed(void *identity_slot_token)
{
    da_identity *id = (da_identity *)identity_slot_token;

    if (!id || id->magic != DA_ID_MAGIC)
        return;

    pthread_mutex_lock(&da_lock);
    if (id->magic == DA_ID_MAGIC)
    {
        if (id->state == DUMA_ACCESS_LIVE)
        {
            id->state = DUMA_ACCESS_FREED;
            id->transition_sequence = ++da_sequence;
        }
        da_identity_put_locked(id); /* slot ownership */
    }
    pthread_mutex_unlock(&da_lock);
}

void duma_access_adapter_reallocated(void *old_token,
                                     void *new_token,
                                     uint64_t new_identity)
{
    da_identity *old_id = (da_identity *)old_token;
    da_identity *new_id = (da_identity *)new_token;

    (void)new_id;
    if (!old_id || old_id->magic != DA_ID_MAGIC)
        return;

    pthread_mutex_lock(&da_lock);
    if (old_id->magic == DA_ID_MAGIC)
    {
        if (old_id->state == DUMA_ACCESS_LIVE)
        {
            old_id->state = DUMA_ACCESS_REALLOCATED;
            old_id->transition_sequence = ++da_sequence;
            old_id->successor = new_identity;
        }
        da_identity_put_locked(old_id); /* old slot ownership */
    }
    pthread_mutex_unlock(&da_lock);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

const char *duma_access_status_name(duma_access_status status)
{
    switch (status)
    {
        case DUMA_ACCESS_OK: return "ok";
        case DUMA_ACCESS_EINVAL: return "invalid_argument";
        case DUMA_ACCESS_ENOENT: return "not_found";
        case DUMA_ACCESS_ESTATE: return "wrong_lifecycle_state";
        case DUMA_ACCESS_ECONFLICT: return "identity_or_range_conflict";
        case DUMA_ACCESS_EOVERFLOW: return "address_overflow";
        case DUMA_ACCESS_ENOMEM: return "diagnostic_storage_exhausted";
        case DUMA_ACCESS_EUNAVAILABLE: return "diagnostics_unavailable";
    }
    return "unknown";
}

const char *duma_access_lifecycle_name(uint32_t state)
{
    switch (state)
    {
        case DUMA_ACCESS_LIVE: return "LIVE";
        case DUMA_ACCESS_FREED: return "FREED";
        case DUMA_ACCESS_REALLOCATED: return "REALLOCATED";
    }
    return "UNASSOCIATED";
}

const char *duma_access_segment_kind_name(uint32_t kind)
{
    switch (kind)
    {
        case DUMA_ACCESS_SEG_UNKNOWN: return "unknown";
        case DUMA_ACCESS_SEG_REQUEST: return "request";
        case DUMA_ACCESS_SEG_ALIGNMENT: return "alignment_margin";
        case DUMA_ACCESS_SEG_GUARD: return "guard_page";
        case DUMA_ACCESS_SEG_FREED_RESERVED: return "freed_retained_mapping";
        case DUMA_ACCESS_SEG_SLOT_TABLE: return "slot_table";
        case DUMA_ACCESS_SEG_FREE_RESERVE: return "free_reserve";
        case DUMA_ACCESS_SEG_DIAGNOSTIC: return "diagnostic_storage";
        case DUMA_ACCESS_SEG_INTERNAL: return "allocator_internal";
    }
    return "invalid";
}

void duma_access_get_stats(duma_access_stats *stats)
{
    if (!stats)
        return;
    pthread_mutex_lock(&da_lock);
    stats->live_identities = da_live_identities;
    stats->live_refs = da_live_refs;
    stats->live_events = da_live_events;
    stats->mapped_bytes = da_mapped_bytes;
    stats->sequence = da_sequence;
    stats->unavailable = da_unavailable;
    pthread_mutex_unlock(&da_lock);
}

static int da_valid_state(uint32_t state)
{
    return state >= DUMA_ACCESS_LIVE && state <= DUMA_ACCESS_REALLOCATED;
}

static int da_valid_kind(uint32_t kind)
{
    return kind <= DUMA_ACCESS_SEG_INTERNAL;
}

static const duma_access_model_identity *da_find_identity(
    const duma_access_model_identity *ids, size_t count, uint64_t id)
{
    size_t i;
    if (!id)
        return 0;
    for (i = 0; i < count; ++i)
        if (ids[i].identity == id)
            return &ids[i];
    return 0;
}

static int da_segment_cmp(const void *a, const void *b)
{
    const duma_access_model_segment *x = (const duma_access_model_segment *)a;
    const duma_access_model_segment *y = (const duma_access_model_segment *)b;
    if (x->start < y->start)
        return -1;
    if (x->start > y->start)
        return 1;
    if (x->end < y->end)
        return -1;
    if (x->end > y->end)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Event construction / shared interpreter                             */
/* ------------------------------------------------------------------ */

typedef struct da_facts
{
    uint64_t address;
    uint64_t size;
    uint32_t op;

    int associated;
    uint64_t ref_identity;
    uint32_t ref_state;
    uint64_t transition_sequence;
    uint64_t successor;
    uint32_t source;
    uint64_t request_start;
    uint64_t request_end;

    const duma_access_model_identity *identities;
    size_t identity_count;
    duma_access_model_segment *segments;
    size_t segment_count;
} da_facts;

static duma_access_status da_build_event(const da_facts *facts,
                                         duma_access_event **out_event)
{
    enum { DA_MAX_CODES = 16 };
    size_t max_out_segments;
    size_t segment_bytes, oob_bytes, code_ptr_bytes, code_store_bytes;
    size_t offset, segment_offset = 0, oob_offset = 0, codes_offset = 0;
    size_t code_store_offset = 0;
    size_t total_size;
    duma_access_event *event;
    char *block;
    duma_access_segment *out_segments;
    duma_access_oob_fragment *out_oob;
    const char **codes;
    char *code_store;
    size_t segment_count = 0, oob_count = 0, code_count = 0;
    size_t i;
    uint64_t pos, query_end;
    uint32_t flags = 0;
    uint64_t reused_by = 0;
    uint64_t first_other_owner = 0;
    int distinct_owner_count = 0;
    uint64_t owners[4];

    if (!out_event)
        return DUMA_ACCESS_EINVAL;
    *out_event = 0;
    if (!facts || (facts->size && facts->segment_count && !facts->segments))
        return DUMA_ACCESS_EINVAL;

    max_out_segments = facts->segment_count * 2 + 2;
    if (max_out_segments < 4)
        max_out_segments = 4;
    segment_bytes = max_out_segments * sizeof(duma_access_segment);
    oob_bytes = 2 * sizeof(duma_access_oob_fragment);
    code_ptr_bytes = (size_t)DA_MAX_CODES * sizeof(const char *);
    code_store_bytes = (size_t)DA_MAX_CODES * 40;

    offset = sizeof(duma_access_event);
    offset = (offset + 15u) & ~(size_t)15u;
    segment_offset = offset;
    offset += segment_bytes;
    offset = (offset + 15u) & ~(size_t)15u;
    oob_offset = offset;
    offset += oob_bytes;
    offset = (offset + 15u) & ~(size_t)15u;
    codes_offset = offset;
    offset += code_ptr_bytes;
    offset = (offset + 15u) & ~(size_t)15u;
    code_store_offset = offset;
    offset += code_store_bytes;
    total_size = offset;

    event = (duma_access_event *)duma_access_raw_alloc(total_size);
    if (!event)
    {
        da_latch_unavailable();
        return DUMA_ACCESS_ENOMEM;
    }

    block = (char *)event;
    out_segments = (duma_access_segment *)(block + segment_offset);
    out_oob = (duma_access_oob_fragment *)(block + oob_offset);
    codes = (const char **)(block + codes_offset);
    code_store = block + code_store_offset;

    memset(event, 0, total_size);
    event->magic = DA_EVT_MAGIC;
    event->report.version = 1;
    event->report.op = facts->op;
    event->report.address = facts->address;
    event->report.size = facts->size;
    event->report.end = facts->address + facts->size;

    pthread_mutex_lock(&da_lock);
    event->report.event_sequence = ++da_sequence;
    ++da_live_events;
    pthread_mutex_unlock(&da_lock);

    event->report.associated = facts->associated;
    event->report.ref_identity = facts->ref_identity;
    event->report.ref_state = facts->ref_state;
    event->report.transition_sequence = facts->transition_sequence;
    event->report.successor_identity = facts->successor;
    event->report.source = facts->source;

    query_end = facts->address + facts->size;

    /* Split the observed range against trusted physical layout intervals. */
    pos = facts->address;
    for (i = 0; i < facts->segment_count && facts->size; ++i)
    {
        const duma_access_model_segment *s = &facts->segments[i];
        uint64_t s0 = s->start > facts->address ? s->start : facts->address;
        uint64_t s1 = s->end < query_end ? s->end : query_end;

        if (s->end <= facts->address || s->start >= query_end)
            continue;
        if (s1 <= s0)
            continue;

        if (s0 > pos && segment_count < max_out_segments)
        {
            duma_access_segment *u = &out_segments[segment_count++];
            memset(u, 0, sizeof(*u));
            u->start = pos;
            u->end = s0;
            u->kind = DUMA_ACCESS_SEG_UNKNOWN;
        }

        if (segment_count < max_out_segments)
        {
            duma_access_segment *o = &out_segments[segment_count++];
            memset(o, 0, sizeof(*o));
            o->start = s0 > pos ? s0 : pos;
            o->end = s1;
            o->kind = s->kind;
            o->protection = s->protection;
            o->owner = s->owner;
            o->request_start = s->request_start;
            o->request_end = s->request_end;

            if (s->owner)
            {
                int k, seen = 0;
                for (k = 0; k < distinct_owner_count; ++k)
                    if (owners[k] == s->owner) { seen = 1; break; }
                if (!seen && distinct_owner_count < 4)
                {
                    owners[distinct_owner_count++] = s->owner;
                    if (distinct_owner_count >= 2)
                        flags |= DUMA_ACCESS_F_CROSSES_OWNERSHIP;
                }
            }

            if (s->kind == DUMA_ACCESS_SEG_GUARD)
                flags |= DUMA_ACCESS_F_GUARD_TOUCHED;
            if (s->kind == DUMA_ACCESS_SEG_FREED_RESERVED)
                flags |= DUMA_ACCESS_F_FREED_RESERVED;
            if (s->kind == DUMA_ACCESS_SEG_UNKNOWN)
                flags |= DUMA_ACCESS_F_UNKNOWN_REGION;
            if (facts->associated && facts->ref_state != DUMA_ACCESS_LIVE &&
                s->kind == DUMA_ACCESS_SEG_REQUEST &&
                s->owner != 0 && s->owner != facts->ref_identity)
            {
                flags |= DUMA_ACCESS_F_REUSED_BY_OTHER;
                reused_by = s->owner;
            }
            if (s->owner != 0 && s->owner != facts->ref_identity &&
                !first_other_owner)
                first_other_owner = s->owner;
        }

        if (s1 > pos)
            pos = s1;
    }

    if (facts->size)
    {
        if (pos < query_end && segment_count < max_out_segments)
        {
            duma_access_segment *u = &out_segments[segment_count++];
            memset(u, 0, sizeof(*u));
            u->start = pos;
            u->end = query_end;
            u->kind = DUMA_ACCESS_SEG_UNKNOWN;
            flags |= DUMA_ACCESS_F_UNKNOWN_REGION;
        }
        else if (segment_count == 0 && segment_count < max_out_segments)
        {
            duma_access_segment *u = &out_segments[segment_count++];
            memset(u, 0, sizeof(*u));
            u->start = facts->address;
            u->end = query_end;
            u->kind = DUMA_ACCESS_SEG_UNKNOWN;
            flags |= DUMA_ACCESS_F_UNKNOWN_REGION;
        }
    }

    /* Spatial relationship to the reference's original request. */
    if (facts->associated && facts->size)
    {
        if (facts->address < facts->request_start)
        {
            uint64_t e = query_end < facts->request_start ? query_end
                                                         : facts->request_start;
            if (e > facts->address && oob_count < 2)
            {
                duma_access_oob_fragment *o = &out_oob[oob_count++];
                o->start = facts->address;
                o->end = e;
                o->side = 1;
                o->boundary = facts->request_start;
            }
        }
        if (query_end > facts->request_end)
        {
            uint64_t s = facts->address > facts->request_end ? facts->address
                                                            : facts->request_end;
            if (query_end > s && oob_count < 2)
            {
                duma_access_oob_fragment *o = &out_oob[oob_count++];
                o->start = s;
                o->end = query_end;
                o->side = 2;
                o->boundary = facts->request_end;
            }
        }
        if (oob_count)
            flags |= DUMA_ACCESS_F_OUT_OF_BOUNDS;
    }

    if (!facts->size)
        flags |= DUMA_ACCESS_F_NO_ACCESS;
    if (!facts->associated)
        flags |= DUMA_ACCESS_F_UNASSOCIATED;

    if (facts->associated && facts->ref_state == DUMA_ACCESS_LIVE &&
        facts->size > 0 && oob_count == 0)
    {
        flags |= DUMA_ACCESS_F_VALID;
    }

    if (facts->associated && facts->ref_state != DUMA_ACCESS_LIVE)
        flags |= DUMA_ACCESS_F_STALE_REFERENCE;
    if (facts->associated && facts->ref_state == DUMA_ACCESS_FREED && facts->size)
        flags |= DUMA_ACCESS_F_USE_AFTER_FREE;
    if (facts->associated && facts->ref_state == DUMA_ACCESS_REALLOCATED)
        flags |= DUMA_ACCESS_F_REALLOCATED_REFERENCE;

    if ((flags & DUMA_ACCESS_F_REUSED_BY_OTHER) == 0 &&
        facts->associated && facts->ref_state != DUMA_ACCESS_LIVE &&
        first_other_owner)
    {
        /* An old reference landing in another live owner's alignment/guard is
         * still physical occupation by another identity, but the explicit
         * reused_by_other contract is anchored on its request segment. */
    }

    /* Build stable, serializable code tokens. */
    if (flags & DUMA_ACCESS_F_VALID)
        codes[code_count++] = "valid";
    if (flags & DUMA_ACCESS_F_NO_ACCESS)
        codes[code_count++] = "no_access";
    if (flags & DUMA_ACCESS_F_UNASSOCIATED)
        codes[code_count++] = "unassociated";
    if (flags & DUMA_ACCESS_F_OUT_OF_BOUNDS)
        codes[code_count++] = "out_of_bounds";
    if (flags & DUMA_ACCESS_F_STALE_REFERENCE)
        codes[code_count++] = "stale_reference";
    if (flags & DUMA_ACCESS_F_USE_AFTER_FREE)
        codes[code_count++] = "use_after_free";
    if (flags & DUMA_ACCESS_F_REALLOCATED_REFERENCE)
        codes[code_count++] = "reallocated_reference";
    if (flags & DUMA_ACCESS_F_REUSED_BY_OTHER)
        codes[code_count++] = "reused_by_other";
    if (flags & DUMA_ACCESS_F_GUARD_TOUCHED)
        codes[code_count++] = "guard_touched";
    if (flags & DUMA_ACCESS_F_FREED_RESERVED)
        codes[code_count++] = "freed_retained_mapping";
    if (flags & DUMA_ACCESS_F_UNKNOWN_REGION)
        codes[code_count++] = "unknown_region";
    if (flags & DUMA_ACCESS_F_CROSSES_OWNERSHIP)
        codes[code_count++] = "crosses_ownership";

    event->report.flags = flags;
    event->report.reused_by_identity = reused_by;
    event->report.segment_count = segment_count;
    event->report.segments = out_segments;
    event->report.oob_count = oob_count;
    event->report.oob = out_oob;
    event->report.code_count = code_count;
    event->report.codes = codes;

    /* The code pointers are string literals; code_store is reserved for future
     * formatted/serialized strings and intentionally currently unused. */
    (void)code_store;

    *out_event = event;
    return DUMA_ACCESS_OK;
}

duma_access_status duma_access_adapter_real_capture(
    duma_access_ref *ref,
    uint64_t address,
    uint64_t size,
    uint32_t op,
    const duma_access_model_segment *physical,
    size_t physical_count,
    duma_access_event **out_event)
{
    da_facts facts;
    duma_access_model_segment *sorted = 0;
    duma_access_status status;
    uint64_t event_sequence_snapshot = 0;

    if (!out_event)
        return DUMA_ACCESS_EINVAL;
    *out_event = 0;
    if (da_is_unavailable())
        return DUMA_ACCESS_EUNAVAILABLE;
    if (physical_count && !physical)
        return DUMA_ACCESS_EINVAL;

    if (physical_count)
    {
        sorted = (duma_access_model_segment *)
            duma_access_raw_alloc(physical_count * sizeof(*sorted));
        if (!sorted)
        {
            da_latch_unavailable();
            return DUMA_ACCESS_ENOMEM;
        }
        memcpy(sorted, physical, physical_count * sizeof(*sorted));
        qsort(sorted, physical_count, sizeof(*sorted), da_segment_cmp);
    }

    memset(&facts, 0, sizeof(facts));
    facts.address = address;
    facts.size = size;
    facts.op = op;
    facts.segments = sorted;
    facts.segment_count = physical_count;

    if (ref)
    {
        da_identity *id;
        pthread_mutex_lock(&da_lock);
        if (ref->magic != DA_REF_MAGIC || !ref->id ||
            ref->id->magic != DA_ID_MAGIC)
        {
            pthread_mutex_unlock(&da_lock);
            duma_access_raw_free(sorted);
            return DUMA_ACCESS_EINVAL;
        }
        id = ref->id;
        facts.associated = 1;
        facts.ref_identity = id->identity;
        facts.ref_state = id->state;
        facts.transition_sequence = id->transition_sequence;
        facts.successor = id->successor;
        facts.source = id->source;
        facts.request_start = id->request_start;
        facts.request_end = id->request_end;
        event_sequence_snapshot = da_sequence;
        pthread_mutex_unlock(&da_lock);
    }

    status = da_build_event(&facts, out_event);
    (void)event_sequence_snapshot;
    duma_access_raw_free(sorted);
    return status;
}

/* ------------------------------------------------------------------ */
/* Public real-address entry points; platform adapter is in duma.c     */
/* ------------------------------------------------------------------ */

duma_access_status duma_access_platform_acquire(uintptr_t user_address,
                                                duma_access_ref **out_ref);
duma_access_status duma_access_platform_capture(
    duma_access_ref *ref,
    uint64_t address,
    uint64_t size,
    uint32_t op,
    duma_access_event **out_event);

duma_access_status duma_access_acquire(uintptr_t user_address,
                                       duma_access_ref **out_ref)
{
    if (!out_ref)
        return DUMA_ACCESS_EINVAL;
    *out_ref = 0;
    if (da_is_unavailable())
        return DUMA_ACCESS_EUNAVAILABLE;
    return duma_access_platform_acquire(user_address, out_ref);
}

duma_access_status duma_access_capture(duma_access_ref *ref,
                                       uintptr_t address,
                                       size_t size,
                                       duma_access_op op,
                                       duma_access_event **out_event)
{
    if (!out_event)
        return DUMA_ACCESS_EINVAL;
    *out_event = 0;
    if (op != DUMA_ACCESS_READ && op != DUMA_ACCESS_WRITE)
        return DUMA_ACCESS_EINVAL;
    if (size > UINTPTR_MAX - address)
        return DUMA_ACCESS_EOVERFLOW;
    if (da_is_unavailable())
        return DUMA_ACCESS_EUNAVAILABLE;

    return duma_access_platform_capture(ref, (uint64_t)address, (uint64_t)size,
                                        (uint32_t)op, out_event);
}

const duma_access_report *duma_access_explain(const duma_access_event *event)
{
    if (!event || event->magic != DA_EVT_MAGIC)
        return 0;
    return &event->report;
}

void duma_access_release_event(duma_access_event *event)
{
    if (!event || event->magic != DA_EVT_MAGIC)
        return;
    event->magic = 0;
    pthread_mutex_lock(&da_lock);
    if (da_live_events)
        --da_live_events;
    pthread_mutex_unlock(&da_lock);
    duma_access_raw_free(event);
}

void duma_access_release_ref(duma_access_ref *ref)
{
    da_identity *id;

    if (!ref || ref->magic != DA_REF_MAGIC)
        return;
    pthread_mutex_lock(&da_lock);
    ref->magic = 0;
    id = ref->id;
    da_identity_put_locked(id);
    if (da_live_refs)
        --da_live_refs;
    pthread_mutex_unlock(&da_lock);
    duma_access_raw_free(ref);
}

void duma_access_release(void *object)
{
    uint32_t magic;
    if (!object)
        return;
    magic = *(const uint32_t *)object;
    if (magic == DA_REF_MAGIC)
        duma_access_release_ref((duma_access_ref *)object);
    else if (magic == DA_EVT_MAGIC)
        duma_access_release_event((duma_access_event *)object);
}

/* ------------------------------------------------------------------ */
/* Integer-array model entry point                                     */
/* ------------------------------------------------------------------ */

duma_access_status duma_access_model_run(const duma_access_model *model,
                                         duma_access_event **out_event)
{
    duma_access_model_segment *segments = 0;
    da_facts facts;
    duma_access_status status;
    size_t i, j;
    uint64_t model_end;

    if (!out_event)
        return DUMA_ACCESS_EINVAL;
    *out_event = 0;
    if (!model || model->version != 1)
        return DUMA_ACCESS_EINVAL;
    if (model->op != DUMA_ACCESS_READ && model->op != DUMA_ACCESS_WRITE)
        return DUMA_ACCESS_EINVAL;
    if (model->identity_count && !model->identities)
        return DUMA_ACCESS_EINVAL;
    if (model->segment_count && !model->segments)
        return DUMA_ACCESS_EINVAL;
    if (da_is_unavailable())
        return DUMA_ACCESS_EUNAVAILABLE;
    if (model->length > UINT64_MAX - model->address)
        return DUMA_ACCESS_EOVERFLOW;
    model_end = model->address + model->length;

    /* Validate identities. */
    for (i = 0; i < model->identity_count; ++i)
    {
        const duma_access_model_identity *a = &model->identities[i];
        if (!a->identity || a->request_start >= a->request_end)
            return DUMA_ACCESS_EINVAL;
        if (!da_valid_state(a->state))
            return DUMA_ACCESS_EINVAL;
        if (a->state == DUMA_ACCESS_LIVE &&
            (a->transition_sequence || a->successor))
            return DUMA_ACCESS_EINVAL;
        if (a->state == DUMA_ACCESS_FREED && a->successor)
            return DUMA_ACCESS_EINVAL;
        if (a->state != DUMA_ACCESS_LIVE && !a->transition_sequence)
            return DUMA_ACCESS_EINVAL;
        if (a->successor &&
            !da_find_identity(model->identities, model->identity_count,
                              a->successor))
            return DUMA_ACCESS_ECONFLICT;
        if (a->state == DUMA_ACCESS_REALLOCATED && !a->successor)
            return DUMA_ACCESS_ECONFLICT;
        for (j = i + 1; j < model->identity_count; ++j)
        {
            const duma_access_model_identity *b = &model->identities[j];
            if (a->identity == b->identity)
                return DUMA_ACCESS_ECONFLICT;
            if (a->transition_sequence &&
                a->transition_sequence == b->transition_sequence)
                return DUMA_ACCESS_ECONFLICT;
        }
    }

    if (model->ref_identity &&
        !da_find_identity(model->identities, model->identity_count,
                          model->ref_identity))
        return DUMA_ACCESS_ECONFLICT;

    /* Copy, validate and sort physical segments. */
    if (model->segment_count)
    {
        segments = (duma_access_model_segment *)
            duma_access_raw_alloc(model->segment_count * sizeof(*segments));
        if (!segments)
        {
            da_latch_unavailable();
            return DUMA_ACCESS_ENOMEM;
        }
        memcpy(segments, model->segments,
               model->segment_count * sizeof(*segments));

        for (i = 0; i < model->segment_count; ++i)
        {
            duma_access_model_segment *s = &segments[i];
            const duma_access_model_identity *owner;

            if (s->start >= s->end || !da_valid_kind(s->kind))
            {
                status = DUMA_ACCESS_EINVAL;
                goto fail;
            }
            if (s->protection > (DUMA_ACCESS_PROT_READ |
                                 DUMA_ACCESS_PROT_WRITE))
            {
                status = DUMA_ACCESS_EINVAL;
                goto fail;
            }

            owner = da_find_identity(model->identities,
                                     model->identity_count, s->owner);
            if (s->owner && !owner)
            {
                status = DUMA_ACCESS_ECONFLICT;
                goto fail;
            }

            if (s->kind == DUMA_ACCESS_SEG_UNKNOWN ||
                s->kind == DUMA_ACCESS_SEG_SLOT_TABLE ||
                s->kind == DUMA_ACCESS_SEG_FREE_RESERVE ||
                s->kind == DUMA_ACCESS_SEG_DIAGNOSTIC ||
                s->kind == DUMA_ACCESS_SEG_INTERNAL)
            {
                if (s->owner != 0)
                {
                    status = DUMA_ACCESS_ECONFLICT;
                    goto fail;
                }
            }
            else
            {
                if (!owner)
                {
                    status = DUMA_ACCESS_ECONFLICT;
                    goto fail;
                }
                if (s->request_start || s->request_end)
                {
                    if (s->request_start != owner->request_start ||
                        s->request_end != owner->request_end)
                    {
                        status = DUMA_ACCESS_ECONFLICT;
                        goto fail;
                    }
                }
                else
                {
                    s->request_start = owner->request_start;
                    s->request_end = owner->request_end;
                }

                if ((s->kind == DUMA_ACCESS_SEG_REQUEST ||
                     s->kind == DUMA_ACCESS_SEG_ALIGNMENT ||
                     s->kind == DUMA_ACCESS_SEG_GUARD) &&
                    owner->state != DUMA_ACCESS_LIVE)
                {
                    status = DUMA_ACCESS_ECONFLICT;
                    goto fail;
                }
                if (s->kind == DUMA_ACCESS_SEG_FREED_RESERVED &&
                    owner->state == DUMA_ACCESS_LIVE)
                {
                    status = DUMA_ACCESS_ECONFLICT;
                    goto fail;
                }
            }
        }

        qsort(segments, model->segment_count, sizeof(*segments),
              da_segment_cmp);
        for (i = 1; i < model->segment_count; ++i)
        {
            if (segments[i].start < segments[i - 1].end)
            {
                status = DUMA_ACCESS_ECONFLICT;
                goto fail;
            }
        }
    }

    memset(&facts, 0, sizeof(facts));
    facts.address = model->address;
    facts.size = model->length;
    facts.op = model->op;
    facts.identities = model->identities;
    facts.identity_count = model->identity_count;
    facts.segments = segments;
    facts.segment_count = model->segment_count;

    if (model->ref_identity)
    {
        const duma_access_model_identity *id =
            da_find_identity(model->identities, model->identity_count,
                             model->ref_identity);
        if (!id)
        {
            status = DUMA_ACCESS_ECONFLICT;
            goto fail;
        }
        facts.associated = 1;
        facts.ref_identity = id->identity;
        facts.ref_state = id->state;
        facts.transition_sequence = id->transition_sequence;
        facts.successor = id->successor;
        facts.source = id->source;
        facts.request_start = id->request_start;
        facts.request_end = id->request_end;
    }

    status = da_build_event(&facts, out_event);
    duma_access_raw_free(segments);
    (void)model_end;
    return status;

fail:
    duma_access_raw_free(segments);
    return status;
}

/*
 * example_access_diagnostic.c
 *
 * Demonstrates DUMA's offline, integer-addressable access diagnostics.
 * This program never dereferences the addresses passed to capture(), and it
 * never touches guard pages or freed memory.
 *
 * Build (after task_support/build.sh):
 *   cc -std=gnu99 -pthread -DDUMA_NO_CPP_SUPPORT \
 *      -DDUMA_NO_GLOBAL_MALLOC_FREE -DDUMA_EXPLICIT_INIT \
 *      -DDUMA_PREFER_GETENV -I. \
 *      example_access_diagnostic.c .task-build/duma.o \
 *      .task-build/print.o .task-build/sem_inc.o \
 *      .task-build/duma_access.o -o "$TMPDIR/duma-access-example"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "duma.h"

static void print_report(const duma_access_report *r)
{
    size_t i;

    if (!r)
    {
        printf("  no report\n");
        return;
    }

    printf("  access: address=0x%llx size=%llu op=%s event_seq=%llu\n",
           (unsigned long long)r->address,
           (unsigned long long)r->size,
           r->op == DUMA_ACCESS_WRITE ? "write" : "read",
           (unsigned long long)r->event_sequence);
    printf("  reference: associated=%d identity=%llu state=%s transition=%llu\n",
           r->associated,
           (unsigned long long)r->ref_identity,
           duma_access_lifecycle_name(r->ref_state),
           (unsigned long long)r->transition_sequence);
    printf("  flags=0x%08x reused_by=%llu codes:",
           r->flags, (unsigned long long)r->reused_by_identity);
    for (i = 0; i < r->code_count; ++i)
        printf(" %s", r->codes[i]);
    if (!r->code_count)
        printf(" (none)");
    printf("\n");

    for (i = 0; i < r->segment_count; ++i)
    {
        const duma_access_segment *s = &r->segments[i];
        printf("  segment[%zu]: 0x%llx-0x%llx %s owner=%llu request=0x%llx-0x%llx\n",
               i,
               (unsigned long long)s->start,
               (unsigned long long)s->end,
               duma_access_segment_kind_name(s->kind),
               (unsigned long long)s->owner,
               (unsigned long long)s->request_start,
               (unsigned long long)s->request_end);
    }

    for (i = 0; i < r->oob_count; ++i)
    {
        const duma_access_oob_fragment *o = &r->oob[i];
        printf("  out_of_bounds[%zu]: 0x%llx-0x%llx %s request boundary 0x%llx\n",
               i,
               (unsigned long long)o->start,
               (unsigned long long)o->end,
               o->side == 1 ? "before" : "after",
               (unsigned long long)o->boundary);
    }
}

int main(void)
{
    void *pointer;
    uintptr_t address;
    duma_access_ref *ref = NULL;
    duma_access_event *event = NULL;
    const duma_access_report *report;
    duma_access_status status;

    setenv("DUMA_MALLOC_FAILEXIT", "0", 1);
    setenv("DUMA_DISABLE_BANNER", "1", 1);
    duma_init();

    pointer = malloc(48);
    if (!pointer)
        return 2;
    address = (uintptr_t)pointer;

    status = duma_access_acquire(address, &ref);
    if (status != DUMA_ACCESS_OK)
    {
        printf("acquire failed: %s\n", duma_access_status_name(status));
        free(pointer);
        return 3;
    }

    printf("1) Healthy live reference and in-request integer capture:\n");
    status = duma_access_capture(ref, address, 48, DUMA_ACCESS_READ, &event);
    if (status != DUMA_ACCESS_OK)
        return 4;
    report = duma_access_explain(event);
    print_report(report);
    duma_access_release_event(event);
    event = NULL;

    free(pointer);

    printf("\n2) Same reference after free; no memory is touched:\n");
    status = duma_access_capture(ref, address, 8, DUMA_ACCESS_READ, &event);
    if (status != DUMA_ACCESS_OK)
        return 5;
    report = duma_access_explain(event);
    print_report(report);
    duma_access_release_event(event);
    duma_access_release_ref(ref);

    printf("\n3) Integer-array reconstruction of exact address reuse:\n");
    {
        enum { A = 0x10000, END = A + 64, MAP_END = A + 0x10000 };
        duma_access_model_identity identities[2];
        duma_access_model_segment segments[1];
        duma_access_model model;

        memset(identities, 0, sizeof(identities));
        identities[0].identity = 101;
        identities[0].request_start = A;
        identities[0].request_end = END;
        identities[0].state = DUMA_ACCESS_FREED;
        identities[0].transition_sequence = 10;

        identities[1].identity = 202;
        identities[1].request_start = A;
        identities[1].request_end = END;
        identities[1].state = DUMA_ACCESS_LIVE;

        segments[0].start = A;
        segments[0].end = END;
        segments[0].kind = DUMA_ACCESS_SEG_REQUEST;
        segments[0].protection = DUMA_ACCESS_PROT_READ | DUMA_ACCESS_PROT_WRITE;
        segments[0].owner = 202;
        segments[0].request_start = A;
        segments[0].request_end = END;

        memset(&model, 0, sizeof(model));
        model.version = 1;
        model.op = DUMA_ACCESS_READ;
        model.address = A;
        model.length = 64;
        model.ref_identity = 101;
        model.identities = identities;
        model.identity_count = 2;
        model.segments = segments;
        model.segment_count = 1;

        status = duma_access_model_run(&model, &event);
        if (status != DUMA_ACCESS_OK)
        {
            printf("model failed: %s\n", duma_access_status_name(status));
            return 6;
        }
        report = duma_access_explain(event);
        print_report(report);
        duma_access_release_event(event);
    }

    puts("\nACCESS_DIAGNOSTIC_EXAMPLE_OK");
    return 0;
}

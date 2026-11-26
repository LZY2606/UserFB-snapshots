# Prefix fragment indexes

`LSMASH_FILE_MODE_FRAGMENT_INDEX` (`1 << 9`) writes one Segment Index box for
each track that has media in the current non-empty movie fragment immediately
before that fragment:

```text
ftyp moov [sidx(track ascending)... moof mdat] ...
```

Each prefix `sidx` has exactly one reference.  The reference is a media
reference (`reference_type = 0`) whose size covers the following `moof` and
`mdat`.  `first_offset` is measured from the end of that track's `sidx` to the
`moof`, so it includes the sizes of the later tracks' `sidx` boxes.  L-SMASH
selects SIDX version 1 when the actual earliest presentation time or
`first_offset` does not fit in 32 bits.

The mode is intended for seekless custom output.  Delivered fragments are never
read back, moved, or rewritten, and finish does not require a remux callback.

Supported configuration is intentionally limited to one self-contained
ISO BMFF file with an ISO Base Media version 6 or newer brand, an empty
initialization sample table, no explicit edit lists, and `avc1` or `mp4a`
sample entries.  The mode bit must be combined with `WRITE`, `BOX`,
`FRAGMENTED`, `INDEX`, `INITIALIZATION`, and `MEDIA`; do not combine it with
`SEGMENT`.  Unsupported mode/brand combinations fail in `lsmash_set_file()`,
and unsupported track/edit/codec combinations fail before the first output
bytes are written.

## C call example

```c
#include "lsmash.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct output_sink {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
};

static int sink_write(void *opaque, uint8_t *buf, int size)
{
    struct output_sink *out = opaque;
    size_t needed = out->size + (size_t)size;
    uint8_t *p;

    if (needed > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 4096;
        while (capacity < needed)
            capacity *= 2;
        p = realloc(out->data, capacity);
        if (!p)
            return -1;
        out->data = p;
        out->capacity = capacity;
    }

    memcpy(out->data + out->size, buf, (size_t)size);
    out->size = needed;
    return size;
}

int write_prefix_indexed_fragments(struct output_sink *sink)
{
    lsmash_root_t *root = lsmash_create_root();
    lsmash_file_parameters_t params;
    lsmash_file_t *file;

    if (!root)
        return -1;

    memset(&params, 0, sizeof(params));
    params.mode = LSMASH_FILE_MODE_WRITE
                | LSMASH_FILE_MODE_BOX
                | LSMASH_FILE_MODE_FRAGMENTED
                | LSMASH_FILE_MODE_INDEX
                | LSMASH_FILE_MODE_INITIALIZATION
                | LSMASH_FILE_MODE_MEDIA
                | LSMASH_FILE_MODE_FRAGMENT_INDEX;
    params.opaque             = sink;
    params.write              = sink_write;
    params.seek               = NULL; /* seekless delivery is supported */
    params.major_brand        = ISOM_BRAND_TYPE_ISO6;
    params.max_chunk_duration = 0.01;
    params.max_async_tolerance = 2.0;
    params.max_chunk_size     = 1u << 20;
    params.max_read_size      = 1u << 20;

    file = lsmash_set_file(root, &params);
    if (!file) {
        lsmash_destroy_root(root);
        return -1;
    }

    /* Set movie/track/media parameters, add an avc1 or mp4a sample entry,
       and then establish the empty initialization movie. */
    if (lsmash_create_fragment_movie(root) < 0)
        goto fail;

    /* Append this fragment's samples to one or more tracks.  Flush each used
       track once after its samples have been appended.  The next call finishes
       the current fragment, emits its sorted prefix SIDX boxes, moof and mdat,
       and starts the next fragment. */
    if (lsmash_create_fragment_movie(root) < 0)
        goto fail;

    /* Finish writes the final fragment exactly once.  On a seekless stream it
       does not seek back to moov or write an initialization duration. */
    if (lsmash_finish_movie(root, NULL) < 0)
        goto fail;

    lsmash_destroy_root(root);
    return 0;

fail:
    lsmash_destroy_root(root);
    return -1;
}
```

The runnable dual-track, multi-fragment verification and independent box parser
is in [`tests/task_prefix_index.c`](../tests/task_prefix_index.c).  Run it with:

```sh
bash task_support/run.sh target
```

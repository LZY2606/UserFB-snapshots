
# cmixer
A lightweight, portable ANSI C audio mixer for games.

#### Features
* Easy to use
* Tiny: two files, around 600sloc
* No dependencies (`stb_vorbis` is optional)
* Support for `wav` and `ogg` files if `stb_vorbis` is used
* Simple API for adding custom formats
* Resampling and seamless looping
* Per-source gain, pan and pitch control
* Fast, fixed-point internal processing


## Getting Started
The library's .c and .h files can be dropped into a project and compiled along
with it. If ogg vorbis support is desired `stb_vorbis.c` should also be added to
the project and the `-DCM_USE_STB_VORBIS` flag passed to the compiler.

Before use, the library should be initialized with the desired samplerate:
```c
cm_init(44100);
```

A sound can be played by first creating a source, then calling `cm_play()` on
that source.
```c
cm_Source *src = cm_new_source_from_file("sound.wav");
cm_play(src);
```

When the `cm_process()` function is called, all the currently playing streams
will be processed and mixed, the master output will be written to the provided
16bit buffer. This output is always stereo interlaced, thus the buffer length
must always be an even number:
```c
cm_Int16 buffer[512];
cm_process(buffer, 512);
```

`cm_process()` can safely be called from a separate thread; if more than one
thread is used a lock event handler must be set:
```c
static void lock_handler(cm_Event *e) {
  if (e->type == CM_EVENT_LOCK) {
    printf("acquired lock\n");
  }
  if (e->type == CM_EVENT_UNLOCK) {
    printf("released lock\n");
  }
}

cm_set_lock(lock_handler);
```

See [doc/api.md](doc/api.md) for an overview of the API or [demo](demo) for
usage demos.

## Auxiliary buses (dry/wet routing)
Sources write into buses, and buses are summed into the master output. A
plain bus sums its inputs; a delay bus adds a feedback delay:

```
y[n] = x[n - D] + feedback * y[n - D]
```

per channel, where `D` is the delay length in frames. A source contributes its
usual post-resampling, post-gain and post-pan stereo signal to its bus, so a
dry/wet split sends one source into a plain bus and fans that bus out into a
dry edge and a delay bus. The source is decoded and advanced only once.

```c
#include "cmixer.h"

static void handler(cm_Event *e) {
  /* fill e->buffer with e->length signed 16-bit stereo frames */
}

int main(void) {
  cm_Int16 out[512];
  cm_SourceInfo info = {handler, NULL, 44100, 44100 * 2};
  cm_Bus *dry, *wet, *split;
  cm_Source *src;

  cm_init(44100);

  /* A plain bus for the source, and a delay bus with 0.3 s and 40% feedback */
  split = cm_new_bus(1.0);
  dry = cm_new_bus(1.0);
  wet = cm_new_delay_bus(13230, 0.4);   /* 0.3 * 44100 frames */

  src = cm_new_source(&info);
  cm_set_source_bus(src, split);        /* source -> split (decoded once) */
  cm_connect_bus(split, dry, 1.0);      /* split -> dry path */
  cm_connect_bus(split, wet, 0.7);      /* split -> wet path at 70% */
  cm_connect_bus(dry, cm_get_master_bus(), 1.0);
  cm_connect_bus(wet, cm_get_master_bus(), 1.0);

  cm_play(src);
  while (1) {
    cm_process(out, 512);               /* 256 stereo frames */
    /* ... write `out` to the audio device ... */
    break;
  }

  cm_destroy_source(src);
  cm_disconnect_bus(split, dry);
  cm_disconnect_bus(split, wet);
  cm_disconnect_bus(dry, cm_get_master_bus());
  cm_disconnect_bus(wet, cm_get_master_bus());
  cm_destroy_bus(split);
  cm_destroy_bus(dry);
  cm_destroy_bus(wet);
  return 0;
}
```

`cm_new_source_from_file()` / `cm_new_source_from_mem()` work the same way;
routing is independent of how the source produces its samples.

Bus and edge gains are applied to the bus output; they are never fed back into
the delay state. Intermediate buses are not clipped, so large branches may
cancel at the master. The routing graph must stay acyclic, and the master bus
cannot be used as an edge source.

### Ownership
`cm_new_bus()` and `cm_new_delay_bus()` return buses owned by the library.
`cm_destroy_bus()` frees a bus, but only if no source is routed to it and it
has no incoming or outgoing edges. A rejected destroy leaves the graph and the
sound unchanged. Destroying a source does not leave a dangling route: the
source is removed from the routing table automatically. The master bus returned
by `cm_get_master_bus()` is permanent and cannot be destroyed. A new source is
routed to the master output by default; `cm_set_source_master()` restores that
default. Bus operations must be called between `cm_process()` calls from a
single thread.

## License
This library is free software; you can redistribute it and/or modify it under
the terms of the MIT license. See [LICENSE](LICENSE) for details.

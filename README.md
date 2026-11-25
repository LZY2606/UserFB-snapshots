
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

## Auxiliary buses and dry/wet routing

Each source is decoded, resampled, panned and gain-adjusted once. Its final
stereo contribution is sent to one target bus (the permanent master bus by
default). Buses sum their inputs; ordinary buses pass that sum through, while
delay buses apply independent left/right feedback delay states. Bus gain and
edge gain are applied after the ordinary sum or delay equation, so they are not
fed back into a delay state.

The following complete offline example sends a source to an ordinary mix bus,
then splits that bus into a dry edge to master and a wet edge through a feedback
delay bus:

```c
#include "cmixer.h"
#include <stdio.h>

struct square { int position; };

static void square_handler(cm_Event *e) {
  struct square *s = e->udata;
  int i;
  if (e->type == CM_EVENT_REWIND) {
    s->position = 0;
  } else if (e->type == CM_EVENT_SAMPLES) {
    for (i = 0; i < e->length; i += 2) {
      int phase = s->position++ % 96;
      e->buffer[i]     = phase < 48 ? 9000 : -9000;
      e->buffer[i + 1] = phase < 48 ? 5000 : -5000;
    }
  }
}

int main(void) {
  enum { BLOCK = 256, FRAMES = 96000 };
  struct square state = {0};
  cm_Int16 buffer[BLOCK * 2];
  cm_SourceInfo info = { square_handler, &state, 48000, 24000 };
  cm_Source *src;
  cm_Bus *mix, *delay, *master;
  int i;

  cm_init(48000);
  src = cm_new_source(&info);
  mix = cm_new_bus();
  delay = cm_new_delay_bus(4800, 0.35); /* 100 ms, feedback 0.35 */
  master = cm_master_bus();
  if (!src || !mix || !delay) return 1;

  cm_set_source_bus(src, mix);
  cm_connect_bus(mix, master, 0.8);       /* dry */
  cm_connect_bus(mix, delay, 0.5);        /* wet send */
  cm_connect_bus(delay, master, 0.35);    /* wet return */

  cm_play(src);
  for (i = 0; i < FRAMES / BLOCK; i++) {
    cm_process(buffer, BLOCK * 2);
    /* Use or write BLOCK stereo frames from buffer here. */
  }

  cm_destroy_source(src);
  cm_disconnect_bus(mix, master);
  cm_disconnect_bus(mix, delay);
  cm_disconnect_bus(delay, master);
  cm_destroy_bus(delay);
  cm_destroy_bus(mix);
  return 0;
}
```

Compile and run the checked-in copy with:

```sh
cc -std=c99 -Isrc demo/aux_mix.c src/cmixer.c -lm -o aux_mix
./aux_mix
```

The same program is provided in `demo/aux_mix.c`. Connections are directed and
must not form a graph cycle; `cm_connect_bus()` returns `CM_ERROR` and leaves
the existing graph unchanged if the new edge would create one. Connecting an
already-connected pair only updates its edge gain. Use
`cm_set_edge_gain()` or call `cm_connect_bus()` again to adjust an edge, and
`cm_disconnect_bus()` to remove it. `cm_clear_delay_history()` clears only one
delay bus's two channel histories.

### Ownership

* `cm_master_bus()` returns cmixer's permanent master sink. It must not be
  destroyed and edges may not start at it.
* Buses returned by `cm_new_bus()` and `cm_new_delay_bus()` are owned by the
  caller. `cm_destroy_bus()` succeeds only when the bus has no source
  references and no incoming or outgoing edges; a refused destroy leaves the
  bus and all routing audible.
* `cm_set_source_bus()` does not transfer bus ownership. Destroy or reassign
  all sources referencing a bus and disconnect its edges before destroying it.
* A source never owns its target bus. Destroying a source removes its reference
  without leaving a dangling route; `cm_use_master_bus(src)` restores the
  default route.
* Bus connections are non-owning edges. Disconnect them before destroying
  either endpoint.
* For `cm_new_source_from_mem()`, the caller still owns `data` and must keep it
  alive until after `cm_destroy_source()`.

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

## License
This library is free software; you can redistribute it and/or modify it under
the terms of the MIT license. See [LICENSE](LICENSE) for details.

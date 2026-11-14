
# cmixer API
* [Functions](#functions)
* [Auxiliary buses](#auxiliary-buses)
* [Events](#events)


## Functions

##### const char\* cm_get_error(void)
Returns and clears the last error. Returns NULL if there is no error.

##### void cm_init(int samplerate)
Initializes the library — this must be called before any other function is used.
`samplerate` should be the samplerate used by your output device.

##### void cm_set_lock(cm_EventHandler lock)
Sets a lock event handler. A lock handler must be set if the library will be
used from multiple threads. See *[events](#events)*.

##### void cm_set_master_gain(double gain)
Sets the master gain — this is the overall volume of the resultant audio
outputted by `cm_process()`. This is `1` by default.

##### void cm_process(cm_Int16 \*dst, int len)
Processes and mixes all the current streams and outputs the result to `dst`.
This function always outputs signed 16bit stereo interlaced. `len` is the number
of samples to process, this should always be an even number. Sources are
summed into their destination buses, then the bus graph is mixed and the master
bus is converted to 16-bit and clipped. With no auxiliary buses the output is
byte-for-byte the same as the classic direct-to-master mixing.

##### cm_Source\* cm_new_source(const cm_SourceInfo \*info)
Creates a new source using the information stored in `info`. All fields of the
`info` struct should be set:

Type               | Field      | Description
-------------------|------------|-----------------------------------------------
cm_EventHandler    | handler    | Event handler for stream
void\*             | udata      | User value passed to event handler in event
int                | samplerate | Native samplerate of stream
int                | length     | Length in frames

NULL is returned if an error occurs.

##### cm_Source\* cm_new_source_from_file(const char \*filename)
Loads the given file into memory and creates a source from it; NULL is returned
if an error occurs. If you want to create several sources using the same audio
data, the file should be loaded into memory and `cm_new_source_from_mem()`
should be used instead.

##### cm_Source\* cm_new_source_from_mem(void \*data, int size)
Creates a source using the file data stored in memory, the library does not own
the `data` and will not free it when it is destroyed. NULL is returned if an
error occurs.

##### void cm_destroy_source(cm_Source \*src)
Frees all resources a source was using, if the source is playing it is stopped
immediately. This should be called on a source when  you are done with it; the
source is no longer valid once it is destroyed.

##### double cm_get_length(cm_Source \*src)
Returns the length in seconds of the source's audio data.

##### double cm_get_position(cm_Source \*src)
Returns the current playhead position of the source in seconds.

##### int cm_get_state(cm_Source \*src)
Returns the current play state of the source, this can be one of the following:
* `CM_STATE_PLAYING`
* `CM_STATE_PAUSED`
* `CM_STATE_STOPPED`

##### void cm_set_gain(cm_Source \*src, double gain)
Sets the gain (volume) of the source. For example: `0.5` will make it quieter,
`2` will make it louder. `1` is the default.

##### void cm_set_pan(cm_Source \*src, double pan)
Sets the left-right panning of the source — `-1` is fully-left, `1` is
fully-right, `0` is centered (default).

##### void cm_set_pitch(cm_Source \*src, double pitch)
Sets the playback pitch (speed) of the source; by default this is `1`. `2` is
one octave higher, `0.5` is one octave lower.

##### void cm_set_loop(cm_Source \*src, int loop)
Enables looping playback if `loop` is non-zero. By default looping is disabled.

##### void cm_play(cm_Source \*src)
Plays the source. If the source is already playing this function has no effect;
call `cm_stop()` before calling this function to play it from the beginning.

##### void cm_pause(cm_Source \*src)
Pauses the source's playback. This stops playback without losing the current
position, calling `cm_play()` will continue playing where it left off.

##### void cm_stop(cm_Source \*src)
Stops playing and rewinds the stream's play position back to the beginning.


## Auxiliary buses
Sources contribute their post-resampling, post-gain and post-pan stereo signal
to a bus. Buses are summed into downstream buses, and the permanent master bus
is the final summing point. Intermediate buffers are floating point and are
never clipped; clipping only happens once, on the final 16-bit master output,
after the master gain is applied.

A plain bus's output is the sum of its current inputs multiplied by its gain. A
delay bus applies, independently per channel,

```
y[n] = x[n - D] + feedback * y[n - D]
```

for output frame `n`, where `D` is the delay length in frames and frames with a
negative index read as zero. The delay is timed against the master samplerate.
`feedback` must be in `[0, 1)`. Bus and edge gains are applied to the bus output
and are never written back into the delay history. A bus with no path to the
master still advances its effect clock, so an empty read keeps tails decaying.

All bus functions must be called between `cm_process()` calls, from the same
thread that drives rendering. They do not take the lock callback.

##### cm_Bus\* cm_new_bus(double gain)
Creates a plain summing bus with the given output gain. Returns NULL on
allocation failure or if `gain` is not finite.

##### cm_Bus\* cm_new_delay_bus(int delay, double feedback)
Creates a delay bus. `delay` must be a positive number of frames and `feedback`
must be finite and in `[0, 1)`. Invalid arguments are rejected before any state
is created and NULL is returned.

##### int cm_destroy_bus(cm_Bus \*bus)
Frees a bus. The master bus and buses referenced by a live source, or having an
incoming or outgoing edge, cannot be freed. A rejected destroy changes nothing
and returns `0`; success returns `1`. Destroying a source removes its reference,
so a bus freed afterwards never has a dangling route.

##### void cm_set_bus_gain(cm_Bus \*bus, double gain)
Sets the bus output gain. Non-finite values are rejected and leave the gain
unchanged.

##### cm_Bus\* cm_get_master_bus(void)
Returns the permanent master output bus. It is always present, is the final
summing point, and cannot be destroyed or used as an edge source.

##### cm_Bus\* cm_get_source_bus(cm_Source \*src)
Returns the bus the source currently feeds. New sources feed the master bus.

##### void cm_set_source_bus(cm_Source \*src, cm_Bus \*bus)
Routes the source into `bus`. The source is still decoded and advanced exactly
once; only its contribution changes destination. Passing a bus that is not part
of the graph leaves the routing unchanged.

##### void cm_set_source_master(cm_Source \*src)
Restores the default route of the source: the master output bus.

##### int cm_connect_bus(cm_Bus \*from, cm_Bus \*to, double gain)
Connects `from` to `to` with the given gain. Returns `0` and leaves the graph
untouched if the edge would create a cycle, if `from` is the master bus, if
either bus is invalid, or if `gain` is not finite. Connecting an already
connected pair only updates its gain (the same as `cm_set_edge_gain()`).
Multiple upstream buses are summed at the destination.

##### int cm_set_edge_gain(cm_Bus \*from, cm_Bus \*to, double gain)
Updates the gain of an existing edge. Returns `0` if the edge does not exist or
the gain is not finite.

##### int cm_disconnect_bus(cm_Bus \*from, cm_Bus \*to)
Removes an edge. Disconnecting an input edge does not clear the destination's
delay history; the effect keeps advancing, and reconnecting does not replay the
tail from the disconnected period. Returns `0` if the edge does not exist.

##### void cm_clear_bus_history(cm_Bus \*bus)
Clears the delay history of one delay bus. Only that bus's two channel states
(and its read/write clock) are reset; source positions and other buses are
left untouched. Has no effect on plain or invalid buses.


## Events
All event handlers are passed a pointer to a `cm_Event`, the `type` field of
the event is set to one of the following:

#### CM_EVENT_LOCK
Passed to the lock handler set by `cm_set_lock()` when the lock should be
acquired.

#### CM_EVENT_UNLOCK
Passed to the lock handler set by `cm_set_lock()` when the lock should be
released.

#### CM_EVENT_DESTROY
Passed to a source's event handler when that source is going to be destroyed.
Any additional resources the source is using should be cleaned up when this
event is received.

#### CM_EVENT_SAMPLES
Passed to a source's event handler when more audio samples are required. The
`buffer` field should be filled with the number of audio samples specified by
the `length` field. The audio written should always be signed 16bit stereo
interlaced. If the stream reaches the end before it has filled the buffer it
should loop back to the beginning and continue writing to the buffer until it is
filled.

The audio written to the buffer will automatically be resampled by the library
on playback if the samplerate differs from the one passed to `cm_init()`.

#### CM_EVENT_REWIND
Passed to a source's event handler when the stream should rewind to the
beginning.

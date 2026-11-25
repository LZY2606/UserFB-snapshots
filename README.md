FFTConvolver
============

FFTConvolver is a C++ library for highly efficient convolution of
audio data (e.g. for usage in real-time convolution reverbs etc.).

- Partitioned convolution algorithm (using uniform block sizes)
- Optional support for non-uniform block sizes (TwoStageFFTConvolver)
- Asynchronous two-stage convolution with a non-blocking tail pipeline
  (AsyncFFTConvolver) for audio callbacks that must not wait on worker threads
- No external dependencies (FFT already included)
- Optional optimization for SSE (enabled by defining FFTCONVOLVER_USE_SSE)


AsyncFFTConvolver
-----------------

`AsyncFFTConvolver` keeps the short-head / long-tail partition scheme of
`TwoStageFFTConvolver`, but the long tail convolution is split into
independently computable tail jobs (`TailJob`). The first
`2 * tailBlockSize` IR coefficients run in a short-block head convolver; the
remaining coefficients use `2 * tailBlockSize` point FFTs with
`tailBlockSize` long partitions. Every time `tailBlockSize` real input
samples have been collected, one ordered tail job is produced. Its result
window starts `2 * tailBlockSize` samples after the input block, so the
algorithm introduces no extra latency and never reads future input.

### TailExecutor contract

The host supplies a `TailExecutor`:

- `submit(run, arg)` is called from the audio thread and must be non-blocking
  and allocation-free. Returning `true` means the call will run exactly once,
  either before `submit` returns or at any later time. Returning `false` means
  the call was rejected and will never run.
- `quiesce()` blocks until every accepted call has fully returned. It may only
  be called from the non-audio thread that also owns `init()`, `reset()` and
  destruction.
- A null executor forces fully local tail processing.

### Fallback and lifetime rules

- At most one accepted background call may be accessing a job at any time. A
  worker only touches its own read-only input snapshot, the shared read-only IR
  plan and its private workspace; the convolver keeps rotating its
  frequency-domain ring independently.
- When a tail result is due and no complete background result has been
  published, the audio thread computes that same job immediately with a
  private workspace. It never waits, locks, allocates, frees or calls
  `quiesce()` inside `process()`.
- A background result that finishes after the local fallback is discarded by
  its absolute sample position: it can neither advance the overlap-add a second
  time nor overwrite a newer generation. While an old worker still holds its
  slot, subsequent jobs are completed locally; the slot is only reused after
  the worker has published completion and stopped accessing it.
- `init()` may replace a running configuration, but first calls `quiesce()` to
  reclaim every accepted job, and stores its own copy of the IR. Invalid
  parameters leave the previous configuration and audio history untouched.
- `reset()` (and the destructor) calls `quiesce()` before releasing any job
  memory, clears the configuration and makes subsequent `process()` calls emit
  zeros. The executor must stay valid until `reset()` has returned.

See `examples/async_tail.cpp` for a real `std::thread` based executor.


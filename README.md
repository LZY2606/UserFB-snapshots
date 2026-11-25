FFTConvolver
============

FFTConvolver is a C++ library for highly efficient convolution of
audio data (e.g. for usage in real-time convolution reverbs etc.).

- Partitioned convolution algorithm (using uniform block sizes)
- Optional support for non-uniform block sizes (TwoStageFFTConvolver)
- No external dependencies (FFT already included)
- Optional optimization for SSE (enabled by defining FFTCONVOLVER_USE_SSE)

Asynchronous tail convolution (AsyncFFTConvolver)
--------------------------------------------------

`AsyncFFTConvolver` keeps the short-head / long-tail two-stage design but
moves the expensive tail partitions onto a host-provided worker while the
audio callback stays non-blocking. The first `2*tailBlockSize` impulse
response coefficients run through a short `headBlockSize` partitioned head
convolver; the remaining coefficients use `2*tailBlockSize` point FFTs with
`tailBlockSize` long partitions.

Every `tailBlockSize` collected input samples produce one ordered,
independently computable tail job (`TailJob`). Each job owns a private,
read-only copy of its input block and of every input spectrum it needs,
plus its own FFT and multiply-accumulate workspaces, so a worker only ever
touches data it holds itself (the impulse-response spectra are shared but
immutable after initialization).

### TailExecutor contract and lifetime

- `submit(run, arg)` is called on the audio thread. It must never block and
  must never allocate. Returning `true` means the callback is accepted and
  runs exactly once - it may finish before `submit` returns or arbitrarily
  later. Returning `false` means it was rejected and will never run.
- At most one accepted background call may still reference a job's buffers.
  While that slot is occupied, newer jobs are not submitted; they are
  computed on the audio thread instead. The slot is reused only after the
  worker has published completion and no longer accesses any job buffer.
- `quiesce()` blocks until every accepted callback has fully returned. It is
  only callable from the non-audio thread that owns `init`/`reset`/
  destruction, never from `process()`. The executor must stay valid until
  `reset()` (and the destruction of the convolver) has returned.
- A null executor selects fully local processing: every tail job is computed
  on the calling thread, with identical numerical results.

### Fallback rule

A tail job is due `2*tailBlockSize` samples after the input block that
produced it. If its background result has not been published by then, the
audio thread computes the very same job itself using an independent
workspace so that no audio is missing, duplicated or delayed. A background
result that arrives afterwards is discarded: overlap is never advanced twice
and a newer generation is never overwritten. `process()` never allocates,
frees, locks, waits or calls `quiesce()`; all workspaces grow only with the
impulse response and partition sizes during initialization.

See `examples/async_tail.cpp` for a real `std::thread` based executor.


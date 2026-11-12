# Prime-factor fast path (Bluestein) and caller-supplied workspaces

This build of KISS FFT accelerates transform lengths whose factorization
contains a prime factor of 17 or larger.  Such lengths used to fall back to
the quadratic generic butterfly (`kf_bfly_generic`) and to allocate a
temporary buffer on every call.  They are now evaluated with Bluestein's
chirp-z algorithm, and every entry point can run on caller-supplied
workspace without touching the heap.

Scope: the fast path and the `*_work` entry points exist for the default
`float` build and for `kiss_fft_scalar=double`.  Fixed-point and SIMD builds
keep the original algorithm and entry points; the header-only C++
implementation (`kissfft.hh`, `kissfft_i32.hh`) is unchanged.

## What the planner does

`kiss_fft_alloc` factors the requested length `N`.  If every prime factor
is below 17, the plan is exactly the classic mixed-radix plan (same code
path, same operation order, same results as before).  Otherwise the plan
becomes a Bluestein plan:

```
X[k] = chirp[k] * ( (x .* chirp) (*) conj(chirp) )[k],
chirp[n] = exp(s * pi * i * n^2 / N),  s = -1 forward, +1 inverse
```

The circular convolution `(*)` has length `M = kiss_fft_next_fast_size(2*N-1)`
(only 2/3/5 factors) and is evaluated with two nested ordinary KISS FFT
plans of length `M` that live inside the same single plan block.  The
kernel spectrum `FFT(conj(chirp))` is precomputed at plan time and
pre-scaled by `1/M`, which exactly cancels the missing normalization of
the inverse convolution FFT — once, not zero times and not `1/N`.

The chirp phases are computed as `pi * (n^2 mod 2N) / N` with 64-bit
integer squares, so the index arithmetic cannot overflow and the argument
handed to the trig functions stays below `pi`.

Memory stays linear: the plan holds `N` chirp values, `M` kernel values,
and the two length-`M` sub-plans — no `N x N` matrix anywhere.

Real even-length transforms (`kiss_fftr*`, e.g. 514 = 2*257) and
multi-dimensional transforms (`kiss_fftnd*`, e.g. 3x257) inherit the fast
path through their complex sub-plans; no separate algorithm is layered on
top.

## Scaling convention

Unchanged from classic KISS FFT: floating-point transforms are
unnormalized in both directions, so a forward transform followed by an
inverse transform yields `N` times the input.  Divide by `N` to recover
the original signal.

## Workspace API

Execution never allocates or frees heap memory, never uses stack arrays
that grow with `N`, and never writes to the plan.  The caller provides a
scratch buffer with plain `malloc` alignment; the same buffer may be reused
by any plan once the previous call has returned, and one plan may be used
with several workspaces interchangeably.

```c
#include "kiss_fft.h"

kiss_fft_cfg cfg = kiss_fft_alloc(257, 0, NULL, NULL);   /* plan once */
size_t bytes = kiss_fft_work_size(cfg);                  /* query scratch */
void *work = malloc(bytes);                              /* caller-owned  */

kiss_fft_stride_work(cfg, fin, fout, 1, work, bytes);    /* no heap use   */
/* fin == fout is allowed for stride 1 */

free(work);
kiss_fft_free(cfg);          /* plan is still one block, freed as before */
```

Real transforms (input `N` scalars, output `N/2+1` complex; the in-place
form shares one buffer of `N+2` scalars):

```c
#include "kiss_fftr.h"

kiss_fftr_cfg fc = kiss_fftr_alloc(514, 0, NULL, NULL);
kiss_fftr_cfg ic = kiss_fftr_alloc(514, 1, NULL, NULL);
size_t bytes = kiss_fftr_work_size(fc);   /* same value for ic */
void *work = malloc(bytes);

kiss_fftr_work(fc, timedata, freqdata, work, bytes);
kiss_fftri_work(ic, freqdata, timedata, work, bytes);  /* gives 514*x */
```

Multi-dimensional complex transforms (same dimension order and per-axis
strides as `kiss_fftnd`, full `fin == fout` in-place form supported):

```c
#include "kiss_fftnd.h"

int dims[2] = { 3, 257 };
kiss_fftnd_cfg nc = kiss_fftnd_alloc(dims, 2, 0, NULL, NULL);
size_t bytes = kiss_fftnd_work_size(nc);
void *work = malloc(bytes);

kiss_fftnd_work(nc, fin, fout, work, bytes);
```

All `*_work` functions return 0 on success and -1 on invalid arguments
(NULL cfg/input/output, non-positive stride, missing or too-small
workspace); in the failure case outputs and plans are left unchanged.
The size queries return 0 for a NULL cfg, and a reported size of 0 means
`work` may be NULL.

## Ownership and reentrancy

* The plan is immutable during execution.  The temporary buffers that the
  classic `kiss_fftr` / `kiss_fftnd` entries keep inside the plan are
  *not* used by the `*_work` entries; their scratch comes entirely from
  the caller's workspace.
* The legacy entries (`kiss_fft`, `kiss_fft_stride`, `kiss_fftr`,
  `kiss_fftri`, `kiss_fftnd`) run the same fast kernels and simply prepare
  the same kind of workspace once per call; plans are still freed as a
  single block, and the `mem`/`lenmem` two-call allocation style is
  unchanged (query with `mem == NULL`, then a big-enough caller block is
  initialized in place with no further heap allocation).
* A workspace holds no state between calls: zero-padding tails and
  convolution intermediates from one call never leak into the next.

## Complexity

For a length with a large prime factor, the old path cost `O(N * p)` for
the largest prime `p` (quadratic for prime `N`).  The Bluestein path costs
`O(M log M)` with `M < 2*(2N-1)` plus `O(N)` chirp multiplications, and
uses `O(M)` plan and workspace memory.  Lengths with only small prime
factors keep the original mixed-radix schedule and its costs.

## Diagnostic hook

`kiss_fft_op_count_reset()` / `kiss_fft_op_count_get()` report the generic
butterfly work actually executed (number of invocations, sum of radices,
complex multiply-adds).  Transforms that take the Bluestein path perform
zero generic-butterfly work; small radices (7, 11, 13) still show up there,
which the test suite uses to prove the large prime factors have left the
quadratic loop.

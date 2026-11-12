/*
 * bench.c - scalar vs NEON timings for libmpeg2 on AArch64.
 *
 * Three workloads, all with the same compiler and the same -O3:
 *
 *   idct    - mpeg2_idct_copy + mpeg2_idct_add over a deterministic
 *             block corpus.  The scalar arm reaches the static C kernels
 *             through the public function pointers; the NEON arm calls the
 *             NEON entry points directly.
 *   motion  - all eight 16-wide put/avg o/x/y/xy kernels over a large
 *             reference picture.  Scalar uses mpeg2_mc_c, NEON uses
 *             mpeg2_mc_neon.
 *   stream  - a whole decode of a supplied MPEG-2 elementary stream
 *             through the public parser, once in a child that forces the
 *             C back end (mpeg2_accel(0)) and once in a child that takes
 *             the automatic (NEON) selection.
 *
 * Each case is warmed up and then timed over several runs; the reported
 * figure is the median.  Auto-vectorisation is not disabled anywhere and
 * no absolute millisecond threshold is applied: the numbers are
 * machine dependent and are printed for comparison only.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "mpeg2.h"
#include "attributes.h"
#include "mpeg2_internal.h"

extern void mpeg2_idct_copy_neon (int16_t *, uint8_t *, int);
extern void mpeg2_idct_add_neon (int, int16_t *, uint8_t *, int);
extern void (* mpeg2_idct_copy) (int16_t *, uint8_t *, int);
extern void (* mpeg2_idct_add) (int, int16_t *, uint8_t *, int);
extern mpeg2_mc_t mpeg2_mc_c, mpeg2_mc_neon;

#define BLOCKS 4096
#define REPS_IDCT 200
#define REPS_MC 300
#define WARMUP 2
#define RUNS 7

static int16_t blocks[BLOCKS][64];
static uint8_t dest[BLOCKS][16 * 8 + 16];

static uint32_t rng = 0xdeadbeefu;

static uint32_t rnext (void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

static void prepare (void)
{
    int b, i;

    for (b = 0; b < BLOCKS; b++) {
	for (i = 0; i < 64; i++)
	    blocks[b][i] = (int16_t) ((int) (rnext () & 0x7ff) - 1024);
	for (i = 0; i < (int) sizeof (dest[b]); i++)
	    dest[b][i] = (uint8_t) (rnext () >> 24);
    }
}

static double now_seconds (void)
{
    struct timespec ts;

    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static int cmp_double (const void * a, const void * b)
{
    double x = *(const double *) a, y = *(const double *) b;

    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static double median (double * v, int n)
{
    qsort (v, (size_t) n, sizeof (double), cmp_double);
    return v[n / 2];
}

static volatile uint32_t sink;

static void idct_scalar (void)
{
    int r, b;

    for (r = 0; r < REPS_IDCT; r++) {
	for (b = 0; b < BLOCKS; b++) {
	    mpeg2_idct_copy (blocks[b], dest[b], 13);
	    mpeg2_idct_add (129, blocks[b], dest[b], 13);
	}
    }
    sink ^= dest[0][0];
}

static void idct_neon (void)
{
    int r, b;

    for (r = 0; r < REPS_IDCT; r++) {
	for (b = 0; b < BLOCKS; b++) {
	    mpeg2_idct_copy_neon (blocks[b], dest[b], 13);
	    mpeg2_idct_add_neon (129, blocks[b], dest[b], 13);
	}
    }
    sink ^= dest[0][0];
}

#define MC_ROWS 256
#define MC_COLS 256
static uint8_t mc_ref[MC_ROWS * MC_COLS];
static uint8_t mc_dest[MC_ROWS * MC_COLS];

#define MC_LOOP(body)							\
    int r, y, x;							\
    for (r = 0; r < REPS_MC; r++) {					\
	for (y = 0; y + 24 < MC_ROWS; y += 16) {			\
	    for (x = 0; x + 24 < MC_COLS; x += 16) {			\
		uint8_t * d = mc_dest + y * MC_COLS + x;		\
		const uint8_t * p = mc_ref + y * MC_COLS + x;		\
		body;							\
	    }								\
	}								\
    }

static void mc_scalar (void)
{
    MC_LOOP (
	     mpeg2_mc_c.put[0] (d, p, MC_COLS, 16);
	     mpeg2_mc_c.put[1] (d, p, MC_COLS, 16);
	     mpeg2_mc_c.put[2] (d, p, MC_COLS, 16);
	     mpeg2_mc_c.put[3] (d, p, MC_COLS, 16);
	     mpeg2_mc_c.avg[0] (d, p, MC_COLS, 16);
	     mpeg2_mc_c.avg[1] (d, p, MC_COLS, 16);
	     mpeg2_mc_c.avg[2] (d, p, MC_COLS, 16);
	     mpeg2_mc_c.avg[3] (d, p, MC_COLS, 16));
    sink ^= mc_dest[0];
}

static void mc_neon (void)
{
    MC_LOOP (
	     mpeg2_mc_neon.put[0] (d, p, MC_COLS, 16);
	     mpeg2_mc_neon.put[1] (d, p, MC_COLS, 16);
	     mpeg2_mc_neon.put[2] (d, p, MC_COLS, 16);
	     mpeg2_mc_neon.put[3] (d, p, MC_COLS, 16);
	     mpeg2_mc_neon.avg[0] (d, p, MC_COLS, 16);
	     mpeg2_mc_neon.avg[1] (d, p, MC_COLS, 16);
	     mpeg2_mc_neon.avg[2] (d, p, MC_COLS, 16);
	     mpeg2_mc_neon.avg[3] (d, p, MC_COLS, 16));
    sink ^= mc_dest[0];
}

static void run_pair (const char * name, void (*scalar) (void),
		      void (*neon) (void), int reps)
{
    double ts[RUNS], tn[RUNS];
    double s, n, unit = 1.0;
    int i;

    for (i = 0; i < WARMUP; i++) {
	scalar ();
	neon ();
    }
    for (i = 0; i < RUNS; i++) {
	double t0 = now_seconds ();

	scalar ();
	ts[i] = now_seconds () - t0;
	t0 = now_seconds ();
	neon ();
	tn[i] = now_seconds () - t0;
    }
    s = median (ts, RUNS);
    n = median (tn, RUNS);
    (void) reps;
    if (s < 1e-6)
	unit = 1e6;
    printf ("%-7s scalar %8.2f ms  neon %8.2f ms  speedup %6.2fx\n",
	    name, s * 1e3 * unit, n * 1e3 * unit, s / n);
}

/* ---- whole-stream timing, in a fresh process per measurement -------- */

/*
 * mpeg2_accel is process-global and single shot, and this benchmark has
 * already forced the C back end in its own process.  Each stream
 * measurement therefore re-executes the benchmark binary as a child with
 * the request in the environment; the child does nothing but decode and
 * print its elapsed time.
 */
static void stream_child (void)
{
    const char * path = getenv ("MPEG2_BENCH_STREAM_CHILD");
    const char * reqs = getenv ("MPEG2_BENCH_STREAM_REQ");
    FILE * f;
    uint8_t chunk[73];
    size_t n = 1;
    mpeg2dec_t * dec;
    double t0, t1;

    if (!path || !reqs)
	return;
    mpeg2_accel ((uint32_t) strtoul (reqs, NULL, 0));
    dec = mpeg2_init ();
    f = fopen (path, "rb");
    if (!dec || !f)
	_exit (2);
    t0 = now_seconds ();
    while (n) {
	mpeg2_state_t s = mpeg2_parse (dec);

	if (s == STATE_BUFFER) {
	    n = fread (chunk, 1, sizeof (chunk), f);
	    mpeg2_buffer (dec, chunk, chunk + n);
	}
    }
    t1 = now_seconds ();
    fclose (f);
    mpeg2_close (dec);
    printf ("%.9f\n", t1 - t0);
    fflush (stdout);
    _exit (0);
}

static int self_path (char * out, size_t out_len)
{
#ifdef __APPLE__
    uint32_t size = (uint32_t) out_len;

    return _NSGetExecutablePath (out, &size) == 0 ? 0 : 1;
#else
    ssize_t n = readlink ("/proc/self/exe", out, out_len - 1);

    if (n <= 0)
	return 1;
    out[n] = 0;
    return 0;
#endif
}

static double stream_once (uint32_t req, const char * path)
{
    char exe[512];
    char reqstr[64];
    int fd[2];
    pid_t pid;
    char line[64];
    double secs = -1.0;
    ssize_t rc;

    if (self_path (exe, sizeof (exe)))
	return -1.0;
    snprintf (reqstr, sizeof (reqstr), "%u", (unsigned) req);
    if (pipe (fd))
	return -1.0;
    pid = fork ();
    if (pid < 0)
	return -1.0;
    if (pid == 0) {
	char * argv[2];
	char * e1, * e2;

	close (fd[0]);
	if (dup2 (fd[1], STDOUT_FILENO) < 0)
	    _exit (4);
	close (fd[1]);
	e1 = malloc (strlen (path) + 32);
	e2 = malloc (strlen (reqstr) + 32);
	if (!e1 || !e2)
	    _exit (5);
	sprintf (e1, "MPEG2_BENCH_STREAM_CHILD=%s", path);
	sprintf (e2, "MPEG2_BENCH_STREAM_REQ=%s", reqstr);
	putenv (e1);
	putenv (e2);
	argv[0] = exe;
	argv[1] = NULL;
	execv (exe, argv);
	_exit (6);
    }
    close (fd[1]);
    rc = read (fd[0], line, sizeof (line) - 1);
    close (fd[0]);
    waitpid (pid, NULL, 0);
    if (rc <= 0)
	return -1.0;
    line[rc] = 0;
    if (sscanf (line, "%lf", &secs) != 1)
	return -1.0;
    return secs;
}

static void run_stream (const char * path)
{
    double ts[RUNS], tn[RUNS];
    double s, n;
    int i;

    for (i = 0; i < WARMUP; i++) {
	stream_once (0, path);
	stream_once (MPEG2_ACCEL_DETECT, path);
    }
    for (i = 0; i < RUNS; i++) {
	ts[i] = stream_once (0, path);
	tn[i] = stream_once (MPEG2_ACCEL_DETECT, path);
	if (ts[i] < 0 || tn[i] < 0) {
	    printf ("stream: decode failed\n");
	    return;
	}
    }
    s = median (ts, RUNS);
    n = median (tn, RUNS);
    printf ("%-7s scalar %8.3f ms  neon %8.3f ms  speedup %6.2fx\n",
	    "stream", s * 1e3, n * 1e3, s / n);
}

int main (int argc, char ** argv)
{
    const char * stream = (argc > 1) ? argv[1] : "tests/neon_move.m2v";

    stream_child ();

    /*
     * mpeg2_accel is single shot, so pick the C path first: that is the
     * documented way to force the portable kernels, and it leaves
     * mpeg2_idct_copy/add and mpeg2_mc pointing at the C routines for
     * the scalar arm.  The NEON arm calls the NEON symbols directly, which
     * is the same code the product selects by default.  On AArch64 the
     * capability is architectural; check it without installing anything.
     */
    if (!(mpeg2_detect_accel (MPEG2_ACCEL_DETECT) &
	  MPEG2_ACCEL_ARM_AARCH64_NEON)) {
	printf ("bench: AArch64 NEON back end is not available\n");
	return 2;
    }
    if (mpeg2_accel (0) != 0) {
	printf ("bench: could not force the C back end\n");
	return 2;
    }

    prepare ();
    {
	int i;

	for (i = 0; i < MC_ROWS * MC_COLS; i++) {
	    mc_ref[i] = (uint8_t) (rnext () >> 24);
	    mc_dest[i] = (uint8_t) (rnext () >> 24);
	}
    }

#if defined(__clang__)
    printf ("compiler: clang at -O3 (auto-vectorisation left on)\n");
#elif defined(__GNUC__)
    printf ("compiler: gcc at -O3 (auto-vectorisation left on)\n");
#else
    printf ("compiler: -O3 (auto-vectorisation left on)\n");
#endif
    printf ("idct:   %d blocks x %d reps, stride 13, copy+add\n",
	    BLOCKS, REPS_IDCT);
    printf ("motion: %dx%d reference, %d reps, eight 16-wide kernels\n",
	    MC_COLS, MC_ROWS, REPS_MC);
    printf ("stream: %s, whole decode, one fresh process per run\n",
	    stream);
    printf ("figures are medians of %d runs after %d warm-up runs\n\n",
	    RUNS, WARMUP);

    run_pair ("idct", idct_scalar, idct_neon, REPS_IDCT);
    run_pair ("motion", mc_scalar, mc_neon, REPS_MC);
    run_stream (stream);
    printf ("\nbench done (no pass/fail threshold is applied)\n");
    return 0;
}

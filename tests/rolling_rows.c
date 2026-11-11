/* Tests for vsDoTransformRows(), the rolling-shutter transform entry point.
 *
 * Standalone binary, built and run by tests/run_rolling_rows.sh -- it is not
 * part of the tests.c suite.  The production output is checked against the
 * independent per-pixel reference in rolling_rows_ref.c, which shares no
 * code with the warp loops being tested.
 *
 * THE FIXED POINT ERROR BUDGET (why the tolerances below are what they are)
 *
 * The production warp computes source coordinates in 16.16 fixed point, the
 * reference in double.  The discrepancy per coordinate component is bounded
 * by, in pixels:
 *
 *   - zcos/zsin quantized to 16.16 (truncation):        <= 2^-16 each,
 *     multiplied by the destination offsets |dx|,|dy|;
 *   - the >>16 in the backward map (floor):             <  2^-16;
 *   - c_tx/c_ty (fToFp16 truncation plus the subsample
 *     shift, another floor):                            <= 2*2^-16;
 *   - the float (not double) z in the production loop:  <= 2^-24 relative,
 *     negligible next to the above;
 *   - the stepped row accumulation:                     exact (integer adds).
 *
 * For the frames used here (<= 201 px in either axis, so |dx|+|dy| <= 200)
 * this sums to <= 200*2^-16 + 3*2^-16 < 0.0031 px.  Bilinear interpolation
 * amplifies a coordinate error by at most the largest adjacent-sample
 * difference (<= 255), i.e. <= 0.8 value units, and the production
 * interpolator's truncation compensation (+1) against the reference's
 * round-to-nearest adds at most 1.5.  Total <= 2.3; RR_TOL_MAX = 4 keeps
 * nearly 2x of headroom while still catching any geometric mistake, which
 * moves whole pixel columns, not hundredths.  The mean is smaller in
 * practice: measured across the cases below it is 0.17-0.25 for planar
 * formats (whose interpolator adds the +1) and 0.50-0.58 for packed ones
 * (whose rounding convention simply differs by half a unit on average),
 * so RR_TOL_MEAN = 1.0 leaves headroom while a real sub-pixel geometry
 * error would move it by several units.
 *
 * Pixels whose source coordinate falls near the frame border are EXCLUDED
 * from the bounded comparison: the production border interpolator feathers
 * the default value over 10 px outward, which the reference does not
 * reproduce.  Pixels far enough outside that the feather has run out are
 * checked EXACTLY against the border fill value instead.  So every pixel is
 * either compared with a bound, compared exactly, or deliberately skipped in
 * a named zone -- this is a per-pixel comparison, not a mean-brightness one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "transform.h"
#include "transformtype_operations.h"
#include "rolling_rows_ref.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int checks = 0, failures = 0;
#define CHECK(cond) do { \
    checks++; \
    if(!(cond)){ \
      failures++; \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while(0)

#define RR_TOL_MAX  4
#define RR_TOL_MEAN 1.0

static int planeW(const VSFrameInfo* fi, int p){
  return CHROMA_SIZE(fi->width,  vsGetPlaneWidthSubS(fi, p));
}
static int planeH(const VSFrameInfo* fi, int p){
  return CHROMA_SIZE(fi->height, vsGetPlaneHeightSubS(fi, p));
}
/* bytes of actual image data per row of plane p */
static int planeRowBytes(const VSFrameInfo* fi, int p){
  return planeW(fi, p) * (fi->pFormat < PF_PACKED ? 1 : fi->bytesPerPixel);
}
static double toLuma(int j, int sub){
  return (j + 0.5)*(double)(1 << sub) - 0.5;
}

/* Recognizable grid texture, painted in LUMA coordinates so every plane of
   every format shows the same picture: a smooth gradient base, bright grid
   lines every 24 luma px, and two discs whose displacement is measurable by
   eye and by machine. */
static double gridLuma(double lx, double ly){
  double v = fmod(lx*1.7 + ly*2.3 + 256.0*64.0, 256.0);
  double d1, d2;
  if(fmod(lx, 24.0) < 2.0 || fmod(ly, 24.0) < 2.0) v = 245.0;
  d1 = (lx-31.0)*(lx-31.0) + (ly-27.0)*(ly-27.0);
  d2 = (lx-93.0)*(lx-93.0) + (ly-71.0)*(ly-71.0);
  if(d1 < 12.0*12.0) v = 35.0;
  if(d2 <  8.0* 8.0) v = 210.0;
  return v;
}
static double gridChroma(double lx, double ly){
  double v = 128.0 + 90.0*sin(lx*0.05)*cos(ly*0.07);
  if(fmod(lx + ly, 40.0) < 3.0) v = 220.0;
  if(fmod(lx - ly + 4096.0, 56.0) < 3.0) v = 60.0;
  return v;
}
static uint8_t toByte(double v){
  int r = (int)floor(v + 0.5);
  return r < 0 ? 0 : (r > 255 ? 255 : (uint8_t)r);
}

/* Fills the valid pixels of a frame (padding, if any, is left alone) with
   the grid texture, shifted by (mx,my) luma px to fake camera motion. */
static void fillGrid(VSFrame* f, const VSFrameInfo* fi, double mx, double my){
  if(fi->pFormat < PF_PACKED){
    int p, x, y;
    for(p=0; p<fi->planes; p++){
      int wsub = vsGetPlaneWidthSubS(fi, p);
      int hsub = vsGetPlaneHeightSubS(fi, p);
      for(y=0; y<planeH(fi,p); y++)
        for(x=0; x<planeW(fi,p); x++){
          double lx = toLuma(x, wsub) - mx;
          double ly = toLuma(y, hsub) - my;
          f->data[p][x + y*f->linesize[p]] =
            toByte(p == 0 ? gridLuma(lx, ly) : gridChroma(lx, ly));
        }
    }
  }else{
    int n = fi->bytesPerPixel;
    int x, y;
    for(y=0; y<fi->height; y++)
      for(x=0; x<fi->width; x++){
        double lx = x - mx, ly = y - my;
        uint8_t* px = &f->data[0][x*n + y*f->linesize[0]];
        px[0] = toByte(gridLuma(lx, ly));
        px[1] = toByte(gridChroma(lx, ly));
        px[2] = toByte(255.0 - gridLuma(ly, lx));
        if(n > 3) px[3] = toByte(200.0 + 55.0*sin(lx*0.09));
      }
  }
}

static VSTransformConfig rowsConfig(VSBorderType crop, VSInterpolType ip){
  VSTransformConfig conf = vsTransformGetDefaultConfig("rolling_rows");
  conf.crop           = crop;
  conf.interpolType   = ip;
  /* the default is VSLensCorrectWobble, which the rows entry rejects */
  conf.lensCorrection = VSLensCorrectOff;
  return conf;
}

/* Runs the production rows path on src into dest (one fresh VSTransformData)
   and checks every call to succeed. */
static void runRows(const VSFrameInfo* fi, const VSFrame* src, VSFrame* dest,
                    VSTransform top, VSTransform bottom,
                    VSBorderType crop, VSInterpolType ip){
  VSTransformConfig conf = rowsConfig(crop, ip);
  VSTransformData td;
  CHECK(vsTransformDataInit(&td, &conf, fi, fi) == VS_OK);
  CHECK(vsTransformPrepare(&td, src, dest) == VS_OK);
  CHECK(vsDoTransformRows(&td, top, bottom) == VS_OK);
  CHECK(vsTransformFinish(&td) == VS_OK);
  vsTransformDataCleanup(&td);
}

/* Runs the legacy single-transform path. */
static void runLegacy(const VSFrameInfo* fi, const VSFrame* src, VSFrame* dest,
                      VSTransform t, VSBorderType crop, VSInterpolType ip){
  VSTransformConfig conf = rowsConfig(crop, ip);
  VSTransformData td;
  CHECK(vsTransformDataInit(&td, &conf, fi, fi) == VS_OK);
  CHECK(vsTransformPrepare(&td, src, dest) == VS_OK);
  CHECK(vsDoTransform(&td, t) == VS_OK);
  CHECK(vsTransformFinish(&td) == VS_OK);
  vsTransformDataCleanup(&td);
}

static int framesEqual(const VSFrame* a, const VSFrame* b, const VSFrameInfo* fi){
  int p, y;
  int planes = fi->pFormat < PF_PACKED ? fi->planes : 1;
  for(p=0; p<planes; p++)
    for(y=0; y<planeH(fi,p); y++)
      if(memcmp(a->data[p] + (size_t)y*a->linesize[p],
                b->data[p] + (size_t)y*b->linesize[p],
                (size_t)planeRowBytes(fi,p)) != 0)
        return 0;
  return 1;
}

/* Per-pixel comparison of production against the independent reference, over
   the pixels where the two are comparable; see the budget comment at the top
   of this file.  Crop mode only: the border fill is then the constant the
   far-outside pixels are checked against. */
static void compareVsRef(const VSFrame* prod, const VSFrame* ref,
                         const VSFrameInfo* fi,
                         VSTransform top, VSTransform bottom, const char* what){
  int packed = fi->pFormat >= PF_PACKED;
  int bpp    = packed ? fi->bytesPerPixel : 1;
  int planes = packed ? 1 : fi->planes;
  int p, y, x, k;
  long n = 0, skipped = 0, far = 0, farBad = 0;
  int maxDiff = 0;
  double sum = 0.0;
  for(p=0; p<planes; p++){
    int hsub = vsGetPlaneHeightSubS(fi, p);
    int w = planeW(fi,p), h = planeH(fi,p);
    int def = packed ? 16 : (p == 0 ? 0 : 0x80);
    for(y=0; y<h; y++){
      VSTransform t = rrRefTransformAt(top, bottom,
                                       rrRefRowU(fi->height, hsub, y));
      for(x=0; x<w; x++){
        double xs, ys;
        int interior, outside;
        rrRefSrcCoord(fi, p, x, y, &t, &xs, &ys);
        /* interior: both production (fixed point, <= 0.0031 px off, see the
           budget above) and reference take their plain bilinear path */
        interior = xs >= 1.0 && xs <= w-2.0 && ys >= 1.0 && ys <= h-2.0;
        if(packed){
          /* interpolateNall: def as soon as the floor index leaves the frame */
          outside = xs < -0.5 || xs > w+0.5 || ys < -0.5 || ys > h+0.5;
        }else{
          /* the 10 px outward feather of interpolateBiLinBorder has fully
             run out: the result is exactly the default value */
          outside = xs < -20.5 || xs > w+19.5 || ys < -20.5 || ys > h+19.5;
        }
        for(k=0; k<bpp; k++){
          int a = prod->data[p][x*bpp + k + y*prod->linesize[p]];
          int b = ref ->data[p][x*bpp + k + y*ref ->linesize[p]];
          if(interior){
            int d = a - b;
            if(d < 0) d = -d;
            if(d > maxDiff) maxDiff = d;
            sum += d;
            n++;
          }else if(outside){
            far++;
            if(a != def) farBad++;
          }else{
            skipped++;
          }
        }
      }
    }
  }
  fprintf(stderr, "  %-28s max %2d mean %.4f far %ld/%ld bad, %ld skipped\n",
          what, maxDiff, n ? sum/n : 0.0, farBad, far, skipped/(long)bpp);
  CHECK(n > 0);
  CHECK(maxDiff <= RR_TOL_MAX);
  CHECK(n == 0 || sum/n <= RR_TOL_MEAN);
  CHECK(farBad == 0);
}

/* Runs production (crop) and the reference on the same source and compares. */
static void checkCase(const VSFrameInfo* fi, const VSFrame* src,
                      VSTransform top, VSTransform bottom, const char* what){
  VSFrame prod, ref;
  vsFrameAllocate(&prod, fi);
  vsFrameAllocate(&ref,  fi);
  runRows(fi, src, &prod, top, bottom, VSCropBorder, VS_BiLinear);
  rrRefWarp(src, &ref, fi, top, bottom, 1);
  compareVsRef(&prod, &ref, fi, top, bottom, what);
  vsFrameFree(&prod);
  vsFrameFree(&ref);
}

/* ------------------------------------------------------------------ */

/* Identical endpoints must give exactly the old entry's output. */
static void testIdenticalEndpoints(void){
  static const struct { VSPixelFormat pf; const char* name; } fmts[] = {
    {PF_GRAY8,   "gray8"},  {PF_YUV420P, "yuv420p"}, {PF_YUV422P, "yuv422p"},
    {PF_YUV440P, "yuv440p"},{PF_YUV444P, "yuv444p"}, {PF_RGB24,   "rgb24"},
    {PF_RGBA,    "rgba"},
  };
  size_t f;
  int mode;
  fprintf(stderr, "--- identical endpoints == vsDoTransform ---\n");
  for(f=0; f<sizeof(fmts)/sizeof(fmts[0]); f++){
    VSFrameInfo fi;
    VSFrame src, a, b;
    VSTransform t;
    CHECK(vsFrameInfoInit(&fi, 160, 120, fmts[f].pf));
    vsFrameAllocate(&src, &fi);
    vsFrameAllocate(&a,   &fi);
    vsFrameAllocate(&b,   &fi);
    fillGrid(&src, &fi, 0.0, 0.0);
    for(mode=0; mode<2; mode++){
      VSBorderType crop = mode ? VSKeepBorder : VSCropBorder;
      int i;
      for(i=0; i<2; i++){
        /* i=0: a real transform; i=1: the identity (old entry's fast path) */
        t = i ? null_transform() : new_transform(2.5, -3.25, 0.031, 4.0, 0,0,0);
        runRows  (&fi, &src, &a, t, t, crop, VS_BiLinear);
        runLegacy(&fi, &src, &b, t,    crop, VS_BiLinear);
        CHECK(framesEqual(&a, &b, &fi));
      }
    }
    fprintf(stderr, "  %-8s byte-identical to vsDoTransform (both border modes)\n",
            fmts[f].name);
    vsFrameFree(&src); vsFrameFree(&a); vsFrameFree(&b);
  }
}

/* The first and last luma rows of a rows run are warped with exactly `top`
   and `bottom`, so they must equal the legacy output for those transforms
   byte for byte. */
static void testEndpointRowsExact(void){
  VSFrameInfo fi;
  VSFrame src, rows, topOut, botOut;
  VSTransform top    = new_transform( 3.5, -2.0,  0.021, 2.0, 0,0,0);
  VSTransform bottom = new_transform(-3.0,  2.5, -0.017, 0.0, 0,0,0);
  int p;
  fprintf(stderr, "--- first/last row equal the endpoint transforms exactly ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV444P));
  vsFrameAllocate(&src,    &fi);
  vsFrameAllocate(&rows,   &fi);
  vsFrameAllocate(&topOut, &fi);
  vsFrameAllocate(&botOut, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);
  runRows  (&fi, &src, &rows, top, bottom, VSCropBorder, VS_BiLinear);
  runLegacy(&fi, &src, &topOut, top,       VSCropBorder, VS_BiLinear);
  runLegacy(&fi, &src, &botOut, bottom,    VSCropBorder, VS_BiLinear);
  /* 4:4:4 has no subsampling, so u=0 and u=1 hold on every plane */
  for(p=0; p<fi.planes; p++){
    int w = planeW(&fi,p);
    CHECK(memcmp(rows.data[p], topOut.data[p], (size_t)w) == 0);
    CHECK(memcmp(rows.data[p]   + (size_t)(fi.height-1)*rows.linesize[p],
                 botOut.data[p] + (size_t)(fi.height-1)*botOut.linesize[p],
                 (size_t)w) == 0);
  }
  /* and a packed format, whole rows including all channels */
  {
    VSFrameInfo fp;
    VSFrame srcP, rowsP, topP, botP;
    CHECK(vsFrameInfoInit(&fp, 160, 120, PF_RGB24));
    vsFrameAllocate(&srcP,  &fp);
    vsFrameAllocate(&rowsP, &fp);
    vsFrameAllocate(&topP,  &fp);
    vsFrameAllocate(&botP,  &fp);
    fillGrid(&srcP, &fp, 0.0, 0.0);
    runRows  (&fp, &srcP, &rowsP, top, bottom, VSCropBorder, VS_BiLinear);
    runLegacy(&fp, &srcP, &topP,  top,         VSCropBorder, VS_BiLinear);
    runLegacy(&fp, &srcP, &botP,  bottom,      VSCropBorder, VS_BiLinear);
    CHECK(memcmp(rowsP.data[0], topP.data[0], (size_t)fp.width*3) == 0);
    CHECK(memcmp(rowsP.data[0] + (size_t)(fp.height-1)*rowsP.linesize[0],
                 botP.data[0]  + (size_t)(fp.height-1)*botP.linesize[0],
                 (size_t)fp.width*3) == 0);
    /* a middle row must NOT be either endpoint's row -- the interpolation is
       really happening */
    int mid = memcmp(rowsP.data[0] + (size_t)60*rowsP.linesize[0],
                     topP.data[0]  + (size_t)60*topP.linesize[0],
                     (size_t)fp.width*3);
    CHECK(mid != 0);
    vsFrameFree(&srcP); vsFrameFree(&rowsP);
    vsFrameFree(&topP); vsFrameFree(&botP);
  }
  vsFrameFree(&src); vsFrameFree(&rows);
  vsFrameFree(&topOut); vsFrameFree(&botOut);
}

/* All seven supported formats against the reference. */
static void testFormatsVsReference(void){
  static const struct { VSPixelFormat pf; const char* name; } fmts[] = {
    {PF_GRAY8,   "gray8"},  {PF_YUV420P, "yuv420p"}, {PF_YUV422P, "yuv422p"},
    {PF_YUV440P, "yuv440p"},{PF_YUV444P, "yuv444p"}, {PF_RGB24,   "rgb24"},
    {PF_RGBA,    "rgba"},
  };
  VSTransform top    = new_transform( 3.5, -2.0,  0.021, 2.0, 0,0,0);
  VSTransform bottom = new_transform(-3.0,  2.5, -0.017,-1.5, 0,0,0);
  size_t f;
  fprintf(stderr, "--- seven formats against the independent reference ---\n");
  for(f=0; f<sizeof(fmts)/sizeof(fmts[0]); f++){
    VSFrameInfo fi;
    VSFrame src;
    CHECK(vsFrameInfoInit(&fi, 160, 120, fmts[f].pf));
    vsFrameAllocate(&src, &fi);
    fillGrid(&src, &fi, 0.0, 0.0);
    checkCase(&fi, &src, top, bottom, fmts[f].name);
    vsFrameFree(&src);
  }
}

/* Opposite translations at the two ends: the middle of the frame is nearly
   untouched while top and bottom move in opposite directions. */
static void testOppositeTranslation(void){
  VSFrameInfo fi;
  VSFrame src;
  VSTransform top    = new_transform( 8.0, 0.0, 0.0, 0.0, 0,0,0);
  VSTransform bottom = new_transform(-8.0, 0.0, 0.0, 0.0, 0,0,0);
  fprintf(stderr, "--- opposite translations at top and bottom ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_GRAY8));
  vsFrameAllocate(&src, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);
  checkCase(&fi, &src, top, bottom, "gray8 +x/-x");
  vsFrameFree(&src);
  /* same on a horizontally subsampled format, so the chroma rows interpolate
     on their own exposure positions */
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV422P));
  vsFrameAllocate(&src, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);
  checkCase(&fi, &src, top, bottom, "yuv422p +x/-x");
  vsFrameFree(&src);
}

/* Rotation interpolation across the +/-pi wrap: the arc from 2.6 to -2.6 is
   2*pi-5.2 rad long the short way (through pi), not 5.2 the long way.  And
   endpoints exactly pi apart take the positive direction. */
static void testRotationAcrossPi(void){
  VSFrameInfo fi;
  VSFrame src;
  VSTransform top, bottom;
  fprintf(stderr, "--- rotation across +/-pi and the exact-pi tie ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_GRAY8));
  vsFrameAllocate(&src, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);

  top    = new_transform(0.0, 0.0,  2.6, 0.0, 0,0,0);
  bottom = new_transform(0.0, 0.0, -2.6, 0.0, 0,0,0);
  checkCase(&fi, &src, top, bottom, "gray8 2.6 -> -2.6");

  top    = new_transform(0.0, 0.0, -M_PI/2, 0.0, 0,0,0);
  bottom = new_transform(0.0, 0.0,  M_PI/2, 0.0, 0,0,0);
  checkCase(&fi, &src, top, bottom, "gray8 -pi/2 -> +pi/2");

  /* the same tie the other way round must give the same frames: both are
     exactly pi apart and both resolve to the positive direction */
  top    = new_transform(0.0, 0.0,  M_PI/2, 0.0, 0,0,0);
  bottom = new_transform(0.0, 0.0, -M_PI/2, 0.0, 0,0,0);
  checkCase(&fi, &src, top, bottom, "gray8 +pi/2 -> -pi/2");
  vsFrameFree(&src);

  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV444P));
  vsFrameAllocate(&src, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);
  top    = new_transform(0.0, 0.0,  2.6, 0.0, 0,0,0);
  bottom = new_transform(0.0, 0.0, -2.6, 0.0, 0,0,0);
  checkCase(&fi, &src, top, bottom, "yuv444p 2.6 -> -2.6");
  vsFrameFree(&src);
}

/* Zoom and rotation varying together. */
static void testZoomAndRotation(void){
  VSFrameInfo fi;
  VSFrame src;
  VSTransform top    = new_transform( 1.0, -1.5,  0.13,  6.0, 0,0,0);
  VSTransform bottom = new_transform(-2.0,  1.0, -0.09, -4.0, 0,0,0);
  fprintf(stderr, "--- zoom combined with rotation ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV420P));
  vsFrameAllocate(&src, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);
  checkCase(&fi, &src, top, bottom, "yuv420p zoom+rot");
  vsFrameFree(&src);
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_RGB24));
  vsFrameAllocate(&src, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);
  checkCase(&fi, &src, top, bottom, "rgb24 zoom+rot");
  vsFrameFree(&src);
}

/* A single-row frame has no interval to interpolate over: every row (the
   one row) uses u=0.5, the midpoint transform.  Checked exactly with an
   integer midpoint shift, and with a midpoint that lands far outside. */
static void testSingleRow(void){
  VSFrameInfo fi;
  VSFrame src, dest;
  VSTransform top, bottom;
  int x, p;
  fprintf(stderr, "--- single-row and single-pixel frames (u=0.5) ---\n");

  /* 64x1 gray, midpoint shift exactly 0.5: the row is only one scanline tall,
     so its samples always take the border path, which clamps the floor index
     -- dest[x] = src[clamp(x-1)].  A row of varying pixels makes that a real
     check, and a midpoint of 0.5 distinguishes u=0.5 from both endpoints
     (which would shift by 10 and -9). */
  CHECK(vsFrameInfoInit(&fi, 64, 1, PF_GRAY8));
  vsFrameAllocate(&src,  &fi);
  vsFrameAllocate(&dest, &fi);
  for(x=0; x<64; x++) src.data[0][x] = (unsigned char)(x*37 + 11);
  top    = new_transform(10.0, 0.0, 0.0, 0.0, 0,0,0);
  bottom = new_transform(-9.0, 0.0, 0.0, 0.0, 0,0,0);
  runRows(&fi, &src, &dest, top, bottom, VSCropBorder, VS_BiLinear);
  for(x=0; x<64; x++){
    int sx = x-1 < 0 ? 0 : x-1;
    CHECK(dest.data[0][x] == src.data[0][sx]);
  }
  /* midpoint far outside (95 px): every pixel is the border fill */
  top    = new_transform(100.0, 0.0, 0.0, 0.0, 0,0,0);
  bottom = new_transform( 90.0, 0.0, 0.0, 0.0, 0,0,0);
  runRows(&fi, &src, &dest, top, bottom, VSCropBorder, VS_BiLinear);
  for(x=0; x<64; x++)
    CHECK(dest.data[0][x] == 0);
  vsFrameFree(&src); vsFrameFree(&dest);

  /* 32x1 yuv444p: the u=0.5 midpoint applies to all three planes; 4:4:4 has
     no subsampling, so every plane maps the same way */
  CHECK(vsFrameInfoInit(&fi, 32, 1, PF_YUV444P));
  vsFrameAllocate(&src,  &fi);
  vsFrameAllocate(&dest, &fi);
  for(p=0; p<3; p++)
    for(x=0; x<32; x++)
      src.data[p][x] = (unsigned char)(x*23 + p*70 + 5);
  top    = new_transform( 8.5, 0.0, 0.0, 0.0, 0,0,0);
  bottom = new_transform(-7.5, 0.0, 0.0, 0.0, 0,0,0);
  runRows(&fi, &src, &dest, top, bottom, VSCropBorder, VS_BiLinear);
  for(p=0; p<3; p++)
    for(x=0; x<32; x++){
      int sx = x-1 < 0 ? 0 : x-1;
      CHECK(dest.data[p][x] == src.data[p][sx]);
    }
  vsFrameFree(&src); vsFrameFree(&dest);

  /* 1x1: degenerate but must not crash; with a sub-pixel shift the single
     pixel keeps its value (the border path clamps to the only pixel) */
  CHECK(vsFrameInfoInit(&fi, 1, 1, PF_GRAY8));
  vsFrameAllocate(&src,  &fi);
  vsFrameAllocate(&dest, &fi);
  src.data[0][0] = 173;
  top    = new_transform(0.4, 0.2, 0.0, 0.0, 0,0,0);
  bottom = new_transform(0.2, 0.4, 0.0, 0.0, 0,0,0);
  runRows(&fi, &src, &dest, top, bottom, VSCropBorder, VS_BiLinear);
  CHECK(dest.data[0][0] == 173);
  vsFrameFree(&src); vsFrameFree(&dest);
}

/* Odd frame sizes, and the subsampled formats at the sizes they allow. */
static void testOddSizes(void){
  static const struct { VSPixelFormat pf; int w, h; const char* name; } cases[] = {
    {PF_GRAY8,   101, 63, "gray8 101x63"},
    {PF_YUV444P, 101, 63, "yuv444p 101x63"},
    {PF_RGB24,   101, 63, "rgb24 101x63"},
    {PF_RGBA,     63, 33, "rgba 63x33"},
    {PF_YUV422P, 100, 63, "yuv422p 100x63 (odd height)"},
    {PF_YUV440P, 101, 64, "yuv440p 101x64 (odd width)"},
    {PF_YUV420P, 100, 64, "yuv420p 100x64"},
  };
  VSTransform top    = new_transform( 2.5, -1.5,  0.05, 3.0, 0,0,0);
  VSTransform bottom = new_transform(-2.0,  2.0, -0.04, 0.0, 0,0,0);
  size_t i;
  fprintf(stderr, "--- odd and subsampled sizes ---\n");
  for(i=0; i<sizeof(cases)/sizeof(cases[0]); i++){
    VSFrameInfo fi;
    VSFrame src;
    CHECK(vsFrameInfoInit(&fi, cases[i].w, cases[i].h, cases[i].pf));
    vsFrameAllocate(&src, &fi);
    fillGrid(&src, &fi, 0.0, 0.0);
    checkCase(&fi, &src, top, bottom, cases[i].name);
    vsFrameFree(&src);
  }
}

/* Linesize larger than the row width: the warp must respect each plane's
   linesize and never touch the padding at the row ends. */
static void testPadding(void){
  VSFrameInfo fi;
  VSFrame src, dest, ref;
  int p, y, i;
  const int pad = 13;
  VSTransform top    = new_transform( 3.0, -2.0,  0.04, 2.0, 0,0,0);
  VSTransform bottom = new_transform(-2.5,  1.5, -0.03, 0.0, 0,0,0);
  fprintf(stderr, "--- row-end padding is respected ---\n");
  CHECK(vsFrameInfoInit(&fi, 64, 48, PF_YUV420P));
  vsFrameNull(&src); vsFrameNull(&dest); vsFrameNull(&ref);
  for(p=0; p<fi.planes; p++){
    int ls = planeW(&fi,p) + pad;
    src .linesize[p] = ls;
    dest.linesize[p] = ls;
    ref .linesize[p] = ls;
    src .data[p] = malloc((size_t)ls*planeH(&fi,p));
    dest.data[p] = malloc((size_t)ls*planeH(&fi,p));
    ref .data[p] = malloc((size_t)ls*planeH(&fi,p));
    memset(src .data[p], 0xA5, (size_t)ls*planeH(&fi,p));
    memset(dest.data[p], 0xA5, (size_t)ls*planeH(&fi,p));
    memset(ref .data[p], 0xA5, (size_t)ls*planeH(&fi,p));
  }
  fillGrid(&src, &fi, 0.0, 0.0);
  runRows(&fi, &src, &dest, top, bottom, VSCropBorder, VS_BiLinear);
  rrRefWarp(&src, &ref, &fi, top, bottom, 1);
  compareVsRef(&dest, &ref, &fi, top, bottom, "yuv420p padded");
  for(p=0; p<fi.planes; p++){
    int w = planeW(&fi,p);
    for(y=0; y<planeH(&fi,p); y++)
      for(i=0; i<pad; i++){
        CHECK(dest.data[p][w+i + y*dest.linesize[p]] == 0xA5);
        CHECK(src .data[p][w+i + y*src .linesize[p]] == 0xA5);
      }
  }
  for(p=0; p<fi.planes; p++){
    free(src.data[p]); free(dest.data[p]); free(ref.data[p]);
  }

  /* packed, with padding not a multiple of the pixel size */
  {
    VSFrameInfo fp;
    VSFrame srcP, destP, refP;
    int ls;
    CHECK(vsFrameInfoInit(&fp, 50, 40, PF_RGB24));
    ls = 50*3 + 7;
    vsFrameNull(&srcP); vsFrameNull(&destP); vsFrameNull(&refP);
    srcP.data[0]  = malloc((size_t)ls*40); srcP.linesize[0]  = ls;
    destP.data[0] = malloc((size_t)ls*40); destP.linesize[0] = ls;
    refP.data[0]  = malloc((size_t)ls*40); refP.linesize[0]  = ls;
    memset(srcP.data[0],  0xA5, (size_t)ls*40);
    memset(destP.data[0], 0xA5, (size_t)ls*40);
    memset(refP.data[0],  0xA5, (size_t)ls*40);
    fillGrid(&srcP, &fp, 0.0, 0.0);
    runRows(&fp, &srcP, &destP, top, bottom, VSCropBorder, VS_BiLinear);
    rrRefWarp(&srcP, &refP, &fp, top, bottom, 1);
    compareVsRef(&destP, &refP, &fp, top, bottom, "rgb24 padded");
    for(y=0; y<40; y++)
      for(i=0; i<7; i++){
        CHECK(destP.data[0][50*3+i + y*ls] == 0xA5);
        CHECK(srcP .data[0][50*3+i + y*ls] == 0xA5);
      }
    free(srcP.data[0]); free(destP.data[0]); free(refP.data[0]);
  }
}

/* In-place and separate-buffer operation must give the same output, and a
   source buffer the host still owns must never be written -- also when an
   in-place frame came in between (the decoder-reference situation of
   issue #144, through the rows entry). */
static void testInPlaceAndOwnership(void){
  VSFrameInfo fi;
  VSFrame src, sepOut, work, refOut;
  VSTransform top    = new_transform( 3.0, -2.0,  0.04, 2.0, 0,0,0);
  VSTransform bottom = new_transform(-2.5,  1.5, -0.03, 0.0, 0,0,0);
  int mode;
  fprintf(stderr, "--- in-place vs separate buffers, and source ownership ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV420P));
  vsFrameAllocate(&src,    &fi);
  vsFrameAllocate(&sepOut, &fi);
  vsFrameAllocate(&work,   &fi);
  vsFrameAllocate(&refOut, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);
  for(mode=0; mode<2; mode++){
    VSBorderType crop = mode ? VSKeepBorder : VSCropBorder;
    VSTransformConfig conf = rowsConfig(crop, VS_BiLinear);
    VSTransformData td;
    runRows(&fi, &src, &sepOut, top, bottom, crop, VS_BiLinear);
    /* in-place: the host transforms the buffer it handed in */
    vsFrameCopy(&work, &src, &fi);
    CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
    CHECK(vsTransformPrepare(&td, &work, &work) == VS_OK);
    CHECK(vsDoTransformRows(&td, top, bottom) == VS_OK);
    CHECK(vsTransformFinish(&td) == VS_OK);
    vsTransformDataCleanup(&td);
    CHECK(framesEqual(&sepOut, &work, &fi));
  }
  fprintf(stderr, "  in-place output byte-identical to separate buffers (both modes)\n");

  /* ownership: frame A with separate buffers (src stays decoder-owned),
     then frame B in-place; A's buffer must be untouched throughout */
  {
    VSTransformConfig conf = rowsConfig(VSKeepBorder, VS_BiLinear);
    VSTransformData td;
    VSFrame a, outA, b;
    int dirty = 0, p, y, x;
    vsFrameAllocate(&a,    &fi);
    vsFrameAllocate(&outA, &fi);
    vsFrameAllocate(&b,    &fi);
    fillGrid(&a, &fi, 0.0, 0.0);
    fillGrid(&b, &fi, 1.0, 1.0);
    /* snapshot of A to compare against after both frames ran */
    vsFrameCopy(&refOut, &a, &fi);
    CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
    CHECK(vsTransformPrepare(&td, &a, &outA) == VS_OK);
    CHECK(vsDoTransformRows(&td, top, bottom) == VS_OK);
    CHECK(vsTransformFinish(&td) == VS_OK);
    CHECK(vsTransformPrepare(&td, &b, &b) == VS_OK);
    CHECK(vsDoTransformRows(&td, bottom, top) == VS_OK);
    CHECK(vsTransformFinish(&td) == VS_OK);
    vsTransformDataCleanup(&td);
    for(p=0; p<fi.planes; p++)
      for(y=0; y<planeH(&fi,p); y++)
        for(x=0; x<planeW(&fi,p); x++)
          if(a.data[p][x + y*a.linesize[p]] != refOut.data[p][x + y*refOut.linesize[p]])
            dirty++;
    fprintf(stderr, "  decoder-held source: %d bytes changed (expect 0)\n", dirty);
    CHECK(dirty == 0);
    vsFrameFree(&a); vsFrameFree(&outA); vsFrameFree(&b);
  }
  vsFrameFree(&src); vsFrameFree(&sepOut); vsFrameFree(&work); vsFrameFree(&refOut);
}

/* Both border modes: crop fills uncovered pixels with black/0x80, keep
   leaves the previous stabilized frame's pixels in place. */
static void testBorderModes(void){
  VSFrameInfo fi;
  VSFrame src1, src2, out1, out2, cropOut;
  VSTransform t1top = new_transform( 1.0,  1.0,  0.01, 0.0, 0,0,0);
  VSTransform t1bot = new_transform(-1.0, -1.0, -0.01, 0.0, 0,0,0);
  /* frame 2 shifts hard left at the top and hard right at the bottom, so
     every row has a strip of uncovered pixels on one side */
  VSTransform t2top = new_transform( 30.0, 0.0, 0.0, 0.0, 0,0,0);
  VSTransform t2bot = new_transform(-30.0, 0.0, 0.0, 0.0, 0,0,0);
  VSTransformConfig conf;
  VSTransformData td;
  int x, y, p;
  long kept = 0, keptBad = 0, cropped = 0, cropBad = 0;
  fprintf(stderr, "--- keep vs crop border ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV420P));
  vsFrameAllocate(&src1, &fi);
  vsFrameAllocate(&src2, &fi);
  vsFrameAllocate(&out1, &fi);
  vsFrameAllocate(&out2, &fi);
  vsFrameAllocate(&cropOut, &fi);
  fillGrid(&src1, &fi, 0.0, 0.0);
  fillGrid(&src2, &fi, 1.5, 0.5);

  /* keep border: two frames through one VSTransformData */
  conf = rowsConfig(VSKeepBorder, VS_BiLinear);
  CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
  CHECK(vsTransformPrepare(&td, &src1, &out1) == VS_OK);
  CHECK(vsDoTransformRows(&td, t1top, t1bot) == VS_OK);
  CHECK(vsTransformFinish(&td) == VS_OK);
  CHECK(vsTransformPrepare(&td, &src2, &out2) == VS_OK);
  CHECK(vsDoTransformRows(&td, t2top, t2bot) == VS_OK);
  CHECK(vsTransformFinish(&td) == VS_OK);
  vsTransformDataCleanup(&td);

  /* crop border: same frame 2 on a fresh pipeline */
  runRows(&fi, &src2, &cropOut, t2top, t2bot, VSCropBorder, VS_BiLinear);

  for(p=0; p<fi.planes; p++){
    int hsub = vsGetPlaneHeightSubS(&fi,p);
    int w = planeW(&fi,p), h = planeH(&fi,p);
    int def = p == 0 ? 0 : 0x80;
    for(y=0; y<h; y++){
      VSTransform t = rrRefTransformAt(t2top, t2bot,
                                       rrRefRowU(fi.height, hsub, y));
      for(x=0; x<w; x++){
        double xs, ys;
        rrRefSrcCoord(&fi, p, x, y, &t, &xs, &ys);
        if(xs < -20.5 || xs > w+19.5 || ys < -20.5 || ys > h+19.5){
          /* feather exhausted: keep-mode shows the previous frame exactly,
             crop-mode the fill value exactly */
          kept++;
          if(out2.data[p][x + y*out2.linesize[p]]
             != out1.data[p][x + y*out1.linesize[p]])
            keptBad++;
          cropped++;
          if(cropOut.data[p][x + y*cropOut.linesize[p]] != def)
            cropBad++;
        }
      }
    }
  }
  fprintf(stderr, "  uncovered pixels: %ld kept from previous frame (%ld bad), "
          "%ld cropped to fill (%ld bad)\n", kept, keptBad, cropped, cropBad);
  CHECK(kept > 0);
  CHECK(keptBad == 0);
  CHECK(cropBad == 0);
  vsFrameFree(&src1); vsFrameFree(&src2);
  vsFrameFree(&out1); vsFrameFree(&out2); vsFrameFree(&cropOut);
}

/* Every rejected call: VS_ERROR, and the destination (which is the
   keep-border cache's backing store in crop mode) is not touched. */
static void testRejection(void){
  VSFrameInfo fi;
  VSTransform good = new_transform(1.0, 1.0, 0.01, 0.0, 0,0,0);
  int i;
  fprintf(stderr, "--- invalid calls are rejected before writing anything ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV420P));

  /* bad endpoints */
  {
    VSTransform bad[11];
    int n = 0;
    bad[n] = good; bad[n].x        = NAN;       n++;
    bad[n] = good; bad[n].y        = -INFINITY; n++;
    bad[n] = good; bad[n].alpha    = INFINITY;  n++;
    bad[n] = good; bad[n].zoom     = NAN;       n++;
    bad[n] = good; bad[n].zoom     = 100.0;     n++;
    bad[n] = good; bad[n].zoom     = 250.0;     n++;
    bad[n] = good; bad[n].barrel   = 0.5;       n++;
    bad[n] = good; bad[n].rshutter = 0.5;       n++;
    bad[n] = good; bad[n].extra    = 1;         n++;
    bad[n] = good; bad[n].x        = 40000.0;   n++; /* beyond 16.16 range */
    bad[n] = good; bad[n].zoom     = -1.0e9;    n++;
    for(i=0; i<n; i++){
      VSFrame src, dest;
      VSTransformConfig conf = rowsConfig(VSCropBorder, VS_BiLinear);
      VSTransformData td;
      int unchanged, p, y;
      vsFrameAllocate(&src, &fi);
      vsFrameAllocate(&dest, &fi);
      fillGrid(&src, &fi, 0.0, 0.0);
      for(p=0; p<fi.planes; p++)
        memset(dest.data[p], 0x77, (size_t)dest.linesize[p]*planeH(&fi,p));
      CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
      CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
      /* bad endpoint in `top` for even i, in `bottom` for odd i */
      if(i & 1) CHECK(vsDoTransformRows(&td, good,  bad[i]) == VS_ERROR);
      else      CHECK(vsDoTransformRows(&td, bad[i], good ) == VS_ERROR);
      unchanged = 1;
      for(p=0; p<fi.planes && unchanged; p++)
        for(y=0; y<planeH(&fi,p) && unchanged; y++){
          int x;
          for(x=0; x<planeW(&fi,p); x++)
            if(dest.data[p][x + y*dest.linesize[p]] != 0x77){ unchanged = 0; break; }
        }
      CHECK(unchanged);
      vsTransformDataCleanup(&td);
      vsFrameFree(&src); vsFrameFree(&dest);
    }
    fprintf(stderr, "  %d bad-endpoint variants rejected, destination untouched\n",
            2*n);
  }

  /* unsupported configs */
  {
    VSTransformConfig conf;
    VSTransformData td;
    VSFrame src, dest;
    vsFrameAllocate(&src,  &fi);
    vsFrameAllocate(&dest, &fi);
    fillGrid(&src, &fi, 0.0, 0.0);

    conf = rowsConfig(VSCropBorder, VS_BiLinear);
    conf.lensCorrection = VSLensCorrectWobble;
    CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
    CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
    CHECK(vsDoTransformRows(&td, good, good) == VS_ERROR);
    vsTransformDataCleanup(&td);

    conf = rowsConfig(VSCropBorder, VS_BiLinear);
    conf.lensCorrection = VSLensCorrectFull;
    CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
    CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
    CHECK(vsDoTransformRows(&td, good, good) == VS_ERROR);
    vsTransformDataCleanup(&td);

    conf = rowsConfig(VSCropBorder, VS_BiLinear);
    conf.fov = 90.0;
    CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
    CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
    CHECK(vsDoTransformRows(&td, good, good) == VS_ERROR);
    vsTransformDataCleanup(&td);
    vsFrameFree(&src); vsFrameFree(&dest);
    fprintf(stderr, "  lensCorrection!=Off and fov!=0 rejected\n");
  }

  /* unsupported pixel formats */
  {
    static const VSPixelFormat badFmts[] = {
      PF_YUV410P, PF_YUV411P, PF_YUVA420P, PF_BGR24
    };
    size_t f;
    for(f=0; f<sizeof(badFmts)/sizeof(badFmts[0]); f++){
      VSFrameInfo fb;
      VSFrame src, dest;
      VSTransformConfig conf = rowsConfig(VSCropBorder, VS_BiLinear);
      VSTransformData td;
      CHECK(vsFrameInfoInit(&fb, 160, 120, badFmts[f]));
      vsFrameAllocate(&src, &fb);
      vsFrameAllocate(&dest, &fb);
      CHECK(vsTransformDataInit(&td, &conf, &fb, &fb) == VS_OK);
      CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
      CHECK(vsDoTransformRows(&td, good, good) == VS_ERROR);
      vsTransformDataCleanup(&td);
      vsFrameFree(&src); vsFrameFree(&dest);
    }
    fprintf(stderr, "  yuv410p, yuv411p, yuva420p, bgr24 rejected\n");
  }

  /* mismatched source/destination dimensions */
  {
    VSFrameInfo fiBig;
    VSFrame src, dest;
    VSTransformConfig conf = rowsConfig(VSCropBorder, VS_BiLinear);
    VSTransformData td;
    CHECK(vsFrameInfoInit(&fiBig, 320, 240, PF_YUV420P));
    vsFrameAllocate(&src,  &fi);
    vsFrameAllocate(&dest, &fiBig);
    CHECK(vsTransformDataInit(&td, &conf, &fi, &fiBig) == VS_OK);
    CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
    CHECK(vsDoTransformRows(&td, good, good) == VS_ERROR);
    vsTransformDataCleanup(&td);
    vsFrameFree(&src); vsFrameFree(&dest);
    fprintf(stderr, "  mismatched src/dest size rejected\n");
  }
}

/* A rejected call must not disturb the keep-border cache: the next legal
   frame still finds the previous stabilized frame in it. */
static void testCacheSurvivesRejection(void){
  VSFrameInfo fi;
  VSFrame src1, src2, out1, out2;
  VSTransform t1top = new_transform( 1.0,  1.0,  0.01, 0.0, 0,0,0);
  VSTransform t1bot = new_transform(-1.0, -1.0, -0.01, 0.0, 0,0,0);
  VSTransform t2top = new_transform( 30.0, 0.0,  0.0,  0.0, 0,0,0);
  VSTransform t2bot = new_transform(-30.0, 0.0,  0.0,  0.0, 0,0,0);
  VSTransform bad;
  VSTransformConfig conf;
  VSTransformData td;
  int p, y, x;
  long kept = 0, keptBad = 0;
  fprintf(stderr, "--- keep-border cache survives a rejected call ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV420P));
  vsFrameAllocate(&src1, &fi);
  vsFrameAllocate(&src2, &fi);
  vsFrameAllocate(&out1, &fi);
  vsFrameAllocate(&out2, &fi);
  fillGrid(&src1, &fi, 0.0, 0.0);
  fillGrid(&src2, &fi, 1.5, 0.5);
  bad = t1top;
  bad.x = NAN;

  conf = rowsConfig(VSKeepBorder, VS_BiLinear);
  CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
  CHECK(vsTransformPrepare(&td, &src1, &out1) == VS_OK);
  CHECK(vsDoTransformRows(&td, t1top, t1bot) == VS_OK);
  CHECK(vsTransformFinish(&td) == VS_OK);
  /* the rejected call: no finish, no writes */
  CHECK(vsTransformPrepare(&td, &src2, &out2) == VS_OK);
  CHECK(vsDoTransformRows(&td, bad, t1bot) == VS_ERROR);
  /* the next legal frame on the same pipeline */
  CHECK(vsTransformPrepare(&td, &src2, &out2) == VS_OK);
  CHECK(vsDoTransformRows(&td, t2top, t2bot) == VS_OK);
  CHECK(vsTransformFinish(&td) == VS_OK);
  vsTransformDataCleanup(&td);

  for(p=0; p<fi.planes; p++){
    int hsub = vsGetPlaneHeightSubS(&fi,p);
    int w = planeW(&fi,p), h = planeH(&fi,p);
    for(y=0; y<h; y++){
      VSTransform t = rrRefTransformAt(t2top, t2bot,
                                       rrRefRowU(fi.height, hsub, y));
      for(x=0; x<w; x++){
        double xs, ys;
        rrRefSrcCoord(&fi, p, x, y, &t, &xs, &ys);
        if(xs < -20.5 || xs > w+19.5 || ys < -20.5 || ys > h+19.5){
          kept++;
          if(out2.data[p][x + y*out2.linesize[p]]
             != out1.data[p][x + y*out1.linesize[p]])
            keptBad++;
        }
      }
    }
  }
  fprintf(stderr, "  %ld uncovered pixels still from frame 1 (%ld bad)\n",
          kept, keptBad);
  CHECK(kept > 0);
  CHECK(keptBad == 0);
  vsFrameFree(&src1); vsFrameFree(&src2);
  vsFrameFree(&out1); vsFrameFree(&out2);
}

/* A short sequence of frames through one pipeline, each with its own
   top/bottom pair: per-frame parameters must not leak into the next frame. */
static void testSequence(void){
  VSFrameInfo fi;
  VSTransformConfig conf;
  VSTransformData td;
  int f;
  fprintf(stderr, "--- short sequence, different row parameters per frame ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_YUV420P));
  conf = rowsConfig(VSCropBorder, VS_BiLinear);
  CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
  for(f=0; f<4; f++){
    VSFrame src, dest, ref;
    VSTransform top = new_transform( 2.0 + f,      -1.0 - 0.5*f,
                                     0.02*(f+1),    1.0*f, 0,0,0);
    VSTransform bot = new_transform(-2.0 - 0.5*f,   1.0 + f,
                                    -0.015*(f+1),  -0.5*f, 0,0,0);
    char what[64];
    vsFrameAllocate(&src,  &fi);
    vsFrameAllocate(&dest, &fi);
    vsFrameAllocate(&ref,  &fi);
    fillGrid(&src, &fi, 1.25*f, 0.75*f);
    CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
    CHECK(vsDoTransformRows(&td, top, bot) == VS_OK);
    CHECK(vsTransformFinish(&td) == VS_OK);
    rrRefWarp(&src, &ref, &fi, top, bot, 1);
    snprintf(what, sizeof(what), "sequence frame %d", f);
    compareVsRef(&dest, &ref, &fi, top, bot, what);
    vsFrameFree(&src); vsFrameFree(&dest); vsFrameFree(&ref);
  }
  vsTransformDataCleanup(&td);
}

/* Nearest-neighbour interpolation with integer per-row shifts: an exact
   per-row stepping check, run twice with different slopes on one pipeline so
   leftover row state would show.  Also a smoke run of the other
   interpolation types. */
static void testInterpolTypes(void){
  VSFrameInfo fi;
  VSFrame src, dest;
  VSTransformConfig conf;
  VSTransformData td;
  VSTransform top, bottom;
  int x, y, pass;
  fprintf(stderr, "--- interpolation types and exact per-row stepping ---\n");
  CHECK(vsFrameInfoInit(&fi, 160, 120, PF_GRAY8));
  vsFrameAllocate(&src,  &fi);
  vsFrameAllocate(&dest, &fi);
  fillGrid(&src, &fi, 0.0, 0.0);

  conf = rowsConfig(VSCropBorder, VS_Zero);
  CHECK(vsTransformDataInit(&td, &conf, &fi, &fi) == VS_OK);
  for(pass=0; pass<2; pass++){
    /* row y is shifted by exactly base+y px (pass 0: +4+y, pass 1: -2-y) */
    int base = pass == 0 ? 4 : -2;
    top    = new_transform((double)base, 0.0, 0.0, 0.0, 0,0,0);
    bottom = new_transform((double)(base + 119), 0.0, 0.0, 0.0, 0,0,0);
    CHECK(vsTransformPrepare(&td, &src, &dest) == VS_OK);
    CHECK(vsDoTransformRows(&td, top, bottom) == VS_OK);
    CHECK(vsTransformFinish(&td) == VS_OK);
    for(y=0; y<120; y++)
      for(x=0; x<160; x++){
        int sx = x - (base + y);
        int expect = (sx >= 0 && sx < 160) ? src.data[0][sx + y*src.linesize[0]] : 0;
        CHECK(dest.data[0][x + y*dest.linesize[0]] == expect);
      }
    fprintf(stderr, "  pass %d: row y shifted by exactly %+d px, all rows exact\n",
            pass, base);
  }
  vsTransformDataCleanup(&td);

  /* the other interpolation types run through the same entry */
  {
    static const VSInterpolType ips[] = {VS_Linear, VS_BiLinear, VS_BiCubic};
    VSFrame prev;
    int ip, differs = 0;
    vsFrameAllocate(&prev, &fi);
    memset(prev.data[0], 0, (size_t)prev.linesize[0]*fi.height);
    top    = new_transform( 3.3, -2.2,  0.05, 2.0, 0,0,0);
    bottom = new_transform(-2.7,  1.8, -0.04, 0.0, 0,0,0);
    for(ip=0; ip<3; ip++){
      runRows(&fi, &src, &dest, top, bottom, VSCropBorder, ips[ip]);
      if(!framesEqual(&dest, &prev, &fi)) differs++;
      vsFrameCopy(&prev, &dest, &fi);
    }
    /* each interpolation type produced a different picture: the configured
       interpolator really is used */
    CHECK(differs == 3);
    vsFrameFree(&prev);
  }
  vsFrameFree(&src); vsFrameFree(&dest);
}

int main(void){
  testIdenticalEndpoints();
  testEndpointRowsExact();
  testFormatsVsReference();
  testOppositeTranslation();
  testRotationAcrossPi();
  testZoomAndRotation();
  testSingleRow();
  testOddSizes();
  testPadding();
  testInPlaceAndOwnership();
  testBorderModes();
  testRejection();
  testCacheSurvivesRejection();
  testSequence();
  testInterpolTypes();

  if(failures){
    fprintf(stderr, "rolling rows: %d of %d checks FAILED\n", failures, checks);
    return 1;
  }
  fprintf(stderr, "all %d checks passed\n", checks);
  printf("vidstab rolling rows: ok\n");
  return 0;
}

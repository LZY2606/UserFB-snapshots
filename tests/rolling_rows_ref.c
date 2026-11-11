/* See rolling_rows_ref.h for what this module is and deliberately is not. */

#include "rolling_rows_ref.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double rrRefRowU(int lumaHeight, int hsub, int j){
  double ly = (j + 0.5)*(double)(1 << hsub) - 0.5;
  if (ly < 0.0) ly = 0.0;
  if (ly > lumaHeight - 1) ly = lumaHeight - 1;
  return lumaHeight > 1 ? ly/(lumaHeight - 1) : 0.5;
}

VSTransform rrRefTransformAt(VSTransform top, VSTransform bottom, double u){
  VSTransform t;
  double da = fmod(bottom.alpha - top.alpha, 2.0*M_PI);
  if (da > M_PI) da -= 2.0*M_PI;
  else if (da <= -M_PI) da += 2.0*M_PI;   /* exactly -pi goes the positive way */
  /* endpoint-exact at u=0 and u=1; see the production rowsTransformAt */
  t.x     = top.x    *(1.0-u) + bottom.x    *u;
  t.y     = top.y    *(1.0-u) + bottom.y    *u;
  t.zoom  = top.zoom *(1.0-u) + bottom.zoom *u;
  t.alpha = top.alpha*(1.0-u) + (top.alpha + da)*u;
  t.barrel = 0.0; t.rshutter = 0.0; t.extra = 0;
  return t;
}

void rrRefSrcCoord(const VSFrameInfo* fi, int plane, int x, int y,
                   const VSTransform* t, double* xs, double* ys){
  int wsub = vsGetPlaneWidthSubS(fi, plane);
  int hsub = vsGetPlaneHeightSubS(fi, plane);
  int w = CHROMA_SIZE(fi->width,  wsub);
  int h = CHROMA_SIZE(fi->height, hsub);
  double ax = (double)(1 << wsub), ay = (double)(1 << hsub);
  double dx = x - w/2, dy = y - h/2;      /* integer centre, as documented */
  double z  = 1.0 - t->zoom/100.0;
  double co = cos(t->alpha), si = sin(t->alpha);
  /* x_s = z*cos(a)*dx - z*sin(a)*(ay/ax)*dy + c_s_x - t.x/ax
     y_s = z*sin(a)*(ax/ay)*dx + z*cos(a)*dy + c_s_y - t.y/ay
     -- the similarity backward map, with the sine carried across the plane's
     axis scales so the angle is the luma-space angle. */
  *xs = z*(co*dx - si*(ay/ax)*dy) + w/2 - t->x/ax;
  *ys = z*(si*(ax/ay)*dx + co*dy) + h/2 - t->y/ay;
}

/* Bilinear sample of one plane at the real coordinate (xs,ys); samples
   outside the plane read as def. */
static double rrSample(const uint8_t* img, int linesize, int w, int h,
                       double xs, double ys, double def){
  int ix = (int)floor(xs), iy = (int)floor(ys);
  double fx = xs - ix, fy = ys - iy;
  double v00, v10, v01, v11;
  v00 = (ix >= 0 && ix < w && iy >= 0 && iy < h) ? img[ix   + iy*linesize]     : def;
  v10 = (ix+1 >= 0 && ix+1 < w && iy >= 0 && iy < h) ? img[ix+1 + iy*linesize] : def;
  v01 = (ix >= 0 && ix < w && iy+1 >= 0 && iy+1 < h) ? img[ix   + (iy+1)*linesize] : def;
  v11 = (ix+1 >= 0 && ix+1 < w && iy+1 >= 0 && iy+1 < h) ? img[ix+1 + (iy+1)*linesize] : def;
  return (v00*(1.0-fx) + v10*fx)*(1.0-fy) + (v01*(1.0-fx) + v11*fx)*fy;
}

static uint8_t rrToByte(double v){
  int r = (int)floor(v + 0.5);
  return r < 0 ? 0 : (r > 255 ? 255 : (uint8_t)r);
}

void rrRefWarp(const VSFrame* src, VSFrame* dest, const VSFrameInfo* fi,
               VSTransform top, VSTransform bottom, int crop){
  if(fi->pFormat < PF_PACKED){
    int p, x, y;
    for(p=0; p<fi->planes; p++){
      int wsub = vsGetPlaneWidthSubS(fi, p);
      int hsub = vsGetPlaneHeightSubS(fi, p);
      int w = CHROMA_SIZE(fi->width,  wsub);
      int h = CHROMA_SIZE(fi->height, hsub);
      double black = (p == 0) ? 0.0 : 128.0;
      for(y=0; y<h; y++){
        VSTransform t = rrRefTransformAt(top, bottom,
                                         rrRefRowU(fi->height, hsub, y));
        for(x=0; x<w; x++){
          double xs, ys, def;
          uint8_t* dp = &dest->data[p][x + y*dest->linesize[p]];
          rrRefSrcCoord(fi, p, x, y, &t, &xs, &ys);
          def = crop ? black : (double)*dp;
          *dp = rrToByte(rrSample(src->data[p], src->linesize[p], w, h,
                                  xs, ys, def));
        }
      }
    }
  }else{
    int n = fi->bytesPerPixel;
    int x, y, k;
    for(y=0; y<fi->height; y++){
      VSTransform t = rrRefTransformAt(top, bottom, rrRefRowU(fi->height, 0, y));
      for(x=0; x<fi->width; x++){
        double xs, ys;
        rrRefSrcCoord(fi, 0, x, y, &t, &xs, &ys);
        for(k=0; k<n; k++){
          /* one channel of one pixel: same coordinate, own sample */
          uint8_t* dp = &dest->data[0][x*n + k + y*dest->linesize[0]];
          double def = crop ? 16.0 : (double)*dp;
          int ix, iy;
          double fx, fy, v00, v10, v01, v11;
          const uint8_t* img = src->data[0] + k;
          int ls = src->linesize[0];
          ix = (int)floor(xs); iy = (int)floor(ys);
          fx = xs - ix; fy = ys - iy;
          v00 = (ix >= 0 && ix < fi->width && iy >= 0 && iy < fi->height)
              ? img[ix*n   + iy*ls]     : def;
          v10 = (ix+1 >= 0 && ix+1 < fi->width && iy >= 0 && iy < fi->height)
              ? img[(ix+1)*n + iy*ls]   : def;
          v01 = (ix >= 0 && ix < fi->width && iy+1 >= 0 && iy+1 < fi->height)
              ? img[ix*n   + (iy+1)*ls] : def;
          v11 = (ix+1 >= 0 && ix+1 < fi->width && iy+1 >= 0 && iy+1 < fi->height)
              ? img[(ix+1)*n + (iy+1)*ls] : def;
          *dp = rrToByte((v00*(1.0-fx) + v10*fx)*(1.0-fy)
                         + (v01*(1.0-fx) + v11*fx)*fy);
        }
      }
    }
  }
}

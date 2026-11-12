// Task tests for plugins/poisson_clone.hpp.
//
// Groups registered through register_task_tests(Registry&):
//   equations  - small cases checked against an independent dense solve.
//   mixed      - vector-norm guidance mixing vs per-channel mixing.
//   components - boundary-less components, pinning, disconnected regions.
//   scale      - a 256x256x3 irregular selection, residual and invariance
//                checks, plus a small PPM demo of the seamless paste.
#include "plugins/poisson_clone.hpp"
#include "task_support/registry.hpp"

#include <cmath>
#include <vector>

using namespace cimg_library;

namespace {

const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};

// Guidance selection modes for the dense reference solver.
enum GuidanceMode { mode_normal=0, mode_vector_mixed=1, mode_per_channel_mixed=2 };

// Independent dense reference: builds the full MxM system of the
// specification and solves it with Gaussian elimination. Only meant for
// small images.
CImg<double> reference_clone(const CImg<double>& dst, const CImg<double>& src,
                             const CImg<unsigned char>& mask, const int ox, const int oy,
                             const GuidanceMode mode, const bool tie_to_source=true) {
  const int W = dst.width(), H = dst.height(), C = dst.spectrum();
  CImg<double> out = dst;
  std::vector<int> uid((std::size_t)W*H,-1);
  int N = 0;
  cimg_forY(src,sy) cimg_forX(src,sx) if (mask(sx,sy)) {
    const int tx = sx + ox, ty = sy + oy;
    if (tx>=0 && tx<W && ty>=0 && ty<H) uid[(std::size_t)ty*W + tx] = N++;
  }
  if (!N) return out;
  std::vector<int> xs(N), ys(N);
  cimg_forXY(dst,x,y) {
    const int id = uid[(std::size_t)y*W + x];
    if (id>=0) { xs[id] = x; ys[id] = y; }
  }
  // Components and pinning of boundary-less components (first pixel in
  // destination row order).
  std::vector<char> pinned(N,0), visited(N,0);
  for (int i = 0; i<N; ++i) if (!visited[i]) {
    std::vector<int> comp(1,i);
    visited[i] = 1;
    bool boundary = false;
    for (std::size_t k = 0; k<comp.size(); ++k) {
      const int u = comp[k];
      for (int d = 0; d<4; ++d) {
        const int nx = xs[u] + DX[d], ny = ys[u] + DY[d];
        if (nx<0 || nx>=W || ny<0 || ny>=H) continue;
        const int v = uid[(std::size_t)ny*W + nx];
        if (v<0) boundary = true;
        else if (!visited[v]) { visited[v] = 1; comp.push_back(v); }
      }
    }
    if (!boundary) {
      int first = comp[0];
      for (std::size_t k = 1; k<comp.size(); ++k)
        if (ys[comp[k]]*W + xs[comp[k]]<ys[first]*W + xs[first]) first = comp[k];
      pinned[first] = 1;
    }
  }
  std::vector<int> fid(N,-1);
  int M = 0;
  for (int i = 0; i<N; ++i) if (!pinned[i]) fid[i] = M++;
  if (!M) return out;
  std::vector<double> A((std::size_t)M*M,0.0), B((std::size_t)M*C,0.0);
  for (int i = 0; i<N; ++i) {
    const int fi = fid[i];
    if (fi<0) continue;
    int deg = 0;
    for (int d = 0; d<4; ++d) {
      const int nx = xs[i] + DX[d], ny = ys[i] + DY[d];
      if (nx<0 || nx>=W || ny<0 || ny>=H) continue;
      ++deg;
      const int spx = xs[i] - ox, spy = ys[i] - oy, sqx = nx - ox, sqy = ny - oy;
      const bool q_in_src = sqx>=0 && sqx<src.width() && sqy>=0 && sqy<src.height();
      std::vector<double> sd(C,0.0), td(C,0.0), v(C,0.0);
      for (int c = 0; c<C; ++c) td[c] = dst(xs[i],ys[i],0,c) - dst(nx,ny,0,c);
      if (q_in_src) for (int c = 0; c<C; ++c) sd[c] = src(spx,spy,0,c) - src(sqx,sqy,0,c);
      if (!q_in_src) v = td;
      else if (mode==mode_normal) v = sd;
      else if (mode==mode_vector_mixed) {
        double s2 = 0, t2 = 0;
        for (int c = 0; c<C; ++c) { s2+=sd[c]*sd[c]; t2+=td[c]*td[c]; }
        v = (s2>t2 || (s2==t2 && tie_to_source))?sd:td;
      } else { // per-channel mixing (reference variant the plugin must NOT implement)
        for (int c = 0; c<C; ++c) v[c] = std::abs(sd[c])>=std::abs(td[c])?sd[c]:td[c];
      }
      const int u = uid[(std::size_t)ny*W + nx];
      const bool q_fixed = u<0 || pinned[u];
      for (int c = 0; c<C; ++c) {
        B[(std::size_t)fi*C + c]+=v[c];
        if (q_fixed) B[(std::size_t)fi*C + c]+=dst(nx,ny,0,c);
      }
      if (!q_fixed) A[(std::size_t)fi*M + fid[u]] -= 1.0;
    }
    A[(std::size_t)fi*M + fi] = (double)deg;
  }
  // Gaussian elimination with partial pivoting, all channels at once.
  for (int col = 0; col<M; ++col) {
    int piv = col;
    for (int r = col + 1; r<M; ++r)
      if (std::abs(A[(std::size_t)r*M + col])>std::abs(A[(std::size_t)piv*M + col])) piv = r;
    if (piv!=col) {
      for (int k = col; k<M; ++k) std::swap(A[(std::size_t)col*M + k],A[(std::size_t)piv*M + k]);
      for (int c = 0; c<C; ++c) std::swap(B[(std::size_t)col*C + c],B[(std::size_t)piv*C + c]);
    }
    const double d = A[(std::size_t)col*M + col];
    for (int r = col + 1; r<M; ++r) {
      const double f = A[(std::size_t)r*M + col]/d;
      if (!f) continue;
      for (int k = col; k<M; ++k) A[(std::size_t)r*M + k]-=f*A[(std::size_t)col*M + k];
      for (int c = 0; c<C; ++c) B[(std::size_t)r*C + c]-=f*B[(std::size_t)col*C + c];
    }
  }
  for (int col = M - 1; col>=0; --col) {
    for (int c = 0; c<C; ++c) {
      double s = B[(std::size_t)col*C + c];
      for (int k = col + 1; k<M; ++k) s-=A[(std::size_t)col*M + k]*B[(std::size_t)k*C + c];
      B[(std::size_t)col*C + c] = s/A[(std::size_t)col*M + col];
    }
  }
  for (int i = 0; i<N; ++i) if (fid[i]>=0)
    for (int c = 0; c<C; ++c) out(xs[i],ys[i],0,c) = B[(std::size_t)fid[i]*C + c];
  return out;
}

double max_abs_diff(const CImg<double>& a, const CImg<double>& b) {
  double m = 0;
  cimg_forXYZC(a,x,y,z,c) m = std::max(m,std::abs(a(x,y,z,c) - b(x,y,z,c)));
  return m;
}

// Largest absolute forward difference across the selection boundary.
double boundary_jump(const CImg<double>& img, const CImg<unsigned char>& in_region) {
  double m = 0;
  cimg_forXY(img,x,y) if (in_region(x,y)) {
    for (int d = 0; d<4; ++d) {
      const int nx = x + DX[d], ny = y + DY[d];
      if (nx<0 || nx>=img.width() || ny<0 || ny>=img.height() || in_region(nx,ny)) continue;
      cimg_forC(img,c) m = std::max(m,std::abs(img(x,y,0,c) - img(nx,ny,0,c)));
    }
  }
  return m;
}

void fill_wave(CImg<double>& img, const double phase) {
  cimg_forXYC(img,x,y,c)
    img(x,y,0,c) = 40 + 30*std::sin(0.31*x + phase + 0.7*c) + 25*std::cos(0.23*y - 0.4*c)
                   + 0.35*x + 0.2*y;
}

// ---------------------------------------------------------------- equations
void test_equations_hole(Case& cs) {
  CImg<double> dst(7,6,1,2), src(5,4,1,2);
  fill_wave(dst,0.0); fill_wave(src,1.3);
  CImg<unsigned char> mask(5,4,1,1,0);
  cimg_forXY(mask,x,y) mask(x,y) = 255;
  mask(2,1) = 0; mask(2,2) = 0; // hole inside the selection
  const CImg<double> dst0 = dst, src0 = src;
  const CImg<unsigned char> mask0 = mask;
  const PoissonCloneResult r = poisson_clone(dst,src,mask,1,1,false,1e-10,10000);
  const CImg<double> ref = reference_clone(dst,src,mask,1,1,mode_normal);
  cs.check(max_abs_diff(r.image,ref)<1e-7,"hole: result matches dense reference");
  cs.check(r.relative_residual<1e-8,"hole: reported residual is small");
  cs.check(r.iterations>0,"hole: solver performed iterations");
  // Unselected pixels are left untouched.
  bool untouched = true;
  cimg_forXYC(dst,x,y,c)
    if (!(x>=1 && x<6 && y>=1 && y<5 && mask(x - 1,y - 1)) && dst(x,y,0,c)!=r.image(x,y,0,c))
      untouched = false;
  cs.check(untouched,"hole: unselected pixels unchanged");
  cs.check(max_abs_diff(dst,dst0)==0 && max_abs_diff(src,src0)==0,"hole: inputs not modified");
  cs.check(mask==mask0,"hole: mask not modified");
}

void test_equations_negative_offset(Case& cs) {
  CImg<double> dst(6,5,1,1), src(4,4,1,1);
  fill_wave(dst,0.5); fill_wave(src,2.1);
  CImg<unsigned char> mask(4,4,1,1,0);
  cimg_forXY(mask,x,y) mask(x,y) = 255;
  mask(0,0) = 0; mask(3,3) = 0;
  const PoissonCloneResult r = poisson_clone(dst,src,mask,-2,-1,false,1e-10,10000);
  const CImg<double> ref = reference_clone(dst,src,mask,-2,-1,mode_normal);
  cs.check(max_abs_diff(r.image,ref)<1e-7,"negative offset: matches dense reference");
  cs.check(r.relative_residual<1e-8,"negative offset: residual small");
  // Pixels clipped outside the destination do not take part: column 0 and
  // row 0 only change where a selected source pixel actually lands.
  cs.check(r.image(5,4)==dst(5,4),"negative offset: far corner untouched");
}

void test_equations_no_overlap(Case& cs) {
  CImg<double> dst(5,4,1,3), src(3,3,1,3);
  fill_wave(dst,0.2); fill_wave(src,0.9);
  CImg<unsigned char> mask(3,3,1,1,255);
  PoissonCloneResult r = poisson_clone(dst,src,mask,100,0,false,1e-6,100);
  cs.check(max_abs_diff(r.image,dst)==0,"no overlap: result is a copy of dst");
  cs.check(r.iterations==0,"no overlap: zero iterations");
  cs.check(r.relative_residual==0,"no overlap: zero residual");
  CImg<unsigned char> empty(3,3,1,1,0);
  r = poisson_clone(dst,src,empty,0,0,false,1e-6,100);
  cs.check(max_abs_diff(r.image,dst)==0 && r.iterations==0 && r.relative_residual==0,
           "empty mask: copy of dst with zero stats");
}

void test_equations_arguments(Case& cs) {
  CImg<double> dst(4,4,1,1,1.0), src(4,4,1,1,2.0);
  CImg<unsigned char> mask(4,4,1,1,255);
  bool thrown = false;
  try { poisson_clone(dst,src,mask,0,0,false,0.0,10); } // tolerance not in (0,1)
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: tolerance 0 rejected");
  thrown = false;
  try { poisson_clone(dst,src,mask,0,0,false,1.0,10); }
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: tolerance 1 rejected");
  thrown = false;
  try { poisson_clone(dst,src,mask,0,0,false,1e-3,0); } // zero iteration cap
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: zero iteration cap rejected");
  thrown = false;
  CImg<double> bad = src; bad(1,1) = 1.0/0.0; // non-finite value
  try { poisson_clone(dst,bad,mask,0,0,false,1e-3,10); }
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: non-finite source rejected");
  thrown = false;
  CImg<double> badd = dst; badd(0,0) = 0.0/0.0;
  try { poisson_clone(badd,src,mask,0,0,false,1e-3,10); }
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: non-finite destination rejected");
  thrown = false;
  CImg<unsigned char> wrong_mask(3,4,1,1,255);
  try { poisson_clone(dst,src,wrong_mask,0,0,false,1e-3,10); }
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: mask size mismatch rejected");
  thrown = false;
  CImg<double> src3(4,4,1,3,2.0);
  try { poisson_clone(dst,src3,CImg<unsigned char>(4,4,1,1,255),0,0,false,1e-3,10); }
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: channel mismatch rejected");
  thrown = false;
  CImg<double> volume(4,4,2,1,1.0);
  try { poisson_clone(volume,src,mask,0,0,false,1e-3,10); }
  catch (const CImgArgumentException&) { thrown = true; }
  cs.check(thrown,"arguments: volumetric destination rejected");
}

void test_equations_iteration_limit(Case& cs) {
  CImg<double> dst(24,24,1,1), src(24,24,1,1);
  fill_wave(dst,0.0); fill_wave(src,1.0);
  CImg<unsigned char> mask(24,24,1,1,255);
  bool thrown = false;
  try { poisson_clone(dst,src,mask,0,0,false,1e-12,1); }
  catch (const CImgInstanceException&) { thrown = true; }
  cs.check(thrown,"iteration limit: CImgInstanceException when cap is reached");
}

// ---------------------------------------------------------------- mixed
void test_mixed_vector_vs_per_channel(Case& cs) {
  // Quadratic profiles (non-harmonic, so the guidance divergence does not
  // vanish). On the edge x->x+1 with s = 2x+1 the source difference is
  // (5s,1s) and the target difference is (4s,4s): per-channel picks would be
  // source for channel 0 (25>16) and target for channel 1 (1<16), while the
  // whole-vector norm (26s^2 vs 32s^2) picks the target for both channels.
  CImg<double> dst(5,5,1,2,0.0), src(5,5,1,2,0.0);
  cimg_forXY(dst,x,y) { dst(x,y,0,0) = 4.0*x*x; dst(x,y,0,1) = 4.0*x*x; }
  cimg_forXY(src,x,y) { src(x,y,0,0) = 5.0*x*x; src(x,y,0,1) = 1.0*x*x; }
  CImg<unsigned char> mask(5,5,1,1,0);
  cimg_forXY(mask,x,y) if (x>0 && x<4 && y>0 && y<4) mask(x,y) = 255;
  const PoissonCloneResult r = poisson_clone(dst,src,mask,0,0,true,1e-10,10000);
  const CImg<double> ref_vector = reference_clone(dst,src,mask,0,0,mode_vector_mixed);
  const CImg<double> ref_scalar = reference_clone(dst,src,mask,0,0,mode_per_channel_mixed);
  cs.check(max_abs_diff(r.image,ref_vector)<1e-7,"mixed: matches whole-vector reference");
  cs.check(max_abs_diff(ref_vector,ref_scalar)>1e-3,
           "mixed: vector and per-channel references actually differ");
  cs.check(max_abs_diff(r.image,ref_scalar)>1e-4,"mixed: result is not the per-channel mix");
}

void test_mixed_tie_prefers_source(Case& cs) {
  // On the edge x->x+1 with d = 2x+1 (exact in double precision) the source
  // difference is (5d,0) and the target difference is (3d,4d): the squared
  // norms are both exactly 25d^2, so ties must resolve to the source.
  CImg<double> dst(4,4,1,2,0.0), src(4,4,1,2,0.0);
  cimg_forXY(dst,x,y) { dst(x,y,0,0) = 3.0*x*x; dst(x,y,0,1) = 4.0*x*x; }
  cimg_forXY(src,x,y) { src(x,y,0,0) = 5.0*x*x; src(x,y,0,1) = 0.0; }
  CImg<unsigned char> mask(4,4,1,1,0);
  cimg_forXY(mask,x,y) if (x>0 && x<3 && y>0 && y<3) mask(x,y) = 255;
  const PoissonCloneResult r = poisson_clone(dst,src,mask,0,0,true,1e-10,10000);
  const CImg<double> ref_tie_src = reference_clone(dst,src,mask,0,0,mode_vector_mixed,true);
  const CImg<double> ref_tie_dst = reference_clone(dst,src,mask,0,0,mode_vector_mixed,false);
  cs.check(max_abs_diff(r.image,ref_tie_src)<1e-7,"mixed tie: source wins ties");
  cs.check(max_abs_diff(ref_tie_src,ref_tie_dst)>1e-3,"mixed tie: tie rule changes the result");
}

void test_mixed_edge_outside_source(Case& cs) {
  // Selection on the source border: edges whose far endpoint falls outside
  // the source image must use the target difference, in both modes.
  CImg<double> dst(6,6,1,2), src(3,3,1,2);
  fill_wave(dst,0.1); fill_wave(src,2.7);
  CImg<unsigned char> mask(3,3,1,1,255);
  const PoissonCloneResult rn = poisson_clone(dst,src,mask,2,2,false,1e-10,10000);
  const CImg<double> refn = reference_clone(dst,src,mask,2,2,mode_normal);
  cs.check(max_abs_diff(rn.image,refn)<1e-7,"outside edge: normal mode matches reference");
  const PoissonCloneResult rm = poisson_clone(dst,src,mask,2,2,true,1e-10,10000);
  const CImg<double> refm = reference_clone(dst,src,mask,2,2,mode_vector_mixed);
  cs.check(max_abs_diff(rm.image,refm)<1e-7,"outside edge: mixed mode matches reference");
}

// ---------------------------------------------------------------- components
void test_components_full_selection(Case& cs) {
  CImg<double> dst(5,4,1,2), src(5,4,1,2);
  fill_wave(dst,0.3); fill_wave(src,1.9);
  CImg<unsigned char> mask(5,4,1,1,255); // no fixed boundary anywhere
  const PoissonCloneResult r = poisson_clone(dst,src,mask,0,0,false,1e-10,10000);
  const CImg<double> ref = reference_clone(dst,src,mask,0,0,mode_normal);
  cs.check(max_abs_diff(r.image,ref)<1e-7,"full selection: matches dense reference");
  cs.check(r.image(0,0,0,0)==dst(0,0,0,0) && r.image(0,0,0,1)==dst(0,0,0,1),
           "full selection: row-first pixel pinned to dst");
  cs.check(r.relative_residual<1e-8,"full selection: residual small");
}

void test_components_disconnected(Case& cs) {
  CImg<double> dst(9,7,1,1), src(9,7,1,1);
  fill_wave(dst,0.8); fill_wave(src,2.2);
  CImg<unsigned char> mask(9,7,1,1,0);
  for (int y = 1; y<=2; ++y) for (int x = 1; x<=2; ++x) mask(x,y) = 255; // blob A
  for (int y = 4; y<=5; ++y) for (int x = 5; x<=7; ++x) mask(x,y) = 255; // blob B
  mask(6,4) = 0; // hole inside blob B
  mask(7,5) = 255;
  const PoissonCloneResult r = poisson_clone(dst,src,mask,0,0,false,1e-10,10000);
  const CImg<double> ref = reference_clone(dst,src,mask,0,0,mode_normal);
  cs.check(max_abs_diff(r.image,ref)<1e-7,"disconnected: matches dense reference");
  cs.check(r.image(4,3)==dst(4,3) && r.image(0,0)==dst(0,0),
           "disconnected: pixels between blobs unchanged");
}

void test_components_single_pixel(Case& cs) {
  CImg<double> dst(4,4,1,1), src(2,2,1,1);
  fill_wave(dst,0.4); fill_wave(src,1.1);
  CImg<unsigned char> mask(2,2,1,1,0);
  mask(1,0) = 255;
  const PoissonCloneResult r = poisson_clone(dst,src,mask,1,2,false,1e-10,1000);
  const CImg<double> ref = reference_clone(dst,src,mask,1,2,mode_normal);
  cs.check(max_abs_diff(r.image,ref)<1e-8,"single pixel: matches dense reference");
  // 1x1 destination fully selected: only a pinned pixel, nothing to solve.
  CImg<double> d1(1,1,1,1,7.0), s1(1,1,1,1,3.0);
  CImg<unsigned char> m1(1,1,1,1,255);
  const PoissonCloneResult r1 = poisson_clone(d1,s1,m1,0,0,false,1e-6,10);
  cs.check(r1.image(0,0)==7.0 && r1.iterations==0 && r1.relative_residual==0,
           "single pixel: 1x1 full selection stays at dst value");
}

void test_components_two_regions_one_image(Case& cs) {
  // Mask covering the whole destination through two disconnected source
  // regions placed side by side: one boundary-less component per region is
  // impossible (they would merge), so both regions keep their boundary and
  // no pinning may occur. Compare against the dense reference.
  CImg<double> dst(8,4,1,1), src(4,4,1,1);
  fill_wave(dst,1.4); fill_wave(src,0.6);
  CImg<unsigned char> mask(4,4,1,1,255);
  CImg<double> acc = dst;
  const PoissonCloneResult r1 = poisson_clone(acc,src,mask,0,0,false,1e-10,10000);
  const PoissonCloneResult r2 = poisson_clone(acc,src,mask,4,0,false,1e-10,10000);
  const CImg<double> ref1 = reference_clone(acc,src,mask,0,0,mode_normal);
  const CImg<double> ref2 = reference_clone(acc,src,mask,4,0,mode_normal);
  cs.check(max_abs_diff(r1.image,ref1)<1e-7,"two regions: left half matches reference");
  cs.check(max_abs_diff(r2.image,ref2)<1e-7,"two regions: right half matches reference");
  cs.check(r1.image(7,3)==acc(7,3) && r2.image(0,0)==acc(0,0),
           "two regions: opposite halves untouched");
}

// ---------------------------------------------------------------- scale
void irregular_mask(CImg<unsigned char>& mask) {
  mask.fill(0);
  const int W = mask.width(), H = mask.height();
  cimg_forXY(mask,x,y) {
    const double dx1 = x - 0.35*W, dy1 = y - 0.4*H, dx2 = x - 0.7*W, dy2 = y - 0.65*H;
    const bool disc1 = dx1*dx1 + dy1*dy1<0.09*W*H;
    const bool disc2 = dx2*dx2 + dy2*dy2<0.03*W*H;
    const bool bar = x>0.15*W && x<0.5*W && y>0.7*H && y<0.8*H;
    const bool hole = std::abs(x - 0.35*W)<0.03*W && std::abs(y - 0.4*H)<0.05*H;
    if ((disc1 || disc2 || bar) && !hole) mask(x,y) = 255;
  }
}

void test_scale_large(Case& cs) {
  const int W = 256, H = 256, C = 3;
  CImg<double> dst(W,H,1,C), src(W,H,1,C);
  fill_wave(dst,0.0); fill_wave(src,1.7);
  CImg<unsigned char> mask(W,H,1,1);
  irregular_mask(mask);
  const PoissonCloneResult r = poisson_clone(dst,src,mask,-13,9,false,1e-8,20000);
  cs.check(r.relative_residual<1e-6,"scale: reported residual meets tolerance");
  cs.check(r.iterations>0 && r.iterations<=20000,"scale: iteration count in range");
  // Unselected pixels must be exactly unchanged.
  CImg<unsigned char> region(W,H,1,1,0);
  cimg_forXY(src,x,y) if (mask(x,y)) {
    const int tx = x - 13, ty = y + 9;
    if (tx>=0 && tx<W && ty>=0 && ty<H) region(tx,ty) = 255;
  }
  bool untouched = true;
  cimg_forXYC(dst,x,y,c) if (!region(x,y) && r.image(x,y,0,c)!=dst(x,y,0,c)) untouched = false;
  cs.check(untouched,"scale: unselected pixels exactly unchanged");
  // Independent spot check of a free equation away from the boundary.
  bool found = false;
  cimg_forXY(dst,x,y) if (!found && region(x,y)) {
    bool interior = true;
    for (int d = 0; d<4; ++d)
      if (x+DX[d]<0 || x+DX[d]>=W || y+DY[d]<0 || y+DY[d]>=H || !region(x+DX[d],y+DY[d]))
        interior = false;
    if (interior) {
      found = true;
      const int sx = x + 13, sy = y - 9;
      cimg_forC(dst,c) {
        const double lhs = 4*r.image(x,y,0,c) - r.image(x+1,y,0,c) - r.image(x-1,y,0,c)
                           - r.image(x,y+1,0,c) - r.image(x,y-1,0,c);
        const double rhs = 4*src(sx,sy,0,c) - src(sx+1,sy,0,c) - src(sx-1,sy,0,c)
                           - src(sx,sy+1,0,c) - src(sx,sy-1,0,c);
        cs.check(std::abs(lhs - rhs)<1e-4,"scale: interior equation holds");
      }
    }
  }
  cs.check(found,"scale: found an interior pixel to check");
}

void test_scale_demo_ppm(Case& cs) {
  const int W = 160, H = 120, C = 3;
  CImg<double> dst(W,H,1,C), src(W,H,1,C);
  cimg_forXYC(dst,x,y,c) dst(x,y,0,c) = 30 + 0.8*x + 0.4*y + 20*c; // smooth bright ramp
  fill_wave(src,2.4);
  CImg<unsigned char> mask(W,H,1,1,0);
  cimg_forXY(mask,x,y) {
    const double dx = x - 0.5*W, dy = y - 0.5*H;
    if (dx*dx + dy*dy<0.12*W*H) mask(x,y) = 255;
  }
  const PoissonCloneResult r = poisson_clone(dst,src,mask,0,0,true,1e-8,20000);
  cs.check(r.relative_residual<1e-6,"demo: residual meets tolerance");
  // Naive masked paste for comparison.
  CImg<double> naive = dst;
  naive.draw_image(0,0,0,0,src,mask,1,255);
  CImg<unsigned char> region(W,H,1,1,0);
  cimg_forXY(mask,x,y) if (mask(x,y)) region(x,y) = 255;
  const double jump_naive = boundary_jump(naive,region);
  const double jump_clone = boundary_jump(r.image,region);
  cs.check(jump_clone<jump_naive,"demo: boundary seam reduced vs masked paste");
  CImg<unsigned char> vis = r.image.get_normalize(0,255);
  vis.save("task_tests/poisson_clone_demo.ppm");
  const CImg<unsigned char> back("task_tests/poisson_clone_demo.ppm");
  cs.check(back.width()==W && back.height()==H && back.spectrum()==3,
           "demo: PPM written and readable");
}

} // namespace

void register_task_tests(Registry& reg) {
  reg.add("equations","hole_and_boundary",test_equations_hole);
  reg.add("equations","negative_offset_clipping",test_equations_negative_offset);
  reg.add("equations","no_overlap",test_equations_no_overlap);
  reg.add("equations","invalid_arguments",test_equations_arguments);
  reg.add("equations","iteration_limit",test_equations_iteration_limit);
  reg.add("mixed","vector_vs_per_channel",test_mixed_vector_vs_per_channel);
  reg.add("mixed","tie_prefers_source",test_mixed_tie_prefers_source);
  reg.add("mixed","edge_outside_source",test_mixed_edge_outside_source);
  reg.add("components","full_selection_pinned",test_components_full_selection);
  reg.add("components","disconnected_blobs",test_components_disconnected);
  reg.add("components","single_pixel",test_components_single_pixel);
  reg.add("components","two_regions",test_components_two_regions_one_image);
  reg.add("scale","large_irregular_selection",test_scale_large);
  reg.add("scale","demo_ppm",test_scale_demo_ppm);
}

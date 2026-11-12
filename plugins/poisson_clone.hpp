/*
 #
 #  File        : poisson_clone.hpp
 #                ( C++ header file - CImg plug-in )
 #
 #  Description : Gradient-domain (Poisson) seamless cloning for 2d images.
 #
 #                This plug-in solves the Poisson equation with Dirichlet
 #                boundary conditions over a masked region, so that a source
 #                patch can be pasted into a destination image without the
 #                visible brightness seam left by a plain masked
 #                'draw_image()' paste.
 #
 #                poisson_clone(dst,src,mask,offset_x,offset_y,
 #                              mixed,tolerance,max_iterations)
 #
 #                - dst, src : 2d (depth 1) CImg<double> with the same,
 #                  non-zero number of channels. Values are treated as
 #                  linear floating-point intensities: no gamma conversion,
 #                  no clamping, no rounding is applied.
 #                - mask : single-channel CImg<unsigned char> with the same
 #                  width/height as src. Non-zero pixels are selected.
 #                - offset_x, offset_y : translation applied to the source
 #                  selection before it is composited onto dst.
 #                - mixed : when true, the guidance field of each edge is the
 #                  vector (over all channels) with the largest Euclidean
 #                  norm between the source and the target differences
 #                  (ties resolve to the source). When false, the source
 #                  difference is used, except for edges with one endpoint
 #                  outside the source image, where the target difference
 #                  is used instead.
 #                - tolerance : relative residual target in (0,1).
 #                - max_iterations : non-zero cap on the solver iterations.
 #
 #                The returned PoissonCloneResult contains the composited
 #                'image' (same size as dst), the 'iterations' count (maximum
 #                number of solver rounds actually executed over the
 #                channels) and the 'relative_residual' recomputed from the
 #                returned image. The inputs are never modified.
 #
 #                Failure behavior:
 #                - CImgArgumentException on non-finite input values, illegal
 #                  dimensions, a tolerance outside (0,1) or a zero iteration
 #                  cap.
 #                - CImgInstanceException when the iteration cap is reached
 #                  before the tolerance is met.
 #
 #  Licenses    : This file is 'dual-licensed', you have to choose one
 #                of the two licenses below to apply.
 #
 #                CeCILL-C
 #                The CeCILL-C license is close to the GNU LGPL.
 #                ( http://cecill.info/licences/Licence_CeCILL-C_V1-en.html )
 #
 #                or  CeCILL v2.1
 #                The CeCILL license is compatible with the GNU GPL.
 #                ( http://cecill.info/licences/Licence_CeCILL_V2.1-en.html )
 #
*/
#ifndef cimg_poisson_clone_hpp
#define cimg_poisson_clone_hpp

#include "CImg.h"

#include <cmath>
#include <vector>

namespace cimg_library {

  //! Result of a Poisson seamless cloning solve.
  struct PoissonCloneResult {
    CImg<double> image;        ///< Composited image, same size as the destination.
    unsigned iterations;       ///< Maximum number of solver rounds executed over the channels.
    double relative_residual;  ///< 2-norm of the free-equation residuals of the returned image,
                               ///< divided by max(1,2-norm of the right-hand side).
    PoissonCloneResult():iterations(0),relative_residual(0) {}
  };

  //! Reconstruct the selected region of \c dst in the gradient domain so that the
  //! details of \c src connect seamlessly to the boundary of \c dst.
  inline PoissonCloneResult poisson_clone(const CImg<double>& dst, const CImg<double>& src,
                                          const CImg<unsigned char>& mask,
                                          const int offset_x, const int offset_y,
                                          const bool mixed=false, const double tolerance=1e-6,
                                          const unsigned max_iterations=100000) {
    if (!dst._width || !dst._height || dst._depth!=1 || !dst._spectrum ||
        !src._width || !src._height || src._depth!=1 || src._spectrum!=dst._spectrum ||
        mask._width!=src._width || mask._height!=src._height ||
        mask._depth!=1 || mask._spectrum!=1)
      throw CImgArgumentException("poisson_clone(): Invalid image dimensions (images must be 2d, "
                                  "share a non-zero channel count, and the mask must be a "
                                  "single-channel image with the source width/height).");
    if (!(tolerance>0 && tolerance<1))
      throw CImgArgumentException("poisson_clone(): Tolerance %g is not in the open "
                                  "interval (0,1).",tolerance);
    if (!max_iterations)
      throw CImgArgumentException("poisson_clone(): Iteration limit is zero.");
    for (const double *ptrd = dst._data, *const ptre = ptrd + dst.size(); ptrd<ptre; ++ptrd)
      if (!std::isfinite(*ptrd))
        throw CImgArgumentException("poisson_clone(): Non-finite value in destination image.");
    for (const double *ptrs = src._data, *const ptre = ptrs + src.size(); ptrs<ptre; ++ptrs)
      if (!std::isfinite(*ptrs))
        throw CImgArgumentException("poisson_clone(): Non-finite value in source image.");

    const int W = dst._width, H = dst._height, C = dst._spectrum;
    const int dxs[4] = {1,-1,0,0}, dys[4] = {0,0,1,-1};

    PoissonCloneResult result;
    result.image = dst;

    // Selected source pixels landing inside the destination are the unknowns.
    std::vector<int> uid((std::size_t)W*H,-1);
    int N = 0;
    cimg_forY(src,sy) cimg_forX(src,sx) if (mask(sx,sy)) {
      const int tx = sx + offset_x, ty = sy + offset_y;
      if (tx>=0 && tx<W && ty>=0 && ty<H) uid[(std::size_t)ty*W + tx] = N++;
    }
    if (!N) return result; // Empty intersection: plain copy of the destination.

    std::vector<int> xs(N), ys(N);
    cimg_forXY(dst,x,y) {
      const int id = uid[(std::size_t)y*W + x];
      if (id>=0) { xs[id] = x; ys[id] = y; }
    }

    // Connected components of the selection, linked through in-destination
    // 4-neighbors. A component without any fixed (out-of-selection)
    // in-destination neighbor has no Dirichlet boundary: pin its first pixel
    // in destination row order to the original dst value.
    std::vector<char> pinned(N,0), visited(N,0);
    for (int i = 0; i<N; ++i) if (!visited[i]) {
      std::vector<int> comp(1,i);
      visited[i] = 1;
      bool has_boundary = false;
      for (std::size_t k = 0; k<comp.size(); ++k) {
        const int u = comp[k];
        for (int d = 0; d<4; ++d) {
          const int nx = xs[u] + dxs[d], ny = ys[u] + dys[d];
          if (nx<0 || nx>=W || ny<0 || ny>=H) continue;
          const int v = uid[(std::size_t)ny*W + nx];
          if (v<0) has_boundary = true;
          else if (!visited[v]) { visited[v] = 1; comp.push_back(v); }
        }
      }
      if (!has_boundary) {
        int first = comp[0];
        for (std::size_t k = 1; k<comp.size(); ++k)
          if (ys[comp[k]]*W + xs[comp[k]]<ys[first]*W + xs[first]) first = comp[k];
        pinned[first] = 1;
      }
    }

    // Free (non-pinned) unknowns.
    std::vector<int> fid(N,-1);
    int M = 0;
    for (int i = 0; i<N; ++i) if (!pinned[i]) fid[i] = M++;
    if (!M) return result; // Only pinned pixels: nothing left to solve.

    // Sparse adjacency (CSR) of the free unknowns, shared by every channel.
    std::vector<int> deg(N,0), row_ptr(M + 1,0);
    for (int i = 0; i<N; ++i) {
      for (int d = 0; d<4; ++d) {
        const int nx = xs[i] + dxs[d], ny = ys[i] + dys[d];
        if (nx<0 || nx>=W || ny<0 || ny>=H) continue;
        ++deg[i];
        if (fid[i]>=0) {
          const int v = uid[(std::size_t)ny*W + nx];
          if (v>=0 && fid[v]>=0) ++row_ptr[fid[i] + 1];
        }
      }
    }
    for (int i = 0; i<M; ++i) row_ptr[i + 1]+=row_ptr[i];
    std::vector<int> col_idx(row_ptr[M]);
    {
      std::vector<int> fill(row_ptr.begin(),row_ptr.end() - 1);
      for (int i = 0; i<N; ++i) if (fid[i]>=0) {
        for (int d = 0; d<4; ++d) {
          const int nx = xs[i] + dxs[d], ny = ys[i] + dys[d];
          if (nx<0 || nx>=W || ny<0 || ny>=H) continue;
          const int v = uid[(std::size_t)ny*W + nx];
          if (v>=0 && fid[v]>=0) col_idx[fill[fid[i]]++] = fid[v];
        }
      }
    }

    // Right-hand sides: sum of the guidance differences over each edge, plus
    // the values of the fixed neighbors (out-of-selection or pinned pixels,
    // always taken from the original dst, never from the updated result).
    std::vector<double> rhs((std::size_t)M*C,0.0);
    {
      std::vector<double> sdiff(C), tdiff(C);
      for (int i = 0; i<N; ++i) if (fid[i]>=0) {
        const int px = xs[i], py = ys[i], spx = px - offset_x, spy = py - offset_y;
        double *const bi = &rhs[(std::size_t)fid[i]*C];
        for (int d = 0; d<4; ++d) {
          const int nx = px + dxs[d], ny = py + dys[d];
          if (nx<0 || nx>=W || ny<0 || ny>=H) continue;
          const int sqx = nx - offset_x, sqy = ny - offset_y;
          const bool q_in_src = sqx>=0 && sqx<src._width && sqy>=0 && sqy<src._height;
          bool use_source = false;
          if (q_in_src) {
            double s2 = 0, t2 = 0;
            for (int c = 0; c<C; ++c) {
              sdiff[c] = src(spx,spy,0,c) - src(sqx,sqy,0,c);
              tdiff[c] = dst(px,py,0,c) - dst(nx,ny,0,c);
              s2+=sdiff[c]*sdiff[c]; t2+=tdiff[c]*tdiff[c];
            }
            use_source = !mixed || s2>=t2; // Ties resolve to the source.
          } else {
            for (int c = 0; c<C; ++c) tdiff[c] = dst(px,py,0,c) - dst(nx,ny,0,c);
          }
          const int v = uid[(std::size_t)ny*W + nx];
          const bool q_fixed = v<0 || pinned[v];
          for (int c = 0; c<C; ++c) {
            bi[c]+=use_source?sdiff[c]:tdiff[c];
            if (q_fixed) bi[c]+=dst(nx,ny,0,c);
          }
        }
      }
    }

    // Matrix-vector product with the shared Laplacian: (A*x)[i] = deg[i]*x[i] - sum(x[j]).
    std::vector<double> diag(M);
    for (int i = 0; i<N; ++i) if (fid[i]>=0) diag[fid[i]] = (double)deg[i];
    const auto apply_A = [&](const std::vector<double>& x, std::vector<double>& y) {
      for (int i = 0; i<M; ++i) {
        double s = diag[i]*x[i];
        for (int k = row_ptr[i]; k<row_ptr[i + 1]; ++k) s-=x[col_idx[k]];
        y[i] = s;
      }
    };

    // Preconditioned (Jacobi) conjugate gradient, one channel at a time.
    std::vector<int> free_of(M);
    for (int i = 0; i<N; ++i) if (fid[i]>=0) free_of[fid[i]] = i;
    std::vector<double> x(M), r(M), z(M), p(M), Ap(M);
    unsigned max_used = 0;
    for (int c = 0; c<C; ++c) {
      double b2 = 0;
      for (int i = 0; i<M; ++i) {
        x[i] = dst(xs[free_of[i]],ys[free_of[i]],0,c); // initial guess: original dst values
        const double bv = rhs[(std::size_t)i*C + c];
        b2+=bv*bv;
      }
      apply_A(x,Ap);
      double rz = 0, r2 = 0;
      for (int i = 0; i<M; ++i) {
        r[i] = rhs[(std::size_t)i*C + c] - Ap[i];
        z[i] = diag[i]>0?r[i]/diag[i]:r[i];
        p[i] = z[i];
        rz+=r[i]*z[i]; r2+=r[i]*r[i];
      }
      const double threshold = tolerance*(b2>1?std::sqrt(b2):1.0);
      unsigned iter = 0;
      while (r2>threshold*threshold) {
        if (iter>=max_iterations)
          throw CImgInstanceException("poisson_clone(): Iteration limit %u reached before the "
                                      "tolerance %g was met (channel %d).",max_iterations,tolerance,c);
        apply_A(p,Ap);
        double pAp = 0;
        for (int i = 0; i<M; ++i) pAp+=p[i]*Ap[i];
        const double alpha = pAp>0?rz/pAp:0;
        for (int i = 0; i<M; ++i) { x[i]+=alpha*p[i]; r[i]-=alpha*Ap[i]; }
        double rz_new = 0; r2 = 0;
        for (int i = 0; i<M; ++i) {
          z[i] = diag[i]>0?r[i]/diag[i]:r[i];
          rz_new+=r[i]*z[i]; r2+=r[i]*r[i];
        }
        const double beta = rz>0?rz_new/rz:0;
        for (int i = 0; i<M; ++i) p[i] = z[i] + beta*p[i];
        rz = rz_new;
        ++iter;
      }
      if (iter>max_used) max_used = iter;
      for (int i = 0; i<N; ++i) if (fid[i]>=0) result.image(xs[i],ys[i],0,c) = x[fid[i]];
    }
    result.iterations = max_used;

    // Recompute the residual of every free equation from the actual output image.
    double r2 = 0, b2 = 0;
    for (int i = 0; i<N; ++i) if (fid[i]>=0) {
      const int px = xs[i], py = ys[i];
      for (int c = 0; c<C; ++c) {
        double lhs = deg[i]*result.image(px,py,0,c);
        for (int d = 0; d<4; ++d) {
          const int nx = px + dxs[d], ny = py + dys[d];
          if (nx<0 || nx>=W || ny<0 || ny>=H) continue;
          const int v = uid[(std::size_t)ny*W + nx];
          if (v>=0 && fid[v]>=0) lhs-=result.image(nx,ny,0,c);
        }
        const double bv = rhs[(std::size_t)fid[i]*C + c], ri = lhs - bv;
        r2+=ri*ri; b2+=bv*bv;
      }
    }
    result.relative_residual = std::sqrt(r2)/(b2>1?std::sqrt(b2):1.0);
    return result;
  }

} // namespace cimg_library

#endif // cimg_poisson_clone_hpp

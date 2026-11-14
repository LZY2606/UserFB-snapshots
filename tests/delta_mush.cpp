// Tests for the delta-mush detail-preserving skinning (demo/delta_mush.h).
// The reference implementations here are deliberately independent of the
// production code: the diffusion matrix is computed with an explicit dense
// inverse, and the optimal rotation is found with Horn's quaternion method
// instead of the production SVD path.

#include "task_support/check.h"
#include "task_support/demo_api.h"
#include "task_support/iqm_fixture.h"
#include "demo/delta_mush.h"

#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <vector>

extern int numtris; // defined in demo.cpp, not part of demo_api.h

namespace
{
using V3 = std::array<double, 3>;
using M33 = std::array<std::array<double, 3>, 3>;
using M34 = std::array<std::array<double, 4>, 3>;

V3 v3(double x, double y, double z) { return {x, y, z}; }
V3 vadd(const V3 &a, const V3 &b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
V3 vsub(const V3 &a, const V3 &b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
V3 vscale(const V3 &a, double s) { return {a[0]*s, a[1]*s, a[2]*s}; }
V3 vcross(const V3 &a, const V3 &b)
{
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
V3 m33apply(const M33 &m, const V3 &v)
{
    V3 r{};
    for(int i = 0; i < 3; i++)
        for(int j = 0; j < 3; j++)
            r[i] += m[i][j]*v[j];
    return r;
}
V3 m34apply(const M34 &m, const V3 &v)
{
    V3 r{};
    for(int i = 0; i < 3; i++)
    {
        r[i] = m[i][3];
        for(int j = 0; j < 3; j++) r[i] += m[i][j]*v[j];
    }
    return r;
}

struct Mesh
{
    int nverts = 0, njoints = 0;
    std::vector<float> pos, nrm, tan; // 3, 3 and 4 floats per vertex
    std::vector<unsigned> tris;
    std::vector<uchar> idx, wgt;      // 4 bytes per vertex
};

// Jacobi eigendecomposition of a symmetric n x n matrix (row-major).
// Eigenvalues come out sorted descending, eigenvectors as columns of vec.
void jacobi(int n, std::vector<double> a, std::vector<double> &val, std::vector<double> &vec)
{
    vec.assign(n*n, 0.0);
    for(int i = 0; i < n; i++) vec[i*n+i] = 1.0;
    for(int sweep = 0; sweep < 300; sweep++)
    {
        int p = 0, q = 1;
        double best = 0;
        for(int i = 0; i < n; i++)
            for(int j = i+1; j < n; j++)
                if(std::fabs(a[i*n+j]) > best) { best = std::fabs(a[i*n+j]); p = i; q = j; }
        double scale = 0;
        for(int i = 0; i < n; i++) scale += std::fabs(a[i*n+i]);
        if(best <= 1e-14*scale) break;
        double app = a[p*n+p], aqq = a[q*n+q], apq = a[p*n+q];
        double tau = (aqq-app)/(2*apq);
        double t = (tau >= 0 ? 1.0 : -1.0)/(std::fabs(tau)+std::sqrt(1+tau*tau));
        double c = 1/std::sqrt(1+t*t), s = t*c;
        for(int k = 0; k < n; k++) { double akp = a[k*n+p], akq = a[k*n+q]; a[k*n+p] = c*akp-s*akq; a[k*n+q] = s*akp+c*akq; }
        for(int k = 0; k < n; k++) { double apk = a[p*n+k], aqk = a[q*n+k]; a[p*n+k] = c*apk-s*aqk; a[q*n+k] = s*apk+c*aqk; }
        for(int k = 0; k < n; k++) { double vkp = vec[k*n+p], vkq = vec[k*n+q]; vec[k*n+p] = c*vkp-s*vkq; vec[k*n+q] = s*vkp+c*vkq; }
    }
    std::vector<int> ord(n);
    for(int i = 0; i < n; i++) ord[i] = i;
    for(int i = 0; i < n; i++)
        for(int j = i+1; j < n; j++)
            if(a[ord[j]*n+ord[j]] > a[ord[i]*n+ord[i]]) std::swap(ord[i], ord[j]);
    val.resize(n);
    std::vector<double> v2(n*n);
    for(int i = 0; i < n; i++)
    {
        val[i] = a[ord[i]*n+ord[i]];
        for(int k = 0; k < n; k++) v2[k*n+i] = vec[k*n+ord[i]];
    }
    vec.swap(v2);
}

// Reference diffusion S = (I + lambda*L)^(-p) via an explicit dense inverse.
std::vector<double> ref_diffusion(const Mesh &m, double lambda, int p)
{
    int n = m.nverts;
    std::set<std::pair<int,int>> edges;
    for(size_t t = 0; t+2 < m.tris.size(); t += 3)
        for(int e = 0; e < 3; e++)
        {
            int a = (int)m.tris[t+e], b = (int)m.tris[t+(e+1)%3];
            if(a != b) edges.insert(std::minmax(a, b));
        }
    std::vector<double> A(n*n, 0.0), inv(n*n, 0.0);
    for(int i = 0; i < n; i++) { A[i*n+i] = 1.0; inv[i*n+i] = 1.0; }
    for(std::set<std::pair<int,int>>::const_iterator it = edges.begin(); it != edges.end(); ++it)
    {
        int a = it->first, b = it->second;
        A[a*n+a] += lambda; A[b*n+b] += lambda;
        A[a*n+b] -= lambda; A[b*n+a] -= lambda;
    }
    // Gauss-Jordan inversion with partial pivoting.
    for(int col = 0; col < n; col++)
    {
        int piv = col;
        for(int r = col+1; r < n; r++)
            if(std::fabs(A[r*n+col]) > std::fabs(A[piv*n+col])) piv = r;
        if(piv != col)
            for(int c = 0; c < n; c++)
            {
                std::swap(A[col*n+c], A[piv*n+c]);
                std::swap(inv[col*n+c], inv[piv*n+c]);
            }
        double d = A[col*n+col];
        for(int c = 0; c < n; c++) { A[col*n+c] /= d; inv[col*n+c] /= d; }
        for(int r = 0; r < n; r++)
        {
            if(r == col) continue;
            double f = A[r*n+col];
            if(!f) continue;
            for(int c = 0; c < n; c++)
            {
                A[r*n+c] -= f*A[col*n+c];
                inv[r*n+c] -= f*inv[col*n+c];
            }
        }
    }
    std::vector<double> S = inv, tmp(n*n);
    for(int step = 1; step < p; step++)
    {
        std::fill(tmp.begin(), tmp.end(), 0.0);
        for(int i = 0; i < n; i++)
            for(int k = 0; k < n; k++)
            {
                double s = S[i*n+k];
                if(!s) continue;
                for(int j = 0; j < n; j++) tmp[i*n+j] += s*inv[k*n+j];
            }
        S = tmp;
    }
    return S;
}

// Horn's quaternion method: rotation maximizing sum_k (R a_k).b_k for the
// cross-covariance B = sum_k a_k b_k^T.
M33 horn_fit(const double B[3][3])
{
    std::vector<double> K(16, 0.0);
    double tr = B[0][0]+B[1][1]+B[2][2];
    K[0] = tr;
    K[1] = K[4] = B[1][2]-B[2][1];
    K[2] = K[8] = B[2][0]-B[0][2];
    K[3] = K[12] = B[0][1]-B[1][0];
    for(int r = 0; r < 3; r++)
        for(int c = 0; c < 3; c++)
            K[(r+1)*4+(c+1)] = B[r][c]+B[c][r]-(r == c ? tr : 0.0);
    std::vector<double> val, vec;
    jacobi(4, K, val, vec);
    double w = vec[0], x = vec[4], y = vec[8], z = vec[12];
    double n = std::sqrt(w*w+x*x+y*y+z*z);
    w /= n; x /= n; y /= n; z /= n;
    M33 R;
    R[0] = {1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)};
    R[1] = {2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)};
    R[2] = {2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)};
    return R;
}

// Determinant of the unconstrained orthogonal fit (polar factor of B),
// used to prove that a test case really exercises the reflection branch.
double unconstrained_det(const double B[3][3])
{
    std::vector<double> BtB(9, 0.0);
    for(int r = 0; r < 3; r++)
        for(int c = 0; c < 3; c++)
            for(int k = 0; k < 3; k++)
                BtB[r*3+c] += B[k][r]*B[k][c];
    std::vector<double> val, vec;
    jacobi(3, BtB, val, vec);
    double U[3][3];
    for(int k = 0; k < 3; k++)
    {
        double s = std::sqrt(val[k]);
        for(int r = 0; r < 3; r++)
            U[r][k] = (B[r][0]*vec[0*3+k]+B[r][1]*vec[1*3+k]+B[r][2]*vec[2*3+k])/s;
    }
    double R[3][3];
    for(int r = 0; r < 3; r++)
        for(int c = 0; c < 3; c++)
            R[r][c] = vec[r*3+0]*U[c][0]+vec[r*3+1]*U[c][1]+vec[r*3+2]*U[c][2];
    return R[0][0]*(R[1][1]*R[2][2]-R[1][2]*R[2][1])
         - R[0][1]*(R[1][0]*R[2][2]-R[1][2]*R[2][0])
         + R[0][2]*(R[1][0]*R[2][1]-R[1][1]*R[2][0]);
}

bool sym_degenerate(const double C[3][3])
{
    std::vector<double> a(9);
    for(int r = 0; r < 3; r++)
        for(int c = 0; c < 3; c++)
            a[r*3+c] = C[r][c];
    std::vector<double> val, vec;
    jacobi(3, a, val, vec);
    return !(val[0] > 0) || val[2] <= 1e-9*val[0];
}

// One-sided Jacobi SVD (columns of B orthogonalized directly, so the
// smallest singular value stays resolved near machine precision).
void svd3(const double B[3][3], double s[3], double U[3][3], double V[3][3])
{
    double a[3][3];
    for(int i = 0; i < 3; i++)
        for(int j = 0; j < 3; j++)
        {
            a[i][j] = B[i][j];
            V[i][j] = i == j ? 1.0 : 0.0;
        }
    for(int sweep = 0; sweep < 200; sweep++)
    {
        bool rotated = false;
        for(int p = 0; p < 3; p++)
            for(int q = p+1; q < 3; q++)
            {
                double alpha = 0, beta = 0, gamma = 0;
                for(int r = 0; r < 3; r++)
                {
                    alpha += a[r][p]*a[r][p];
                    beta += a[r][q]*a[r][q];
                    gamma += a[r][p]*a[r][q];
                }
                if(!(alpha > 0) || !(beta > 0)) continue;
                if(std::fabs(gamma) <= 1e-16*std::sqrt(alpha*beta)) continue;
                double tau = (beta-alpha)/(2*gamma);
                double t = (tau >= 0 ? 1.0 : -1.0)/(std::fabs(tau)+std::sqrt(1+tau*tau));
                double c = 1/std::sqrt(1+t*t), sn = t*c;
                for(int r = 0; r < 3; r++)
                {
                    double ap = a[r][p], aq = a[r][q];
                    a[r][p] = c*ap-sn*aq;
                    a[r][q] = sn*ap+c*aq;
                }
                for(int r = 0; r < 3; r++)
                {
                    double vp = V[r][p], vq = V[r][q];
                    V[r][p] = c*vp-sn*vq;
                    V[r][q] = sn*vp+c*vq;
                }
                rotated = true;
            }
        if(!rotated) break;
    }
    for(int k = 0; k < 3; k++)
    {
        double n = 0;
        for(int r = 0; r < 3; r++) n += a[r][k]*a[r][k];
        s[k] = std::sqrt(n);
        for(int r = 0; r < 3; r++) U[r][k] = s[k] > 0 ? a[r][k]/s[k] : 0.0;
    }
    for(int i = 0; i < 2; i++)
        for(int j = i+1; j < 3; j++)
            if(s[j] > s[i])
            {
                std::swap(s[i], s[j]);
                for(int r = 0; r < 3; r++)
                {
                    std::swap(U[r][i], U[r][j]);
                    std::swap(V[r][i], V[r][j]);
                }
            }
}

bool cross_degenerate(const double B[3][3])
{
    double s[3], U[3][3], V[3][3];
    svd3(B, s, U, V);
    return !(s[0] > 0) || s[2] <= 1e-9*s[0];
}

M34 blended_matrix(const Mesh &m, const std::vector<M34> &pal, int v)
{
    M34 B{};
    for(int k = 0; k < 4; k++)
    {
        double w = m.wgt[4*v+k]/255.0;
        if(!w) continue;
        const M34 &P = pal[m.idx[4*v+k]];
        for(int r = 0; r < 3; r++)
            for(int c = 0; c < 4; c++)
                B[r][c] += w*P[r][c];
    }
    return B;
}

// Classic CPU skinning rules (blended matrix for position, adjoint-transpose
// for directions) on one vertex.
void legacy_attr(const Mesh &m, const M34 &B, int v, V3 &pos, V3 &nrm, V3 &tan, V3 &bit)
{
    V3 x = {m.pos[3*v], m.pos[3*v+1], m.pos[3*v+2]};
    V3 n = {m.nrm[3*v], m.nrm[3*v+1], m.nrm[3*v+2]};
    V3 t = {m.tan[4*v], m.tan[4*v+1], m.tan[4*v+2]};
    pos = m34apply(B, x);
    double cr[3][3];
    for(int r = 0; r < 3; r++)
    {
        int r0 = (r+1)%3, r1 = (r+2)%3;
        for(int c = 0; c < 3; c++)
        {
            int c0 = (c+1)%3, c1 = (c+2)%3;
            cr[r][c] = B[r0][c0]*B[r1][c1]-B[r0][c1]*B[r1][c0];
        }
    }
    for(int r = 0; r < 3; r++)
    {
        nrm[r] = cr[r][0]*n[0]+cr[r][1]*n[1]+cr[r][2]*n[2];
        tan[r] = cr[r][0]*t[0]+cr[r][1]*t[1]+cr[r][2]*t[2];
    }
    bit = vscale(vcross(nrm, tan), m.tan[4*v+3]);
}

struct RefOut
{
    std::vector<V3> pos, nrm, tan, bit;
    int fallbacks = 0;
};

// Reference delta-mush evaluation: direct per-vertex neighbourhood fit.
RefOut ref_eval(const Mesh &m, const std::vector<double> &S, const std::vector<M34> &pal)
{
    int n = m.nverts;
    std::vector<V3> x(n), y(n);
    for(int k = 0; k < n; k++)
    {
        x[k] = {m.pos[3*k], m.pos[3*k+1], m.pos[3*k+2]};
        y[k] = m34apply(blended_matrix(m, pal, k), x[k]);
    }
    RefOut out;
    out.pos.resize(n); out.nrm.resize(n); out.tan.resize(n); out.bit.resize(n);
    for(int i = 0; i < n; i++)
    {
        const double *sr = &S[i*n];
        double a = 0;
        V3 xb{}, yb{};
        for(int k = 0; k < n; k++)
        {
            a += sr[k];
            xb = vadd(xb, vscale(x[k], sr[k]));
            yb = vadd(yb, vscale(y[k], sr[k]));
        }
        bool deg = !(a > 0);
        V3 xbar{}, ybar{};
        double B[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
        if(!deg)
        {
            xbar = vscale(xb, 1/a);
            ybar = vscale(yb, 1/a);
            double C[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
            for(int k = 0; k < n; k++)
            {
                V3 d = vsub(x[k], xbar);
                for(int r = 0; r < 3; r++)
                    for(int c = 0; c < 3; c++)
                        C[r][c] += sr[k]*d[r]*d[c];
            }
            if(sym_degenerate(C)) deg = true;
        }
        if(!deg)
        {
            for(int k = 0; k < n; k++)
            {
                V3 dx = vsub(x[k], xbar), dy = vsub(y[k], ybar);
                for(int r = 0; r < 3; r++)
                    for(int c = 0; c < 3; c++)
                        B[r][c] += sr[k]*dx[r]*dy[c];
            }
            if(cross_degenerate(B)) deg = true;
        }
        if(deg)
        {
            out.fallbacks++;
            legacy_attr(m, blended_matrix(m, pal, i), i, out.pos[i], out.nrm[i], out.tan[i], out.bit[i]);
        }
        else
        {
            M33 R = horn_fit(B);
            V3 t = vsub(ybar, m33apply(R, xbar));
            out.pos[i] = vadd(m33apply(R, x[i]), t);
            V3 nn = {m.nrm[3*i], m.nrm[3*i+1], m.nrm[3*i+2]};
            V3 tt = {m.tan[4*i], m.tan[4*i+1], m.tan[4*i+2]};
            out.nrm[i] = m33apply(R, nn);
            out.tan[i] = m33apply(R, tt);
            out.bit[i] = vscale(vcross(out.nrm[i], out.tan[i]), m.tan[4*i+3]);
        }
    }
    return out;
}

Matrix3x4 to_geom(const M34 &m)
{
    return Matrix3x4(Vec4((float)m[0][0], (float)m[0][1], (float)m[0][2], (float)m[0][3]),
                     Vec4((float)m[1][0], (float)m[1][1], (float)m[1][2], (float)m[1][3]),
                     Vec4((float)m[2][0], (float)m[2][1], (float)m[2][2], (float)m[2][3]));
}

std::vector<Matrix3x4> to_geom(const std::vector<M34> &pal)
{
    std::vector<Matrix3x4> r(pal.size());
    for(size_t j = 0; j < pal.size(); j++) r[j] = to_geom(pal[j]);
    return r;
}

M33 rot_axis(double angle, double ax, double ay, double az)
{
    double n = std::sqrt(ax*ax+ay*ay+az*az);
    double s = std::sin(angle/2)/n, w = std::cos(angle/2);
    double x = ax*s, y = ay*s, z = az*s;
    M33 R;
    R[0] = {1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)};
    R[1] = {2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)};
    R[2] = {2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)};
    return R;
}

M34 make_affine(const M33 &R, double sx, double sy, double sz, const V3 &t)
{
    M34 m{};
    for(int r = 0; r < 3; r++)
    {
        m[r][0] = R[r][0]*sx; m[r][1] = R[r][1]*sy; m[r][2] = R[r][2]*sz;
        m[r][3] = t[r];
    }
    return m;
}

M34 make_rigid(const M33 &R, const V3 &t)
{
    return make_affine(R, 1, 1, 1, t);
}

bool close3(const float *got, const V3 &want, double tol = 2e-5)
{
    for(int c = 0; c < 3; c++)
        if(!std::isfinite(got[c]) || std::fabs(got[c]-want[c]) > tol) return false;
    return true;
}

bool check_vertex(const DeltaMushOutput &out, const RefOut &ref, int v, double tol = 2e-5)
{
    return close3(&out.position[3*v], ref.pos[v], tol)
        && close3(&out.normal[3*v], ref.nrm[v], tol)
        && close3(&out.tangent[3*v], ref.tan[v], tol)
        && close3(&out.bitangent[3*v], ref.bit[v], tol);
}

bool check_all(const DeltaMushOutput &out, const RefOut &ref, int n, double tol = 2e-5)
{
    for(int v = 0; v < n; v++)
        if(!check_vertex(out, ref, v, tol)) return false;
    return true;
}

DeltaMushBinding *bind_mesh(const Mesh &m, double lambda, int p)
{
    return deltamush_bind(m.pos.data(), m.nrm.data(), m.tan.data(),
                          m.tris.data(), (int)m.tris.size()/3,
                          m.idx.data(), m.wgt.data(), m.nverts, m.njoints, lambda, p);
}

Mesh make_tube()
{
    Mesh m;
    m.njoints = 3;
    const int rings = 4, seg = 6;
    const double pi = 3.14159265358979323846;
    for(int r = 0; r < rings; r++)
    {
        double cx = 0.4*r, cy = 0.12*r*r, cz = 0.05*r;
        double rad = 0.22+0.02*r;
        for(int k = 0; k < seg; k++)
        {
            double a = 2*pi*k/seg + 0.17*r;
            m.pos.push_back((float)(cx + rad*std::cos(a)));
            m.pos.push_back((float)(cy + 0.6*rad*std::sin(a)));
            m.pos.push_back((float)(cz + 0.8*rad*std::sin(a)));
            m.nrm.push_back((float)std::cos(a));
            m.nrm.push_back((float)(0.6*std::sin(a)));
            m.nrm.push_back((float)(0.8*std::sin(a)));
            m.tan.push_back((float)(-std::sin(a)));
            m.tan.push_back(0.3f);
            m.tan.push_back((float)std::cos(a));
            m.tan.push_back(k%2 ? 1.0f : -1.0f);
            uchar id[4] = {0,0,0,0}, wt[4] = {0,0,0,0};
            switch(r)
            {
            case 0: id[0] = 0; wt[0] = 255; break;
            case 1: id[0] = 0; id[1] = 1; wt[0] = (uchar)(190-3*k); wt[1] = (uchar)(65+3*k); break;
            case 2: id[0] = 1; id[1] = 2; wt[0] = (uchar)(150-3*k); wt[1] = (uchar)(105+3*k); break;
            default: id[0] = 2; wt[0] = 255; break;
            }
            for(int q = 0; q < 4; q++) { m.idx.push_back(id[q]); m.wgt.push_back(wt[q]); }
        }
    }
    for(int r = 0; r+1 < rings; r++)
        for(int k = 0; k < seg; k++)
        {
            int k2 = (k+1)%seg;
            unsigned a = r*seg+k, b = (r+1)*seg+k, c = (r+1)*seg+k2, d = r*seg+k2;
            m.tris.push_back(a); m.tris.push_back(b); m.tris.push_back(c);
            m.tris.push_back(a); m.tris.push_back(c); m.tris.push_back(d);
        }
    m.nverts = rings*seg;
    return m;
}

Mesh make_tetra_isolated()
{
    Mesh m;
    m.nverts = 5;
    m.njoints = 2;
    const float pos[15] = {-.3f,.2f,-.4f, 1.2f,-.4f,.1f, .1f,1.1f,.2f, .2f,.3f,1.4f, 2.5f,1.5f,-.5f};
    const float nrm[15] = {.2f,.6f,1.f, -.3f,1.f,.2f, 1.f,.4f,-.2f, .1f,-.6f,1.f, 0.f,.5f,.8f};
    const float tan[20] = {1.f,.1f,-.2f,1.f, .7f,-.2f,.4f,-1.f, -.2f,.8f,.4f,1.f, 1.f,.3f,.2f,-1.f, .5f,.5f,0.f,1.f};
    const unsigned tr[12] = {0,2,1, 0,1,3, 1,2,3, 2,0,3};
    const uchar id[20] = {0,0,0,0, 0,1,0,0, 1,0,0,0, 1,0,0,0, 0,0,0,0};
    const uchar wt[20] = {255,0,0,0, 153,102,0,0, 191,64,0,0, 255,0,0,0, 255,0,0,0};
    m.pos.assign(pos, pos+15);
    m.nrm.assign(nrm, nrm+15);
    m.tan.assign(tan, tan+20);
    m.tris.assign(tr, tr+12);
    m.idx.assign(id, id+20);
    m.wgt.assign(wt, wt+20);
    return m;
}

Mesh make_planar()
{
    Mesh m;
    m.nverts = 9;
    m.njoints = 2;
    for(int i = 0; i < 3; i++)
        for(int j = 0; j < 3; j++)
        {
            m.pos.push_back(0.5f*i-0.4f);
            m.pos.push_back(0.5f*j-0.3f);
            m.pos.push_back(0.0f);
            m.nrm.push_back(0.1f*i);
            m.nrm.push_back(-0.1f*j);
            m.nrm.push_back(1.0f);
            m.tan.push_back(1.0f);
            m.tan.push_back(0.05f*i);
            m.tan.push_back(0.0f);
            m.tan.push_back((i*3+j)%2 ? 1.0f : -1.0f);
            if((i*3+j)%2)
            {
                const uchar id[4] = {0,1,0,0}, wt[4] = {128,127,0,0};
                for(int q = 0; q < 4; q++) { m.idx.push_back(id[q]); m.wgt.push_back(wt[q]); }
            }
            else
            {
                const uchar id[4] = {0,0,0,0}, wt[4] = {255,0,0,0};
                for(int q = 0; q < 4; q++) { m.idx.push_back(id[q]); m.wgt.push_back(wt[q]); }
            }
        }
    for(int i = 0; i < 2; i++)
        for(int j = 0; j < 2; j++)
        {
            unsigned v00 = i*3+j, v10 = (i+1)*3+j, v01 = i*3+j+1, v11 = (i+1)*3+j+1;
            m.tris.push_back(v00); m.tris.push_back(v10); m.tris.push_back(v11);
            m.tris.push_back(v00); m.tris.push_back(v11); m.tris.push_back(v01);
        }
    return m;
}

Mesh make_components()
{
    Mesh m;
    m.nverts = 11;
    m.njoints = 1;
    const float pos[33] = {
        0,0,0, 1,0.2f,0, 0.3f,1,0,       // triangle A
        3,0,1, 4,0.5f,1, 3.5f,1,2,       // triangle B
        9,9,9,                           // isolated
        0,0,0,                           // isolated, coincident with vertex 0
        5,5,5, 5,5,5, 6,5.5f,5           // triangle C, first two coincident
    };
    m.pos.assign(pos, pos+33);
    for(int i = 0; i < 11; i++)
    {
        m.nrm.push_back(0); m.nrm.push_back(0); m.nrm.push_back(1);
        m.tan.push_back(1); m.tan.push_back(0); m.tan.push_back(0); m.tan.push_back(1);
        m.idx.push_back(0); m.idx.push_back(0); m.idx.push_back(0); m.idx.push_back(0);
        m.wgt.push_back(255); m.wgt.push_back(0); m.wgt.push_back(0); m.wgt.push_back(0);
    }
    const unsigned tr[9] = {0,1,2, 3,4,5, 8,9,10};
    m.tris.assign(tr, tr+9);
    return m;
}

bool row_matches(DeltaMushBinding *b, const std::vector<double> &S, int i, double tol)
{
    int n = b ? deltamush_numverts(b) : 0;
    std::vector<double> row(n);
    if(!n || !deltamush_diffusionrow(b, i, row.data())) return false;
    for(int k = 0; k < n; k++)
        if(std::fabs(row[k]-S[i*n+k]) > tol) return false;
    return true;
}

// ---------------------------------------------------------------- diffusion

int case_diffusion_components()
{
    Mesh m = make_components();
    const double lp[4][2] = {{0.1,1},{1,2},{10,5},{0.5,3}};
    for(int c = 0; c < 4; c++)
    {
        double lambda = lp[c][0];
        int p = (int)lp[c][1];
        DeltaMushBinding *b = bind_mesh(m, lambda, p);
        TASK_CHECK(b != 0);
        TASK_CHECK(deltamush_numverts(b) == 11);
        TASK_CHECK(deltamush_numjoints(b) == 1);
        // One joint used everywhere: exactly one moment per vertex.
        TASK_CHECK(deltamush_momentcount(b) == 11);
        std::vector<double> S = ref_diffusion(m, lambda, p);
        bool ok = true;
        for(int i = 0; i < 11; i++) ok = ok && row_matches(b, S, i, 1e-9);
        TASK_CHECK(ok);
        // Disconnected components do not bleed into each other, and the
        // isolated vertices keep identity rows.
        std::vector<double> row(11);
        TASK_CHECK(deltamush_diffusionrow(b, 6, row.data()));
        TASK_CHECK(row[6] == 1.0);
        for(int k = 0; k < 11; k++) TASK_CHECK(k == 6 || row[k] == 0.0);
        // Vertex 7 shares its position with vertex 0 but is not merged:
        // vertex 0 diffuses into its triangle, vertex 7 stays isolated.
        TASK_CHECK(deltamush_diffusionrow(b, 0, row.data()));
        TASK_CHECK(row[0] < 1.0 && row[1] > 0.0 && row[2] > 0.0 && row[3] == 0.0);
        TASK_CHECK(deltamush_diffusionrow(b, 7, row.data()));
        TASK_CHECK(row[7] == 1.0 && row[0] == 0.0);
        // Symmetry of S across the coincident pair 8,9 of triangle C.
        std::vector<double> row8(11), row9(11);
        TASK_CHECK(deltamush_diffusionrow(b, 8, row8.data()));
        TASK_CHECK(deltamush_diffusionrow(b, 9, row9.data()));
        TASK_CHECK(std::fabs(row8[9]-row9[8]) < 1e-12);
        TASK_CHECK(row8[10] > 0.0 && row8[0] == 0.0);
        // Every neighbourhood here is degenerate (planar or worse).
        TASK_CHECK(deltamush_degeneratecount(b) == 11);
        deltamush_free(b);
    }
    return 0;
}

int case_diffusion_tube()
{
    Mesh m = make_tube();
    const double lp[2][2] = {{0.25,3},{2.0,1}};
    for(int c = 0; c < 2; c++)
    {
        double lambda = lp[c][0];
        int p = (int)lp[c][1];
        DeltaMushBinding *b = bind_mesh(m, lambda, p);
        TASK_CHECK(b != 0);
        // Connected mesh using all three joints: 3 moments per vertex.
        TASK_CHECK(deltamush_momentcount(b) == 72);
        TASK_CHECK(deltamush_degeneratecount(b) == 0);
        std::vector<double> S = ref_diffusion(m, lambda, p);
        bool ok = true;
        for(int i = 0; i < m.nverts; i++) ok = ok && row_matches(b, S, i, 1e-9);
        TASK_CHECK(ok);
        // Spot-check symmetry S_ik = S_ki.
        std::vector<double> r3(m.nverts), r11(m.nverts);
        TASK_CHECK(deltamush_diffusionrow(b, 3, r3.data()));
        TASK_CHECK(deltamush_diffusionrow(b, 11, r11.data()));
        TASK_CHECK(std::fabs(r3[11]-r11[3]) < 1e-12);
        deltamush_free(b);
    }
    return 0;
}

// ---------------------------------------------------------------------- fit

int case_fit_rigid()
{
    Mesh m = make_tube();
    DeltaMushBinding *b = bind_mesh(m, 0.7, 2);
    TASK_CHECK(b != 0);
    TASK_CHECK(deltamush_degeneratecount(b) == 0);
    DeltaMushOutput out;
    size_t acc = 0;
    int fb = -1;

    // Identity palette reproduces the bind pose exactly.
    std::vector<M34> id(3, make_rigid(M33{{{1,0,0},{0,1,0},{0,0,1}}}, v3(0,0,0)));
    std::vector<Matrix3x4> gid = to_geom(id);
    TASK_CHECK(deltamush_evaluate(b, gid.data(), &out, &acc, &fb));
    TASK_CHECK(acc == deltamush_momentcount(b));
    TASK_CHECK(fb == 0);
    bool ok = true;
    for(int v = 0; v < m.nverts; v++)
    {
        V3 n = {m.nrm[3*v], m.nrm[3*v+1], m.nrm[3*v+2]};
        V3 t = {m.tan[4*v], m.tan[4*v+1], m.tan[4*v+2]};
        V3 x = {m.pos[3*v], m.pos[3*v+1], m.pos[3*v+2]};
        ok = ok && close3(&out.position[3*v], x)
                && close3(&out.normal[3*v], n)
                && close3(&out.tangent[3*v], t)
                && close3(&out.bitangent[3*v], vscale(vcross(n, t), m.tan[4*v+3]));
    }
    TASK_CHECK(ok);

    // A uniform rigid motion is reproduced exactly on every attribute.
    M33 R = rot_axis(0.7, 1, 2, 3);
    V3 t = v3(0.5, -1, 2);
    std::vector<M34> pal(3, make_rigid(R, t));
    std::vector<Matrix3x4> gpal = to_geom(pal);
    TASK_CHECK(deltamush_evaluate(b, gpal.data(), &out, &acc, &fb));
    TASK_CHECK(acc == deltamush_momentcount(b));
    TASK_CHECK(fb == 0);
    ok = true;
    for(int v = 0; v < m.nverts; v++)
    {
        V3 x = {m.pos[3*v], m.pos[3*v+1], m.pos[3*v+2]};
        V3 n = {m.nrm[3*v], m.nrm[3*v+1], m.nrm[3*v+2]};
        V3 tg = {m.tan[4*v], m.tan[4*v+1], m.tan[4*v+2]};
        V3 rn = m33apply(R, n), rt = m33apply(R, tg);
        ok = ok && close3(&out.position[3*v], vadd(m33apply(R, x), t))
                && close3(&out.normal[3*v], rn)
                && close3(&out.tangent[3*v], rt)
                && close3(&out.bitangent[3*v], vscale(vcross(rn, rt), m.tan[4*v+3]));
    }
    TASK_CHECK(ok);
    deltamush_free(b);
    return 0;
}

int case_fit_reference()
{
    Mesh m = make_tube();
    double lambda = 0.45;
    int p = 3;
    DeltaMushBinding *b = bind_mesh(m, lambda, p);
    TASK_CHECK(b != 0);
    std::vector<double> S = ref_diffusion(m, lambda, p);
    std::vector<M34> pal(3);
    pal[0] = make_affine(rot_axis(0.4, 0, 0, 1), 1.1, 0.95, 1.0, v3(0.2, 0.1, -0.3));
    pal[1] = make_affine(rot_axis(-0.6, 0.3, 1, 0.2), 0.9, 1.05, 1.2, v3(-0.4, 0.3, 0.1));
    pal[2] = make_affine(rot_axis(0.9, 1, -0.2, 0.4), 1.0, 0.85, 1.1, v3(0.1, -0.2, 0.4));
    std::vector<Matrix3x4> gpal = to_geom(pal);
    DeltaMushOutput out;
    size_t acc = 0;
    int fb = -1;
    TASK_CHECK(deltamush_evaluate(b, gpal.data(), &out, &acc, &fb));
    TASK_CHECK(acc == deltamush_momentcount(b));
    TASK_CHECK(fb == 0);
    RefOut ref = ref_eval(m, S, pal);
    TASK_CHECK(ref.fallbacks == 0);
    TASK_CHECK(check_all(out, ref, m.nverts));
    deltamush_free(b);
    return 0;
}

int case_fit_reflection()
{
    Mesh m = make_tetra_isolated();
    double lambda = 1.0;
    int p = 2;
    DeltaMushBinding *b = bind_mesh(m, lambda, p);
    TASK_CHECK(b != 0);
    TASK_CHECK(deltamush_degeneratecount(b) == 1); // only the isolated vertex
    std::vector<double> S = ref_diffusion(m, lambda, p);
    // Reflection composed with rotation, anisotropic scale and translation.
    M33 R = rot_axis(0.5, 0.3, 1, -0.2);
    M34 F = make_affine(R, -1.0, 1.1, 0.9, v3(0.7, -0.4, 0.2));
    std::vector<M34> pal(2, F);
    std::vector<Matrix3x4> gpal = to_geom(pal);
    // Confirm the unconstrained orthogonal factor is really a reflection.
    {
        double B[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
        double a = 0;
        V3 xb{}, yb{};
        for(int k = 0; k < 4; k++)
        {
            V3 x = {m.pos[3*k], m.pos[3*k+1], m.pos[3*k+2]};
            V3 y = m34apply(F, x);
            a += S[0*m.nverts+k];
            xb = vadd(xb, vscale(x, S[k]));
            yb = vadd(yb, vscale(y, S[k]));
        }
        V3 xbar = vscale(xb, 1/a), ybar = vscale(yb, 1/a);
        for(int k = 0; k < 4; k++)
        {
            V3 x = {m.pos[3*k], m.pos[3*k+1], m.pos[3*k+2]};
            V3 dx = vsub(x, xbar), dy = vsub(m34apply(F, x), ybar);
            for(int r = 0; r < 3; r++)
                for(int c = 0; c < 3; c++)
                    B[r][c] += S[k]*dx[r]*dy[c];
        }
        TASK_CHECK(unconstrained_det(B) < 0);
    }
    DeltaMushOutput out;
    size_t acc = 0;
    int fb = -1;
    TASK_CHECK(deltamush_evaluate(b, gpal.data(), &out, &acc, &fb));
    TASK_CHECK(acc == deltamush_momentcount(b));
    TASK_CHECK(fb == 1); // only the isolated vertex falls back
    RefOut ref = ref_eval(m, S, pal);
    TASK_CHECK(ref.fallbacks == 1);
    // The proper-rotation fit matches the reference; the degenerate vertex
    // uses plain skinning. A whole-matrix negation would land far away.
    TASK_CHECK(check_all(out, ref, m.nverts));
    deltamush_free(b);
    return 0;
}

int case_fit_rank_deficient()
{
    // Planar mesh: every rest neighbourhood is rank deficient at bind time.
    Mesh mp = make_planar();
    DeltaMushBinding *bp = bind_mesh(mp, 0.8, 2);
    TASK_CHECK(bp != 0);
    TASK_CHECK(deltamush_degeneratecount(bp) == 9);
    std::vector<double> Sp = ref_diffusion(mp, 0.8, 2);
    std::vector<M34> pal(2);
    pal[0] = make_affine(rot_axis(0.3, 0, 0, 1), 1.05, 0.9, 1.1, v3(0.3, -0.2, 0.5));
    pal[1] = make_affine(rot_axis(-0.4, 0, 1, 0), 0.8, 1.2, 0.95, v3(-0.1, 0.4, -0.6));
    std::vector<Matrix3x4> gpal = to_geom(pal);
    DeltaMushOutput out;
    size_t acc = 0;
    int fb = -1;
    TASK_CHECK(deltamush_evaluate(bp, gpal.data(), &out, &acc, &fb));
    TASK_CHECK(fb == 9);
    TASK_CHECK(acc == deltamush_momentcount(bp));
    RefOut refp = ref_eval(mp, Sp, pal);
    TASK_CHECK(refp.fallbacks == 9);
    TASK_CHECK(check_all(out, refp, mp.nverts));
    deltamush_free(bp);

    // Healthy mesh, but the palette collapses every neighbourhood to a line:
    // the frame cross-covariance is rank deficient and all vertices fall back.
    Mesh mt = make_tetra_isolated();
    DeltaMushBinding *bt = bind_mesh(mt, 1.0, 2);
    TASK_CHECK(bt != 0);
    TASK_CHECK(deltamush_degeneratecount(bt) == 1);
    std::vector<double> St = ref_diffusion(mt, 1.0, 2);
    M34 F{};
    // Exactly float-representable rank-1 matrix so both sides see the same
    // degenerate palette.
    V3 u = v3(0.5, -0.25, 1.0), v = v3(0.5, 0.75, -0.125);
    for(int r = 0; r < 3; r++)
        for(int c = 0; c < 3; c++)
            F[r][c] = u[r]*v[c];
    F[0][3] = 1; F[1][3] = 2; F[2][3] = 3;
    std::vector<M34> palt(2, F);
    std::vector<Matrix3x4> gpalt = to_geom(palt);
    TASK_CHECK(deltamush_evaluate(bt, gpalt.data(), &out, &acc, &fb));
    TASK_CHECK(fb == 5);
    RefOut reft = ref_eval(mt, St, palt);
    TASK_CHECK(reft.fallbacks == 5);
    TASK_CHECK(check_all(out, reft, mt.nverts));
    deltamush_free(bt);
    return 0;
}

// ----------------------------------------------------------------- pipeline

Mesh fixture_mesh(const fixture::Model &model)
{
    Mesh m;
    m.nverts = 4;
    m.njoints = 2;
    m.pos.assign(model.positions, model.positions+12);
    m.nrm.assign(model.normals, model.normals+12);
    m.tan.assign(model.tangents, model.tangents+16);
    const unsigned tr[12] = {0,2,1, 0,1,3, 1,2,3, 2,0,3};
    m.tris.assign(tr, tr+12);
    m.idx.assign(model.indices, model.indices+16);
    m.wgt.assign(model.weights, model.weights+16);
    return m;
}

std::vector<M34> fixture_palette(const fixture::Model &model, double t)
{
    std::array<fixture::A, 2> pal = model.palette(t);
    std::vector<M34> r(2);
    for(int j = 0; j < 2; j++)
        for(int rr = 0; rr < 3; rr++)
            for(int c = 0; c < 4; c++)
                r[j][rr][c] = pal[j][rr][c];
    return r;
}

void fixture_legacy(const fixture::Model &model, const std::array<fixture::A, 2> &pal, int v,
                    V3 &pos, V3 &nrm, V3 &tan, V3 &bit)
{
    fixture::A blended{};
    for(int k = 0; k < 4; k++)
    {
        double w = model.weights[4*v+k]/255.;
        int j = model.indices[4*v+k];
        for(int r = 0; r < 4; r++)
            for(int c = 0; c < 4; c++)
                blended[r][c] += w*pal[j][r][c];
    }
    fixture::V x = {model.positions[3*v], model.positions[3*v+1], model.positions[3*v+2]};
    fixture::V n = {model.normals[3*v], model.normals[3*v+1], model.normals[3*v+2]};
    fixture::V t = {model.tangents[4*v], model.tangents[4*v+1], model.tangents[4*v+2]};
    pos = fixture::point(blended, x);
    nrm = fixture::cofactor(blended, n);
    tan = fixture::cofactor(blended, t);
    bit = fixture::cross(nrm, tan);
    for(auto &q : bit) q *= model.tangents[4*v+3];
}

int case_pipeline_iqm()
{
    fixture::Model model;
    model.write(".task-build/pipeline.iqm");
    TASK_CHECK(loadiqm(".task-build/pipeline.iqm"));
    TASK_CHECK(numverts == 4 && numjoints == 2 && numframes == 2);
    // The fixture exercises non-unit inverse-bind matrices and vertices
    // blended across both joints.
    TASK_CHECK(std::fabs(inversebaseframe[1].a.w) > .1f);
    TASK_CHECK(inblendweight[4] == 153 && inblendweight[5] == 102);

    Mesh m = fixture_mesh(model);
    DeltaMushBinding *ba = deltamush_bind(inposition, innormal, intangent, &tris[0].vertex[0],
                                          numtris, inblendindex, inblendweight,
                                          numverts, numjoints, 0.5, 2);
    DeltaMushBinding *bb = deltamush_bind(inposition, innormal, intangent, &tris[0].vertex[0],
                                          numtris, inblendindex, inblendweight,
                                          numverts, numjoints, 4.0, 1);
    TASK_CHECK(ba != 0 && bb != 0);
    TASK_CHECK(deltamush_degeneratecount(ba) == 0);
    std::vector<double> Sa = ref_diffusion(m, 0.5, 2);
    std::vector<double> Sb = ref_diffusion(m, 4.0, 1);

    const double times[] = {0, 0.375, 1.625};
    for(double t : times)
    {
        std::array<fixture::A, 2> fpal = model.palette(t);
        std::vector<M34> pal = fixture_palette(model, t);

        // Switch off: the classic algorithm fills the output arrays.
        deltamush_switch(0, false);
        TASK_CHECK(!deltamush_switched());
        animateiqm((float)t);
        bool ok = true;
        for(int v = 0; v < 4; v++)
        {
            V3 pos, nrm, tan, bit;
            fixture_legacy(model, fpal, v, pos, nrm, tan, bit);
            ok = ok && close3(outposition+3*v, pos) && close3(outnormal+3*v, nrm)
                    && close3(outtangent+3*v, tan) && close3(outbitangent+3*v, bit);
        }
        TASK_CHECK(ok);

        // Switch on with binding A: the outputs now carry the delta-mush fit.
        deltamush_switch(ba, true);
        TASK_CHECK(deltamush_switched());
        animateiqm((float)t);
        RefOut refa = ref_eval(m, Sa, pal);
        ok = true;
        for(int v = 0; v < 4; v++)
        {
            ok = ok && close3(outposition+3*v, refa.pos[v]) && close3(outnormal+3*v, refa.nrm[v])
                    && close3(outtangent+3*v, refa.tan[v]) && close3(outbitangent+3*v, refa.bit[v]);
        }
        TASK_CHECK(ok);

        // Interleave binding B through the switch, then go back to A: no
        // state may leak between bindings or across switch toggles.
        deltamush_switch(bb, true);
        animateiqm((float)t);
        RefOut refb = ref_eval(m, Sb, pal);
        ok = true;
        for(int v = 0; v < 4; v++)
            ok = ok && close3(outposition+3*v, refb.pos[v]) && close3(outnormal+3*v, refb.nrm[v]);
        TASK_CHECK(ok);
        deltamush_switch(ba, true);
        animateiqm((float)t);
        ok = true;
        for(int v = 0; v < 4; v++)
        {
            ok = ok && close3(outposition+3*v, refa.pos[v]) && close3(outnormal+3*v, refa.nrm[v])
                    && close3(outtangent+3*v, refa.tan[v]) && close3(outbitangent+3*v, refa.bit[v]);
        }
        TASK_CHECK(ok);

        // Toggling the switch off restores the classic algorithm bit-for-bit.
        deltamush_switch(ba, false);
        TASK_CHECK(!deltamush_switched());
        animateiqm((float)t);
        ok = true;
        for(int v = 0; v < 4; v++)
        {
            V3 pos, nrm, tan, bit;
            fixture_legacy(model, fpal, v, pos, nrm, tan, bit);
            ok = ok && close3(outposition+3*v, pos) && close3(outbitangent+3*v, bit);
        }
        TASK_CHECK(ok);
    }

    cleanupiqm();
    deltamush_free(ba);
    deltamush_free(bb);
    delete[] meshdata;
    meshdata = 0;
    animdata = 0;
    return 0;
}

// ----------------------------------------------------------------- ownership

int case_ownership_source_data()
{
    Mesh m = make_tube();
    double lambda = 0.7;
    int p = 2;
    DeltaMushBinding *b = 0;
    {
        // Bind from copies that are then mutated and destroyed.
        std::vector<float> pos = m.pos, nrm = m.nrm, tan = m.tan;
        std::vector<unsigned> tris = m.tris;
        std::vector<uchar> idx = m.idx, wgt = m.wgt;
        b = deltamush_bind(pos.data(), nrm.data(), tan.data(), tris.data(), (int)tris.size()/3,
                           idx.data(), wgt.data(), m.nverts, m.njoints, lambda, p);
        TASK_CHECK(b != 0);
        for(size_t i = 0; i < pos.size(); i++) pos[i] = -1000.0f*pos[i]+7.0f;
        for(size_t i = 0; i < nrm.size(); i++) nrm[i] = 123.0f;
        for(size_t i = 0; i < tan.size(); i++) tan[i] = -45.0f;
        for(size_t i = 0; i < tris.size(); i++) tris[i] = 0;
        for(size_t i = 0; i < idx.size(); i++) idx[i] = 0;
        for(size_t i = 0; i < wgt.size(); i++) wgt[i] = 0;
    }
    std::vector<double> S = ref_diffusion(m, lambda, p);
    std::vector<M34> pal(3);
    pal[0] = make_affine(rot_axis(0.5, 0, 1, 0), 1.0, 1.0, 1.0, v3(0.1, 0.2, 0.3));
    pal[1] = make_affine(rot_axis(-0.3, 1, 0, 1), 1.1, 0.9, 1.0, v3(-0.2, 0.1, 0.4));
    pal[2] = make_affine(rot_axis(0.8, 0, 0.5, 1), 0.95, 1.05, 1.0, v3(0.3, -0.1, -0.2));
    std::vector<Matrix3x4> gpal = to_geom(pal);
    DeltaMushOutput out;
    TASK_CHECK(deltamush_evaluate(b, gpal.data(), &out));
    RefOut ref = ref_eval(m, S, pal);
    // Mutating and freeing the source arrays after binding changed nothing.
    TASK_CHECK(check_all(out, ref, m.nverts));
    deltamush_free(b);
    return 0;
}

int case_ownership_outputs_interleave()
{
    Mesh m = make_tube();
    DeltaMushBinding *ba = bind_mesh(m, 0.3, 1);
    DeltaMushBinding *bb = bind_mesh(m, 3.0, 4);
    TASK_CHECK(ba != 0 && bb != 0);
    std::vector<M34> pal(3);
    pal[0] = make_affine(rot_axis(0.4, 0, 0, 1), 1.1, 0.95, 1.0, v3(0.2, 0.1, -0.3));
    pal[1] = make_affine(rot_axis(-0.6, 0.3, 1, 0.2), 0.9, 1.05, 1.2, v3(-0.4, 0.3, 0.1));
    pal[2] = make_affine(rot_axis(0.9, 1, -0.2, 0.4), 1.0, 0.85, 1.1, v3(0.1, -0.2, 0.4));
    std::vector<Matrix3x4> gpal = to_geom(pal);

    DeltaMushOutput o1, o2, o3;
    TASK_CHECK(deltamush_evaluate(ba, gpal.data(), &o1));
    TASK_CHECK(deltamush_evaluate(bb, gpal.data(), &o2));
    TASK_CHECK(deltamush_evaluate(ba, gpal.data(), &o3));
    // Interleaving the two bindings is deterministic and pollution-free.
    TASK_CHECK(o1.position == o3.position);
    TASK_CHECK(o1.normal == o3.normal);
    TASK_CHECK(o1.tangent == o3.tangent);
    TASK_CHECK(o1.bitangent == o3.bitangent);
    // The two bindings are genuinely different smoothings.
    bool differs = false;
    for(size_t i = 0; i < o1.position.size(); i++)
        if(std::fabs(o1.position[i]-o2.position[i]) > 1e-4) differs = true;
    TASK_CHECK(differs);
    // Later evaluations into other outputs never touch earlier results.
    DeltaMushOutput o4;
    std::vector<M34> pal2(3, make_rigid(rot_axis(0.2, 1, 0, 0), v3(1, 0, 0)));
    std::vector<Matrix3x4> gpal2 = to_geom(pal2);
    TASK_CHECK(deltamush_evaluate(bb, gpal2.data(), &o4));
    TASK_CHECK(o1.position == o3.position);
    TASK_CHECK(o1.position.size() == (size_t)(3*m.nverts));
    TASK_CHECK(o4.position.size() == (size_t)(3*m.nverts));
    deltamush_free(ba);
    deltamush_free(bb);
    return 0;
}

int case_ownership_failures()
{
    Mesh m = make_tetra_isolated();
    DeltaMushBinding *good = bind_mesh(m, 1.0, 2);
    TASK_CHECK(good != 0);

    // Invalid indices, weights, parameters and non-finite data all fail.
    {
        Mesh bad = m;
        bad.tris[0] = 99;
        TASK_CHECK(bind_mesh(bad, 1.0, 2) == 0);
    }
    {
        Mesh bad = m;
        bad.idx[0] = 7;
        TASK_CHECK(bind_mesh(bad, 1.0, 2) == 0);
    }
    {
        Mesh bad = m;
        bad.wgt[0] = 254; // weights no longer sum to 255
        TASK_CHECK(bind_mesh(bad, 1.0, 2) == 0);
    }
    {
        Mesh bad = m;
        bad.wgt[0] = 0; bad.wgt[1] = 255; // used influence not first
        TASK_CHECK(bind_mesh(bad, 1.0, 2) == 0);
    }
    {
        Mesh bad = m;
        bad.pos[0] = std::numeric_limits<float>::quiet_NaN();
        TASK_CHECK(bind_mesh(bad, 1.0, 2) == 0);
    }
    {
        Mesh bad = m;
        bad.tan[3] = std::numeric_limits<float>::infinity();
        TASK_CHECK(bind_mesh(bad, 1.0, 2) == 0);
    }
    TASK_CHECK(bind_mesh(m, 0.0, 2) == 0);   // lambda must be > 0
    TASK_CHECK(bind_mesh(m, -1.0, 2) == 0);
    TASK_CHECK(bind_mesh(m, std::numeric_limits<double>::quiet_NaN(), 2) == 0);
    TASK_CHECK(bind_mesh(m, 1.0, 0) == 0);   // power must be >= 1
    TASK_CHECK(bind_mesh(m, 1.0, -2) == 0);
    TASK_CHECK(deltamush_bind(0, m.nrm.data(), m.tan.data(), m.tris.data(), 4,
                              m.idx.data(), m.wgt.data(), m.nverts, m.njoints, 1.0, 1) == 0);
    TASK_CHECK(deltamush_bind(m.pos.data(), m.nrm.data(), m.tan.data(), m.tris.data(), 4,
                              m.idx.data(), m.wgt.data(), 0, m.njoints, 1.0, 1) == 0);
    TASK_CHECK(deltamush_bind(m.pos.data(), m.nrm.data(), m.tan.data(), m.tris.data(), 4,
                              m.idx.data(), m.wgt.data(), m.nverts, 0, 1.0, 1) == 0);

    // The earlier binding is unaffected by all the failed binds.
    std::vector<M34> pal(2);
    pal[0] = make_affine(rot_axis(0.4, 0, 1, 0), 1.0, 1.0, 1.0, v3(0.2, 0, 0.1));
    pal[1] = make_affine(rot_axis(-0.5, 1, 0, 0.5), 1.1, 0.9, 1.0, v3(-0.1, 0.3, 0));
    std::vector<Matrix3x4> gpal = to_geom(pal);
    DeltaMushOutput out;
    size_t acc = 0;
    int fb = -1;
    TASK_CHECK(deltamush_evaluate(good, gpal.data(), &out, &acc, &fb));
    TASK_CHECK(acc == deltamush_momentcount(good));
    TASK_CHECK(fb == 1);
    std::vector<float> snapshot = out.position;

    // Failed evaluations leave the output object untouched.
    std::vector<Matrix3x4> badpal = gpal;
    badpal[0].a.x = std::numeric_limits<float>::quiet_NaN();
    TASK_CHECK(!deltamush_evaluate(good, badpal.data(), &out));
    TASK_CHECK(out.position == snapshot);
    TASK_CHECK(!deltamush_evaluate(0, gpal.data(), &out));
    TASK_CHECK(!deltamush_evaluate(good, 0, &out));
    TASK_CHECK(out.position == snapshot);
    std::vector<double> drow(deltamush_numverts(good));
    TASK_CHECK(!deltamush_diffusionrow(good, -1, drow.data()));
    TASK_CHECK(!deltamush_diffusionrow(good, 99, drow.data()));
    TASK_CHECK(!deltamush_diffusionrow(0, 0, drow.data()));

    // And the binding still evaluates fine afterwards.
    TASK_CHECK(deltamush_evaluate(good, gpal.data(), &out));
    TASK_CHECK(out.position == snapshot);
    deltamush_free(good);
    return 0;
}

} // namespace

const task_case task_cases[] = {
    { "diffusion", "components_rows", case_diffusion_components },
    { "diffusion", "tube_rows", case_diffusion_tube },
    { "fit", "rigid_motion", case_fit_rigid },
    { "fit", "reference_fit", case_fit_reference },
    { "fit", "reflection", case_fit_reflection },
    { "fit", "rank_deficient", case_fit_rank_deficient },
    { "pipeline", "iqm_animate_switch", case_pipeline_iqm },
    { "ownership", "source_data", case_ownership_source_data },
    { "ownership", "outputs_interleave", case_ownership_outputs_interleave },
    { "ownership", "failures", case_ownership_failures },
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);

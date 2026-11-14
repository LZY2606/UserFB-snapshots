#include "delta_mush.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace
{
    // Symmetric 4x4 moment Omega_ij = sum_k S_ik w_kj u_k u_k^T, u_k = (x_k, 1).
    // Entries are stored in the order 00 01 02 03 11 12 13 22 23 33.
    struct Moment
    {
        int joint;
        double m[10];
    };

    const int SYM4[4][4] = {{0,1,2,3},{1,4,5,6},{2,5,7,8},{3,6,8,9}};

    // Jacobi eigendecomposition of a symmetric 3x3 matrix. Returns the
    // eigenvalues in w, sorted descending, with the matching eigenvectors
    // in the columns of v.
    void jacobi3(const double a[3][3], double w[3], double v[3][3])
    {
        double m[3][3];
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 3; j++)
            {
                m[i][j] = a[i][j];
                v[i][j] = i == j ? 1.0 : 0.0;
            }
        for(int sweep = 0; sweep < 100; sweep++)
        {
            int p = 0, q = 1;
            double best = 0;
            for(int i = 0; i < 3; i++)
                for(int j = i + 1; j < 3; j++)
                    if(std::fabs(m[i][j]) > best) { best = std::fabs(m[i][j]); p = i; q = j; }
            double scale = std::fabs(m[0][0]) + std::fabs(m[1][1]) + std::fabs(m[2][2]);
            if(best <= 1e-15*scale) break;
            double app = m[p][p], aqq = m[q][q], apq = m[p][q];
            double tau = (aqq - app)/(2*apq);
            double t = (tau >= 0 ? 1.0 : -1.0)/(std::fabs(tau) + std::sqrt(1 + tau*tau));
            double c = 1/std::sqrt(1 + t*t), s = t*c;
            for(int k = 0; k < 3; k++)
            {
                double mkp = m[k][p], mkq = m[k][q];
                m[k][p] = c*mkp - s*mkq;
                m[k][q] = s*mkp + c*mkq;
            }
            for(int k = 0; k < 3; k++)
            {
                double mpk = m[p][k], mqk = m[q][k];
                m[p][k] = c*mpk - s*mqk;
                m[q][k] = s*mpk + c*mqk;
            }
            for(int k = 0; k < 3; k++)
            {
                double vkp = v[k][p], vkq = v[k][q];
                v[k][p] = c*vkp - s*vkq;
                v[k][q] = s*vkp + c*vkq;
            }
        }
        int ord[3] = {0, 1, 2};
        for(int i = 0; i < 2; i++)
            for(int j = i + 1; j < 3; j++)
                if(m[ord[j]][ord[j]] > m[ord[i]][ord[i]]) std::swap(ord[i], ord[j]);
        double ws[3], vs[3][3];
        for(int i = 0; i < 3; i++)
        {
            ws[i] = m[ord[i]][ord[i]];
            for(int k = 0; k < 3; k++) vs[k][i] = v[k][ord[i]];
        }
        for(int i = 0; i < 3; i++)
        {
            w[i] = ws[i];
            for(int k = 0; k < 3; k++) v[k][i] = vs[k][i];
        }
    }

    // One-sided Jacobi SVD of a 3x3 matrix: B = U diag(s) V^T with the
    // singular values in s sorted descending and orthonormal columns in U
    // and V. Orthogonalizing the columns of B directly (instead of
    // eigendecomposing B^T B) keeps the smallest singular value resolved
    // down to machine precision, which the 1e-9 degeneracy rule needs.
    void svd3(const double B[3][3], double s[3], double U[3][3], double V[3][3])
    {
        double a[3][3];
        for(int i = 0; i < 3; i++)
            for(int j = 0; j < 3; j++)
            {
                a[i][j] = B[i][j];
                V[i][j] = i == j ? 1.0 : 0.0;
            }
        for(int sweep = 0; sweep < 100; sweep++)
        {
            bool rotated = false;
            for(int p = 0; p < 3; p++)
                for(int q = p + 1; q < 3; q++)
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
                    double tau = (beta - alpha)/(2*gamma);
                    double t = (tau >= 0 ? 1.0 : -1.0)/(std::fabs(tau) + std::sqrt(1 + tau*tau));
                    double c = 1/std::sqrt(1 + t*t), sn = t*c;
                    for(int r = 0; r < 3; r++)
                    {
                        double ap = a[r][p], aq = a[r][q];
                        a[r][p] = c*ap - sn*aq;
                        a[r][q] = sn*ap + c*aq;
                    }
                    for(int r = 0; r < 3; r++)
                    {
                        double vp = V[r][p], vq = V[r][q];
                        V[r][p] = c*vp - sn*vq;
                        V[r][q] = sn*vp + c*vq;
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
            for(int j = i + 1; j < 3; j++)
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

    double melem(const Matrix3x4 &m, int r, int c)
    {
        return (&m.a)[r].v[c];
    }

    bool finitepalette(const Matrix3x4 *palette, int numjoints)
    {
        for(int j = 0; j < numjoints; j++)
            for(int r = 0; r < 3; r++)
                for(int c = 0; c < 4; c++)
                    if(!std::isfinite(melem(palette[j], r, c))) return false;
        return true;
    }
}

struct DeltaMushBinding
{
    int numverts = 0;
    int numjoints = 0;
    double lambda = 0;
    int power = 0;
    // Owned copies of the rest-pose data.
    std::vector<float> pos, nrm, tan;
    std::vector<uchar> idx, wgt;
    // Sparse rows of the diffusion matrix S = (I + lambda*L)^(-power).
    std::vector<std::vector<std::pair<int, double>>> srows;
    // Nonzero moments per vertex.
    std::vector<std::vector<Moment>> moments;
    size_t momentcount = 0;
    // Rest statistics derived from the moments: alpha_i = sum_k S_ik and the
    // weighted rest centre xbar_i.
    std::vector<double> alpha;
    std::vector<double> xbar;
    std::vector<char> degenerate;
    int degeneratecount = 0;
};

DeltaMushBinding *deltamush_bind(const float *positions, const float *normals, const float *tangents,
                                 const uint *triangles, int numtriangles,
                                 const uchar *blendindices, const uchar *blendweights,
                                 int numverts, int numjoints, double lambda, int power)
{
    if(!positions || !normals || !tangents || !blendindices || !blendweights) return 0;
    if(numverts < 1 || numjoints < 1 || numtriangles < 0) return 0;
    if(numtriangles > 0 && !triangles) return 0;
    if(!(lambda > 0) || !std::isfinite(lambda) || power < 1) return 0;
    for(int i = 0; i < 3*numverts; i++)
        if(!std::isfinite(positions[i]) || !std::isfinite(normals[i])) return 0;
    for(int i = 0; i < 4*numverts; i++)
        if(!std::isfinite(tangents[i])) return 0;
    for(int t = 0; t < 3*numtriangles; t++)
        if(triangles[t] >= (uint)numverts) return 0;
    for(int v = 0; v < numverts; v++)
    {
        int sum = 0;
        bool seenzero = false;
        for(int k = 0; k < 4; k++)
        {
            int w = blendweights[4*v + k], j = blendindices[4*v + k];
            if(j >= numjoints) return 0;
            if(!w) seenzero = true;
            else if(seenzero) return 0; // used influences must come first
            sum += w;
        }
        if(sum != 255) return 0;
    }

    try
    {
        std::unique_ptr<DeltaMushBinding> b(new DeltaMushBinding);
        b->numverts = numverts;
        b->numjoints = numjoints;
        b->lambda = lambda;
        b->power = power;
        b->pos.assign(positions, positions + 3*numverts);
        b->nrm.assign(normals, normals + 3*numverts);
        b->tan.assign(tangents, tangents + 4*numverts);
        b->idx.assign(blendindices, blendindices + 4*numverts);
        b->wgt.assign(blendweights, blendweights + 4*numverts);

        // Deduplicated undirected edges of the triangle graph, keyed by
        // vertex index; coincident vertices are not merged.
        std::set<std::pair<int, int>> edges;
        for(int t = 0; t < numtriangles; t++)
            for(int e = 0; e < 3; e++)
            {
                int a = (int)triangles[3*t + e], c = (int)triangles[3*t + (e+1)%3];
                if(a == c) continue;
                edges.insert(std::minmax(a, c));
            }

        // Dense system matrix M = I + lambda*L, L = D - A. Isolated vertices
        // get an identity row.
        const int n = numverts;
        std::vector<double> mat(n*n, 0.0);
        for(int i = 0; i < n; i++) mat[i*n + i] = 1.0;
        for(std::set<std::pair<int, int>>::const_iterator it = edges.begin(); it != edges.end(); ++it)
        {
            int a = it->first, c = it->second;
            mat[a*n + a] += lambda;
            mat[c*n + c] += lambda;
            mat[a*n + c] -= lambda;
            mat[c*n + a] -= lambda;
        }

        // Cholesky factorization M = G*G^T (M is symmetric positive definite).
        std::vector<double> g(n*n, 0.0);
        for(int i = 0; i < n; i++)
            for(int j = 0; j <= i; j++)
            {
                double s = mat[i*n + j];
                for(int k = 0; k < j; k++) s -= g[i*n + k]*g[j*n + k];
                if(i == j)
                {
                    if(!(s > 0) || !std::isfinite(s)) return 0;
                    g[i*n + i] = std::sqrt(s);
                }
                else g[i*n + j] = s/g[j*n + j];
            }

        // Row i of S = M^(-power) e_i, by repeated forward/back substitution.
        b->srows.resize(n);
        std::vector<double> rhs(n), tmp(n), z(n);
        for(int i = 0; i < n; i++)
        {
            std::fill(rhs.begin(), rhs.end(), 0.0);
            rhs[i] = 1.0;
            for(int step = 0; step < power; step++)
            {
                for(int r = 0; r < n; r++)
                {
                    double s = rhs[r];
                    for(int k = 0; k < r; k++) s -= g[r*n + k]*tmp[k];
                    tmp[r] = s/g[r*n + r];
                }
                for(int r = n - 1; r >= 0; r--)
                {
                    double s = tmp[r];
                    for(int k = r + 1; k < n; k++) s -= g[k*n + r]*z[k];
                    z[r] = s/g[r*n + r];
                }
                rhs = z;
            }
            for(int k = 0; k < n; k++)
            {
                if(!std::isfinite(z[k])) return 0;
                if(z[k] != 0) b->srows[i].push_back(std::make_pair(k, z[k]));
            }
        }

        // Fold the diffusion rows and the blend weights into moments.
        b->moments.resize(n);
        b->momentcount = 0;
        std::vector<double> acc(10*(size_t)numjoints);
        for(int i = 0; i < n; i++)
        {
            std::fill(acc.begin(), acc.end(), 0.0);
            const std::vector<std::pair<int, double>> &row = b->srows[i];
            for(size_t e = 0; e < row.size(); e++)
            {
                int k = row[e].first;
                double s = row[e].second;
                double u[4] = { b->pos[3*k], b->pos[3*k+1], b->pos[3*k+2], 1.0 };
                for(int slot = 0; slot < 4; slot++)
                {
                    double w = b->wgt[4*k + slot]/255.0;
                    if(!w) continue;
                    int j = b->idx[4*k + slot];
                    double sw = s*w, *a = &acc[10*(size_t)j];
                    for(int r = 0; r < 4; r++)
                        for(int c = r; c < 4; c++)
                            a[SYM4[r][c]] += sw*u[r]*u[c];
                }
            }
            for(int j = 0; j < numjoints; j++)
            {
                double *a = &acc[10*(size_t)j];
                if(a[9] == 0) continue; // joint unused in this component
                Moment mo;
                mo.joint = j;
                for(int q = 0; q < 10; q++) mo.m[q] = a[q];
                b->moments[i].push_back(mo);
                b->momentcount++;
            }
        }

        // Rest statistics and the bind-time degeneracy test on the rest
        // covariance C_i = sum_k S_ik (x_k - xbar_i)(x_k - xbar_i)^T.
        b->alpha.assign(n, 0.0);
        b->xbar.assign(3*n, 0.0);
        b->degenerate.assign(n, 0);
        b->degeneratecount = 0;
        for(int i = 0; i < n; i++)
        {
            double a = 0, q[3] = {0, 0, 0}, P[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
            const std::vector<Moment> &ms = b->moments[i];
            for(size_t e = 0; e < ms.size(); e++)
            {
                const double *m = ms[e].m;
                a += m[9];
                q[0] += m[3]; q[1] += m[6]; q[2] += m[8];
                P[0][0] += m[0]; P[0][1] += m[1]; P[0][2] += m[2];
                P[1][1] += m[4]; P[1][2] += m[5]; P[2][2] += m[7];
            }
            P[1][0] = P[0][1]; P[2][0] = P[0][2]; P[2][1] = P[1][2];
            b->alpha[i] = a;
            bool deg = !(a > 0) || !std::isfinite(a);
            if(!deg)
            {
                double xb[3] = { q[0]/a, q[1]/a, q[2]/a };
                for(int r = 0; r < 3; r++) b->xbar[3*i + r] = xb[r];
                double C[3][3];
                for(int r = 0; r < 3; r++)
                    for(int c = 0; c < 3; c++)
                        C[r][c] = P[r][c] - a*xb[r]*xb[c];
                double w[3], v[3][3];
                jacobi3(C, w, v);
                if(!(w[0] > 0) || w[2] <= 1e-9*w[0]) deg = true;
            }
            if(deg)
            {
                b->degenerate[i] = 1;
                b->degeneratecount++;
            }
        }

        return b.release();
    }
    catch(...)
    {
        return 0;
    }
}

void deltamush_free(DeltaMushBinding *binding)
{
    delete binding;
}

int deltamush_numverts(const DeltaMushBinding *binding)
{
    return binding ? binding->numverts : 0;
}

int deltamush_numjoints(const DeltaMushBinding *binding)
{
    return binding ? binding->numjoints : 0;
}

size_t deltamush_momentcount(const DeltaMushBinding *binding)
{
    return binding ? binding->momentcount : 0;
}

int deltamush_degeneratecount(const DeltaMushBinding *binding)
{
    return binding ? binding->degeneratecount : 0;
}

bool deltamush_diffusionrow(const DeltaMushBinding *binding, int row, double *out)
{
    if(!binding || !out || row < 0 || row >= binding->numverts) return false;
    std::fill(out, out + binding->numverts, 0.0);
    const std::vector<std::pair<int, double>> &r = binding->srows[row];
    for(size_t e = 0; e < r.size(); e++) out[r[e].first] = r[e].second;
    return true;
}

// Classic CPU skinning for one vertex: position through the blended matrix,
// directions through its adjoint-transpose (cross products of the rows),
// bitangent rebuilt from the rotated normal and tangent.
static void deltamush_fallback(const DeltaMushBinding *b, const Matrix3x4 *palette, int i,
                               double pos[3], double nrm[3], double tan[3], double bit[3])
{
    double m[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
    const uchar *idx = &b->idx[4*i], *wgt = &b->wgt[4*i];
    for(int k = 0; k < 4 && wgt[k]; k++)
    {
        const Matrix3x4 &M = palette[idx[k]];
        double w = wgt[k]/255.0;
        for(int r = 0; r < 3; r++)
            for(int c = 0; c < 4; c++)
                m[r][c] += w*melem(M, r, c);
    }
    const float *x = &b->pos[3*i], *n = &b->nrm[3*i], *t = &b->tan[4*i];
    for(int r = 0; r < 3; r++)
        pos[r] = m[r][0]*x[0] + m[r][1]*x[1] + m[r][2]*x[2] + m[r][3];
    double cr[3][3];
    for(int r = 0; r < 3; r++)
    {
        int r0 = (r+1)%3, r1 = (r+2)%3;
        for(int c = 0; c < 3; c++)
        {
            int c0 = (c+1)%3, c1 = (c+2)%3;
            cr[r][c] = m[r0][c0]*m[r1][c1] - m[r0][c1]*m[r1][c0];
        }
    }
    for(int r = 0; r < 3; r++)
    {
        nrm[r] = cr[r][0]*n[0] + cr[r][1]*n[1] + cr[r][2]*n[2];
        tan[r] = cr[r][0]*t[0] + cr[r][1]*t[1] + cr[r][2]*t[2];
    }
    bit[0] = (nrm[1]*tan[2] - nrm[2]*tan[1])*t[3];
    bit[1] = (nrm[2]*tan[0] - nrm[0]*tan[2])*t[3];
    bit[2] = (nrm[0]*tan[1] - nrm[1]*tan[0])*t[3];
}

bool deltamush_evaluate(const DeltaMushBinding *b, const Matrix3x4 *palette, DeltaMushOutput *output,
                        size_t *accumulations, int *fallbacks)
{
    if(!b || !palette || !output) return false;
    if(!finitepalette(palette, b->numjoints)) return false;

    try
    {
        const int n = b->numverts;
        std::vector<float> op(3*n), on(3*n), ot(3*n), ob(3*n);
        size_t acc = 0;
        int fb = 0;
        for(int i = 0; i < n; i++)
        {
            // H_i = sum_j M_j Omega_ij, a 3x4 matrix holding the weighted
            // cross term sum_k S_ik y_k x_k^T and centre sum_k S_ik y_k.
            double H[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
            const std::vector<Moment> &ms = b->moments[i];
            for(size_t e = 0; e < ms.size(); e++)
            {
                const Matrix3x4 &M = palette[ms[e].joint];
                const double *mo = ms[e].m;
                for(int r = 0; r < 3; r++)
                    for(int c = 0; c < 4; c++)
                    {
                        double s = 0;
                        for(int d = 0; d < 4; d++) s += melem(M, r, d)*mo[SYM4[d][c]];
                        H[r][c] += s;
                    }
                acc++;
            }

            double pos[3], nrm[3], tan[3], bit[3];
            bool deg = b->degenerate[i] != 0;
            double R[3][3] = {{0,0,0},{0,0,0},{0,0,0}}, tt[3] = {0,0,0};
            if(!deg)
            {
                double a = b->alpha[i];
                const double *xb = &b->xbar[3*i];
                double yb[3] = { H[0][3]/a, H[1][3]/a, H[2][3]/a };
                // Centred cross-covariance B = sum_k S_ik (x_k-xbar)(y_k-ybar)^T.
                double B[3][3];
                for(int r = 0; r < 3; r++)
                    for(int c = 0; c < 3; c++)
                        B[r][c] = H[c][r] - xb[r]*H[c][3];
                double sv[3], U[3][3], V[3][3];
                svd3(B, sv, U, V);
                if(!(sv[0] > 0) || sv[2] <= 1e-9*sv[0]) deg = true;
                else
                {
                    // R = V*U^T maximizes trace(R*B) over orthogonal
                    // matrices.
                    for(int r = 0; r < 3; r++)
                        for(int c = 0; c < 3; c++)
                            R[r][c] = V[r][0]*U[c][0] + V[r][1]*U[c][1] + V[r][2]*U[c][2];
                    double det = R[0][0]*(R[1][1]*R[2][2] - R[1][2]*R[2][1])
                               - R[0][1]*(R[1][0]*R[2][2] - R[1][2]*R[2][0])
                               + R[0][2]*(R[1][0]*R[2][1] - R[1][1]*R[2][0]);
                    if(det < 0)
                    {
                        // Reflection: flip only the smallest singular value
                        // direction (the last column after sorting).
                        for(int r = 0; r < 3; r++) U[r][2] = -U[r][2];
                        for(int r = 0; r < 3; r++)
                            for(int c = 0; c < 3; c++)
                                R[r][c] = V[r][0]*U[c][0] + V[r][1]*U[c][1] + V[r][2]*U[c][2];
                    }
                    for(int r = 0; r < 3; r++)
                        tt[r] = yb[r] - (R[r][0]*xb[0] + R[r][1]*xb[1] + R[r][2]*xb[2]);
                }
            }
            if(deg)
            {
                fb++;
                deltamush_fallback(b, palette, i, pos, nrm, tan, bit);
            }
            else
            {
                const float *x = &b->pos[3*i], *nn = &b->nrm[3*i], *ta = &b->tan[4*i];
                for(int r = 0; r < 3; r++)
                {
                    pos[r] = tt[r] + R[r][0]*x[0] + R[r][1]*x[1] + R[r][2]*x[2];
                    nrm[r] = R[r][0]*nn[0] + R[r][1]*nn[1] + R[r][2]*nn[2];
                    tan[r] = R[r][0]*ta[0] + R[r][1]*ta[1] + R[r][2]*ta[2];
                }
                bit[0] = nrm[1]*tan[2] - nrm[2]*tan[1];
                bit[1] = nrm[2]*tan[0] - nrm[0]*tan[2];
                bit[2] = nrm[0]*tan[1] - nrm[1]*tan[0];
                for(int r = 0; r < 3; r++) bit[r] *= ta[3];
            }
            for(int r = 0; r < 3; r++)
            {
                op[3*i + r] = (float)pos[r];
                on[3*i + r] = (float)nrm[r];
                ot[3*i + r] = (float)tan[r];
                ob[3*i + r] = (float)bit[r];
            }
        }
        output->position.swap(op);
        output->normal.swap(on);
        output->tangent.swap(ot);
        output->bitangent.swap(ob);
        if(accumulations) *accumulations = acc;
        if(fallbacks) *fallbacks = fb;
        return true;
    }
    catch(...)
    {
        return false;
    }
}

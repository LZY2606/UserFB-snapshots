#include "polarization.h"
#include <cmath>
#include <utility>

namespace pol {

dvec3 perpendicular_axis(const dvec3& k) {
    // Pick the world axis least aligned with k, then project out the k
    // component.  The choice is deterministic and guarantees no near-zero
    // denominator.
    dvec3 a = (std::abs(k.x) < 0.9) ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
    dvec3 e = a - k * (a * k);
    return e.normalized();
}

double frame_rotation(const dvec3& k, const dvec3& e1, const dvec3& e1_new) {
    dvec3 e2 = cross(k, e1);
    double c = e1 * e1_new;
    double s = e2 * e1_new;
    return std::atan2(s, c);
}

Stokes rotate_stokes(const Stokes& s, double psi) {
    double c = std::cos(2.0 * psi);
    double sn = std::sin(2.0 * psi);
    Stokes out = s;
    out[Q] = c * s[Q] + sn * s[U];
    out[U] = -sn * s[Q] + c * s[U];
    return out;
}

PolarState rebase(const PolarState& st, const dvec3& k_new, const dvec3& e1_new) {
    PolarState out = st;
    double psi = frame_rotation(st.k, st.e1, e1_new);
    for (int ch = 0; ch < CHANNELS; ++ch)
        out.s[ch] = rotate_stokes(st.s[ch], psi);
    out.k = k_new;
    out.e1 = e1_new;
    return out;
}

std::pair<Complex, Complex> fresnel_amplitude(double n1, double n2, double ci) {
    // std::sqrt on a negative real returns a value on the non-negative
    // imaginary branch, exactly the prescribed complex square root.
    double ct_sq = 1.0 - (n1 / n2) * (n1 / n2) * (1.0 - ci * ci);
    Complex ct = std::sqrt(Complex(ct_sq, 0.0));
    Complex rs = (n1 * ci - n2 * ct) / (n1 * ci + n2 * ct);
    Complex rp = (n2 * ci - n1 * ct) / (n2 * ci + n1 * ct);
    return {rs, rp};
}

namespace {

// 2x2 complex matrices, row-major, basis order (s, p) == (e1, e2).
using C2 = std::array<std::array<Complex, 2>, 2>;

C2 mat_mul(const C2& a, const C2& b) {
    C2 r{};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int t = 0; t < 2; ++t)
                r[i][j] += a[i][t] * b[t][j];
    return r;
}

C2 mat_adjoint(const C2& a) {
    C2 r{};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            r[j][i] = std::conj(a[i][j]);
    return r;
}

Complex mat_trace(const C2& a) { return a[0][0] + a[1][1]; }

// Pauli matrices in the order (sigma0, sigma1, sigma2, sigma3), matching
// Stokes (I, Q, U, V).  sigma3 is minus the textbook Pauli matrix because the
// convention here is V = 2 Im(E1 conj(E2)), the opposite of the usual optics
// sign; then S_mu = Tr(rho sigma_mu) reproduces exactly the stated Stokes.
C2 make_pauli(int i) {
    C2 s{};
    switch (i) {
        case 0:
            s[0][0] = Complex(1.0, 0.0);
            s[1][1] = Complex(1.0, 0.0);
            break;
        case 1:
            s[0][0] = Complex(1.0, 0.0);
            s[1][1] = Complex(-1.0, 0.0);
            break;
        case 2:
            s[0][1] = Complex(1.0, 0.0);
            s[1][0] = Complex(1.0, 0.0);
            break;
        default: // 3
            s[0][1] = Complex(0.0,  1.0);
            s[1][0] = Complex(0.0, -1.0);
            break;
    }
    return s;
}

} // namespace

Mueller reflection_mueller(double n1, double n2, double ci) {
    auto [rs, rp] = fresnel_amplitude(n1, n2, ci);
    C2 j{};
    j[0][0] = rs; // diagonal entry order: rs (s axis), then rp (p axis)
    j[1][1] = rp;
    C2 jd = mat_adjoint(j);

    Mueller m{};
    for (int i = 0; i < 4; ++i) {
        C2 si = make_pauli(i);
        for (int k = 0; k < 4; ++k) {
            C2 sk = make_pauli(k);
            m[i][k] = 0.5 * mat_trace(mat_mul(mat_mul(si, j),
                                              mat_mul(sk, jd))).real();
        }
    }
    return m;
}

Stokes apply_mueller(const Mueller& m, const Stokes& s) {
    Stokes out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i] += m[i][j] * s[j];
    return out;
}

Stokes linear_analyzer(const Stokes& s, double theta) {
    double c = std::cos(2.0 * theta);
    double sn = std::sin(2.0 * theta);
    // Mueller matrix of an ideal linear analyzer, transmission axis at theta.
    Mueller m{};
    m[0][0] = 0.5;        m[0][1] = 0.5 * c;     m[0][2] = 0.5 * sn;
    m[1][0] = 0.5 * c;    m[1][1] = 0.5 * c * c; m[1][2] = 0.5 * c * sn;
    m[2][0] = 0.5 * sn;   m[2][1] = 0.5 * c * sn; m[2][2] = 0.5 * sn * sn;
    return apply_mueller(m, s);
}

double analyzer_intensity(const Stokes& s, double theta) {
    return 0.5 * (s[I] + s[Q] * std::cos(2.0 * theta) + s[U] * std::sin(2.0 * theta));
}

} // namespace pol

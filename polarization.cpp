#include "polarization.h"

#include <algorithm>

namespace pol {

Vec3 world_reference_axis(const Vec3& k) {
    Vec3 hint = (std::abs(k.y) < 0.9) ? Vec3(0.0, 1.0, 0.0)
                                      : Vec3(1.0, 0.0, 0.0);
    return projected_reference_axis(k, hint);
}

Vec3 projected_reference_axis(const Vec3& k, const Vec3& hint) {
    Vec3 kk = normalize(k);
    Vec3 projected = hint - kk * dot(hint, kk);
    if (norm2(projected) < 1e-24) projected = world_reference_axis(kk);
    return normalize(projected);
}

Stokes operator*(const Mueller& M, const Stokes& s) {
    Stokes out;
    for (int r = 0; r < 4; ++r) {
        double v = 0.0;
        for (int c = 0; c < 4; ++c) v += M(r, c) * s[c];
        out[r] = v;
    }
    return out;
}

StokesRGB apply(const Mueller& M, const StokesRGB& s) {
    StokesRGB out;
    for (int ch = 0; ch < 3; ++ch) out[ch] = M * s[ch];
    return out;
}

Mueller Mueller::identity() {
    Mueller M;
    for (int i = 0; i < 4; ++i) M(i, i) = 1.0;
    return M;
}

Mueller operator*(const Mueller& A, const Mueller& B) {
    Mueller C;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double v = 0.0;
            for (int k = 0; k < 4; ++k) v += A(r, k) * B(k, c);
            C(r, c) = v;
        }
    return C;
}

FresnelCoefficients fresnel(double n1, double n2, double ci) {
    ci = std::max(0.0, std::min(1.0, ci));
    FresnelCoefficients f;

    double eta = n1 / n2;
    double ct2 = 1.0 - eta * eta * (1.0 - ci * ci);
    if (ct2 >= 0.0) {
        f.ct = {std::sqrt(ct2), 0.0};
    } else {
        // Required branch: sqrt of a negative real number has positive
        // imaginary part.
        f.ct = {0.0, std::sqrt(-ct2)};
    }

    const std::complex<double> one_n1_ci = n1 * ci;
    const std::complex<double> one_n2_ci = n2 * ci;
    const std::complex<double> c_n1_ct = n1 * f.ct;
    const std::complex<double> c_n2_ct = n2 * f.ct;

    f.rs = (one_n1_ci - c_n2_ct) / (one_n1_ci + c_n2_ct);
    f.rp = (one_n2_ci - c_n1_ct) / (one_n2_ci + c_n1_ct);
    return f;
}

Mueller reflection_mueller(const std::complex<double>& rs,
                            const std::complex<double>& rp) {
    double Rs = std::norm(rs);
    double Rp = std::norm(rp);
    double half_sum = 0.5 * (Rs + Rp);
    double half_diff = 0.5 * (Rs - Rp);

    // rs * conj(rp) rotates the (U,V) correlation.  The sign of its imaginary
    // part is what preserves the TIR phase coupling.
    std::complex<double> g = rs * std::conj(rp);
    double A = g.real();
    double B = g.imag();

    Mueller M;
    M(0, 0) = half_sum;
    M(0, 1) = half_diff;
    M(1, 0) = half_diff;
    M(1, 1) = half_sum;

    M(2, 2) = A;
    M(2, 3) = -B;
    M(3, 2) = B;
    M(3, 3) = A;
    return M;
}

Mueller reflection_mueller(double n1, double n2, double ci) {
    FresnelCoefficients f = fresnel(n1, n2, ci);
    return reflection_mueller(f.rs, f.rp);
}

Mueller linear_analyzer(double theta) {
    double c = std::cos(2.0 * theta);
    double s = std::sin(2.0 * theta);

    Mueller M;
    M(0, 0) = 0.5;
    M(0, 1) = 0.5 * c;
    M(0, 2) = 0.5 * s;

    M(1, 0) = 0.5 * c;
    M(1, 1) = 0.5 * c * c;
    M(1, 2) = 0.5 * c * s;

    M(2, 0) = 0.5 * s;
    M(2, 1) = 0.5 * s * c;
    M(2, 2) = 0.5 * s * s;
    return M;
}

double analyzer_intensity(const Stokes& s, double theta) {
    return 0.5 * (s.I + s.Q * std::cos(2.0 * theta)
                    + s.U * std::sin(2.0 * theta));
}

std::array<double, 3> analyzer_intensities(const StokesRGB& s, double theta) {
    return {
        analyzer_intensity(s[0], theta),
        analyzer_intensity(s[1], theta),
        analyzer_intensity(s[2], theta)
    };
}

PolarizationState make_world_state(const StokesRGB& stokes, const Vec3& k) {
    Vec3 kk = normalize(k);
    return {stokes, kk, world_reference_axis(kk)};
}

double reference_angle(const PolarizationState& state, const Vec3& target_e1) {
    Vec3 k = normalize(state.k);
    Vec3 old_e1 = projected_reference_axis(k, state.e1);
    Vec3 old_e2 = cross(k, old_e1);
    Vec3 new_e1 = projected_reference_axis(k, target_e1);
    return std::atan2(dot(new_e1, old_e2), dot(new_e1, old_e1));
}

PolarizationState rotate_reference(const PolarizationState& state, double psi) {
    Vec3 k = normalize(state.k);
    Vec3 old_e1 = projected_reference_axis(k, state.e1);
    Vec3 old_e2 = cross(k, old_e1);

    double c1 = std::cos(psi);
    double s1 = std::sin(psi);
    Vec3 new_e1 = normalize(c1 * old_e1 + s1 * old_e2);

    double c2 = std::cos(2.0 * psi);
    double s2 = std::sin(2.0 * psi);

    PolarizationState out;
    out.k = k;
    out.e1 = new_e1;
    for (int ch = 0; ch < 3; ++ch) {
        const Stokes& a = state.stokes[ch];
        Stokes& b = out.stokes[ch];
        b.I = a.I;
        b.V = a.V;
        b.Q = c2 * a.Q + s2 * a.U;
        b.U = -s2 * a.Q + c2 * a.U;
    }
    return out;
}

PolarizationState change_basis(const PolarizationState& state,
                               const Vec3& new_e1) {
    return rotate_reference(state, reference_angle(state, new_e1));
}

bool physically_valid(const Stokes& s, double tolerance) {
    if (!std::isfinite(s.I) || !std::isfinite(s.Q) ||
        !std::isfinite(s.U) || !std::isfinite(s.V))
        return false;
    if (s.I < 0.0) return false;
    double scale = std::max(1.0, s.I);
    double cone = std::sqrt(s.Q * s.Q + s.U * s.U + s.V * s.V);
    return cone <= s.I + tolerance * scale + 1e-12;
}

bool physically_valid(const StokesRGB& s, double tolerance) {
    for (int ch = 0; ch < 3; ++ch)
        if (!physically_valid(s[ch], tolerance)) return false;
    return true;
}

} // namespace pol

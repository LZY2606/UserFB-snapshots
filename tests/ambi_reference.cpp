#include "ambi_reference.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace ambiref {

constexpr double kPi = std::numbers::pi;
constexpr double kSqrt2 = std::numbers::sqrt2;
constexpr double kSqrt3 = std::numbers::sqrt3;

auto dot3(const std::array<double,3> &a, const std::array<double,3> &b) noexcept -> double
{ return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

auto cross3(const std::array<double,3> &a, const std::array<double,3> &b) noexcept
    -> std::array<double,3>
{ return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }

auto normalized(std::array<double,3> v) -> std::array<double,3>
{
    const double len = std::sqrt(dot3(v, v));
    if(len < 1e-15) throw std::runtime_error("degenerate orientation vector");
    return {v[0]/len, v[1]/len, v[2]/len};
}

AmbiReference::AmbiReference(int order)
    : mOrder{order}
{
    if(order < 0 || order > 4) throw std::runtime_error("unsupported ambisonic order");
}

void AmbiReference::buildLegendre(double cosTheta) const
{
    if(mLegendre.size() == static_cast<std::size_t>(mOrder+1)
        && std::abs(cosTheta - mCachedCosTheta) < 1e-15)
        return;
    // Associated Legendre functions P_l^m(x) WITHOUT the Condon-Shortley
    // phase (the convention used by the N3D/SN3D ambisonics harmonics), via
    // the standard recurrences with the leading sign dropped:
    //   P_0^0 = 1
    //   P_m^m = (2m-1) sin(theta) P_{m-1}^{m-1}
    //   P_{m+1}^m = (2m+1) cos(theta) P_m^m
    //   (l-m) P_l^m = (2l-1) x P_{l-1}^m - (l+m-1) P_{l-2}^m
    const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta*cosTheta));
    mLegendre.assign(static_cast<std::size_t>(mOrder+1),
        std::vector<double>(static_cast<std::size_t>(mOrder+1), 0.0));
    mLegendre[0][0] = 1.0;
    if(mOrder >= 1)
        mLegendre[1][0] = cosTheta;
    for(int l=2; l<=mOrder; ++l)
    {
        const auto ld = static_cast<std::size_t>(l);
        mLegendre[ld][0] = ((2.0*l - 1.0)*cosTheta*mLegendre[ld-1][0]
            - (l - 1.0)*mLegendre[ld-2][0]) / l;
    }
    for(int m=1; m<=mOrder; ++m)
    {
        const auto md = static_cast<std::size_t>(m);
        mLegendre[md][md] = (2.0*m - 1.0) * sinTheta * mLegendre[md-1][md-1];
        if(m+1 <= mOrder)
        {
            mLegendre[md+1][md] = (2.0*m + 1.0) * cosTheta * mLegendre[md][md];
            for(int l=m+2; l<=mOrder; ++l)
            {
                const auto ld = static_cast<std::size_t>(l);
                mLegendre[ld][md] = ((2.0*l - 1.0)*cosTheta*mLegendre[ld-1][md]
                    - (l + m - 1.0)*mLegendre[ld-2][md]) / (l - m);
            }
        }
    }
    mCachedCosTheta = cosTheta;
}

double AmbiReference::realSH(std::size_t acn, const DirA &d) const noexcept
{
    const int n = degreeOf(acn);
    const int m = orderOf(acn);

    // Spherical coordinates of the Ambisonics-frame unit vector: theta from
    // +Z, phi from +X toward +Y.
    const double r = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    const double cosTheta = r > 0.0 ? d[2]/r : 1.0;
    const double phi = std::atan2(d[1], d[0]);

    buildLegendre(cosTheta);
    const double pnm = legendre(n, m >= 0 ? m : -m);

    // Ambisonics N3D normalization (Y_0^0 = 1, no 1/sqrt(4*pi) factor):
    //   N_n^m = sqrt((2n+1) * (n-|m|)!/(n+|m|)!)
    // with the real harmonic convention:
    //   m > 0 -> sqrt(2) N_n^m cos(m phi)
    //   m < 0 -> sqrt(2) N_n^{|m|} sin(|m| phi)
    //   m = 0 -> N_n^0 P_n^0
    double factorialRatio = 1.0;
    for(int k = n - std::abs(m) + 1; k <= n + std::abs(m); ++k)
        factorialRatio /= static_cast<double>(k);
    const double n3d = std::sqrt((2.0*n + 1.0) * factorialRatio);

    if(m > 0)
        return kSqrt2 * n3d * pnm * std::cos(static_cast<double>(m)*phi);
    if(m < 0)
        return kSqrt2 * n3d * pnm * std::sin(static_cast<double>(-m)*phi);
    return n3d * pnm;
}

double AmbiReference::inputScale(InputRep rep, std::size_t acn) noexcept
{
    if(rep == InputRep::AcnN3d)
        return 1.0;
    const int n = degreeOf(acn);
    if(rep == InputRep::AcnSn3d)
    {
        // SN3D = N3D / sqrt(2n+1), so an SN3D-coded component must be
        // multiplied by 1/sqrt(2n+1) when synthesized from N3D.
        return 1.0 / std::sqrt(static_cast<double>(2*n + 1));
    }
    // FuMa (first order). W uses maxN's 1/sqrt(2), the XYZ components use
    // SN3D scaling.
    if(acn == 0)
        return 1.0 / kSqrt2;
    return 1.0 / kSqrt3;
}

std::size_t AmbiReference::inputSlot(InputRep rep, std::size_t acn) noexcept
{
    if(rep != InputRep::FuMa)
        return acn;
    // First-order FuMa frame order is W, X, Y, Z where X/Y/Z are ACN
    // channels 3, 1, 2.
    static constexpr std::array<std::size_t,4> fumaSlotForAcn{{0, 2, 3, 1}};
    return fumaSlotForAcn[acn];
}

DirA AmbiReference::rotateDir(const DirA &localDir, const Orientation &source,
    const Orientation &listener, bool headRelative)
{
    // Build the source's local frame in OpenAL world coordinates. 'at' is the
    // forward axis; right = at x up, matching the library's N.cross_product(V).
    const auto nSrc = normalized(source.at);
    const auto vSrc = normalized(source.up);
    const auto uSrc = normalized(cross3(nSrc, vSrc));

    // A local OpenAL vector p=(px,py,pz) is expressed along the local right,
    // up and forward(-Z) axes, so its world coordinates are the basis-vector
    // combination q = px*U + py*V - pz*N. (Dot products would project p onto
    // the basis instead of recombining the basis vectors.)
    auto toWorld = [&](const std::array<double,3> &p) noexcept -> std::array<double,3> {
        return std::array<double,3>{
            p[0]*uSrc[0] + p[1]*vSrc[0] - p[2]*nSrc[0],
            p[0]*uSrc[1] + p[1]*vSrc[1] - p[2]*nSrc[1],
            p[0]*uSrc[2] + p[1]*vSrc[2] - p[2]*nSrc[2]};
    };

    auto worldAl = toWorld(localDir);

    if(!headRelative)
    {
        // World sources are transformed by the library into the listener's
        // local frame, and the output B-Format field is encoded in that frame
        // (turning the listener makes a fixed world field rotate relative to
        // it). The context matrix maps a world vector to listener-local
        // coordinates (U_L.dot, V_L.dot, -N_L.dot).
        const auto nLis = normalized(listener.at);
        const auto vLis = normalized(listener.up);
        const auto uLis = normalized(cross3(nLis, vLis));
        worldAl = {
            dot3(uLis, worldAl),
            dot3(vLis, worldAl),
            -dot3(nLis, worldAl)};
    }
    return openALToAmbi(worldAl);
}

void AmbiReference::selfTest()
{
    AmbiReference ref{1};
    const auto check = [](double got, double want, const char *name) {
        if(std::abs(got - want) > 1e-12)
            throw std::runtime_error(std::string{"reference sign check failed: "} + name);
    };

    // Constant term is 1 everywhere in the ambisonics N3D convention.
    check(ref.realSH(0, {1.0, 0.0, 0.0}), 1.0, "Y00");

    // First-order axial fields. In the Ambisonics frame:
    //   ACN 1 = sqrt(3) y, ACN 2 = sqrt(3) z, ACN 3 = sqrt(3) x.
    const double k = kSqrt3;
    check(ref.realSH(1, {0.0, 1.0, 0.0}),  k, "Y1-1 (+Y)");
    check(ref.realSH(1, {0.0,-1.0, 0.0}), -k, "Y1-1 (-Y)");
    check(ref.realSH(2, {0.0, 0.0, 1.0}),  k, "Y10 (+Z)");
    check(ref.realSH(2, {0.0, 0.0,-1.0}), -k, "Y10 (-Z)");
    check(ref.realSH(3, {1.0, 0.0, 0.0}),  k, "Y11 (+X)");
    check(ref.realSH(3, {-1.0, 0.0, 0.0}), -k, "Y11 (-X)");
}

} // namespace ambiref

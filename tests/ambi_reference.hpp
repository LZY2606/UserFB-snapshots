// Independent Ambisonics reference used by the offline loopback regression.
//
// This header and its .cpp deliberately do not include any OpenAL Soft
// internal header (in particular not core/ambidefs.h), do not call the
// library's rotator, and do not copy its coefficient tables. Everything here
// is derived from the published definition of the real orthonormal
// (N3D) spherical harmonics evaluated in double precision.
#ifndef TESTS_AMBI_REFERENCE_HPP
#define TESTS_AMBI_REFERENCE_HPP

#include <array>
#include <cstddef>
#include <vector>

namespace ambiref {

/* Input representations exercised by the regression. The loopback device
 * itself is always opened with ACN/N3D output.
 */
enum class InputRep {
    FuMa,      // First order only: FuMa channel order with FuMa scaling.
    AcnSn3d,   // ACN channel order, SN3D (Schmidt semi-normalized) scaling.
    AcnN3d,    // ACN channel order, full N3D orthonormal scaling.
};

/* A right/up/at orientation given in OpenAL world coordinates.
 * OpenAL axes: +X right, +Y up, -Z forward. The default orientation is
 * at=(0,0,-1), up=(0,1,0).
 */
struct Orientation {
    std::array<double,3> at{{0.0, 0.0, -1.0}};
    std::array<double,3> up{{0.0, 1.0, 0.0}};
};

/* A unit direction expressed in Ambisonics coordinates:
 * Ambisonics +X forward, +Y left, +Z up. Spherical coordinates used below
 * are theta = polar angle from +Z (0..pi), phi = azimuth from +X toward +Y.
 */
using DirA = std::array<double,3>;

/* ACN index of degree n, order m (-n <= m <= n): n*(n+1)+m. */
[[nodiscard]] constexpr auto acnIndex(int n, int m) noexcept -> std::size_t
{ return static_cast<std::size_t>(n*n + n + m); }

[[nodiscard]] constexpr auto degreeOf(std::size_t acn) noexcept -> int
{
    int n{0};
    while(static_cast<std::size_t>((n+1)*(n+1)) <= acn) ++n;
    return n;
}

[[nodiscard]] constexpr auto orderOf(std::size_t acn) noexcept -> int
{ return static_cast<int>(acn) - degreeOf(acn)*(degreeOf(acn)+1); }

// Small vector helpers used both by the reference and by the test's signal
// generation. All operate on unit-length OpenAL/Ambisonics direction triples.
[[nodiscard]] auto dot3(const std::array<double,3> &a, const std::array<double,3> &b) noexcept
    -> double;
[[nodiscard]] auto cross3(const std::array<double,3> &a, const std::array<double,3> &b) noexcept
    -> std::array<double,3>;
[[nodiscard]] auto normalized(std::array<double,3> v) -> std::array<double,3>;

class AmbiReference {
public:
    explicit AmbiReference(int order);

    int order() const noexcept { return mOrder; }
    std::size_t channels() const noexcept
    { return static_cast<std::size_t>((mOrder+1)*(mOrder+1)); }

    /* Real N3D spherical harmonic Y_n^m for ACN channel 'acn', evaluated at
     * the unit Ambisonics-coordinate direction d.
     */
    [[nodiscard]] auto realSH(std::size_t acn, const DirA &d) const noexcept -> double;

    /* Multipliers that turn an ACN/N3D signal into the given input
     * representation (the reverse of what the library must undo). FuMa also
     * needs the FuMa channel permutation, see fumaInputChannel.
     */
    [[nodiscard]] static auto inputScale(InputRep rep, std::size_t acn) noexcept -> double;

    /* Given an ACN channel, which physical input buffer slot carries it for
     * the given representation. Identity for ACN layouts; FuMa permutation
     * for first order.
     */
    [[nodiscard]] static auto inputSlot(InputRep rep, std::size_t acn) noexcept -> std::size_t;

    /* Rotate an Ambisonics-coordinate direction fixed in a source's local
     * frame into world Ambisonics coordinates. World sources get both the
     * listener and source orientation; head-relative sources ignore the
     * listener orientation. All vectors are derived geometrically, then the
     * harmonics are re-evaluated on the transformed direction (no rotation
     * matrix recursion).
     */
    [[nodiscard]] static auto rotateDir(const DirA &localDir, const Orientation &source,
        const Orientation &listener, bool headRelative) -> DirA;

    /* Verifies the signs of this implementation against closed-form values:
     * the constant term and the three axial first-order fields. Throws on
     * failure.
     */
    static void selfTest();

private:
    int mOrder;
    // Associated Legendre functions P_l^m(cos(theta)), indexed [l][m],
    // 0 <= m <= l <= mOrder, built with the standard recurrence including
    // the Condon-Shortley phase. The table is cached per evaluation direction
    // (all channels of one frame share the same polar angle), so it is
    // logically mutable state.
    mutable std::vector<std::vector<double>> mLegendre;
    mutable double mCachedCosTheta{2.0};

    void buildLegendre(double cosTheta) const;
    [[nodiscard]] auto legendre(int n, int m) const noexcept -> double
    { return mLegendre[static_cast<std::size_t>(n)][static_cast<std::size_t>(m)]; }
};

/* Convert an OpenAL-coordinate unit direction to Ambisonics coordinates.
 * Ambisonics Y = OpenAL -X, Ambisonics Z = OpenAL +Y, Ambisonics X =
 * OpenAL -Z.
 */
[[nodiscard]] inline auto openALToAmbi(const std::array<double,3> &al) noexcept -> DirA
{ return DirA{-al[2], -al[0], al[1]}; }

} // namespace ambiref

#endif // TESTS_AMBI_REFERENCE_HPP

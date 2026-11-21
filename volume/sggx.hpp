#pragma once

//-----------------------------------------------------------------------------
// SGGX microflake distribution (specular microflakes; no Fresnel term, no
// surface cosine term, no Smith masking term), sequential C++17 only.
//
// Direction conventions (unit vectors, world space):
//   d  : direction of travel along a query ray.
//   wi : incident direction pointing back along the ray, wi = -d.
//   wo : direction of travel after scattering.
//   h  = normalize(wi + wo); the zero-measure set wi + wo == 0 evaluates to 0.
//
//   sigma(w) = sqrt(w^T S w)
//   D(m)     = 1 / (pi * sqrt(det S) * (m^T S^-1 m)^2)
//   p(wo|wi) = D(h) / (4 * sigma(wi))   (phase function == directional pdf)
//
// The symmetric tensor S is stored as its six independent components
// (xx, yy, zz, xy, xz, yz). S = s^2 I degenerates to extinction rho * s and
// the uniform sphere phase function 1 / (4 pi).
//-----------------------------------------------------------------------------

#include <cmath>

namespace microflake {

	constexpr double kPi = 3.14159265358979323846;

	//-------------------------------------------------------------------------
	// Vec3: minimal self-contained vector type (C++17 standard library only)
	//-------------------------------------------------------------------------

	struct Vec3 {

		double x, y, z;

		[[nodiscard]]
		constexpr double operator[](int i) const noexcept {
			return (&x)[i];
		}
	};

	[[nodiscard]]
	constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept {
		return { a.x + b.x, a.y + b.y, a.z + b.z };
	}

	[[nodiscard]]
	constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept {
		return { a.x - b.x, a.y - b.y, a.z - b.z };
	}

	[[nodiscard]]
	constexpr Vec3 operator-(const Vec3& a) noexcept {
		return { -a.x, -a.y, -a.z };
	}

	[[nodiscard]]
	constexpr Vec3 operator*(const Vec3& a, double s) noexcept {
		return { a.x * s, a.y * s, a.z * s };
	}

	[[nodiscard]]
	constexpr Vec3 operator*(double s, const Vec3& a) noexcept {
		return a * s;
	}

	[[nodiscard]]
	inline Vec3 operator/(const Vec3& a, double s) noexcept {
		return a * (1.0 / s);
	}

	[[nodiscard]]
	constexpr double Dot(const Vec3& a, const Vec3& b) noexcept {
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	[[nodiscard]]
	constexpr Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
		return {
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x
		};
	}

	[[nodiscard]]
	constexpr double Norm2(const Vec3& a) noexcept {
		return Dot(a, a);
	}

	[[nodiscard]]
	inline double Norm(const Vec3& a) noexcept {
		return std::sqrt(Norm2(a));
	}

	[[nodiscard]]
	inline Vec3 Normalize(const Vec3& a) noexcept {
		return a / Norm(a);
	}

	//-------------------------------------------------------------------------
	// Sggx
	//-------------------------------------------------------------------------

	class Sggx {

	public:

		// Components of the symmetric positive definite tensor S in the order
		// (xx, yy, zz, xy, xz, yz). Throws std::invalid_argument unless every
		// component is finite and S is positive definite.
		Sggx(double xx, double yy, double zz,
		     double xy, double xz, double yz);

		// sigma(w) = sqrt(w^T S w) for a unit direction w. This is also the
		// projected area of the microflakes along w.
		[[nodiscard]]
		double Sigma(const Vec3& w) const noexcept;

		// Projected area of the microflake distribution along unit direction
		// w: integral_{m.w > 0} D(m) (m.w) dm == Sigma(w).
		[[nodiscard]]
		double ProjectedArea(const Vec3& w) const noexcept {
			return Sigma(w);
		}

		// D(m) = 1 / (pi * sqrt(det S) * (m^T S^-1 m)^2) for a unit m.
		[[nodiscard]]
		double D(const Vec3& m) const noexcept;

		// p(wo|wi) = D(normalize(wi + wo)) / (4 sigma(wi)); returns 0 on the
		// zero-measure set wi + wo == 0. wi and wo are unit directions.
		[[nodiscard]]
		double Phase(const Vec3& wo, const Vec3& wi) const noexcept;

		// Directional pdf of the scattered direction; identical to Phase.
		[[nodiscard]]
		double Pdf(const Vec3& wo, const Vec3& wi) const noexcept {
			return Phase(wo, wi);
		}

		// Pdf of a visible microflake normal m given incident direction wi:
		// D(m) max(0, m.wi) / sigma(wi).
		[[nodiscard]]
		double VisibleNormalPdf(const Vec3& m, const Vec3& wi) const noexcept;

		// Exact visible-normal sampling from two caller supplied uniforms in
		// [0,1). No global random source, no rejection resampling.
		[[nodiscard]]
		Vec3 SampleVisibleNormal(const Vec3& wi, double u1, double u2) const noexcept;

		// Samples a visible normal m and reflects wi about it:
		// wo = 2 (m.wi) m - wi. pdf is the directional pdf of wo (== Phase).
		[[nodiscard]]
		Vec3 SampleReflection(const Vec3& wi, double u1, double u2, double& pdf) const noexcept;

		// Six components of S in the order (xx, yy, zz, xy, xz, yz).
		[[nodiscard]]
		const double* Components() const noexcept {
			return m_s;
		}

		[[nodiscard]]
		double SqrtDet() const noexcept {
			return m_sqrt_det;
		}

	private:

		double m_s[6];     // xx, yy, zz, xy, xz, yz
		double m_inv[6];   // S^-1 in the same order
		double m_chol[6];  // Cholesky L (S = L L^T): l00, l10, l11, l20, l21, l22
		double m_sqrt_det; // sqrt(det S)
	};

	// Eigenvalues (descending) of the symmetric 3x3 matrix given as
	// (xx, yy, zz, xy, xz, yz).
	void SymmetricEigenvalues3(const double s[6], double out[3]) noexcept;
}

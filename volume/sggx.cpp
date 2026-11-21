#include "sggx.hpp"

#include <stdexcept>

namespace microflake {

	Sggx::Sggx(double xx, double yy, double zz,
	           double xy, double xz, double yz) {
		const double s[6] = { xx, yy, zz, xy, xz, yz };
		for (const double v : s) {
			if (!std::isfinite(v)) {
				throw std::invalid_argument("Sggx: non-finite tensor component");
			}
		}

		// Cholesky factorization S = L L^T; positive pivots <=> SPD.
		const double l00 = std::sqrt(xx);
		if (!(l00 > 0.0)) {
			throw std::invalid_argument("Sggx: tensor is not positive definite");
		}
		const double l10 = xy / l00;
		const double l20 = xz / l00;
		const double d11 = yy - l10 * l10;
		if (!(d11 > 0.0)) {
			throw std::invalid_argument("Sggx: tensor is not positive definite");
		}
		const double l11 = std::sqrt(d11);
		const double l21 = (yz - l10 * l20) / l11;
		const double d22 = zz - l20 * l20 - l21 * l21;
		if (!(d22 > 0.0)) {
			throw std::invalid_argument("Sggx: tensor is not positive definite");
		}
		const double l22 = std::sqrt(d22);

		m_chol[0] = l00; m_chol[1] = l10; m_chol[2] = l11;
		m_chol[3] = l20; m_chol[4] = l21; m_chol[5] = l22;
		m_sqrt_det = l00 * l11 * l22;

		// Inverse of the symmetric matrix
		//     [ xx xy xz ]
		// S = [ xy yy yz ]
		//     [ xz yz zz ]
		// via its adjugate.
		const double c00 =  yy * zz - yz * yz;
		const double c01 =  xz * yz - xy * zz;
		const double c02 =  xy * yz - xz * yy;
		const double c11 =  xx * zz - xz * xz;
		const double c12 =  xy * xz - xx * yz;
		const double c22 =  xx * yy - xy * xy;
		const double det = xx * c00 + xy * c01 + xz * c02;
		if (!(det > 0.0) || !std::isfinite(det)) {
			throw std::invalid_argument("Sggx: tensor is not positive definite");
		}
		const double inv_det = 1.0 / det;

		m_s[0] = xx; m_s[1] = yy; m_s[2] = zz;
		m_s[3] = xy; m_s[4] = xz; m_s[5] = yz;
		m_inv[0] = c00 * inv_det; m_inv[1] = c11 * inv_det; m_inv[2] = c22 * inv_det;
		m_inv[3] = c01 * inv_det; m_inv[4] = c02 * inv_det; m_inv[5] = c12 * inv_det;
	}

	namespace {

		[[nodiscard]]
		constexpr double QuadraticForm(const double m[6], const Vec3& v) noexcept {
			return m[0] * v.x * v.x + m[1] * v.y * v.y + m[2] * v.z * v.z
			     + 2.0 * (m[3] * v.x * v.y + m[4] * v.x * v.z + m[5] * v.y * v.z);
		}
	}

	double Sggx::Sigma(const Vec3& w) const noexcept {
		return std::sqrt(QuadraticForm(m_s, w));
	}

	double Sggx::D(const Vec3& m) const noexcept {
		const double q = QuadraticForm(m_inv, m);
		return 1.0 / (kPi * m_sqrt_det * q * q);
	}

	double Sggx::Phase(const Vec3& wo, const Vec3& wi) const noexcept {
		const Vec3 hsum = wi + wo;
		const double n2 = Norm2(hsum);
		if (!(n2 > 0.0)) {
			// Zero-measure antipodal set wi + wo == 0.
			return 0.0;
		}
		const Vec3 h = hsum / std::sqrt(n2);
		return D(h) / (4.0 * Sigma(wi));
	}

	double Sggx::VisibleNormalPdf(const Vec3& m, const Vec3& wi) const noexcept {
		const Vec3 w = Normalize(wi);
		const double c = Dot(m, w);
		if (!(c > 0.0)) {
			return 0.0;
		}
		return D(m) * c / Sigma(w);
	}

	Vec3 Sggx::SampleVisibleNormal(const Vec3& wi, double u1, double u2) const noexcept {
		const Vec3 w = Normalize(wi);

		// In the space stretched by L^-1 the visible-normal distribution is
		// cosine weighted about w' = normalize(L^T wi): sample u on the
		// hemisphere about w' with density (u.w')/pi, then m = normalize(L u)
		// has density D(m) (m.wi) / sigma(wi). Exact, no rejection.
		Vec3 wp = {
			m_chol[0] * w.x + m_chol[1] * w.y + m_chol[3] * w.z,
			m_chol[2] * w.y + m_chol[4] * w.z,
			m_chol[5] * w.z
		};
		wp = Normalize(wp);

		const Vec3 a  = (std::fabs(wp.x) > 0.1) ? Vec3{ 0.0, 1.0, 0.0 } : Vec3{ 1.0, 0.0, 0.0 };
		const Vec3 t1 = Normalize(Cross(a, wp));
		const Vec3 t2 = Cross(wp, t1);

		const double cos_theta = std::sqrt(1.0 - u1 > 0.0 ? 1.0 - u1 : 0.0);
		const double sin_theta = std::sqrt(u1 > 0.0 ? u1 : 0.0);
		const double phi = 2.0 * kPi * u2;
		const Vec3 u = t1 * (sin_theta * std::cos(phi))
		             + t2 * (sin_theta * std::sin(phi))
		             + wp * cos_theta;

		const Vec3 m = {
			m_chol[0] * u.x,
			m_chol[1] * u.x + m_chol[2] * u.y,
			m_chol[3] * u.x + m_chol[4] * u.y + m_chol[5] * u.z
		};
		return Normalize(m);
	}

	Vec3 Sggx::SampleReflection(const Vec3& wi, double u1, double u2, double& pdf) const noexcept {
		const Vec3 w = Normalize(wi);
		const Vec3 m = SampleVisibleNormal(w, u1, u2);
		pdf = D(m) / (4.0 * Sigma(w));
		return (2.0 * Dot(m, w)) * m - w;
	}

	void SymmetricEigenvalues3(const double s[6], double out[3]) noexcept {
		// Closed-form eigenvalues of a symmetric 3x3 (Smith's trigonometric
		// method): robust for the well conditioned spectra used here.
		const double a = s[0], b = s[1], c = s[2];
		const double d = s[3], e = s[4], f = s[5];

		const double p1 = d * d + e * e + f * f;
		const double q  = (a + b + c) / 3.0;
		const double p2 = (a - q) * (a - q) + (b - q) * (b - q) + (c - q) * (c - q) + 2.0 * p1;
		const double p  = std::sqrt(p2 / 6.0);

		if (!(p > 0.0)) {
			// Scalar matrix.
			out[0] = out[1] = out[2] = q;
			return;
		}

		// B = (A - q I) / p, det(B) / 2 in [-1, 1].
		const double b00 = (a - q) / p, b01 = d / p,       b02 = e / p;
		const double b10 = b01,         b11 = (b - q) / p, b12 = f / p;
		const double b20 = b02,         b21 = b12,         b22 = (c - q) / p;
		const double det_b =
			b00 * (b11 * b22 - b12 * b21)
		  - b01 * (b10 * b22 - b12 * b20)
		  + b02 * (b10 * b21 - b11 * b20);
		const double r = det_b / 2.0;

		double phi;
		if (r <= -1.0) {
			phi = kPi / 3.0;
		}
		else if (r >= 1.0) {
			phi = 0.0;
		}
		else {
			phi = std::acos(r) / 3.0;
		}

		const double eig1 = q + 2.0 * p * std::cos(phi);
		const double eig3 = q + 2.0 * p * std::cos(phi + 2.0 * kPi / 3.0);
		const double eig2 = 3.0 * q - eig1 - eig3;

		out[0] = eig1; // largest
		out[1] = eig2;
		out[2] = eig3; // smallest
	}
}

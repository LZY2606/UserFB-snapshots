// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2026 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#pragma once

#include <Jolt/Physics/Body/Body.h>

JPH_NAMESPACE_BEGIN

/// Storage for the first body of the elliptical friction constraint part, specialized per motion type.
/// The constructor is never called, this is just plain storage embedded in a contact constraint.
template <EMotionType Type1>
class FrictionEllipsePart1
{
};

template <>
class FrictionEllipsePart1<EMotionType::Kinematic>
{
protected:
	// Note: Constructor will not be called
	Float3						mR1xU;											///< r1 x u
	Float3						mR1xV;											///< r1 x v
};

template <>
class FrictionEllipsePart1<EMotionType::Dynamic> : public FrictionEllipsePart1<EMotionType::Kinematic>
{
protected:
	// Note: Constructor will not be called
	Float3						mInvI1_R1xU;									///< inv(I1) * (r1 x u)
	Float3						mInvI1_R1xV;									///< inv(I1) * (r1 x v)
};

/// Storage for the second body of the elliptical friction constraint part, specialized per motion type.
template <EMotionType Type2>
class FrictionEllipsePart2
{
};

template <>
class FrictionEllipsePart2<EMotionType::Kinematic>
{
protected:
	// Note: Constructor will not be called
	Float3						mR2xU;											///< r2 x u
	Float3						mR2xV;											///< r2 x v
};

template <>
class FrictionEllipsePart2<EMotionType::Dynamic> : public FrictionEllipsePart2<EMotionType::Kinematic>
{
protected:
	// Note: Constructor will not be called
	Float3						mInvI2_R2xU;									///< inv(I2) * (r2 x u)
	Float3						mInvI2_R2xV;									///< inv(I2) * (r2 x v)
};

/// A 2x2 tangential (sliding) friction constraint part implementing an elliptical Coulomb friction model.
///
/// Given an orthonormal tangential basis (u, v) on the contact plane, p = (p_u, p_v) is the accumulated tangential
/// impulse in that basis and N is the sum of the accumulated non-penetration impulses of all contact points of the
/// manifold. The allowed impulse region is the ellipse
///
/// \f[
/// (p_u / \mu_{along})^2 + (p_v / \mu_{across})^2 \leq N^2
/// \f]
///
/// The effective inverse mass matrix K is the full 2x2 matrix \f$K = J M^{-1} J^T\f$, including the off-diagonal
/// terms caused by the eccentric contact lever arms and the world space inertia tensors of both bodies. The stop
/// impulse q (the impulse that removes all tangential relative velocity, ignoring the friction limit) is
/// \f$q = p + K^{-1} (J v - b)\f$ (using the same lambda sign convention as ContactConstraintPart). The applied
/// impulse is the point inside the ellipse closest to q measured in the K norm:
/// \f$\arg\min (p - q)^T K (p - q)\f$. This is NOT equivalent to clamping the two tangential axes separately or to
/// radially scaling an isotropic solution onto the ellipse (K is generally not diagonal).
template <EMotionType Type1, EMotionType Type2>
class FrictionEllipseConstraintPart : public FrictionEllipsePart1<Type1>, public FrictionEllipsePart2<Type2>
{
public:
	/// Deactivate this constraint
	inline void					Deactivate()
	{
		mInvK[0][0] = 0.0f;
		mInvK[0][1] = 0.0f;
		mInvK[1][0] = 0.0f;
		mInvK[1][1] = 0.0f;
		mTotalLambda[0] = 0.0f;
		mTotalLambda[1] = 0.0f;
	}

	/// Check if constraint is active
	inline bool					IsActive() const
	{
		return mInvK[0][0] != 0.0f;
	}

	/// Override the total lagrange multiplier (used to seed the warm start). Components are in the (u, v) basis.
	inline void					SetTotalLambda(float inLambdaU, float inLambdaV)
	{
		mTotalLambda[0] = inLambdaU;
		mTotalLambda[1] = inLambdaV;
	}

	/// Return the accumulated impulse in the (u, v) basis.
	inline float				GetTotalLambdaU() const
	{
		return mTotalLambda[0];
	}
	inline float				GetTotalLambdaV() const
	{
		return mTotalLambda[1];
	}

	/// Get the tangential basis axes
	inline void					GetAxes(Vec3 &outU, Vec3 &outV) const
	{
		outU = Vec3::sLoadFloat3Unsafe(mU);
		outV = Vec3::sLoadFloat3Unsafe(mV);
	}

	/// Calculate the constraint properties.
	/// @param inInvMass1,inInvMass2 Scaled inverse masses (0 for static / kinematic bodies).
	/// @param inInvI1,inInvI2 Scaled world space inverse inertia tensors (zero for static / kinematic bodies).
	/// @param inR1,inR2 Friction application points relative to the center of mass, in world space.
	/// @param inU,inV Orthonormal tangential basis, u is the projected groove direction, v = normal x u.
	/// @param inBiasU,inBiasV Relative surface velocity projected on u and v (works as a velocity bias, same convention as the contact parts).
	/// @param inMuAlong,inMuAcross Ellipse semi-axis coefficients, valid range [0.05, 2].
	JPH_INLINE void				CalculateConstraintProperties(float inInvMass1, Mat44Arg inInvI1, Vec3Arg inR1, float inInvMass2, Mat44Arg inInvI2, Vec3Arg inR2, Vec3Arg inU, Vec3Arg inV, float inBiasU, float inBiasV, float inMuAlong, float inMuAcross)
	{
		JPH_ASSERT(inU.IsNormalized(1.0e-5f));
		JPH_ASSERT(inV.IsNormalized(1.0e-5f));
		JPH_ASSERT(abs(inU.Dot(inV)) < 1.0e-4f);

		inU.StoreFloat3(&mU);
		inV.StoreFloat3(&mV);
		mBias[0] = inBiasU;
		mBias[1] = inBiasV;
		mMuAlong = inMuAlong;
		mMuAcross = inMuAcross;
		mInvMass1 = inInvMass1;
		mInvMass2 = inInvMass2;

		// Calculate the lever arm cross products and their inverse inertia responses for both axes
		Vec3 r1_x_u, r1_x_v, invi1_r1_x_u, invi1_r1_x_v;
		if constexpr (Type1 != EMotionType::Static)
		{
			r1_x_u = inR1.Cross(inU);
			r1_x_v = inR1.Cross(inV);
			r1_x_u.StoreFloat3(&this->mR1xU);
			r1_x_v.StoreFloat3(&this->mR1xV);

			if constexpr (Type1 == EMotionType::Dynamic)
			{
				invi1_r1_x_u = inInvI1.Multiply3x3(r1_x_u);
				invi1_r1_x_v = inInvI1.Multiply3x3(r1_x_v);
				invi1_r1_x_u.StoreFloat3(&this->mInvI1_R1xU);
				invi1_r1_x_v.StoreFloat3(&this->mInvI1_R1xV);
			}
			else
			{
				invi1_r1_x_u = Vec3::sZero();
				invi1_r1_x_v = Vec3::sZero();
			}
		}
		else
		{
			r1_x_u = r1_x_v = invi1_r1_x_u = invi1_r1_x_v = Vec3::sZero();
		}

		Vec3 r2_x_u, r2_x_v, invi2_r2_x_u, invi2_r2_x_v;
		if constexpr (Type2 != EMotionType::Static)
		{
			r2_x_u = inR2.Cross(inU);
			r2_x_v = inR2.Cross(inV);
			r2_x_u.StoreFloat3(&this->mR2xU);
			r2_x_v.StoreFloat3(&this->mR2xV);

			if constexpr (Type2 == EMotionType::Dynamic)
			{
				invi2_r2_x_u = inInvI2.Multiply3x3(r2_x_u);
				invi2_r2_x_v = inInvI2.Multiply3x3(r2_x_v);
				invi2_r2_x_u.StoreFloat3(&this->mInvI2_R2xU);
				invi2_r2_x_v.StoreFloat3(&this->mInvI2_R2xV);
			}
			else
			{
				invi2_r2_x_u = Vec3::sZero();
				invi2_r2_x_v = Vec3::sZero();
			}
		}
		else
		{
			r2_x_u = r2_x_v = invi2_r2_x_u = invi2_r2_x_v = Vec3::sZero();
		}

		// Full 2x2 effective inverse mass matrix K = J M^-1 J^T, including off-diagonal terms
		float inv_mass = inInvMass1 + inInvMass2;
		float k00 = inv_mass + invi1_r1_x_u.Dot(r1_x_u) + invi2_r2_x_u.Dot(r2_x_u);
		float k11 = inv_mass + invi1_r1_x_v.Dot(r1_x_v) + invi2_r2_x_v.Dot(r2_x_v);
		float k01 = invi1_r1_x_u.Dot(r1_x_v) + invi2_r2_x_u.Dot(r2_x_v);

		// Invert K. If the tangential system is degenerate (the contact cannot move tangentially, e.g. all relevant
		// degrees of freedom locked) keep the part deactivated so no undefined impulse is applied.
		float det = k00 * k11 - k01 * k01;
		if (!isfinite(k00) || !isfinite(k11) || !isfinite(k01) || k00 <= FLT_MIN || det <= FLT_MIN)
		{
			Deactivate();
			return;
		}

		float inv_det = 1.0f / det;
		mInvK[0][0] = k11 * inv_det;
		mInvK[0][1] = -k01 * inv_det;
		mInvK[1][0] = -k01 * inv_det;
		mInvK[1][1] = k00 * inv_det;
	}

	/// Warm start using the accumulated impulse of the previous update (already expressed in the current (u, v) basis).
	/// The impulse is clipped against the current ellipse using the warm-started normal impulse bound inN (>= 0) so
	/// that a rotated groove direction, smaller coefficients or a disabled ellipse can never leave a stale impulse.
	JPH_INLINE bool				WarmStart(Vec3 &ioLinearVelocity1, Vec3 &ioAngularVelocity1, Vec3 &ioLinearVelocity2, Vec3 &ioAngularVelocity2, float inWarmStartImpulseRatio, float inN)
	{
		float scaled[2] = { mTotalLambda[0] * inWarmStartImpulseRatio, mTotalLambda[1] * inWarmStartImpulseRatio };
		ProjectToEllipse(scaled, inN, mTotalLambda);
		return ApplyImpulse(mTotalLambda[0], mTotalLambda[1], ioLinearVelocity1, ioAngularVelocity1, ioLinearVelocity2, ioAngularVelocity2);
	}

	/// Solve one velocity constraint iteration.
	/// @param inN Sum of the accumulated non-penetration impulses of all contact points (the friction bound before the friction coefficient is applied).
	/// @return True if an impulse was applied.
	JPH_INLINE bool				SolveVelocityConstraint(Vec3 &ioLinearVelocity1, Vec3 &ioAngularVelocity1, Vec3 &ioLinearVelocity2, Vec3 &ioAngularVelocity2, float inN)
	{
		// Calculate J v - b for both tangential axes
		float jv_u = sCalculateJv(0, Vec3::sLoadFloat3Unsafe(mU), ioLinearVelocity1, ioAngularVelocity1, ioLinearVelocity2, ioAngularVelocity2) - mBias[0];
		float jv_v = sCalculateJv(1, Vec3::sLoadFloat3Unsafe(mV), ioLinearVelocity1, ioAngularVelocity1, ioLinearVelocity2, ioAngularVelocity2) - mBias[1];

		// Stop impulse (no friction bound). Uses the same lambda convention as ContactConstraintPart:
		// the accumulated impulse is applied body1 -= lambda * axis, body2 += lambda * axis, so the stop
		// solution is q = p + K^-1 (J v - b).
		float q[2];
		q[0] = mTotalLambda[0] + (mInvK[0][0] * jv_u + mInvK[0][1] * jv_v);
		q[1] = mTotalLambda[1] + (mInvK[1][0] * jv_u + mInvK[1][1] * jv_v);

		// Find the K-norm closest point inside the ellipse
		float new_lambda[2];
		ProjectToEllipse(q, inN, new_lambda);

		// Only the difference with the accumulated impulse is applied to the velocities
		bool applied = ApplyImpulse(new_lambda[0] - mTotalLambda[0], new_lambda[1] - mTotalLambda[1], ioLinearVelocity1, ioAngularVelocity1, ioLinearVelocity2, ioAngularVelocity2);
		mTotalLambda[0] = new_lambda[0];
		mTotalLambda[1] = new_lambda[1];
		return applied;
	}

private:
	/// Calculate the jacobian multiplied by the velocity along tangential axis inAxisIndex (0 = u, 1 = v).
	JPH_INLINE float				sCalculateJv(int inAxisIndex, Vec3Arg inAxis, Vec3Arg inLinearVelocity1, Vec3Arg inAngularVelocity1, Vec3Arg inLinearVelocity2, Vec3Arg inAngularVelocity2) const
	{
		Vec3 acc;
		if constexpr (Type1 != EMotionType::Static && Type2 != EMotionType::Static)
			acc = inAxis * (inLinearVelocity1 - inLinearVelocity2);
		else if constexpr (Type1 != EMotionType::Static)
			acc = inAxis * inLinearVelocity1;
		else if constexpr (Type2 != EMotionType::Static)
			acc = inAxis * (-inLinearVelocity2);
		else
			JPH_ASSERT(false); // Static vs static is nonsensical!

		if constexpr (Type1 != EMotionType::Static)
		{
			Vec3 r1_x_axis = Vec3::sLoadFloat3Unsafe(inAxisIndex == 0 ? this->mR1xU : this->mR1xV);
			acc += r1_x_axis * inAngularVelocity1;
		}
		if constexpr (Type2 != EMotionType::Static)
		{
			Vec3 r2_x_axis = Vec3::sLoadFloat3Unsafe(inAxisIndex == 0 ? this->mR2xU : this->mR2xV);
			acc -= r2_x_axis * inAngularVelocity2;
		}

		return acc.ReduceSum();
	}

	/// Apply a tangential impulse delta (in the (u, v) basis) to the velocities of both bodies.
	JPH_INLINE bool				ApplyImpulse(float inDeltaU, float inDeltaV, Vec3 &ioLinearVelocity1, Vec3 &ioAngularVelocity1, Vec3 &ioLinearVelocity2, Vec3 &ioAngularVelocity2) const
	{
		if (inDeltaU == 0.0f && inDeltaV == 0.0f)
			return false;

		Vec3 u = Vec3::sLoadFloat3Unsafe(mU);
		Vec3 v = Vec3::sLoadFloat3Unsafe(mV);
		Vec3 linear_impulse = inDeltaU * u + inDeltaV * v;

		if constexpr (Type1 == EMotionType::Dynamic)
		{
			ioLinearVelocity1 -= mInvMass1 * linear_impulse;
			ioAngularVelocity1 -= inDeltaU * Vec3::sLoadFloat3Unsafe(this->mInvI1_R1xU) + inDeltaV * Vec3::sLoadFloat3Unsafe(this->mInvI1_R1xV);
		}
		if constexpr (Type2 == EMotionType::Dynamic)
		{
			ioLinearVelocity2 += mInvMass2 * linear_impulse;
			ioAngularVelocity2 += inDeltaU * Vec3::sLoadFloat3Unsafe(this->mInvI2_R2xU) + inDeltaV * Vec3::sLoadFloat3Unsafe(this->mInvI2_R2xV);
		}
		return true;
	}

	/// Project the unconstrained impulse inQ onto the ellipse
	/// (p_u / mu_along)^2 + (p_v / mu_across)^2 <= inN^2 while minimizing (p - q)^T K (p - q).
	/// From the KKT system (K + t D^-2) p = K q with D = diag(mu), the active-constraint solution is
	/// p_i = q_i / (1 + t (K^-1)_ii / mu_i^2), t >= 0, found with a bisection.
	JPH_INLINE void				ProjectToEllipse(const float inQ[2], float inN, float outP[2]) const
	{
		if (inN <= 0.0f)
		{
			// No normal impulse available, no tangential impulse is allowed.
			outP[0] = 0.0f;
			outP[1] = 0.0f;
			return;
		}

		float mu[2] = { mMuAlong, mMuAcross };
		// From (K + t D^-2) p = K q with D = diag(mu): p_i = q_i / (1 + t (K^-1)_ii / mu_i^2)
		float w[2] = { mInvK[0][0] / Square(mu[0]), mInvK[1][1] / Square(mu[1]) };

		auto evaluate = [&](float inT, float outP[2]) -> float
		{
			float g = 0.0f;
			for (int i = 0; i < 2; ++i)
			{
				outP[i] = inQ[i] / (1.0f + inT * w[i]);
				float scaled = outP[i] / mu[i];
				g += Square(scaled);
			}
			return g;
		};

		// If the stop impulse is inside the ellipse it is the solution
		float n_sq = Square(inN);
		float p[2];
		if (evaluate(0.0f, p) <= n_sq)
		{
			outP[0] = p[0];
			outP[1] = p[1];
			return;
		}

		// Bracketing: g(t) decreases monotonically towards 0, so grow the upper bound until the ellipse is satisfied.
		float lo = 0.0f, hi = 1.0f;
		float g_hi = evaluate(hi, p);
		for (int i = 0; i < 64 && g_hi > n_sq; ++i)
		{
			lo = hi;
			hi *= 2.0f;
			g_hi = evaluate(hi, p);
		}

		// Bisection on g(t) = N^2. 60 iterations is far beyond float precision, the result stays finite for every valid input.
		for (int i = 0; i < 60; ++i)
		{
			float mid = 0.5f * (lo + hi);
			if (evaluate(mid, p) > n_sq)
				lo = mid;
			else
				hi = mid;
		}

		evaluate(hi, outP);
	}

	Float3						mU;												///< First tangential axis (projected, normalized groove direction)
	Float3						mV;												///< Second tangential axis (normal x u)
	float						mInvK[2][2];									///< Inverse of the effective inverse mass matrix K (the effective mass matrix)
	float						mBias[2];										///< Surface velocity bias in the (u, v) basis
	float						mTotalLambda[2];								///< Accumulated tangential impulse in the (u, v) basis
	float						mMuAlong;										///< Ellipse semi-axis coefficient along u
	float						mMuAcross;										///< Ellipse semi-axis coefficient along v
	float						mInvMass1;										///< Scaled inverse mass of body 1 (0 when not dynamic)
	float						mInvMass2;										///< Scaled inverse mass of body 2 (0 when not dynamic)
};

JPH_NAMESPACE_END

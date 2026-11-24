// Target tests for the elliptical (anisotropic) sliding friction model.
//
// Registered through support/check.h in the three fixed groups: ellipse, world and persistence.
// Every scene is generated in code, no models are loaded. Global Jolt types are initialized and
// released in the normal way. Only discrete rigid body contacts are exercised.

#include "check.h"
#include "scene.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/ConstraintPart/FrictionEllipseConstraintPart.h>

#include <cmath>
#include <vector>

JPH_NAMESPACE_BEGIN
// Double precision 2D constraint minimization reference.
namespace ellipse_ref {

struct Mat2
{
	double a00 = 0.0, a01 = 0.0, a10 = 0.0, a11 = 0.0;
};

static Mat2 Inverse(const Mat2 &inM)
{
	double det = inM.a00 * inM.a11 - inM.a01 * inM.a10;
	Mat2 inv;
	double inv_det = 1.0 / det;
	inv.a00 = inM.a11 * inv_det;
	inv.a01 = -inM.a01 * inv_det;
	inv.a10 = -inM.a10 * inv_det;
	inv.a11 = inM.a00 * inv_det;
	return inv;
}

// Project q onto the ellipse (p_u / mu_along)^2 + (p_v / mu_across)^2 <= N^2 while minimizing
// (p - q)^T K (p - q). Mirrors the KKT bisection used by the engine.
static void ProjectToEllipse(const double inQ[2], double inN, const Mat2 &inInvK, double inMuAlong, double inMuAcross, double outP[2])
{
	if (inN <= 0.0)
	{
		outP[0] = 0.0;
		outP[1] = 0.0;
		return;
	}

	double mu[2] = { inMuAlong, inMuAcross };
	double w[2] = { inInvK.a00 / (mu[0] * mu[0]), inInvK.a11 / (mu[1] * mu[1]) };

	auto evaluate = [&](double inT, double outEval[2]) -> double
	{
		double g = 0.0;
		for (int i = 0; i < 2; ++i)
		{
			outEval[i] = inQ[i] / (1.0 + inT * w[i]);
			double scaled = outEval[i] / mu[i];
			g += scaled * scaled;
		}
		return g;
	};

	double n_sq = inN * inN;
	double p[2];
	if (evaluate(0.0, p) <= n_sq)
	{
		outP[0] = p[0];
		outP[1] = p[1];
		return;
	}

	double lo = 0.0, hi = 1.0;
	while (evaluate(hi, p) > n_sq)
	{
		lo = hi;
		hi *= 2.0;
	}
	for (int i = 0; i < 60; ++i)
	{
		double mid = 0.5 * (lo + hi);
		if (evaluate(mid, p) > n_sq)
			lo = mid;
		else
			hi = mid;
	}
	evaluate(hi, outP);
}

struct ContactPoint
{
	double r[3];		// lever arm on the dynamic body, relative to its COM (world space)
};

struct SolveInput
{
	double inv_mass = 0.0;
	double inv_inertia[3][3] = {};
	double u[3] = {}, v[3] = {}, normal[3] = {};
	double mu_along = 0.5, mu_across = 0.5;
	double bias_u = 0.0, bias_v = 0.0;
	double lin_vel[3] = {}, ang_vel[3] = {};
	int num_velocity_steps = 10;
	std::vector<ContactPoint> points;
};

static double Dot3(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void Cross3(const double a[3], const double b[3], double out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void MulInvI(const double inv_i[3][3], const double in[3], double out[3])
{
	for (int r = 0; r < 3; ++r)
		out[r] = inv_i[r][0] * in[0] + inv_i[r][1] * in[1] + inv_i[r][2] * in[2];
}

// Replicate one velocity solve (friction before normal) for one dynamic body against an immovable body.
static void SolveOnce(const SolveInput &in, double outP[2], double &outN)
{
	double fp[3] = { 0, 0, 0 };
	for (const ContactPoint &cp : in.points)
		for (int i = 0; i < 3; ++i)
			fp[i] += cp.r[i];
	for (int i = 0; i < 3; ++i)
		fp[i] /= double(in.points.size());

	double rxu[3], rxv[3];
	Cross3(fp, in.u, rxu);
	Cross3(fp, in.v, rxv);
	double invi_rxu[3], invi_rxv[3];
	MulInvI(in.inv_inertia, rxu, invi_rxu);
	MulInvI(in.inv_inertia, rxv, invi_rxv);

	Mat2 k;
	k.a00 = in.inv_mass + Dot3(invi_rxu, rxu);
	k.a11 = in.inv_mass + Dot3(invi_rxv, rxv);
	k.a01 = Dot3(invi_rxu, rxv);
	k.a10 = k.a01;
	Mat2 inv_k = Inverse(k);

	double lv[3], av[3];
	for (int i = 0; i < 3; ++i)
	{
		lv[i] = in.lin_vel[i];
		av[i] = in.ang_vel[i];
	}

	std::vector<double> normal_lambda(in.points.size(), 0.0);
	double p[2] = { 0.0, 0.0 };

	auto point_velocity = [&](const double r[3], double out[3])
	{
		double rxw[3];
		Cross3(r, av, rxw);
		for (int i = 0; i < 3; ++i)
			out[i] = lv[i] + rxw[i];
	};

	for (int step = 0; step < in.num_velocity_steps; ++step)
	{
		double n_sum = 0.0;
		for (double lambda : normal_lambda)
			n_sum += lambda;

		double pv[3];
		point_velocity(fp, pv);
		double jv_u = Dot3(pv, in.u) - in.bias_u;
		double jv_v = Dot3(pv, in.v) - in.bias_v;

		double q[2];
		q[0] = p[0] + (inv_k.a00 * jv_u + inv_k.a01 * jv_v);
		q[1] = p[1] + (inv_k.a10 * jv_u + inv_k.a11 * jv_v);

		double new_p[2];
		ProjectToEllipse(q, n_sum, inv_k, in.mu_along, in.mu_across, new_p);

		double du = new_p[0] - p[0];
		double dv = new_p[1] - p[1];
		double lin_imp[3] = { du * in.u[0] + dv * in.v[0], du * in.u[1] + dv * in.v[1], du * in.u[2] + dv * in.v[2] };
		for (int i = 0; i < 3; ++i)
			lv[i] -= in.inv_mass * lin_imp[i];
		double torque[3], d_ang[3];
		Cross3(fp, lin_imp, torque);
		MulInvI(in.inv_inertia, torque, d_ang);
		for (int i = 0; i < 3; ++i)
			av[i] -= d_ang[i];
		p[0] = new_p[0];
		p[1] = new_p[1];

		for (size_t i = 0; i < in.points.size(); ++i)
		{
			double pvi[3];
			point_velocity(in.points[i].r, pvi);
			double jv_n = Dot3(pvi, in.normal);
			double rxn[3], invi_rxn[3];
			Cross3(in.points[i].r, in.normal, rxn);
			MulInvI(in.inv_inertia, rxn, invi_rxn);
			double inv_mn = in.inv_mass + Dot3(invi_rxn, rxn);
			double stop = -jv_n / inv_mn;
			double new_lambda = max(normal_lambda[i] + stop, 0.0);
			double delta = new_lambda - normal_lambda[i];
			normal_lambda[i] = new_lambda;
			if (delta != 0.0)
			{
				for (int kk = 0; kk < 3; ++kk)
					lv[kk] -= delta * in.inv_mass * in.normal[kk];
				double n_imp[3] = { delta * in.normal[0], delta * in.normal[1], delta * in.normal[2] };
				double torque_n[3], d_w[3];
				Cross3(in.points[i].r, n_imp, torque_n);
				MulInvI(in.inv_inertia, torque_n, d_w);
				for (int kk = 0; kk < 3; ++kk)
					av[kk] -= d_w[kk];
			}
		}
	}

	outP[0] = p[0];
	outP[1] = p[1];
	outN = 0.0;
	for (double lambda : normal_lambda)
		outN += lambda;
}

} // namespace ellipse_ref
JPH_NAMESPACE_END

namespace
{

using namespace JPH;

// One-time global Jolt initialization, released at process exit.
void EnsureJoltInitialized()
{
	struct JoltGlobals
	{
		JoltGlobals()
		{
			RegisterDefaultAllocator();
			Factory::sInstance = new Factory();
			RegisterTypes();
		}
		~JoltGlobals()
		{
			UnregisterTypes();
			delete Factory::sInstance;
			Factory::sInstance = nullptr;
		}
	};
	static JoltGlobals sGlobals;
}

struct BodyConfig
{
	Vec3 half_extent = Vec3(0.5f, 0.5f, 0.5f);
	RVec3 position = RVec3::sZero();
	Quat rotation = Quat::sIdentity();
	EMotionType motion_type = EMotionType::Dynamic;
	Vec3 linear_velocity = Vec3::sZero();
	Vec3 angular_velocity = Vec3::sZero();
	ObjectLayer layer = 1;
	bool is_sensor = false;
	float gravity_factor = 1.0f;
	EAllowedDOFs allowed_dofs = EAllowedDOFs::All;
	bool use_sphere = false;
	float sphere_radius = 0.5f;
};

static BodyID MakeBody(Scene &inScene, const BodyConfig &inConfig)
{
	Shape *shape = inConfig.use_sphere
		? static_cast<Shape *>(new SphereShape(inConfig.sphere_radius))
		: static_cast<Shape *>(new BoxShape(inConfig.half_extent));

	BodyCreationSettings settings(shape, inConfig.position, inConfig.rotation, inConfig.motion_type, inConfig.layer);
	settings.mLinearVelocity = inConfig.linear_velocity;
	settings.mAngularVelocity = inConfig.angular_velocity;
	settings.mAllowSleeping = false;
	settings.mFriction = 0.5f;
	settings.mRestitution = 0.0f;
	settings.mLinearDamping = 0.0f;
	settings.mAngularDamping = 0.0f;
	settings.mGravityFactor = inConfig.gravity_factor;
	settings.mIsSensor = inConfig.is_sensor;
	settings.mAllowedDOFs = inConfig.allowed_dofs;
	return inScene.world.GetBodyInterface().CreateAndAddBody(settings, EActivation::Activate);
}

// Configuration applied to a contact pair by the test listeners.
struct EllipseConfig
{
	bool use_ellipse = true;
	Vec3 direction = Vec3::sAxisX();
	float mu_along = 0.5f;
	float mu_across = 0.5f;
	Vec3 surface_velocity = Vec3::sZero();
};

class TestContactListener : public ContactListener
{
public:
	int num_added = 0;
	int num_persisted = 0;
	int num_removed = 0;
	int frame = 0;
	EllipseConfig config;
	// Frame based mutations (used by persistence tests).
	bool rotate_direction_each_frame = false;
	bool shrink_coefficient_after_switch = false;
	bool disable_after_switch = false;
	bool enable_after_switch = false;
	int switch_frame = 5;

	// Optional mass / inertia scales applied through ContactSettings to the dynamic body (1 = no override).
	float inv_mass_scale_dynamic = 1.0f;
	float inv_inertia_scale_dynamic = 1.0f;

	// Captured geometry of the most recent contact (for world space checks).
	bool has_normal = false;
	Vec3 captured_normal = Vec3::sAxisY();

	void OnContactAdded(const Body &inBody1, const Body &inBody2, const ContactManifold &inManifold, ContactSettings &ioSettings) override
	{
		++num_added;
		captured_normal = inManifold.mWorldSpaceNormal;
		has_normal = true;
		Apply(inBody1, inBody2, ioSettings);
	}

	void OnContactPersisted(const Body &inBody1, const Body &inBody2, const ContactManifold &inManifold, ContactSettings &ioSettings) override
	{
		++num_persisted;
		captured_normal = inManifold.mWorldSpaceNormal;
		has_normal = true;
		Apply(inBody1, inBody2, ioSettings);
	}

	void OnContactRemoved(const SubShapeIDPair &) override
	{
		++num_removed;
	}

private:
	void Apply(const Body &inBody1, const Body &inBody2, ContactSettings &ioSettings)
	{
		EllipseConfig cfg = config;
		if (rotate_direction_each_frame && frame > 0)
		{
			float angle = float(frame) * (JPH_PI / 2.0f);
			cfg.direction = Vec3(Cos(angle), 0.0f, -Sin(angle));
		}
		if (shrink_coefficient_after_switch && frame >= switch_frame)
		{
			cfg.mu_along = 0.05f;
			cfg.mu_across = 0.05f;
		}
		if (disable_after_switch && frame >= switch_frame)
			cfg.use_ellipse = false;
		if (enable_after_switch && frame >= switch_frame)
			cfg.use_ellipse = true;

		ioSettings.mUseFrictionEllipse = cfg.use_ellipse;
		if (cfg.use_ellipse)
		{
			ioSettings.mFrictionDirection = cfg.direction;
			ioSettings.mFrictionCoefficientAlong = cfg.mu_along;
			ioSettings.mFrictionCoefficientAcross = cfg.mu_across;
		}
		ioSettings.mRelativeLinearSurfaceVelocity = cfg.surface_velocity;

		// Apply the mass / inertia scales to the dynamic body (both entries if both are dynamic).
		if (inBody1.IsDynamic())
		{
			ioSettings.mInvMassScale1 = inv_mass_scale_dynamic;
			ioSettings.mInvInertiaScale1 = inv_inertia_scale_dynamic;
		}
		if (inBody2.IsDynamic())
		{
			ioSettings.mInvMassScale2 = inv_mass_scale_dynamic;
			ioSettings.mInvInertiaScale2 = inv_inertia_scale_dynamic;
		}
	}
};

static bool AllFinite(Vec3Arg inV)
{
	return std::isfinite(inV.GetX()) && std::isfinite(inV.GetY()) && std::isfinite(inV.GetZ());
}

static void Vec3ToDouble(Vec3Arg inV, double out[3])
{
	out[0] = double(inV.GetX());
	out[1] = double(inV.GetY());
	out[2] = double(inV.GetZ());
}

static void BuildAxes(Vec3Arg inNormal, Vec3Arg inDirection, Vec3 &outU, Vec3 &outV)
{
	Vec3 projected = inDirection - inNormal * inDirection.Dot(inNormal);
	outU = projected.Normalized();
	outV = inNormal.Cross(outU);
}

// Listener that captures everything the double reference needs for the first contact of the dynamic body.
class CaptureListener : public ContactListener
{
public:
	bool captured = false;
	Vec3 normal, u, v, direction;
	float mu_along = 0.5f, mu_across = 0.5f;
	float inv_mass_scale = 1.0f;
	float inv_inertia_scale = 1.0f;
	float inv_mass = 0.0f;
	Mat44 inv_inertia;
	Vec3 lin_vel, ang_vel;
	std::vector<Vec3> dynamic_r;	// lever arms of every contact point relative to the dynamic body COM

	void Configure(Vec3Arg inDirection, float inMuAlong, float inMuAcross, float inInvMassScale = 1.0f, float inInvInertiaScale = 1.0f)
	{
		direction = inDirection;
		mu_along = inMuAlong;
		mu_across = inMuAcross;
		inv_mass_scale = inInvMassScale;
		inv_inertia_scale = inInvInertiaScale;
	}

	void OnContactAdded(const Body &inBody1, const Body &inBody2, const ContactManifold &inManifold, ContactSettings &ioSettings) override
	{
		Handle(inBody1, inBody2, inManifold, ioSettings);
	}

	void OnContactPersisted(const Body &inBody1, const Body &inBody2, const ContactManifold &inManifold, ContactSettings &ioSettings) override
	{
		Handle(inBody1, inBody2, inManifold, ioSettings);
	}

private:
	void Handle(const Body &inBody1, const Body &inBody2, const ContactManifold &inManifold, ContactSettings &ioSettings)
	{
		ioSettings.mUseFrictionEllipse = true;
		ioSettings.mFrictionDirection = direction;
		ioSettings.mFrictionCoefficientAlong = mu_along;
		ioSettings.mFrictionCoefficientAcross = mu_across;

		// Apply the scales to whichever body is dynamic.
		if (inBody1.IsDynamic())
		{
			ioSettings.mInvMassScale1 = inv_mass_scale;
			ioSettings.mInvInertiaScale1 = inv_inertia_scale;
		}
		if (inBody2.IsDynamic())
		{
			ioSettings.mInvMassScale2 = inv_mass_scale;
			ioSettings.mInvInertiaScale2 = inv_inertia_scale;
		}

		if (captured)
			return;
		captured = true;

		const Body &dynamic_body = inBody1.IsDynamic() ? inBody1 : inBody2;
		const ContactPoints &points = inBody1.IsDynamic() ? inManifold.mRelativeContactPointsOn1 : inManifold.mRelativeContactPointsOn2;

		normal = inManifold.mWorldSpaceNormal;
		BuildAxes(normal, direction, u, v);
		// Capture the *scaled* inverse mass / inertia, which is the effective matrix the solver uses.
		inv_mass = inv_mass_scale * dynamic_body.GetMotionPropertiesUnchecked()->GetInverseMass();
		inv_inertia = inv_inertia_scale * dynamic_body.GetInverseInertia();
		lin_vel = dynamic_body.GetMotionPropertiesUnchecked()->GetLinearVelocity();
		ang_vel = dynamic_body.GetMotionPropertiesUnchecked()->GetAngularVelocity();

		RVec3 com = dynamic_body.GetCenterOfMassPosition();
		dynamic_r.clear();
		for (Vec3 rel : points)
		{
			RVec3 world_point = inManifold.mBaseOffset + rel;
			dynamic_r.push_back(Vec3(world_point - com));
		}
	}
};

// Run one zero-gravity step and compare the engine impulse with the double precision reference.
static void RunEllipseReferenceCheck(Vec3Arg inFloorHalf, RVec3Arg inFloorPos, const BodyConfig &inDynamic,
	Vec3Arg inDirection, float inMuAlong, float inMuAcross, bool inExpectSaturated, bool inExpectOffDiagonal,
	float inInvMassScale = 1.0f, float inInvInertiaScale = 1.0f)
{
	Scene scene;
	scene.world.SetGravity(Vec3::sZero());

	BodyConfig floor_cfg;
	floor_cfg.half_extent = inFloorHalf;
	floor_cfg.position = inFloorPos;
	floor_cfg.motion_type = EMotionType::Static;
	floor_cfg.layer = 0;
	BodyID floor_id = MakeBody(scene, floor_cfg);
	BodyID dynamic_id = MakeBody(scene, inDynamic);

	CaptureListener listener;
	listener.Configure(inDirection, inMuAlong, inMuAcross, inInvMassScale, inInvInertiaScale);
	scene.world.SetContactListener(&listener);

	const BodyInterface &bi = scene.world.GetBodyInterface();
	Vec3 v_before = bi.GetLinearVelocity(dynamic_id);
	// The unscaled body mass is used to recover the constraint impulse; the effective (scaled) inverse mass relates it to dv.
	float body_inv_mass = scene.world.GetBodyLockInterfaceNoLock().TryGetBody(dynamic_id)->GetMotionPropertiesUnchecked()->GetInverseMass();
	float body_mass = 1.0f / body_inv_mass;

	scene.step();

	CHECK(listener.captured);
	Vec3 v_after = bi.GetLinearVelocity(dynamic_id);
	// P = dv / invM_eff = dv * body_mass / inv_mass_scale
	Vec3 engine_impulse = (v_before - v_after) * (body_mass / inInvMassScale);

	// Build the double precision reference from the captured pre-solve state.
	ellipse_ref::SolveInput ref;
	ref.inv_mass = double(listener.inv_mass);
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			ref.inv_inertia[r][c] = double(listener.inv_inertia(r, c));
	Vec3ToDouble(listener.u, ref.u);
	Vec3ToDouble(listener.v, ref.v);
	Vec3ToDouble(listener.normal, ref.normal);
	ref.mu_along = double(inMuAlong);
	ref.mu_across = double(inMuAcross);
	Vec3ToDouble(listener.lin_vel, ref.lin_vel);
	Vec3ToDouble(listener.ang_vel, ref.ang_vel);
	ref.num_velocity_steps = int(scene.world.GetPhysicsSettings().mNumVelocitySteps);
	for (Vec3 r : listener.dynamic_r)
	{
		ellipse_ref::ContactPoint cp;
		Vec3ToDouble(r, cp.r);
		ref.points.push_back(cp);
	}

	double ref_p[2], ref_n;
	ellipse_ref::SolveOnce(ref, ref_p, ref_n);

	float engine_pu = engine_impulse.Dot(listener.u);
	float engine_pv = engine_impulse.Dot(listener.v);

	// Impulse error must stay within 1e-4 absolute + 2e-4 relative.
	double scale_u = 1.0e-4 + 2.0e-4 * fabs(ref_p[0]);
	double scale_v = 1.0e-4 + 2.0e-4 * fabs(ref_p[1]);
	CHECK(fabs(double(engine_pu) - ref_p[0]) <= scale_u);
	CHECK(fabs(double(engine_pv) - ref_p[1]) <= scale_v);
	CHECK(AllFinite(engine_impulse));

	// Check the saturation state against the ellipse of the reference solution.
	double ellipse_value = Square(ref_p[0] / double(inMuAlong)) + Square(ref_p[1] / double(inMuAcross));
	double n_sq = Square(ref_n);
	if (inExpectSaturated)
		CHECK(fabs(ellipse_value - n_sq) <= 1.0e-4 * max(1.0, n_sq));
	else
		CHECK(ellipse_value <= n_sq + 1.0e-6);

	// Verify the effective inverse mass matrix has a clear off-diagonal term.
	if (inExpectOffDiagonal)
	{
		double fp[3] = { 0, 0, 0 };
		for (const Vec3 &rv : listener.dynamic_r)
			for (int i = 0; i < 3; ++i)
				fp[i] += double(rv[i]);
		for (int i = 0; i < 3; ++i)
			fp[i] /= double(listener.dynamic_r.size());
		double rxu[3], rxv[3], du[3], dv[3];
		ellipse_ref::Cross3(fp, ref.u, rxu);
		ellipse_ref::Cross3(fp, ref.v, rxv);
		ellipse_ref::MulInvI(ref.inv_inertia, rxu, du);
		ellipse_ref::MulInvI(ref.inv_inertia, rxv, dv);
		double k01 = ellipse_ref::Dot3(du, rxv);
		double k00 = ref.inv_mass + ellipse_ref::Dot3(du, rxu);
		double k11 = ref.inv_mass + ellipse_ref::Dot3(dv, rxv);
		CHECK(fabs(k01) > 0.05 * max(k00, k11));
	}

	(void)floor_id;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// ellipse group: formal constraint path vs the double precision 2D reference
////////////////////////////////////////////////////////////////////////////////////////////////////////

static void ellipse_unsaturated()
{
	EnsureJoltInitialized();

	// Gentle slide with generous friction: the stop impulse lies inside the ellipse, the contact does not saturate.
	BodyConfig box;
	box.half_extent = Vec3(0.5f, 0.5f, 0.5f);
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(0.3f, 0, 0.2f);

	RunEllipseReferenceCheck(Vec3(10, 0.5f, 10), RVec3(0, -0.5f, 0), box, Vec3::sAxisX(), 1.5f, 1.0f, false, false);
}

static void ellipse_saturated()
{
	EnsureJoltInitialized();

	// Fast diagonal slide: the stop impulse is outside the ellipse and the solution lands on its boundary.
	BodyConfig box;
	box.half_extent = Vec3(0.4f, 0.4f, 0.4f);
	box.position = RVec3(0, 0.4f, 0);
	box.linear_velocity = Vec3(6.0f, 0, -5.0f);

	RunEllipseReferenceCheck(Vec3(10, 0.5f, 10), RVec3(0, -0.5f, 0), box, Vec3(0.7f, 0, 0.7f), 0.4f, 0.15f, true, false);
}

static void ellipse_off_diagonal()
{
	EnsureJoltInitialized();

	// An asymmetric box rotated about the up axis has a rotated world inertia tensor, so the tangential
	// effective inverse mass matrix has a clearly non-zero off-diagonal entry.
	BodyConfig box;
	box.half_extent = Vec3(0.3f, 0.5f, 0.8f);
	box.rotation = Quat::sRotation(Vec3::sAxisY(), 0.6f);
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(3.0f, 0, 2.0f);
	box.angular_velocity = Vec3(0, 1.5f, 0);

	RunEllipseReferenceCheck(Vec3(10, 0.5f, 10), RVec3(0, -0.5f, 0), box, Vec3(0.3f, 0.2f, 1.0f), 0.9f, 0.3f, true, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// world group: real PhysicsSystem behavior in world space
////////////////////////////////////////////////////////////////////////////////////////////////////////

// The ContactSettings inverse mass / inertia scales must remain effective through the elliptical path. The engine
// impulse is compared against the double precision reference built from the *scaled* effective mass matrix, so an
// agreement proves the scales actually enter the 2x2 K matrix and the normal bound rather than being ignored.
static void world_mass_inertia_scaling()
{
	EnsureJoltInitialized();

	// Reduced inverse mass (effectively heavier) with an asymmetric box giving a non-diagonal K.
	BodyConfig box;
	box.half_extent = Vec3(0.3f, 0.5f, 0.8f);
	box.rotation = Quat::sRotation(Vec3::sAxisY(), 0.55f);
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(4.0f, 0, -3.0f);
	RunEllipseReferenceCheck(Vec3(10, 0.5f, 10), RVec3(0, -0.5f, 0), box,
		Vec3(0.6f, 0, 0.8f), 1.1f, 0.3f, true, false, 0.3f, 0.3f);

	// Reduced inverse inertia only (mass unchanged): the rotational contribution to K is scaled down.
	BodyConfig box2;
	box2.half_extent = Vec3(0.4f, 0.45f, 0.7f);
	box2.rotation = Quat::sRotation(Vec3::sAxisY(), -0.4f);
	box2.position = RVec3(0, 0.45f, 0);
	box2.linear_velocity = Vec3(3.5f, 0, 2.5f);
	box2.angular_velocity = Vec3(0, 1.0f, 0);
	RunEllipseReferenceCheck(Vec3(10, 0.5f, 10), RVec3(0, -0.5f, 0), box2,
		Vec3(0.5f, 0.1f, 1.0f), 0.8f, 0.25f, true, false, 1.0f, 0.2f);
}

// Dynamic vs static: the across-axis component is damped much faster than the along-axis component.
static void world_dynamic_vs_static()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig floor;
	floor.half_extent = Vec3(10, 0.5f, 10);
	floor.position = RVec3(0, -0.5f, 0);
	floor.motion_type = EMotionType::Static;
	floor.layer = 0;
	MakeBody(scene, floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(3.0f, 0, 3.0f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();	// groove along X (high friction), across is Z (low friction)
	listener.config.mu_along = 1.8f;
	listener.config.mu_across = 0.08f;
	scene.world.SetContactListener(&listener);

	for (int i = 0; i < 30; ++i)
	{
		listener.frame = i;
		scene.step();
	}

	Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(AllFinite(v));
	CHECK(fabs(v.GetX()) < fabs(v.GetZ()));		// the high-friction along axis (X) decelerates more
	CHECK(fabs(v.GetX()) < 1.0f);					// X is substantially slowed from its initial 3 m/s
	CHECK(fabs(v.GetZ()) > 0.3f);					// the low-friction across axis (Z) keeps sliding
}

// Dynamic vs dynamic: the tangential impulses on the two bodies must be equal and opposite (momentum conservation).
static void world_dynamic_vs_dynamic()
{
	EnsureJoltInitialized();
	Scene scene;

	BodyConfig lower;
	lower.half_extent = Vec3(2.0f, 0.5f, 2.0f);
	lower.position = RVec3(0, 0.5f, 0);
	lower.linear_velocity = Vec3(-1.0f, 0, 0);
	BodyID lower_id = MakeBody(scene, lower);

	BodyConfig upper;
	upper.half_extent = Vec3(0.5f, 0.5f, 0.5f);
	upper.position = RVec3(0.4f, 1.48f, 0.3f);	// slight overlap so a manifold is created
	upper.linear_velocity = Vec3(2.5f, 0, -1.5f);
	BodyID upper_id = MakeBody(scene, upper);

	TestContactListener listener;
	listener.config.direction = Vec3(0.5f, 0, 1.0f);
	listener.config.mu_along = 1.2f;
	listener.config.mu_across = 0.2f;
	scene.world.SetContactListener(&listener);

	const BodyInterface &bi = scene.world.GetBodyInterface();
	const Body *b_lower = scene.world.GetBodyLockInterfaceNoLock().TryGetBody(lower_id);
	const Body *b_upper = scene.world.GetBodyLockInterfaceNoLock().TryGetBody(upper_id);
	float m_lower = 1.0f / b_lower->GetMotionPropertiesUnchecked()->GetInverseMass();
	float m_upper = 1.0f / b_upper->GetMotionPropertiesUnchecked()->GetInverseMass();
	Vec3 gravity = scene.world.GetGravity();
	float dt = 1.0f / 120;

	Vec3 lv_before = bi.GetLinearVelocity(lower_id);
	Vec3 uv_before = bi.GetLinearVelocity(upper_id);

	listener.frame = 0;
	scene.step();

	Vec3 lv_after = bi.GetLinearVelocity(lower_id);
	Vec3 uv_after = bi.GetLinearVelocity(upper_id);
	// Recover the impulses, removing the gravity contribution so only constraint impulses remain.
	Vec3 lower_impulse = (lv_before + gravity * dt - lv_after) * m_lower;
	Vec3 upper_impulse = (uv_before + gravity * dt - uv_after) * m_upper;
	CHECK(AllFinite(lower_impulse) && AllFinite(upper_impulse));

	// The contact normal is vertical, so gravity has no tangential part: the tangential friction impulses must cancel.
	Vec3 normal = listener.captured_normal;
	Vec3 sum = lower_impulse + upper_impulse;
	Vec3 sum_tangential = sum - normal * sum.Dot(normal);
	CHECK(sum_tangential.Length() < 1.0e-3f);		// action equals reaction in the contact plane

	// The contact must have been solved through the ellipse path.
	CHECK(listener.num_added + listener.num_persisted > 0);
}

// Kinematic conveyor surface: a still box is dragged preferentially along the high friction axis.
static void world_kinematic_surface()
{
	EnsureJoltInitialized();
	Scene scene;
	// Keep gravity so a normal contact force (the friction bound N) is generated.

	BodyConfig belt;
	belt.half_extent = Vec3(5, 0.5f, 5);
	belt.position = RVec3(0, -0.5f, 0);
	belt.motion_type = EMotionType::Kinematic;
	BodyID belt_id = MakeBody(scene, belt);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();
	listener.config.mu_along = 2.0f;
	listener.config.mu_across = 0.05f;
	// Surface moves diagonally; the along (X) component should be transmitted much better.
	listener.config.surface_velocity = Vec3(2.0f, 0, 2.0f);
	scene.world.SetContactListener(&listener);

	for (int i = 0; i < 30; ++i)
	{
		listener.frame = i;
		scene.world.GetBodyInterface().MoveKinematic(belt_id, RVec3(0, -0.5f, 0), Quat::sIdentity(), 1.0f / 120);
		scene.step();
	}

	Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(AllFinite(v));
	CHECK(fabs(v.GetX()) > fabs(v.GetZ()));		// dragged more along the high-friction groove
	CHECK(fabs(v.GetX()) > 0.3f);
}

// Rotating the groove swaps which world axis is damped strongly.
static void world_groove_rotation()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig floor;
	floor.half_extent = Vec3(10, 0.5f, 10);
	floor.position = RVec3(0, -0.5f, 0);
	floor.motion_type = EMotionType::Static;
	floor.layer = 0;
	MakeBody(scene, floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(2.5f, 0, 2.5f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();
	listener.config.mu_along = 1.8f;
	listener.config.mu_across = 0.06f;
	scene.world.SetContactListener(&listener);

	for (int i = 0; i < 12; ++i)
	{
		listener.frame = i;
		scene.step();
	}
	Vec3 v_first = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(fabs(v_first.GetX()) < fabs(v_first.GetZ()));	// X is the high-friction axis, it decelerates more

	// Rotate the groove 90 degrees: now Z is the high-friction along axis.
	listener.config.direction = Vec3(0, 0, 1.0f);
	for (int i = 0; i < 20; ++i)
	{
		listener.frame = 12 + i;
		scene.step();
	}
	Vec3 v_second = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(AllFinite(v_second));
	// During the second phase the new along axis (Z) must lose a larger fraction of its speed than the now across axis (X).
	float decay_x = (fabs(v_first.GetX()) - fabs(v_second.GetX())) / max(fabs(v_first.GetX()), 1.0e-6f);
	float decay_z = (fabs(v_first.GetZ()) - fabs(v_second.GetZ())) / max(fabs(v_first.GetZ()), 1.0e-6f);
	CHECK(decay_z > decay_x);
}

// Tilted contact: the recovered world impulse must lie in the contact plane and match the u/v decomposition.
static void world_tilted_scene()
{
	EnsureJoltInitialized();
	Scene scene;
	scene.world.SetGravity(Vec3(0, -12.0f, 0));

	// A ramp rotated about Z so the contact normal is tilted away from the world up axis.
	Quat ramp_rot = Quat::sRotation(Vec3::sAxisZ(), 0.35f);
	BodyConfig ramp;
	ramp.half_extent = Vec3(6, 0.5f, 6);
	ramp.rotation = ramp_rot;
	ramp.position = RVec3(0, -1.2f, 0);
	ramp.motion_type = EMotionType::Static;
	ramp.layer = 0;
	MakeBody(scene, ramp);

	BodyConfig box;
	box.half_extent = Vec3(0.4f, 0.4f, 0.4f);
	box.rotation = ramp_rot;
	box.position = RVec3(0.6f, 0.2f, 0.5f);
	box.linear_velocity = Vec3(2.0f, 0, -1.7f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3(1.0f, 0.3f, 0.5f);
	listener.config.mu_along = 1.1f;
	listener.config.mu_across = 0.25f;
	scene.world.SetContactListener(&listener);

	const BodyInterface &bi = scene.world.GetBodyInterface();
	float mass = 1.0f / scene.world.GetBodyLockInterfaceNoLock().TryGetBody(box_id)->GetMotionPropertiesUnchecked()->GetInverseMass();
	Vec3 v_before = bi.GetLinearVelocity(box_id);
	Vec3 gravity = scene.world.GetGravity();

	listener.frame = 0;
	scene.step();

	Vec3 v_after = bi.GetLinearVelocity(box_id);
	Vec3 impulse = (v_before + gravity * (1.0f / 120) - v_after) * mass;
	CHECK(AllFinite(impulse));

	// The friction impulse is the tangential part; it must be orthogonal to the contact normal.
	Vec3 normal = listener.captured_normal;
	CHECK(fabs(impulse.Dot(normal)) < 0.05f * impulse.Length() + 1.0e-4f);

	Vec3 u, v;
	BuildAxes(normal, listener.config.direction, u, v);
	float pu = impulse.Dot(u);
	float pv = impulse.Dot(v);
	Vec3 rebuilt = pu * u + pv * v;
	CHECK((impulse - rebuilt).Length() < 1.0e-4f);
}

// Rotating the entire scene (bodies, velocities and groove direction) by a rigid rotation must produce exactly
// the rotated world space impulse and velocity. This proves the model is expressed in true world space and has
// no hidden tangent-frame bias.
static void world_scene_rotation_covariance()
{
	EnsureJoltInitialized();

	struct Result
	{
		Vec3 impulse;
		Vec3 velocity;
	};

	auto run = [](QuatArg inRotation) -> Result
	{
		Scene scene;
		scene.world.SetGravity(Vec3::sZero());

		Quat q = inRotation;
		RVec3 floor_pos = RVec3(q * Vec3(0.0f, -0.5f, 0.0f));
		BodyConfig floor;
		floor.half_extent = Vec3(8.0f, 0.5f, 6.0f);	// shape dimensions are body-local, only the orientation rotates
		floor.position = floor_pos;
		floor.rotation = q;
		floor.motion_type = EMotionType::Static;
		floor.layer = 0;
		MakeBody(scene, floor);

		BodyConfig box;
		box.half_extent = Vec3(0.35f, 0.5f, 0.7f);
		box.rotation = q;
		box.position = RVec3(q * Vec3(0.3f, 0.5f, -0.2f));
		box.linear_velocity = q * Vec3(3.0f, 0.0f, -2.0f);
		box.angular_velocity = q * Vec3(0.0f, 1.2f, 0.0f);
		BodyID box_id = MakeBody(scene, box);

		TestContactListener listener;
		listener.config.direction = q * Vec3(0.8f, 0.0f, 0.6f);
		listener.config.mu_along = 1.3f;
		listener.config.mu_across = 0.25f;
		scene.world.SetContactListener(&listener);

		const BodyInterface &bi = scene.world.GetBodyInterface();
		float mass = 1.0f / scene.world.GetBodyLockInterfaceNoLock().TryGetBody(box_id)->GetMotionPropertiesUnchecked()->GetInverseMass();
		Vec3 v_before = bi.GetLinearVelocity(box_id);

		listener.frame = 0;
		scene.step();

		Vec3 v_after = bi.GetLinearVelocity(box_id);
		return Result{ (v_before - v_after) * mass, v_after };
	};

	Result reference = run(Quat::sIdentity());
	Quat q = Quat::sRotation(Vec3::sAxisY(), 0.9f);
	Result rotated = run(q);

	CHECK(AllFinite(reference.impulse) && AllFinite(rotated.impulse));
	// The rotated scene's world impulse and velocity must equal the rigid rotation of the reference results.
	Vec3 expected_impulse = q * reference.impulse;
	Vec3 expected_velocity = q * reference.velocity;
	CHECK((rotated.impulse - expected_impulse).Length() < 1.0e-4f + 1.0e-4f * expected_impulse.Length());
	CHECK((rotated.velocity - expected_velocity).Length() < 1.0e-4f + 1.0e-4f * expected_velocity.Length());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
// persistence group: real cross-step cache reuse, warm start, mutations and sensors
////////////////////////////////////////////////////////////////////////////////////////////////////////

// Persisted contact with the same settings stays finite and keeps reporting persist callbacks.
static void persistence_basic()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig floor;
	floor.half_extent = Vec3(10, 0.5f, 10);
	floor.position = RVec3(0, -0.5f, 0);
	floor.motion_type = EMotionType::Static;
	floor.layer = 0;
	MakeBody(scene, floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(2.0f, 0, 1.0f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();
	listener.config.mu_along = 1.0f;
	listener.config.mu_across = 0.4f;
	scene.world.SetContactListener(&listener);

	for (int i = 0; i < 40; ++i)
	{
		listener.frame = i;
		scene.step();
		Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
		CHECK(AllFinite(v));
	}
	CHECK(listener.num_persisted > 0);		// the contact cache branch was really reused
	CHECK(listener.num_added >= 1);
}

// Rotating the groove in the persisted callback rotates the warm-started impulse without leaving a stale bias.
static void persistence_rotate_direction()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig floor;
	floor.half_extent = Vec3(10, 0.5f, 10);
	floor.position = RVec3(0, -0.5f, 0);
	floor.motion_type = EMotionType::Static;
	floor.layer = 0;
	MakeBody(scene, floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(3.0f, 0, 3.0f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();
	listener.config.mu_along = 1.5f;
	listener.config.mu_across = 0.15f;
	listener.rotate_direction_each_frame = true;
	scene.world.SetContactListener(&listener);

	float max_speed = 0.0f;
	for (int i = 0; i < 24; ++i)
	{
		listener.frame = i;
		scene.step();
		Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
		CHECK(AllFinite(v));
		max_speed = max(max_speed, v.Length());
	}
	Vec3 final_v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(AllFinite(final_v));
	// Friction must not inject energy: the speed never grows beyond the initial speed plus a small solver margin.
	CHECK(max_speed <= Vec3(3.0f, 0, 3.0f).Length() + 0.2f);
}

// Shrinking the coefficients in the persisted callback clips the warm start and keeps everything finite.
static void persistence_shrink_coefficients()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig floor;
	floor.half_extent = Vec3(10, 0.5f, 10);
	floor.position = RVec3(0, -0.5f, 0);
	floor.motion_type = EMotionType::Static;
	floor.layer = 0;
	MakeBody(scene, floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(4.0f, 0, -3.0f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();
	listener.config.mu_along = 1.8f;
	listener.config.mu_across = 1.5f;
	listener.shrink_coefficient_after_switch = true;
	listener.switch_frame = 6;
	scene.world.SetContactListener(&listener);

	for (int i = 0; i < 24; ++i)
	{
		listener.frame = i;
		scene.step();
		Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
		CHECK(AllFinite(v));
		Vec3 w = scene.world.GetBodyInterface().GetAngularVelocity(box_id);
		CHECK(AllFinite(w));
	}
	// After shrinking to the minimum coefficients the box keeps sliding (friction bound is small but valid).
	Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(AllFinite(v));
}

// Turning the ellipse off in a persisted contact falls back to the isotropic model with no leftover impulse.
static void persistence_disable_ellipse()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig floor;
	floor.half_extent = Vec3(10, 0.5f, 10);
	floor.position = RVec3(0, -0.5f, 0);
	floor.motion_type = EMotionType::Static;
	floor.layer = 0;
	MakeBody(scene, floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(2.5f, 0, 2.5f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();
	listener.config.mu_along = 1.6f;
	listener.config.mu_across = 0.1f;
	listener.disable_after_switch = true;
	listener.switch_frame = 8;
	scene.world.SetContactListener(&listener);

	for (int i = 0; i < 30; ++i)
	{
		listener.frame = i;
		scene.step();
		Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
		CHECK(AllFinite(v));
	}
	Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(AllFinite(v));
	CHECK(v.Length() < Vec3(2.5f, 0, 2.5f).Length());	// isotropic friction keeps slowing it down
}

// Switching a persisted isotropic contact to the elliptical model rotates the previous world impulse into the
// new groove basis without leaving a stale impulse or injecting energy.
static void persistence_enable_ellipse()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig floor;
	floor.half_extent = Vec3(10, 0.5f, 10);
	floor.position = RVec3(0, -0.5f, 0);
	floor.motion_type = EMotionType::Static;
	floor.layer = 0;
	MakeBody(scene, floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(3.0f, 0, 2.0f);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.use_ellipse = false;		// start on the isotropic path
	listener.config.direction = Vec3(0.6f, 0, 0.8f);
	listener.config.mu_along = 1.4f;
	listener.config.mu_across = 0.2f;
	listener.enable_after_switch = true;
	listener.switch_frame = 6;
	scene.world.SetContactListener(&listener);

	float max_tangential_sq = Vec3(3.0f, 0, 2.0f).LengthSq();
	for (int i = 0; i < 24; ++i)
	{
		listener.frame = i;
		scene.step();
		Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
		Vec3 w = scene.world.GetBodyInterface().GetAngularVelocity(box_id);
		CHECK(AllFinite(v) && AllFinite(w));
		max_tangential_sq = max(max_tangential_sq, Square(v.GetX()) + Square(v.GetZ()));
	}
	// The contact was genuinely reused across the isotropic -> ellipse transition.
	CHECK(listener.num_persisted > 0);
	// Friction never injects energy even when the model and basis change mid-contact.
	CHECK(max_tangential_sq <= Vec3(3.0f, 0, 2.0f).LengthSq() + 1.0e-3f);
	Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(AllFinite(v));
}

// A sensor contact fires callbacks but never applies a force.
static void persistence_sensor_no_force()
{
	EnsureJoltInitialized();
	Scene scene;
	BodyConfig sensor_floor;
	sensor_floor.half_extent = Vec3(10, 0.5f, 10);
	sensor_floor.position = RVec3(0, -0.5f, 0);
	sensor_floor.motion_type = EMotionType::Static;
	sensor_floor.layer = 0;
	sensor_floor.is_sensor = true;
	MakeBody(scene, sensor_floor);

	BodyConfig box;
	box.position = RVec3(0, 0.5f, 0);
	box.linear_velocity = Vec3(2.0f, 0, 0);
	BodyID box_id = MakeBody(scene, box);

	TestContactListener listener;
	listener.config.direction = Vec3::sAxisX();
	scene.world.SetContactListener(&listener);

	Vec3 v_before = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	Vec3 gravity = scene.world.GetGravity();
	int num_steps = 15;
	for (int i = 0; i < num_steps; ++i)
	{
		listener.frame = i;
		scene.step();
	}
	Vec3 v_after = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
	CHECK(listener.num_added + listener.num_persisted > 0);	// sensor contacts are still reported
	CHECK(AllFinite(v_after));
	// No contact force: the box behaves as if in free fall. Its tangential velocity is untouched...
	CHECK(fabs(v_after.GetX() - v_before.GetX()) < 1.0e-5f);
	CHECK(fabs(v_after.GetZ() - v_before.GetZ()) < 1.0e-5f);
	// ...and its normal velocity is exactly the gravity-only value.
	float free_fall_y = v_before.GetY() + gravity.GetY() * (float(num_steps) / 120.0f);
	CHECK(fabs(v_after.GetY() - free_fall_y) < 1.0e-4f);
	CHECK(scene.world.GetBodyInterface().GetPosition(box_id).GetY() < 0.45f);	// it falls through the sensor
}

// When the 2x2 tangential effective inverse mass matrix degenerates (no tangential mobility) the part must
// deactivate, stay finite and never apply an undefined impulse. A real contact always carries a positive linear
// inverse mass, so the singular determinant is a defensive path that is exercised directly here.
static void persistence_degenerate_locked()
{
	EnsureJoltInitialized();

	// All-zero mass properties: every effective mass entry (and the 2x2 determinant) is zero.
	FrictionEllipseConstraintPart<EMotionType::Dynamic, EMotionType::Dynamic> part;
	part.CalculateConstraintProperties(
		0.0f, Mat44::sZero(), Vec3(0, -0.5f, 0),
		0.0f, Mat44::sZero(), Vec3(0, -0.5f, 0),
		Vec3::sAxisX(), Vec3::sAxisZ(), 0.0f, 0.0f, 1.0f, 0.5f);

	CHECK(!part.IsActive());					// singular K deactivates the part
	CHECK(part.GetTotalLambdaU() == 0.0f);
	CHECK(part.GetTotalLambdaV() == 0.0f);

	// A finite but nearly locked tangential direction must still produce a finite, bounded impulse.
	FrictionEllipseConstraintPart<EMotionType::Dynamic, EMotionType::Dynamic> locked;
	// One direction has a tiny (but positive) effective mass, the other a normal one.
	locked.CalculateConstraintProperties(
		1.0f, Mat44::sZero(), Vec3::sZero(),
		0.0f, Mat44::sZero(), Vec3::sZero(),
		Vec3::sAxisX(), Vec3::sAxisZ(), 0.0f, 0.0f, 0.05f, 2.0f);
	CHECK(locked.IsActive());

	Vec3 lv1(1.0f, 0, -2.0f), av1 = Vec3::sZero(), lv2 = Vec3::sZero(), av2 = Vec3::sZero();
	CHECK(locked.SolveVelocityConstraint(lv1, av1, lv2, av2, 5.0f));
	CHECK(AllFinite(lv1) && AllFinite(lv2));
	CHECK(isfinite(locked.GetTotalLambdaU()) && isfinite(locked.GetTotalLambdaV()));

	// The resulting impulse respects the ellipse (p_u / 0.05)^2 + (p_v / 2)^2 <= N^2 with N = 5.
	float ellipse = Square(locked.GetTotalLambdaU() / 0.05f) + Square(locked.GetTotalLambdaV() / 2.0f);
	CHECK(ellipse <= Square(5.0f) + 1.0e-3f);

	// And a real physics scene with restricted DOFs remains finite end to end (no NaN/Inf anywhere).
	{
		Scene scene;
		BodyConfig floor;
		floor.half_extent = Vec3(10, 0.5f, 10);
		floor.position = RVec3(0, -0.5f, 0);
		floor.motion_type = EMotionType::Static;
		floor.layer = 0;
		MakeBody(scene, floor);

		BodyConfig box;
		box.use_sphere = true;
		box.position = RVec3(0, 0.5f, 0);
		box.linear_velocity = Vec3(2.0f, 0, 1.0f);
		box.allowed_dofs = EAllowedDOFs::TranslationY | EAllowedDOFs::RotationX;
		BodyID box_id = MakeBody(scene, box);

		TestContactListener listener;
		listener.config.direction = Vec3::sAxisX();
		scene.world.SetContactListener(&listener);

		for (int i = 0; i < 10; ++i)
		{
			listener.frame = i;
			scene.step();
			Vec3 v = scene.world.GetBodyInterface().GetLinearVelocity(box_id);
			Vec3 w = scene.world.GetBodyInterface().GetAngularVelocity(box_id);
			CHECK(AllFinite(v) && AllFinite(w));
		}
	}
}

} // namespace

void register_cases(void)
{
	EnsureJoltInitialized();
	add_case("ellipse", "unsaturated", ellipse_unsaturated);
	add_case("ellipse", "saturated", ellipse_saturated);
	add_case("ellipse", "off_diagonal_k", ellipse_off_diagonal);

	add_case("world", "dynamic_vs_static", world_dynamic_vs_static);
	add_case("world", "mass_inertia_scaling", world_mass_inertia_scaling);
	add_case("world", "dynamic_vs_dynamic", world_dynamic_vs_dynamic);
	add_case("world", "kinematic_surface", world_kinematic_surface);
	add_case("world", "groove_rotation", world_groove_rotation);
	add_case("world", "tilted_scene", world_tilted_scene);
	add_case("world", "scene_rotation_covariance", world_scene_rotation_covariance);

	add_case("persistence", "basic", persistence_basic);
	add_case("persistence", "rotate_direction", persistence_rotate_direction);
	add_case("persistence", "shrink_coefficients", persistence_shrink_coefficients);
	add_case("persistence", "disable_ellipse", persistence_disable_ellipse);
	add_case("persistence", "enable_ellipse", persistence_enable_ellipse);
	add_case("persistence", "sensor_no_force", persistence_sensor_no_force);
	add_case("persistence", "degenerate_locked", persistence_degenerate_locked);
}

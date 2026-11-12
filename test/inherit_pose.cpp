// Inheritance / pose / skinning tests.
//
// Every FBX document below is generated in memory as ASCII FBX and loaded through
// the public ofbx::load() entry point. Expected values are derived independently
// from the Autodesk "Transformations" inheritance formulas (RrSs / RSrs / Rrs),
// not read back from the code under test: local matrices may come from
// Object::evalLocal() (whose definition is unchanged), but every world-space
// expectation is recomputed with the small matrix helpers in this file.
//
// Matrices authored in Cluster Transform/TransformLink arrays are written in
// DMatrix (column major) value order, i.e. the same layout getTransformMatrix()
// returns, so the documented space chain can be checked value by value.

#include "ofbx.h"
#include "ofbx_pose.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

constexpr double EPS = 1e-6;
constexpr int64_t FBX_SECOND = 46186158000;


bool near(double a, double b)
{
	return std::fabs(a - b) < EPS;
}

bool nearVec(const ofbx::DVec3& a, const ofbx::DVec3& b)
{
	return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

ofbx::DMatrix matIdentity()
{
	return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

ofbx::DMatrix matMul(const ofbx::DMatrix& a, const ofbx::DMatrix& b)
{
	ofbx::DMatrix r;
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			double v = 0;
			for (int k = 0; k < 4; ++k) v += a.m[k * 4 + row] * b.m[col * 4 + k];
			r.m[col * 4 + row] = v;
		}
	}
	return r;
}

ofbx::DVec3 matPoint(const ofbx::DMatrix& m, const ofbx::DVec3& p)
{
	return
	{
		m.m[0] * p.x + m.m[4] * p.y + m.m[8]  * p.z + m.m[12],
		m.m[1] * p.x + m.m[5] * p.y + m.m[9]  * p.z + m.m[13],
		m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
	};
}

ofbx::DMatrix matTranslation(double x, double y, double z)
{
	ofbx::DMatrix m = matIdentity();
	m.m[12] = x;
	m.m[13] = y;
	m.m[14] = z;
	return m;
}

ofbx::DMatrix matRotZ90()
{
	// column major
	return {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

ofbx::DMatrix matScale(double x, double y, double z)
{
	ofbx::DMatrix m = matIdentity();
	m.m[0] = x;
	m.m[5] = y;
	m.m[10] = z;
	return m;
}


std::string array16(const std::string& name, const double v[16])
{
	// The ASCII tokenizer treats a leading '*' array as everything up to the
	// colon after the 'a' tag, so values must start immediately after it.
	std::string s = "  " + name + ": *16 {\n   a:";
	for (int i = 0; i < 16; ++i)
	{
		s += " ";
		s += std::to_string(v[i]);
		if (i != 15) s += ",";
		if (i % 4 == 3 && i != 15) s += "\n    ";
	}
	s += "\n  }\n";
	return s;
}

ofbx::IScene* loadString(const std::string& text)
{
	const auto* bytes = reinterpret_cast<const ofbx::u8*>(text.data());
	ofbx::IScene* scene = ofbx::load(bytes, text.size(), 0);
	if (!scene) throw std::runtime_error(std::string("load failed: ") + ofbx::getError());
	return scene;
}

const ofbx::Object* findNode(const ofbx::IScene& scene, const char* short_name)
{
	const std::string full = std::string("Model::") + short_name;
	for (int i = 0; i < scene.getAllObjectCount(); ++i)
	{
		const ofbx::Object* o = scene.getAllObjects()[i];
		if (full == o->name) return o;
	}
	return nullptr;
}

const ofbx::AnimationLayer* firstLayer(const ofbx::IScene& scene)
{
	for (int i = 0; i < scene.getAllObjectCount(); ++i)
	{
		const ofbx::Object* o = scene.getAllObjects()[i];
		if (o->getType() == ofbx::Object::Type::ANIMATION_LAYER)
			return static_cast<const ofbx::AnimationLayer*>(o);
	}
	return nullptr;
}

const ofbx::DMatrix& poseWorld(const std::vector<ofbx::NodePose>& pose, const ofbx::Object* node)
{
	for (const ofbx::NodePose& np : pose)
	{
		if (np.node == node) return np.world;
	}
	throw std::runtime_error("node missing from pose");
}

std::string prop3(const char* name, double x, double y, double z)
{
	return std::string("   P: \"") + name + "\", \"" + name + "\", \"\", \"A\","
		+ std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + "\n";
}

std::string inheritProp(int value)
{
	return std::string("   P: \"InheritType\", \"enum\", \"\", \"\",") + std::to_string(value) + "\n";
}

std::string model(const char* id_name, int id, const char* cls, const std::string& props)
{
	std::string s = " Model: " + std::to_string(id) + ", \"Model::" + id_name + "\", \"" + cls + "\" {\n";
	s += "  Version: 232\n";
	s += "  Properties70:  {\n";
	s += props;
	s += "  }\n";
	s += " }\n";
	return s;
}

} // namespace


// ---------------------------------------------------------------------------
// static_modes: anisotropic parent scale combined with a non commuting Z
// rotation under all three inheritance modes, plus a three level Rrs chain
// proving ancestor scale survives while only the direct parent scale is lost.
// ---------------------------------------------------------------------------
FBX_CASE(static_modes)
{
	std::string fbx =
		"; generated\n"
		"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
		"Objects:  {\n";
	fbx += model("A", 100, "LimbNode", prop3("Lcl Scaling", 2, 3, 1));
	fbx += model("B0", 110, "LimbNode", prop3("Lcl Translation", 3, 0, 0) + prop3("Lcl Rotation", 0, 0, 90) + inheritProp(0));
	fbx += model("B1", 111, "LimbNode", prop3("Lcl Translation", 3, 0, 0) + prop3("Lcl Rotation", 0, 0, 90) + inheritProp(1));
	fbx += model("B2", 112, "LimbNode", prop3("Lcl Translation", 3, 0, 0) + prop3("Lcl Rotation", 0, 0, 90) + inheritProp(2));
	fbx += model("Bdef", 113, "LimbNode", prop3("Lcl Translation", 3, 0, 0) + prop3("Lcl Rotation", 0, 0, 90));
	fbx += " Model: 114, \"Model::B60\", \"LimbNode\" {\n"
	       "  Version: 232\n"
	       "  Properties60:  {\n"
	       "   Property: \"InheritType\", \"enum\", \"\", 0\n"
	       "   Property: \"Lcl Translation\", \"Lcl Translation\", \"\", 3,0,0\n"
	       "   Property: \"Lcl Rotation\", \"Lcl Rotation\", \"\", 0,0,90\n"
	       "  }\n"
	       " }\n";
	fbx += " Model: 115, \"Model::B61\", \"LimbNode\" {\n"
	       "  Version: 232\n"
	       "  Properties60:  {\n"
	       "   Property: \"InheritType\", \"enum\", \"\", 1\n"
	       "   Property: \"Lcl Translation\", \"Lcl Translation\", \"\", 3,0,0\n"
	       "   Property: \"Lcl Rotation\", \"Lcl Rotation\", \"\", 0,0,90\n"
	       "  }\n"
	       " }\n";
	fbx += model("D", 120, "LimbNode", prop3("Lcl Scaling", 4, 1, 1));
	fbx += model("E", 121, "LimbNode", prop3("Lcl Rotation", 0, 0, 90) + inheritProp(2));
	fbx +=
		"}\n"
		"Connections:  {\n"
		" C: \"OO\",100,0\n"
		" C: \"OO\",110,100\n C: \"OO\",111,100\n C: \"OO\",112,100\n C: \"OO\",113,100\n C: \"OO\",114,100\n C: \"OO\",115,100\n"
		" C: \"OO\",120,100\n C: \"OO\",121,120\n"
		"}\n";

	ofbx::IScene* scene = loadString(fbx);

	const ofbx::Object* b0 = findNode(*scene, "B0");
	const ofbx::Object* b1 = findNode(*scene, "B1");
	const ofbx::Object* b2 = findNode(*scene, "B2");
	const ofbx::Object* bdef = findNode(*scene, "Bdef");
	const ofbx::Object* b60 = findNode(*scene, "B60");
	const ofbx::Object* b61 = findNode(*scene, "B61");
	const ofbx::Object* e = findNode(*scene, "E");
	FBX_CHECK(b0 && b1 && b2 && bdef && b60 && b61 && e);

	FBX_CHECK(b0->getInheritType() == 0);
	FBX_CHECK(b1->getInheritType() == 1);
	FBX_CHECK(b2->getInheritType() == 2);
	FBX_CHECK(bdef->getInheritType() == 0); // missing property defaults to RrSs
	FBX_CHECK(b60->getInheritType() == 0);  // Properties60 RrSs
	FBX_CHECK(b61->getInheritType() == 1);  // Properties60 RSrs

	// Local transform definition is untouched.
	const ofbx::DMatrix local = b0->getLocalTransform();
	FBX_CHECK(near(local.m[12], 3.0) && near(local.m[13], 0.0));
	FBX_CHECK(nearVec(matPoint(local, {1, 0, 0}), {3, 1, 0}));

	const ofbx::DVec3 p{1, 0, 0};

	// RrSs: Rlin * diag(2,3,1) maps (1,0,0) to (0,2,0); translation stays (6,0,0).
	FBX_CHECK(nearVec(matPoint(b0->getGlobalTransform(), p), {6, 2, 0}));
	FBX_CHECK(near(b0->getGlobalTransform().m[12], 6.0));
	FBX_CHECK(near(b0->getGlobalTransform().m[13], 0.0));
	// RSrs: diag(2,3,1) * Rlin maps the point to (0,3,0).
	FBX_CHECK(nearVec(matPoint(b1->getGlobalTransform(), p), {6, 3, 0}));
	// Rrs: parent's own scale removed, point sees pure Rlin -> (0,1,0).
	FBX_CHECK(nearVec(matPoint(b2->getGlobalTransform(), p), {6, 1, 0}));
	// Translation is always transformed by the full parent world matrix.
	FBX_CHECK(near(b2->getGlobalTransform().m[12], 6.0));
	FBX_CHECK(near(b2->getGlobalTransform().m[13], 0.0));
	// Default and Properties60 behave exactly like explicit RrSs.
	FBX_CHECK(nearVec(matPoint(bdef->getGlobalTransform(), p), {6, 2, 0}));
	FBX_CHECK(nearVec(matPoint(b60->getGlobalTransform(), p), {6, 2, 0}));
	// Properties60 non default enum (RSrs) is honored too.
	FBX_CHECK(nearVec(matPoint(b61->getGlobalTransform(), p), {6, 3, 0}));

	// Three level chain A(scale 2,3,1) -> D(scale 4,1,1) -> E(Rrs, Z90):
	// D's own scale 4 is compensated, A's inherited scale 2 remains,
	// so Rz90 * diag(2,3,1) maps (1,0,0) to (0,2,0).
	FBX_CHECK(nearVec(matPoint(e->getGlobalTransform(), p), {0, 2, 0}));

	// Static pose through the new entry point must match the static interface.
	std::vector<ofbx::NodePose> pose;
	FBX_CHECK(ofbx::evaluatePose(*scene, nullptr, 0.0, pose));
	for (const ofbx::NodePose& np : pose)
	{
		const ofbx::DMatrix ref = np.node->getGlobalTransform();
		for (int i = 0; i < 16; ++i) FBX_CHECK(near(np.world.m[i], ref.m[i]));
	}

	scene->destroy();
}


// ---------------------------------------------------------------------------
// animated_ancestry: curves on ancestor nodes of a three level chain, partial
// component coverage falling back to curve node defaults, and static fallback
// when a property or a whole component is not animated.
// ---------------------------------------------------------------------------
FBX_CASE(animated_ancestry)
{
	std::string fbx =
		"; generated\n"
		"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
		"Objects:  {\n";
	fbx += model("A", 100, "LimbNode", prop3("Lcl Scaling", 2, 1, 1));
	fbx += model("B", 101, "LimbNode", prop3("Lcl Rotation", 0, 0, 90) + inheritProp(0));
	fbx += model("C", 102, "LimbNode", prop3("Lcl Translation", 1, 0, 0) + inheritProp(0));
	fbx += model("F", 103, "LimbNode", prop3("Lcl Translation", 0, 0, 0));
	fbx +=
		" AnimationCurveNode: 200, \"AnimCurveNode::A|Translation\", \"Lcl Translation\" {\n"
		"  Properties70:  {\n"
		"   P: \"d|X\", \"Number\", \"\", \"A\",0\n"
		"   P: \"d|Y\", \"Number\", \"\", \"A\",0\n"
		"   P: \"d|Z\", \"Number\", \"\", \"A\",0\n"
		"  }\n"
		" }\n"
		" AnimationCurveNode: 210, \"AnimCurveNode::B|Scaling\", \"Lcl Scaling\" {\n"
		"  Properties70:  {\n"
		"   P: \"d|X\", \"Number\", \"\", \"A\",1\n"
		"   P: \"d|Y\", \"Number\", \"\", \"A\",1\n"
		"   P: \"d|Z\", \"Number\", \"\", \"A\",1\n"
		"  }\n"
		" }\n"
		" AnimationCurveNode: 220, \"AnimCurveNode::F|Translation\", \"Lcl Translation\" {\n"
		"  Properties70:  {\n"
		"   P: \"d|X\", \"Number\", \"\", \"A\",0\n"
		"   P: \"d|Y\", \"Number\", \"\", \"A\",7\n"
		"   P: \"d|Z\", \"Number\", \"\", \"A\",0\n"
		"  }\n"
		" }\n"
		" AnimationCurve: 201, \"\" {\n"
		"  KeyTime: *2 {\n   a: 0," + std::to_string(FBX_SECOND) + "\n  }\n"
		"  KeyValueFloat: *2 {\n   a: 0.000000, 10.000000\n  }\n"
		" }\n"
		" AnimationCurve: 211, \"\" {\n"
		"  KeyTime: *2 {\n   a: 0," + std::to_string(FBX_SECOND) + "\n  }\n"
		"  KeyValueFloat: *2 {\n   a: 1.000000, 3.000000\n  }\n"
		" }\n"
		" AnimationCurve: 221, \"\" {\n"
		"  KeyTime: *2 {\n   a: 0," + std::to_string(FBX_SECOND) + "\n  }\n"
		"  KeyValueFloat: *2 {\n   a: 0.000000, 2.000000\n  }\n"
		" }\n"
		" AnimationStack: 500, \"AnimStack::Take\", \"\" {\n}\n"
		" AnimationLayer: 501, \"AnimLayer::Base\", \"\" {\n}\n"
		"}\n"
		"Connections:  {\n"
		" C: \"OO\",100,0\n C: \"OO\",101,100\n C: \"OO\",102,101\n C: \"OO\",103,0\n"
		" C: \"OP\",200,100,\"Lcl Translation\"\n C: \"OP\",210,101,\"Lcl Scaling\"\n C: \"OP\",220,103,\"Lcl Translation\"\n"
		" C: \"OO\",501,500\n C: \"OO\",200,501\n C: \"OO\",210,501\n C: \"OO\",220,501\n"
		" C: \"OP\",201,200,\"d|X\"\n C: \"OP\",211,210,\"d|Y\"\n C: \"OP\",221,220,\"d|X\"\n"
		"}\n";

	ofbx::IScene* scene = loadString(fbx);
	const ofbx::AnimationLayer* layer = firstLayer(*scene);
	FBX_CHECK(layer);

	std::vector<ofbx::NodePose> pose;
	FBX_CHECK(ofbx::evaluatePose(*scene, layer, 0.5, pose));

	const ofbx::Object* a = findNode(*scene, "A");
	const ofbx::Object* b = findNode(*scene, "B");
	const ofbx::Object* c = findNode(*scene, "C");
	const ofbx::Object* f = findNode(*scene, "F");

	// Output order is the relative isNode() order of getAllObjects().
	int idx = 0;
	for (int i = 0; i < scene->getAllObjectCount(); ++i)
	{
		const ofbx::Object* o = scene->getAllObjects()[i];
		if (o->isNode()) { FBX_CHECK(pose[idx].node == o); ++idx; }
	}
	FBX_CHECK(idx == (int)pose.size());

	// A at t = 0.5: translation interpolated to 5, static scale (2,1,1) retained.
	const ofbx::DMatrix& wa = poseWorld(pose, a);
	FBX_CHECK(near(wa.m[12], 5.0) && near(wa.m[13], 0.0));
	FBX_CHECK(near(wa.m[0], 2.0) && near(wa.m[5], 1.0));

	// B: RrSs with animated scale y = 2 gives Rz90 * diag(2,2,1).
	const ofbx::DMatrix& wb = poseWorld(pose, b);
	FBX_CHECK(near(wb.m[12], 5.0) && near(wb.m[13], 0.0));
	FBX_CHECK(near(wb.m[1], 2.0) && near(wb.m[4], -2.0));

	// C local point (1,0,0): G_B * (2,0,0) + G_B translation -> (5,4,0).
	FBX_CHECK(nearVec(matPoint(poseWorld(pose, c), {1, 0, 0}), {5, 4, 0}));

	// F: only d|X is curved; missing d|Y uses the curve node default 7, not the
	// static Lcl Translation value 0.
	const ofbx::DMatrix& wf = poseWorld(pose, f);
	FBX_CHECK(near(wf.m[12], 1.0) && near(wf.m[13], 7.0) && near(wf.m[14], 0.0));

	// Layer null evaluates the static pose, identical to getGlobalTransform().
	std::vector<ofbx::NodePose> static_pose;
	FBX_CHECK(ofbx::evaluatePose(*scene, nullptr, 0.25, static_pose));
	for (const ofbx::NodePose& np : static_pose)
	{
		const ofbx::DMatrix ref = np.node->getGlobalTransform();
		for (int i = 0; i < 16; ++i) FBX_CHECK(near(np.world.m[i], ref.m[i]));
	}

	scene->destroy();
}


// ---------------------------------------------------------------------------
// pivot_translation: rotation pivot changes the local origin; inheritance
// modes differ on the linear part but never on translation, which always goes
// through the parent's complete (scaled) world matrix.
// ---------------------------------------------------------------------------
FBX_CASE(pivot_translation)
{
	std::string fbx =
		"; generated\n"
		"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
		"Objects:  {\n";
	fbx += model("Q", 100, "LimbNode", prop3("Lcl Translation", 1, 1, 0) + prop3("Lcl Scaling", 2, 2, 2));
	std::string pprops = prop3("Lcl Translation", 3, 0, 0) + prop3("Lcl Rotation", 0, 0, 90)
		+ prop3("RotationPivot", 1, 0, 0);
	fbx += model("P0", 110, "LimbNode", pprops + inheritProp(0));
	fbx += model("P2", 111, "LimbNode", pprops + inheritProp(2));
	fbx +=
		"}\n"
		"Connections:  {\n"
		" C: \"OO\",100,0\n C: \"OO\",110,100\n C: \"OO\",111,100\n"
		"}\n";

	ofbx::IScene* scene = loadString(fbx);
	const ofbx::Object* q = findNode(*scene, "Q");
	const ofbx::Object* p0 = findNode(*scene, "P0");
	const ofbx::Object* p2 = findNode(*scene, "P2");

	// Independent expectation for the local matrix with RotationPivot (1,0,0):
	// L = T(3) * T(1) * Rz90 * T(-1), so the local origin lands at (4,-1,0).
	const ofbx::DMatrix l_expected = matMul(
		matTranslation(3, 0, 0),
		matMul(matTranslation(1, 0, 0), matMul(matRotZ90(), matTranslation(-1, 0, 0))));
	const ofbx::DMatrix l_actual = p0->getLocalTransform();
	for (int i = 0; i < 16; ++i) FBX_CHECK(near(l_actual.m[i], l_expected.m[i]));
	FBX_CHECK(nearVec({l_actual.m[12], l_actual.m[13], l_actual.m[14]}, {4, -1, 0}));

	// Parent world built from unchanged local transforms only.
	const ofbx::DMatrix gq = q->getGlobalTransform();

	// Translation column: full parent world applied to the pivoted local origin.
	const ofbx::DVec3 expected_t = matPoint(gq, {4, -1, 0});
	FBX_CHECK(nearVec(expected_t, {9, -1, 0}));
	const ofbx::DMatrix& w0 = p0->getGlobalTransform();
	const ofbx::DMatrix& w2 = p2->getGlobalTransform();
	FBX_CHECK(nearVec({w0.m[12], w0.m[13], w0.m[14]}, {9, -1, 0}));
	// Rrs must not strip parent scale from the translation.
	FBX_CHECK(nearVec({w2.m[12], w2.m[13], w2.m[14]}, {9, -1, 0}));

	// Linear parts: RrSs keeps parent scale (2 * Rz90), Rrs cancels it (Rz90).
	FBX_CHECK(near(w0.m[1], 2.0) && near(w0.m[4], -2.0));
	FBX_CHECK(near(w2.m[1], 1.0) && near(w2.m[4], -1.0));

	// End to end points.
	FBX_CHECK(nearVec(matPoint(w0, {1, 0, 0}), {9, 1, 0}));
	FBX_CHECK(nearVec(matPoint(w2, {1, 0, 0}), {9, 0, 0}));

	// Same expectations through evaluatePose.
	std::vector<ofbx::NodePose> pose;
	FBX_CHECK(ofbx::evaluatePose(*scene, nullptr, 0.0, pose));
	FBX_CHECK(nearVec(matPoint(poseWorld(pose, p0), {1, 0, 0}), {9, 1, 0}));
	FBX_CHECK(nearVec(matPoint(poseWorld(pose, p2), {1, 0, 0}), {9, 0, 0}));

	scene->destroy();
}


// ---------------------------------------------------------------------------
// skin_spaces: non identity geometric transform, bind matrices (including a
// rotated Transform) and a control point weighted on two bones. The check runs
// the full chain on an actual point rather than comparing intermediate TRS.
// ---------------------------------------------------------------------------
FBX_CASE(skin_spaces)
{
	// column major authored matrices
	const double transform1[16] = {
		0, 1, 0, 0,  -1, 0, 0, 0,  0, 0, 1, 0,  10, 0, 0, 1}; // T(10) * Rz90
	const double link1[16] = {
		1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  4, 0, 0, 1};   // T(4)
	const double transform2[16] = {
		1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 20, 0, 1};  // T(0,20)
	const double link2[16] = {
		1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 2, 0, 1};   // T(0,2)

	std::string fbx =
		"; generated\n"
		"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
		"Objects:  {\n";
	fbx += model("bone1", 100, "LimbNode", prop3("Lcl Translation", 1, 0, 0));
	fbx += model("bone2", 101, "LimbNode", prop3("Lcl Translation", 0, 2, 0));
	fbx +=
		" Geometry: 400, \"Geometry::mesh\", \"Mesh\" {\n"
		"  Vertices: *3 {\n   a: 1.000000,1.000000,1.000000, 0.000000,0.000000,0.000000, 0.000000,0.000000,0.000000\n  }\n"
		"  PolygonVertexIndex: *3 {\n   a: 0,1,-3\n  }\n"
		" }\n";
	fbx += model("mesh", 410, "Mesh",
		prop3("GeometricTranslation", 0, 0, 5) + prop3("GeometricScaling", 2, 1, 1));
	fbx +=
		" Deformer: 300, \"Deformer::skin\", \"Skin\" {\n}\n"
		" Deformer: 310, \"Deformer::c1\", \"Cluster\" {\n"
		"  Indexes: *1 {\n   a: 0\n  }\n"
		"  Weights: *1 {\n   a: 0.250000\n  }\n";
	fbx += array16("Transform", transform1);
	fbx += array16("TransformLink", link1);
	fbx +=
		" }\n"
		" Deformer: 311, \"Deformer::c2\", \"Cluster\" {\n"
		"  Indexes: *1 {\n   a: 0\n  }\n"
		"  Weights: *1 {\n   a: 0.750000\n  }\n";
	fbx += array16("Transform", transform2);
	fbx += array16("TransformLink", link2);
	fbx +=
		" }\n"
		"}\n"
		"Connections:  {\n"
		" C: \"OO\",100,0\n C: \"OO\",101,0\n"
		" C: \"OO\",400,410\n C: \"OO\",300,400\n"
		" C: \"OO\",310,300\n C: \"OO\",311,300\n"
		" C: \"OO\",100,310\n C: \"OO\",101,311\n"
		"}\n";

	ofbx::IScene* scene = loadString(fbx);
	FBX_CHECK(scene->getMeshCount() == 1);
	const ofbx::Mesh* mesh = scene->getMesh(0);
	const ofbx::Skin* skin = mesh->getSkin();
	FBX_CHECK(skin && skin->getClusterCount() == 2);

	std::vector<ofbx::NodePose> pose;
	FBX_CHECK(ofbx::evaluatePose(*scene, nullptr, 0.0, pose));

	std::vector<ofbx::SkinPose> skins;
	FBX_CHECK(ofbx::buildSkinMatrices(*mesh, pose, skins));
	FBX_CHECK(skins.size() == 1);
	FBX_CHECK(skins[0].clusters.size() == 2);
	FBX_CHECK(skins[0].clusters[0].cluster == skin->getCluster(0));
	FBX_CHECK(skins[0].clusters[1].cluster == skin->getCluster(1));

	// Independent reference chain for cluster 1, including the rotated Transform:
	// G(p) = (2,1,6); T10*Rz90 -> (9,2,6); inv T4 -> (5,2,6); W1=T1 -> (6,2,6).
	const ofbx::DMatrix g = matMul(matScale(2, 1, 1), matTranslation(0, 0, 5));
	const ofbx::DMatrix t1 = matMul(matTranslation(10, 0, 0), matRotZ90());
	const ofbx::DMatrix ref1 = matMul(matTranslation(1, 0, 0),
		matMul(matTranslation(-4, 0, 0), matMul(t1, g)));
	const ofbx::DMatrix ref2 = matMul(matTranslation(0, 2, 0),
		matMul(matTranslation(0, -2, 0), matMul(matTranslation(0, 20, 0), g)));
	for (int i = 0; i < 16; ++i)
	{
		FBX_CHECK(near(skins[0].clusters[0].matrix.m[i], ref1.m[i]));
		FBX_CHECK(near(skins[0].clusters[1].matrix.m[i], ref2.m[i]));
	}

	const ofbx::DVec3 control_point{1, 1, 1};
	const ofbx::DVec3 p1 = matPoint(skins[0].clusters[0].matrix, control_point);
	const ofbx::DVec3 p2 = matPoint(skins[0].clusters[1].matrix, control_point);
	FBX_CHECK(nearVec(p1, {6, 2, 6}));
	FBX_CHECK(nearVec(p2, {2, 21, 6}));

	// Weighted blend on two bones.
	const ofbx::DVec3 blended{
		0.25 * p1.x + 0.75 * p2.x,
		0.25 * p1.y + 0.75 * p2.y,
		0.25 * p1.z + 0.75 * p2.z,
	};
	FBX_CHECK(nearVec(blended, {3, 16.25, 6}));

	scene->destroy();
}


// ---------------------------------------------------------------------------
// isolation_and_errors: every documented failure mode returns false without
// touching the output, queries are read only and can interleave across scenes,
// and the static interface keeps its legacy behavior on unsupported inputs.
// ---------------------------------------------------------------------------
FBX_CASE(isolation_and_errors)
{
	const std::string anim_fbx =
		"; generated\n"
		"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
		"Objects:  {\n"
		" Model: 100, \"Model::A\", \"LimbNode\" {\n"
		"  Version: 232\n"
		"  Properties70:  {\n   P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",2,1,1\n  }\n"
		" }\n"
		"}\n"
		"Connections:  {\n C: \"OO\",100,0\n}\n";

	// One model per document keeps every rejection reason independent.
	auto single_node_fbx = [](const std::string& props) {
		return std::string(
			"; generated\n"
			"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
			"Objects:  {\n")
			+ model("n", 100, "LimbNode", props)
			+ "}\n"
			"Connections:  {\n C: \"OO\",100,0\n}\n";
	};

	std::vector<ofbx::NodePose> sentinel;
	sentinel.push_back(ofbx::NodePose{nullptr, {}});

	// Unknown inheritance value: pose evaluation refuses, legacy static path
	// still multiplies matrices and returns the authored translation.
	{
		ofbx::IScene* s = loadString(single_node_fbx(prop3("Lcl Translation", 1, 2, 3) + inheritProp(5)));
		const ofbx::Object* n = findNode(*s, "n");
		FBX_CHECK(n->getInheritType() == -1);
		const ofbx::DMatrix legacy = n->getGlobalTransform();
		FBX_CHECK(std::isfinite(legacy.m[12]) && nearVec({legacy.m[12], legacy.m[13], legacy.m[14]}, {1, 2, 3}));
		FBX_CHECK(!ofbx::evaluatePose(*s, nullptr, 0.0, sentinel));
		s->destroy();
	}
	// Zero local scale.
	{
		ofbx::IScene* s = loadString(single_node_fbx(prop3("Lcl Scaling", 0, 1, 1)));
		FBX_CHECK(!ofbx::evaluatePose(*s, nullptr, 0.0, sentinel));
		s->destroy();
	}
	// Reflected (negative) local scale.
	{
		ofbx::IScene* s = loadString(single_node_fbx(prop3("Lcl Scaling", -1, 1, 1)));
		FBX_CHECK(!ofbx::evaluatePose(*s, nullptr, 0.0, sentinel));
		s->destroy();
	}
	// Non finite times on an otherwise valid scene.
	{
		ofbx::IScene* s = loadString(anim_fbx);
		FBX_CHECK(!ofbx::evaluatePose(*s, nullptr, std::nan(""), sentinel));
		FBX_CHECK(!ofbx::evaluatePose(*s, nullptr, INFINITY, sentinel));
		s->destroy();
	}
	// Every failure above must leave the output untouched.
	FBX_CHECK(sentinel.size() == 1 && sentinel[0].node == nullptr);

	// Skin failures need a scene whose pose itself is valid, so the rejection
	// can only come from buildSkinMatrices().
	auto skin_fbx = [&](bool with_link) {
		std::string f =
			"; generated\n"
			"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
			"Objects:  {\n";
		f += model("bone", 100, "LimbNode", prop3("Lcl Translation", 1, 0, 0));
		f +=
			" Geometry: 400, \"Geometry::mesh\", \"Mesh\" {\n"
			"  Vertices: *3 {\n   a: 0,0,0, 0,0,0, 0,0,0\n  }\n"
			"  PolygonVertexIndex: *3 {\n   a: 0,1,-3\n  }\n"
			" }\n";
		f += model("mesh", 410, "Mesh", "");
		f +=
			" Deformer: 300, \"Deformer::skin\", \"Skin\" {\n}\n"
			" Deformer: 310, \"Deformer::c\", \"Cluster\" {\n";
		const double zero16[16] = {0};
		f += array16("TransformLink", zero16); // singular, never invertible
		f +=
			" }\n"
			"}\n"
			"Connections:  {\n"
			" C: \"OO\",100,0\n C: \"OO\",400,410\n C: \"OO\",300,400\n C: \"OO\",310,300\n";
		if (with_link) f += " C: \"OO\",100,310\n";
		f += "}\n";
		return f;
	};

	// Singular TransformLink: pose is fine, skin build must fail cleanly.
	{
		ofbx::IScene* s = loadString(skin_fbx(true));
		std::vector<ofbx::NodePose> pose;
		FBX_CHECK(ofbx::evaluatePose(*s, nullptr, 0.0, pose));
		std::vector<ofbx::SkinPose> skins;
		skins.push_back(ofbx::SkinPose{});
		FBX_CHECK(!ofbx::buildSkinMatrices(*s->getMesh(0), pose, skins));
		FBX_CHECK(skins.size() == 1 && skins[0].skin == nullptr);
		s->destroy();
	}
	// Cluster without a link object: distinct failure path, same guarantee.
	{
		ofbx::IScene* s = loadString(skin_fbx(false));
		std::vector<ofbx::NodePose> pose;
		FBX_CHECK(ofbx::evaluatePose(*s, nullptr, 0.0, pose));
		std::vector<ofbx::SkinPose> skins;
		FBX_CHECK(!ofbx::buildSkinMatrices(*s->getMesh(0), pose, skins));
		FBX_CHECK(skins.empty());
		s->destroy();
	}

	// Read only / interleaved access across two independently loaded scenes.
	ofbx::IScene* s1 = loadString(anim_fbx);
	ofbx::IScene* s2 = loadString(anim_fbx);
	std::vector<ofbx::NodePose> q1, q2, q1b;
	FBX_CHECK(ofbx::evaluatePose(*s1, nullptr, 0.0, q1));
	FBX_CHECK(ofbx::evaluatePose(*s2, nullptr, 0.0, q2));
	FBX_CHECK(ofbx::evaluatePose(*s1, nullptr, 0.0, q1b));
	FBX_CHECK(q1.size() == q2.size() && q1.size() == q1b.size());
	for (size_t i = 0; i < q1.size(); ++i)
	{
		for (int k = 0; k < 16; ++k)
		{
			FBX_CHECK(near(q1[i].world.m[k], q1b[i].world.m[k]));
			FBX_CHECK(near(q1[i].world.m[k], q2[i].world.m[k]));
		}
	}
	const ofbx::Object* a1 = findNode(*s1, "A");
	const ofbx::DMatrix before = a1->getGlobalTransform();
	FBX_CHECK(a1->getGlobalTransform().m[0] == before.m[0]);

	// A layer from a different scene must be rejected.
	const std::string layer_fbx =
		"; generated\n"
		"FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
		"Objects:  {\n"
		" Model: 100, \"Model::A\", \"LimbNode\" {\n Version: 232\n Properties70:  {\n }\n }\n"
		" AnimationStack: 500, \"AnimStack::Take\", \"\" {\n}\n"
		" AnimationLayer: 501, \"AnimLayer::Base\", \"\" {\n}\n"
		"}\n"
		"Connections:  {\n C: \"OO\",100,0\n C: \"OO\",501,500\n}\n";
	ofbx::IScene* s3 = loadString(layer_fbx);
	const ofbx::AnimationLayer* foreign_layer = firstLayer(*s3);
	FBX_CHECK(foreign_layer);
	FBX_CHECK(!ofbx::evaluatePose(*s1, foreign_layer, 0.0, q1));
	FBX_CHECK(q1.size() == q1b.size()); // output untouched

	s3->destroy();
	s2->destroy();
	s1->destroy();
}

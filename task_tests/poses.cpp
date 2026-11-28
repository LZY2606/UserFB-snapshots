#include "task_support/check.h"

#include "meshoptimizer.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

// Multi-pose simplifier regression tests.
// Everything here only relies on the public meshopt_PoseSimplifier C API and C++11.

namespace
{

const size_t STRIDE = 12;

void makeGrid(std::vector<float>& positions, unsigned n, float bump)
{
	positions.clear();
	positions.reserve(size_t(n) * n * 3);

	for (unsigned y = 0; y < n; ++y)
		for (unsigned x = 0; x < n; ++x)
		{
			// same bump pattern as the baseline: odd/odd vertices lift up
			float z = bump && (x % 2 && y % 2) ? bump : 0.f;
			positions.push_back(float(x));
			positions.push_back(float(y));
			positions.push_back(z);
		}
}

void makeGridIndices(std::vector<unsigned int>& indices, unsigned n)
{
	indices.clear();

	for (unsigned y = 0; y + 1 < n; ++y)
		for (unsigned x = 0; x + 1 < n; ++x)
		{
			unsigned int a = y * n + x, b = a + 1, c = a + n, d = c + 1;
			unsigned int tri[6] = {a, b, c, b, d, c};
			indices.insert(indices.end(), tri, tri + 6);
		}
}

// ---- independent QEM reimplementation, mirroring fillFaceQuadrics/quadricFromTriangle ----

struct TestQuadric
{
	float a00, a11, a22, a10, a20, a21, b0, b1, b2, c, w;
};

void addQuadric(TestQuadric& q, const TestQuadric& r)
{
	q.a00 += r.a00; q.a11 += r.a11; q.a22 += r.a22;
	q.a10 += r.a10; q.a20 += r.a20; q.a21 += r.a21;
	q.b0 += r.b0; q.b1 += r.b1; q.b2 += r.b2; q.c += r.c; q.w += r.w;
}

TestQuadric faceQuadric(const float* p0, const float* p1, const float* p2)
{
	float p10[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
	float p20[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};

	float nx = p10[1] * p20[2] - p10[2] * p20[1];
	float ny = p10[2] * p20[0] - p10[0] * p20[2];
	float nz = p10[0] * p20[1] - p10[1] * p20[0];

	float len = sqrtf(nx * nx + ny * ny + nz * nz);
	nx /= len; ny /= len; nz /= len;

	float d = -(nx * p0[0] + ny * p0[1] + nz * p0[2]);
	float weight = sqrtf(len); // matches quadricFromTriangle: sqrtf(cross length)

	TestQuadric q;
	q.a00 = nx * nx * weight; q.a11 = ny * ny * weight; q.a22 = nz * nz * weight;
	q.a10 = nx * ny * weight; q.a20 = nx * nz * weight; q.a21 = ny * nz * weight;
	q.b0 = nx * d * weight; q.b1 = ny * d * weight; q.b2 = nz * d * weight;
	q.c = d * d * weight; q.w = weight;
	return q;
}

float evalQuadric(const TestQuadric& q, const float* v)
{
	float rx = (q.b0 + q.a10 * v[1]) * 2.f + q.a00 * v[0];
	float ry = (q.b1 + q.a21 * v[2]) * 2.f + q.a11 * v[1];
	float rz = (q.b2 + q.a20 * v[0]) * 2.f + q.a22 * v[2];

	float r = q.c + rx * v[0] + ry * v[1] + rz * v[2];
	return q.w == 0.f ? 0.f : fabsf(r) / q.w;
}

// independently accumulate the original face quadrics of every vertex for one pose
std::vector<TestQuadric> initialQuadrics(const std::vector<unsigned int>& indices, const std::vector<float>& positions)
{
	size_t vertex_count = positions.size() / 3;
	std::vector<TestQuadric> q(vertex_count);
	memset(&q[0], 0, vertex_count * sizeof(TestQuadric));

	for (size_t i = 0; i < indices.size(); i += 3)
	{
		TestQuadric f = faceQuadric(&positions[indices[i] * 3], &positions[indices[i + 1] * 3], &positions[indices[i + 2] * 3]);
		addQuadric(q[indices[i]], f);
		addQuadric(q[indices[i + 1]], f);
		addQuadric(q[indices[i + 2]], f);
	}

	return q;
}

// multi-pose linear cost of collapsing singleton i into singleton j, max across poses
float singletonCollapseCost(const std::vector<std::vector<TestQuadric> >& quadrics, const std::vector<std::vector<float> >& poses, unsigned i, unsigned j)
{
	float result = 0.f;

	for (size_t p = 0; p < poses.size(); ++p)
	{
		TestQuadric q = quadrics[p][i];
		addQuadric(q, quadrics[p][j]);

		float e = sqrtf(evalQuadric(q, &poses[p][j * 3]));
		if (e > result)
			result = e;
	}

	return result;
}

// replicate rescalePositions: normalize against the full-vertex bounding box longest axis, min mapped to 0
std::vector<float> normalizedCopy(const std::vector<float>& positions)
{
	float mn[3] = {1e30f, 1e30f, 1e30f};
	float mx[3] = {-1e30f, -1e30f, -1e30f};

	for (size_t v = 0; v < positions.size() / 3; ++v)
		for (int k = 0; k < 3; ++k)
		{
			float f = positions[v * 3 + k];
			mn[k] = mn[k] < f ? mn[k] : f;
			mx[k] = mx[k] > f ? mx[k] : f;
		}

	float extent = mx[0] - mn[0];
	for (int k = 1; k < 3; ++k)
		extent = extent > mx[k] - mn[k] ? extent : mx[k] - mn[k];
	float scale = extent == 0.f ? 0.f : 1.f / extent;

	std::vector<float> out(positions.size());
	for (size_t v = 0; v < positions.size() / 3; ++v)
		for (int k = 0; k < 3; ++k)
			out[v * 3 + k] = (positions[v * 3 + k] - mn[k]) * scale;

	return out;
}

// ============================== ensemble group ==============================

int planeReduction()
{
	const unsigned n = 17;
	std::vector<float> flat;
	makeGrid(flat, n, 0.f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {flat.data()};
	const size_t strides[] = {STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 1, 0);
	TASK_CHECK(s != 0);

	size_t count = meshopt_simplifyPoseStep(s, 768, 0.1f);
	TASK_CHECK(count > 0);
	TASK_CHECK(count <= 768);
	TASK_CHECK(count % 3 == 0);
	TASK_CHECK(count < indices.size());

	std::vector<unsigned int> out(indices.size());
	std::vector<unsigned int> remap(n * n);
	float error = -1.f;
	size_t read = meshopt_readPoseSimplifier(s, out.data(), remap.data(), &error);
	TASK_CHECK(read == count);
	TASK_CHECK(error >= 0.f && error <= 0.1f + 1e-6f);

	for (size_t i = 0; i < count; ++i)
		TASK_CHECK(out[i] < n * n);

	// every surviving face stays non-degenerate
	for (size_t i = 0; i < count; i += 3)
		TASK_CHECK(out[i] != out[i + 1] && out[i] != out[i + 2] && out[i + 1] != out[i + 2]);

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int foldPreservation()
{
	// mirrors the baseline 9x9 flat/bent comparison through the persistent multi-pose session
	const unsigned n = 9;
	std::vector<float> flat, bent;
	makeGrid(flat, n, 0.f);
	makeGrid(bent, n, 2.f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const size_t strides1[] = {STRIDE};
	const size_t strides2[] = {STRIDE, STRIDE};

	const float* flatPoses[] = {flat.data()};
	meshopt_PoseSimplifier* sf = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, flatPoses, strides1, 1, 0);
	TASK_CHECK(sf != 0);
	size_t flatCount = meshopt_simplifyPoseStep(sf, 96, 0.005f);
	TASK_CHECK(flatCount < indices.size());

	const float* bentPoses[] = {bent.data()};
	meshopt_PoseSimplifier* sb = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, bentPoses, strides1, 1, 0);
	TASK_CHECK(sb != 0);
	size_t bentCount = meshopt_simplifyPoseStep(sb, 96, 0.005f);

	// the raised folds must block aggressive reduction at this error ceiling
	TASK_CHECK(bentCount > flatCount);

	const float* bothPoses[] = {flat.data(), bent.data()};
	meshopt_PoseSimplifier* se = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, bothPoses, strides2, 2, 0);
	TASK_CHECK(se != 0);
	size_t ensembleCount = meshopt_simplifyPoseStep(se, 96, 0.005f);

	// shared decision-making is gated by the pose that needs the folds: it must not simplify like flat alone
	TASK_CHECK(ensembleCount >= bentCount);
	TASK_CHECK(ensembleCount > flatCount);

	meshopt_destroyPoseSimplifier(sf);
	meshopt_destroyPoseSimplifier(sb);
	meshopt_destroyPoseSimplifier(se);
	return 0;
}

int noFlipAnyPose()
{
	const unsigned n = 12;
	std::vector<float> flat, bent;
	makeGrid(flat, n, 0.f);
	makeGrid(bent, n, 1.5f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {flat.data(), bent.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	TASK_CHECK(s != 0);

	size_t count = meshopt_simplifyPoseStep(s, 0, 0.2f);
	TASK_CHECK(count > 0 && count % 3 == 0);

	std::vector<unsigned int> out(indices.size());
	meshopt_readPoseSimplifier(s, out.data(), 0, 0);

	for (size_t i = 0; i < count; i += 3)
	{
		unsigned a = out[i], b = out[i + 1], c = out[i + 2];
		TASK_CHECK(a != b && a != c && b != c);

		for (size_t p = 0; p < 2; ++p)
		{
			const std::vector<float>& pos = p == 0 ? flat : bent;
			const float* pa = &pos[a * 3];
			const float* pb = &pos[b * 3];
			const float* pc = &pos[c * 3];

			// z component of the oriented normal; grid winding keeps it strictly positive unless a flip/degeneration happened
			float crossz = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
			TASK_CHECK(crossz > 0.f);
		}
	}

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int partialLocksStillReduce()
{
	const unsigned n = 17;
	std::vector<float> flat, bent;
	makeGrid(flat, n, 0.f);
	makeGrid(bent, n, 1.f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	std::vector<unsigned char> lock(n * n, 0);
	for (unsigned y = 3; y + 3 < n; y += 4)
		for (unsigned x = 3; x + 3 < n; x += 4)
			lock[y * n + x] = 1;

	const float* poses[] = {flat.data(), bent.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, lock.data());
	TASK_CHECK(s != 0);

	size_t count = meshopt_simplifyPoseStep(s, 768, 0.2f);
	TASK_CHECK(count < indices.size()); // a partial lock must not freeze the whole mesh

	std::vector<unsigned int> remap(n * n);
	meshopt_readPoseSimplifier(s, 0, remap.data(), 0);

	for (size_t i = 0; i < n * n; ++i)
		if (lock[i])
			TASK_CHECK(remap[i] == i); // locked vertices remain their own representative

	// and an all-locked session must not collapse anything
	meshopt_PoseSimplifier* locked = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, std::vector<unsigned char>(n * n, 1).data());
	TASK_CHECK(locked != 0);
	TASK_CHECK(meshopt_simplifyPoseStep(locked, 100, 1e9f) == indices.size());
	meshopt_destroyPoseSimplifier(locked);

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int strideHandling()
{
	const unsigned n = 9;
	std::vector<float> packed, padded;
	makeGrid(packed, n, 1.f);
	size_t sf = 24 / sizeof(float);
	padded.assign(n * n * sf, 0.f);
	for (size_t v = 0; v < size_t(n) * n; ++v)
	{
		padded[v * sf + 0] = packed[v * 3 + 0];
		padded[v * sf + 1] = packed[v * 3 + 1];
		padded[v * sf + 2] = packed[v * 3 + 2];
	}

	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* pose12[] = {packed.data()};
	const size_t stride12[] = {12};
	const float* pose24[] = {padded.data()};
	const size_t stride24[] = {24};

	meshopt_PoseSimplifier* a = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, pose12, stride12, 1, 0);
	meshopt_PoseSimplifier* b = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, pose24, stride24, 1, 0);
	TASK_CHECK(a != 0 && b != 0);

	size_t ca = meshopt_simplifyPoseStep(a, 200, 0.1f);
	size_t cb = meshopt_simplifyPoseStep(b, 200, 0.1f);
	TASK_CHECK(ca == cb);

	float ea = 0.f, eb = 0.f;
	std::vector<unsigned int> oa(indices.size()), ob(indices.size());
	meshopt_readPoseSimplifier(a, oa.data(), 0, &ea);
	meshopt_readPoseSimplifier(b, ob.data(), 0, &eb);
	TASK_CHECK(ea == eb);
	TASK_CHECK(memcmp(oa.data(), ob.data(), ca * sizeof(unsigned int)) == 0);

	meshopt_destroyPoseSimplifier(a);
	meshopt_destroyPoseSimplifier(b);
	return 0;
}

// ============================== persistence group ==============================

int multistepContinuation()
{
	const unsigned n = 17;
	std::vector<float> flat, bent;
	makeGrid(flat, n, 0.f);
	makeGrid(bent, n, 1.f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {flat.data(), bent.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	TASK_CHECK(s != 0);

	float previousError = 0.f;
	size_t previousCount = indices.size();

	const size_t targets[] = {1200, 1000, 768};
	const float ceilings[] = {0.005f, 0.02f, 0.1f};

	for (int k = 0; k < 3; ++k)
	{
		size_t count = meshopt_simplifyPoseStep(s, targets[k], ceilings[k]);
		TASK_CHECK(count <= previousCount);
		TASK_CHECK(count > 0);

		float error = 0.f;
		meshopt_readPoseSimplifier(s, 0, 0, &error);
		TASK_CHECK(error >= previousError - 1e-9f); // monotonic, never decreases
		TASK_CHECK(error <= ceilings[k] + 1e-6f);

		previousError = error;
		previousCount = count;
	}

	TASK_CHECK(previousCount <= 768);
	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int noopAndResumeRules()
{
	const unsigned n = 13;
	std::vector<float> flat, bent;
	makeGrid(flat, n, 0.f);
	makeGrid(bent, n, 1.f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {flat.data(), bent.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	TASK_CHECK(s != 0);

	size_t count = meshopt_simplifyPoseStep(s, 400, 0.01f);
	float error = 0.f;
	meshopt_readPoseSimplifier(s, 0, 0, &error);

	// target not below current count: state unchanged
	TASK_CHECK(meshopt_simplifyPoseStep(s, count + 100, 1.f) == count);
	float unchanged = 0.f;
	meshopt_readPoseSimplifier(s, 0, 0, &unchanged);
	TASK_CHECK(unchanged == error);

	// error ceiling below the error already reached: state unchanged
	if (error > 0.f)
		TASK_CHECK(meshopt_simplifyPoseStep(s, 100, error * 0.5f) == count);

	// raising the error ceiling lets the session continue from the preserved state
	size_t continued = meshopt_simplifyPoseStep(s, count > 60 ? count - 60 : 0, 0.2f);
	TASK_CHECK(continued <= count);

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int independentClusterQem()
{
	// 3x3 grid: only the interior center vertex (id 4) can collapse; borders are locked.
	// Driving to remove exactly two triangles performs one singleton->singleton collapse, so the reported
	// session error must equal the independently recomputed minimum multi-pose edge cost.
	const unsigned n = 3;
	std::vector<float> raw0, raw1;
	makeGrid(raw0, n, 0.f);
	makeGrid(raw1, n, 0.5f);

	// quadrics are computed on per-pose normalized coordinates, exactly like the library does internally
	std::vector<float> pose0 = normalizedCopy(raw0);
	std::vector<float> pose1 = normalizedCopy(raw1);

	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {pose0.data(), pose1.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	TASK_CHECK(s != 0);

	size_t target = indices.size() - 6; // one edge collapse removes two triangles
	size_t count = meshopt_simplifyPoseStep(s, target, 1.f);
	TASK_CHECK(count == target);

	float reported = 0.f;
	meshopt_readPoseSimplifier(s, 0, 0, &reported);

	std::vector<std::vector<TestQuadric> > q0, q1;
	q0.push_back(initialQuadrics(indices, pose0));
	q1.push_back(initialQuadrics(indices, pose1));
	std::vector<std::vector<TestQuadric> > quadrics;
	quadrics.push_back(q0[0]);
	quadrics.push_back(q1[0]);
	std::vector<std::vector<float> > poseData;
	poseData.push_back(pose0);
	poseData.push_back(pose1);

	// center vertex 4 connects to 1, 3, 5, 7; the neighbors are border-locked so only center->neighbor is legal
	const unsigned center = 4;
	const unsigned neighbors[4] = {1, 3, 5, 7};
	float expected = 1e30f;

	for (int k = 0; k < 4; ++k)
	{
		float c = singletonCollapseCost(quadrics, poseData, center, neighbors[k]);
		expected = expected < c ? expected : c;
	}

	TASK_CHECK(reported > 0.f);
	TASK_CHECK(fabsf(reported - expected) <= 1e-5f * (1.f + expected));

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int sourceReleaseAndIsolation()
{
	const unsigned n = 9;
	std::vector<float> flat, bent;
	makeGrid(flat, n, 0.f);
	makeGrid(bent, n, 1.f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {flat.data(), bent.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* a = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	meshopt_PoseSimplifier* b = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	TASK_CHECK(a != 0 && b != 0);

	// mutate and release every source buffer after creation
	std::fill(flat.begin(), flat.end(), -123.f);
	std::fill(bent.begin(), bent.end(), -123.f);
	std::fill(indices.begin(), indices.end(), 0u);

	size_t ca = meshopt_simplifyPoseStep(a, 200, 0.1f);
	TASK_CHECK(ca > 0 && ca <= 200);

	// the second session must be completely unaffected by the first one's work
	size_t untouched = meshopt_readPoseSimplifier(b, 0, 0, 0);
	TASK_CHECK(untouched == indices.size()); // session b never ran a step

	meshopt_destroyPoseSimplifier(a);
	meshopt_destroyPoseSimplifier(b);
	return 0;
}

// ============================== identity group ==============================

int trajectorySplitNotWelded()
{
	// vertices 0 and 8 coincide in pose 0 but separate in pose 1: they must keep distinct identities
	const unsigned n = 3;
	std::vector<float> p0, p1;
	makeGrid(p0, n, 0.f);
	makeGrid(p1, n, 0.f);

	p0[8 * 3 + 0] = p0[0]; p0[8 * 3 + 1] = p0[1]; p0[8 * 3 + 2] = p0[2];

	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {p0.data(), p1.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	TASK_CHECK(s != 0);

	std::vector<unsigned int> remap(n * n);
	meshopt_readPoseSimplifier(s, 0, remap.data(), 0);
	TASK_CHECK(remap[0] != remap[8]);

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int trajectoryMatchWelded()
{
	// identical full trajectory in every pose: distinct vertex ids must weld even though they are not adjacent
	const unsigned n = 3;
	std::vector<float> p0, p1;
	makeGrid(p0, n, 0.f);
	makeGrid(p1, n, 0.f);

	p0[8 * 3 + 0] = p0[0]; p0[8 * 3 + 1] = p0[1]; p0[8 * 3 + 2] = p0[2];
	p1[8 * 3 + 0] = p1[0]; p1[8 * 3 + 1] = p1[1]; p1[8 * 3 + 2] = p1[2];

	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {p0.data(), p1.data()};
	const size_t strides[] = {STRIDE, STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 2, 0);
	TASK_CHECK(s != 0);

	std::vector<unsigned int> remap(n * n);
	meshopt_readPoseSimplifier(s, 0, remap.data(), 0);
	TASK_CHECK(remap[0] == remap[8]);

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int remapAndOriginalIds()
{
	const unsigned n = 17;
	std::vector<float> flat;
	makeGrid(flat, n, 0.f);
	std::vector<unsigned int> indices;
	makeGridIndices(indices, n);

	const float* poses[] = {flat.data()};
	const size_t strides[] = {STRIDE};

	meshopt_PoseSimplifier* s = meshopt_createPoseSimplifier(indices.data(), indices.size(), n * n, poses, strides, 1, 0);
	TASK_CHECK(s != 0);

	size_t count = meshopt_simplifyPoseStep(s, 768, 0.1f);
	TASK_CHECK(count > 0 && count <= 768);

	std::vector<unsigned int> out(indices.size());
	std::vector<unsigned int> remap(n * n);
	meshopt_readPoseSimplifier(s, out.data(), remap.data(), 0);

	std::vector<unsigned char> used(n * n, 0);
	for (size_t i = 0; i < count; ++i)
	{
		TASK_CHECK(out[i] < n * n); // output always references original vertex ids
		used[out[i]] = 1;
	}

	for (size_t i = 0; i < size_t(n) * n; ++i)
	{
		if (used[i])
		{
			// referenced vertices map to a used representative, and representatives are fixed points
			TASK_CHECK(used[remap[i]]);
			TASK_CHECK(remap[remap[i]] == remap[i]);
		}
		else
		{
			TASK_CHECK(remap[i] == i); // unreferenced vertices map to themselves
		}
	}

	meshopt_destroyPoseSimplifier(s);
	return 0;
}

int nullSafety()
{
	TASK_CHECK(meshopt_simplifyPoseStep(0, 0, 0.f) == 0);
	TASK_CHECK(meshopt_readPoseSimplifier(0, 0, 0, 0) == 0);
	meshopt_destroyPoseSimplifier(0); // must be a no-op, not a crash
	return 0;
}

} // namespace

const task_case task_cases[] = {
    {"ensemble", "plane_reduction", planeReduction},
    {"ensemble", "fold_preservation", foldPreservation},
    {"ensemble", "no_flip_any_pose", noFlipAnyPose},
    {"ensemble", "partial_locks_still_reduce", partialLocksStillReduce},
    {"ensemble", "stride_handling", strideHandling},

    {"persistence", "multistep_continuation", multistepContinuation},
    {"persistence", "noop_and_resume_rules", noopAndResumeRules},
    {"persistence", "independent_cluster_qem", independentClusterQem},
    {"persistence", "source_release_and_isolation", sourceReleaseAndIsolation},

    {"identity", "trajectory_split_not_welded", trajectorySplitNotWelded},
    {"identity", "trajectory_match_welded", trajectoryMatchWelded},
    {"identity", "remap_and_original_ids", remapAndOriginalIds},
    {"identity", "null_safety", nullSafety},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

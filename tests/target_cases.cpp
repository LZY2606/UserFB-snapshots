#include "test_api.h"

#include "DepthOfField.h"
#include "Renderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace swr;

namespace {

const float kEps = 1e-4f;

bool closeTo(float a, float b, float eps = kEps)
{
	return std::fabs(a - b) <= eps;
}

// Independent reference for the signed defocus radius.
float referenceRadius(float z, float f, float a, float s, float h, float imageHeight, float maxRadius)
{
	if (!(z > 0.0f) || !(s > f) || !(f > 0.0f) || !(h > 0.0f) || !(a >= 0.0f))
		return 0.0f;
	const float r = imageHeight / h * a * f / (s - f) * (z - s) / z;
	return std::max(-maxRadius, std::min(maxRadius, r));
}

DepthOfFieldCamera makeCamera(float aperture)
{
	DepthOfFieldCamera camera;
	camera.focalLength = 0.05f;
	camera.apertureRadius = aperture;
	camera.focusDistance = 1.2f;
	camera.sensorHeight = 0.024f;
	return camera;
}

DepthOfFieldResolveParams makeParams(float aperture, float maxRadius, int height)
{
	DepthOfFieldResolveParams params;
	params.camera = makeCamera(aperture);
	params.imageHeight = (float)height;
	params.maxRadius = maxRadius;
	params.background[0] = 0.1f;
	params.background[1] = 0.2f;
	params.background[2] = 0.3f;
	return params;
}

// Fixed disc tables for radius 1 and radius 2 (independent of the implementation).
const int kDisc1[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int kDisc2[13][2] = {
	{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
	{2, 0}, {-2, 0}, {0, 2}, {0, -2}};

// Pixel shader writing into a DepthOfFieldBuffer.
struct BufferPixelShader : PixelShaderBase<BufferPixelShader> {
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static const int AVarCount = 0;
	static const int PVarCount = 1;

	static DepthOfFieldBuffer *buffer;

	static void drawPixel(const PixelData &p)
	{
		buffer->submit(p.x, p.y, p.pvar[0], p.pvar[0], p.pvar[0], p.w, p.z);
	}
};

DepthOfFieldBuffer *BufferPixelShader::buffer = nullptr;

struct BufferVertexShader : VertexShaderBase<BufferVertexShader> {
	static const int AttribCount = 1;
	static const int AVarCount = 0;
	static const int PVarCount = 1;

	struct Input {
		float ndcX, ndcY, viewDepth, shade;
	};

	static void processVertex(VertexShaderInput in, VertexShaderOutput *out)
	{
		const Input *v = static_cast<const Input *>(in[0]);
		// Multiply the NDC position by w so the perspective divide restores it
		// while the clip w carries the view depth.
		out->x = v->ndcX * v->viewDepth;
		out->y = v->ndcY * v->viewDepth;
		out->z = 0.0f;
		out->w = v->viewDepth;
		out->pvar[0] = v->shade;
	}
};

// Screen-space barycentric coordinates for a triangle with the given
// window-space vertices.
void barycentric(float px, float py,
                 const float sx[3], const float sy[3],
                 float l[3])
{
	const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
	const float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
	const float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
	l[0] = w0;
	l[1] = w1;
	l[2] = 1.0f - w0 - w1;
}

void testBlurRadiusAndValidation(Suite &s)
{
	const float f = 0.05f, a = 0.03f, focus = 1.2f, h = 0.024f, H = 144.0f, maxR = 14.0f;

	DepthOfFieldCamera camera = makeCamera(a);
	s.require(camera.valid(), "default camera must be valid");

	// Focus plane has no defocus.
	s.require(signedBlurRadius(focus, camera, H, maxR) == 0.0f, "focus plane radius must be zero");

	// Near layers are negative, far layers non-negative.
	const float nearRadius = signedBlurRadius(0.62f, camera, H, maxR);
	const float farRadius = signedBlurRadius(2.6f, camera, H, maxR);
	s.require(nearRadius < 0.0f, "in-front-of-focus layer must be negative");
	s.require(farRadius > 0.0f, "behind-focus layer must be non-negative");
	s.require(closeTo(nearRadius, referenceRadius(0.62f, f, a, focus, h, H, maxR), 1e-3f),
	          "near radius must match the thin lens formula");
	s.require(closeTo(farRadius, referenceRadius(2.6f, f, a, focus, h, H, maxR), 1e-3f),
	          "far radius must match the thin lens formula");

	// Maximum radius clamps the magnitude, preserving the sign.
	s.require(signedBlurRadius(2.6f, camera, H, 1.0f) == 1.0f, "far radius must clamp to +maxRadius");
	s.require(signedBlurRadius(0.62f, camera, H, 1.0f) == -1.0f, "near radius must clamp to -maxRadius");

	// Invalid camera parameters are rejected.
	DepthOfFieldCamera bad = camera;
	bad.focusDistance = bad.focalLength;
	s.require(!bad.valid(), "focus distance equal to focal length must be invalid");
	bad = camera;
	bad.apertureRadius = -0.1f;
	s.require(!bad.valid(), "negative aperture must be invalid");
	bad = camera;
	bad.sensorHeight = 0.0f;
	s.require(!bad.valid(), "non-positive sensor height must be invalid");
	bad = camera;
	bad.focalLength = 0.0f;
	s.require(!bad.valid(), "non-positive focal length must be invalid");
}

void testFocusPlaneAndZeroAperture(Suite &s)
{
	const int w = 4, h = 4;
	DepthOfFieldBuffer buffer(w, h);
	buffer.clear();
	buffer.submit(1, 1, 0.2f, 0.4f, 0.6f, 1.2f, 0.4f);   // exactly on the focus plane
	buffer.submit(2, 2, 3.5f, 0.25f, 1.75f, 2.6f, 0.4f); // HDR far layer, zero aperture below

	DepthOfFieldResolveParams params = makeParams(0.03f, 14.0f, h);
	DepthOfFieldResult focus;
	resolveDepthOfField(buffer, params, focus);

	const size_t i11 = ((size_t)1 * w + 1) * 3;
	s.require(closeTo(focus.rgb[i11 + 0], 0.2f) && closeTo(focus.rgb[i11 + 1], 0.4f) &&
	              closeTo(focus.rgb[i11 + 2], 0.6f),
	          "a pixel on the focus plane must keep its linear color");

	DepthOfFieldResolveParams zero = params;
	zero.camera.apertureRadius = 0.0f;
	DepthOfFieldResult sharp;
	resolveDepthOfField(buffer, zero, sharp);
	const size_t i22 = ((size_t)2 * w + 2) * 3;
	s.require(sharp.rgb[i22 + 0] == 3.5f && sharp.rgb[i22 + 1] == 0.25f && sharp.rgb[i22 + 2] == 1.75f,
	          "zero aperture must preserve HDR linear color above 1");
	s.require(focus.rgb[i11 + 0] > 0.19f && focus.rgb[i11 + 0] < 0.21f,
	          "focused pixel must not be tinted by the background");
}

void testDiscWeightsAndNearCoverage(Suite &s)
{
	// --- A near source at the corner of a 3x3 buffer with radius exactly 1.
	// The disc has 5 cells, but only 3 are inside the image. The divisor still
	// uses the complete disc, so every covered pixel gets weight 1/5.
	{
		const int w = 3, h = 3;
		DepthOfFieldBuffer buffer(w, h);
		buffer.clear();
		buffer.submit(0, 0, 0.5f, 0.25f, 0.125f, 0.62f, 0.3f); // near layer

		DepthOfFieldResolveParams params = makeParams(10.0f, 1.0f, h); // huge aperture, clamp to 1
		DepthOfFieldResult result;
		resolveDepthOfField(buffer, params, result);

		const float bg[3] = {0.1f, 0.2f, 0.3f};
		const float src[3] = {0.5f, 0.25f, 0.125f};
		const float expected[3] = {
			src[0] / 5.0f + bg[0] * 4.0f / 5.0f,
			src[1] / 5.0f + bg[1] * 4.0f / 5.0f,
			src[2] / 5.0f + bg[2] * 4.0f / 5.0f};

		for (int i = 0; i < 5; ++i) {
			const int x = kDisc1[i][0];
			const int y = kDisc1[i][1];
			if (x < 0 || y < 0 || x >= w || y >= h)
				continue; // Outside the frame: dropped, not re-normalised.
			const size_t p = ((size_t)y * w + x) * 3;
			s.require(closeTo(result.rgb[p + 0], expected[0]) &&
			              closeTo(result.rgb[p + 1], expected[1]) &&
			              closeTo(result.rgb[p + 2], expected[2]),
			          "near disc cell weight must use the complete disc denominator");
		}
		// A cell outside the radius keeps the background.
		const size_t out = ((size_t)2 * w + 2) * 3;
		s.require(closeTo(result.rgb[out + 0], bg[0]) && closeTo(result.rgb[out + 1], bg[1]) &&
		              closeTo(result.rgb[out + 2], bg[2]),
		          "cell outside the disc must stay background");
		s.require(result.radius[0] == -1.0f, "near radius map must be signed and clamped");
	}

	// --- A far source in the centre of a 5x5 buffer with radius exactly 2:
	// 13 cells, each fully normalised to the source color.
	{
		const int w = 5, h = 5;
		DepthOfFieldBuffer buffer(w, h);
		buffer.clear();
		buffer.submit(2, 2, 0.4f, 0.6f, 0.8f, 2.6f, 0.3f); // far layer

		DepthOfFieldResolveParams params = makeParams(10.0f, 2.0f, h);
		DepthOfFieldResult result;
		resolveDepthOfField(buffer, params, result);

		for (int i = 0; i < 13; ++i) {
			const int x = 2 + kDisc2[i][0];
			const int y = 2 + kDisc2[i][1];
			const size_t p = ((size_t)y * w + x) * 3;
			s.require(closeTo(result.rgb[p + 0], 0.4f) && closeTo(result.rgb[p + 1], 0.6f) &&
			              closeTo(result.rgb[p + 2], 0.8f),
			          "far disc cell must be the fully normalised source color");
		}
		s.require(result.radius[((size_t)2 * w + 2)] == 2.0f, "far radius map must be positive");
	}

	// --- Near coverage saturates at one. The centre pixel receives 1/5 from a
	// radius-1.25 source and 1/9 from each of eight radius-1.75 neighbours. The
	// raw weight sum is 1.0889, so the coverage must clamp to 1 and the color
	// must be the plain weighted average.
	{
		const int w = 3, h = 3;
		DepthOfFieldBuffer buffer(w, h);
		buffer.clear();

		const float f = 0.05f, a = 0.03f, focus = 1.2f, hh = 0.024f, H = 3.0f;
		const float K = H / hh * a * f / (focus - f);
		// Invert r = K * (z - focus) / z for the two desired radii.
		const float zNear1 = K * focus / (K - (-1.25f));
		const float zNear15 = K * focus / (K - (-1.75f));

		buffer.submit(1, 1, 1.0f, 0.0f, 0.0f, zNear1, 0.3f);
		for (int y = 0; y < 3; ++y)
			for (int x = 0; x < 3; ++x)
				if (!(x == 1 && y == 1))
					buffer.submit(x, y, 0.0f, 1.0f, 0.0f, zNear15, 0.3f);

		DepthOfFieldResolveParams params = makeParams(a, 2.0f, h);
		params.imageHeight = H;
		DepthOfFieldResult result;
		resolveDepthOfField(buffer, params, result);

		const float weightSum = 1.0f / 5.0f + 8.0f / 9.0f;
		const float expectedR = (1.0f / 5.0f) / weightSum;
		const float expectedG = (8.0f / 9.0f) / weightSum;
		const size_t centre = ((size_t)1 * w + 1) * 3;
		s.require(closeTo(result.rgb[centre + 0], expectedR, 1e-3f) &&
		              closeTo(result.rgb[centre + 1], expectedG, 1e-3f),
		          "near coverage must clamp to one and keep the weighted average");
		s.require(closeTo(result.radius[((size_t)1 * w + 1)], -1.25f, 1e-2f),
		          "near radius must be written to the radius map");
	}
}

void testLayerComposition(Suite &s)
{
	// A near and a far source whose radius-1 discs overlap at the centre.
	const int w = 3, h = 3;
	DepthOfFieldBuffer buffer(w, h);
	buffer.clear();
	buffer.submit(0, 1, 1.0f, 0.0f, 0.0f, 0.62f, 0.3f); // near, red
	buffer.submit(2, 1, 0.0f, 1.0f, 0.0f, 2.6f, 0.4f);   // far, green

	DepthOfFieldResolveParams params = makeParams(10.0f, 1.0f, h);
	DepthOfFieldResult result;
	resolveDepthOfField(buffer, params, result);

	// At (1,1): near weight 1/5 gives coverage 0.2; the far layer is fully
	// normalised to green and sits underneath. The layers are not normalised
	// together, so the result is 0.2 red over 0.8 green.
	const size_t centre = ((size_t)1 * w + 1) * 3;
	s.require(closeTo(result.rgb[centre + 0], 0.2f) && closeTo(result.rgb[centre + 1], 0.8f) &&
	              closeTo(result.rgb[centre + 2], 0.0f),
	          "near and far layers must be composited without joint normalisation");

	// A near-only cell is the near color scaled by coverage over the background.
	const size_t left = ((size_t)1 * w + 0) * 3;
	s.require(closeTo(result.rgb[left + 0], 0.2f + 0.1f * 0.8f) &&
	              closeTo(result.rgb[left + 1], 0.2f * 0.8f),
	          "near layer must cover the background by its coverage");
}

void testScreenEdgeAndHDR(Suite &s)
{
	// A far, HDR source at the left border must still contribute where its
	// disc enters the image, and colors above one must survive the
	// normalisation (they are not clamped to 1).
	const int w = 5, h = 3;
	DepthOfFieldBuffer buffer(w, h);
	buffer.clear();
	buffer.submit(0, 1, 2.5f, 1.2f, 0.4f, 2.6f, 0.4f);

	DepthOfFieldResolveParams params = makeParams(10.0f, 2.0f, h);
	DepthOfFieldResult result;
	resolveDepthOfField(buffer, params, result);

	// A radius-2 disc has 13 cells, two of which lie left of the image. Those
	// cells are simply dropped, but the remaining far-layer cells still
	// normalise to the weighted average, so the HDR source keeps its value.
	const size_t right = ((size_t)1 * w + 1) * 3;
	s.require(closeTo(result.rgb[right + 0], 2.5f, 1e-4f) &&
	              closeTo(result.rgb[right + 1], 1.2f, 1e-4f) &&
	              closeTo(result.rgb[right + 2], 0.4f, 1e-4f),
	          "far layer must normalise to its weighted average without clamping HDR");
	s.require(result.rgb[right + 0] > 1.0f && result.rgb[right + 1] > 1.0f,
	          "HDR far colors above one must survive the resolve");
	s.require(result.radius[((size_t)1 * w + 0)] == 2.0f, "far radius at the source must be +max");

	// The pixel two steps right lies exactly on the radius-2 rim and is the
	// last in-frame cell of the source row. It is still inside the disc.
	s.require(closeTo(result.rgb[((size_t)1 * w + 2) * 3 + 0], 2.5f, 1e-4f),
	          "the rim cell must lie inside the radius-2 disc");

	// The final pixel outside every disc keeps the untouched background.
	const size_t untouched = ((size_t)0 * w + 4) * 3;
	s.require(closeTo(result.rgb[untouched + 0], 0.1f) && closeTo(result.rgb[untouched + 2], 0.3f),
	          "pixels beyond the disc must keep the background");

	// Dropping off-screen cells must not shrink the layer where it remains:
	// the cell one right and one down is in-frame but the cell two right and
	// one down is outside the disc (distance sqrt(5) > 2).
	s.require(closeTo(result.rgb[((size_t)2 * w + 2) * 3 + 0], 0.1f, 1e-4f),
	          "cells beyond the radius must stay background");
}

void testBackgroundAndInputImmutability(Suite &s)
{
	// Empty frame: pure background, zero radii.
	{
		DepthOfFieldBuffer buffer(4, 2);
		buffer.clear();
		DepthOfFieldResolveParams params = makeParams(0.03f, 8.0f, 2);
		DepthOfFieldResult result;
		resolveDepthOfField(buffer, params, result);
		bool ok = result.rgb.size() == 4u * 2u * 3u;
		for (size_t i = 0; i < result.rgb.size(); i += 3)
			ok = ok && result.rgb[i + 0] == 0.1f && result.rgb[i + 1] == 0.2f && result.rgb[i + 2] == 0.3f;
		for (size_t i = 0; i < result.radius.size(); ++i)
			ok = ok && result.radius[i] == 0.0f;
		s.require(ok, "an empty buffer must resolve to the linear background");
	}

	// The input buffer is never modified by the resolve.
	{
		DepthOfFieldBuffer buffer(5, 4);
		buffer.clear();
		for (int y = 0; y < 4; ++y)
			for (int x = 0; x < 5; ++x)
				buffer.submit(x, y, 0.1f * x, 0.1f * y, 0.05f, 0.5f + 0.4f * x, 0.2f + 0.1f * y);

		std::vector<DepthOfFieldSample> before(buffer.data(), buffer.data() + 5 * 4);
		DepthOfFieldResolveParams params = makeParams(0.05f, 6.0f, 4);
		DepthOfFieldResult first, second;
		resolveDepthOfField(buffer, params, first);
		resolveDepthOfField(buffer, params, second);

		const bool unchanged = [&] {
			for (size_t i = 0; i < before.size(); ++i) {
				const DepthOfFieldSample &a = before[i];
				const DepthOfFieldSample &b = buffer.data()[i];
				if (a.r != b.r || a.g != b.g || a.b != b.b || a.viewDepth != b.viewDepth ||
				    a.windowDepth != b.windowDepth || a.valid != b.valid)
					return false;
			}
			return true;
		}();
		s.require(unchanged, "resolve must not modify the input buffer");
		s.require(first.rgb == second.rgb, "resolve must be deterministic and use only the original frame");
	}
}

void testDepthVisibility(Suite &s)
{
	DepthOfFieldBuffer buffer(3, 3);
	buffer.clear();

	s.require(buffer.submit(1, 1, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f), "first fragment must be accepted");
	s.require(!buffer.submit(1, 1, 0.0f, 1.0f, 0.0f, 2.0f, 0.7f), "larger window depth must be rejected");
	s.require(buffer.sample(1, 1).r == 1.0f, "rejected fragment must not overwrite the surface");
	s.require(buffer.submit(1, 1, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f), "equal window depth keeps the later fragment");
	s.require(buffer.sample(1, 1).b == 1.0f, "later equal-depth fragment must win");
	s.require(!buffer.submit(3, 3, 1.0f, 1.0f, 1.0f, 0.1f, 0.1f), "out-of-bounds fragment must be rejected");
	s.require(!buffer.sample(0, 0).valid, "a fragment never submitted leaves an invalid sample");
}

void testRealTriangles(Suite &s)
{
	const int size = 32;
	const float f = 0.05f, a = 0.03f, focus = 1.2f, h = 0.024f, maxR = 8.0f;
	const float H = (float)size;

	// Three vertices with different clip w (view depths 1, 2 and 4).
	BufferVertexShader::Input vertices[3] = {
		{-0.8f, -0.8f, 1.0f, 0.25f},
		{0.8f, -0.8f, 2.0f, 0.55f},
		{0.0f, 0.8f, 4.0f, 0.85f}};
	int indices[3] = {0, 1, 2};

	Rasterizer rasterizer;
	VertexProcessor processor(&rasterizer);
	rasterizer.setRasterMode(RasterMode::Span);
	rasterizer.setScissorRect(0, 0, size, size);
	rasterizer.setPixelShader<BufferPixelShader>();
	processor.setViewport(0, 0, size, size);
	processor.setCullMode(CullMode::None);
	processor.setVertexShader<BufferVertexShader>();
	processor.setVertexAttribPointer(0, sizeof(BufferVertexShader::Input), vertices);

	DepthOfFieldBuffer buffer(size, size);
	buffer.clear();
	BufferPixelShader::buffer = &buffer;
	processor.drawElements(DrawMode::Triangle, 3, indices);
	BufferPixelShader::buffer = nullptr;

	// Window-space positions after the perspective divide and viewport map.
	const float px = size / 2.0f;
	float sx[3], sy[3];
	const float ndcX[3] = {-0.8f, 0.8f, 0.0f};
	const float ndcY[3] = {-0.8f, -0.8f, 0.8f};
	for (int i = 0; i < 3; ++i) {
		sx[i] = px * ndcX[i] + px;
		sy[i] = px * -ndcY[i] + px;
	}

	const int testX = 15, testY = 15;
	const float cx = testX + 0.5f, cy = testY + 0.5f;
	float lambda[3];
	barycentric(cx, cy, sx, sy, lambda);

	// Perspective-correct view depth: interpolate 1/w affinely, then invert.
	const float invw = lambda[0] / 1.0f + lambda[1] / 2.0f + lambda[2] / 4.0f;
	const float expectedDepth = 1.0f / invw;

	const DepthOfFieldSample &sample = buffer.sample(testX, testY);
	s.require(sample.valid, "the triangle must cover the tested pixel");
	s.require(closeTo(sample.viewDepth, expectedDepth, 2e-3f),
	          "view depth must be perspective-correct across different w");

	// Window depth comes from the projected z (depth range 0..1 maps z=0 to 0.5).
	s.require(closeTo(sample.windowDepth, 0.5f, 1e-3f), "window depth must be submitted separately");

	DepthOfFieldResolveParams params = makeParams(a, maxR, size);
	params.imageHeight = H;
	DepthOfFieldResult result;
	resolveDepthOfField(buffer, params, result);
	const float expectedRadius = referenceRadius(expectedDepth, f, a, focus, h, H, maxR);
	s.require(closeTo(result.radius[(size_t)testY * size + testX], expectedRadius, 2e-3f),
	          "radius of a real triangle must match the thin lens formula");

	// The closest visible view depth must approach the w=1 vertex (within one
	// pixel of it); equal-depth vertices would not exercise this.
	float minDepth = 1e30f;
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
			if (buffer.sample(x, y).valid)
				minDepth = std::min(minDepth, buffer.sample(x, y).viewDepth);
	s.require(minDepth > 1.0f && minDepth < 1.05f,
	          "view depth near the w=1 vertex must approach 1");
}

} // namespace

void target_cases(Suite &s)
{
	s.run("blur_radius_and_validation", [&] { testBlurRadiusAndValidation(s); });
	s.run("focus_plane_and_zero_aperture", [&] { testFocusPlaneAndZeroAperture(s); });
	s.run("disc_weights_and_near_coverage", [&] { testDiscWeightsAndNearCoverage(s); });
	s.run("layer_composition", [&] { testLayerComposition(s); });
	s.run("screen_edge_and_hdr", [&] { testScreenEdgeAndHDR(s); });
	s.run("background_and_input_immutability", [&] { testBackgroundAndInputImmutability(s); });
	s.run("depth_visibility", [&] { testDepthVisibility(s); });
	s.run("real_triangles", [&] { testRealTriangles(s); });
}

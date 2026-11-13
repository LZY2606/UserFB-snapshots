/*
MIT License

Copyright (c) 2017-2020 Markus Trenkwalder

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Headless depth-of-field preview. Geometry is generated procedurally and
// drawn through the ordinary VertexProcessor/Rasterizer/pixel shader path into
// a DepthOfFieldBuffer. No window, SDL or graphics device is involved.

#include "DepthOfField.h"
#include "Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace swr;

namespace {

const int kWidth = 192;
const int kHeight = 144;
const float kAspect = (float)kWidth / (float)kHeight;
const float kFovY = 60.0f * 3.14159265358979323846f / 180.0f;
const float kNear = 0.2f;
const float kFar = 6.0f;

struct Vertex {
	float x, y, z; // Right-handed view space. The camera looks down -z.
	float r, g, b;
	int material;
};

struct Triangle {
	Vertex v[3];
};

struct PreviewPixelShader : PixelShaderBase<PreviewPixelShader> {
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static const int AVarCount = 0;
	static const int PVarCount = 7;

	static DepthOfFieldBuffer *buffer;
	static long discarded;

	static void drawPixel(const PixelData &p)
	{
		const float vx = p.pvar[0];
		const float vy = p.pvar[1];
		float r = p.pvar[3];
		float g = p.pvar[4];
		float b = p.pvar[5];
		const int material = (int)std::floor(p.pvar[6] + 0.5f);

		if (material == 2) {
			// Punch screen-space holes into the near occluder so the geometry
			// behind it stays visible where the fragment is discarded. The
			// decision only depends on the pixel coordinate, so the Span and
			// Block rasterizers agree on the interior.
			if ((p.x % 11) < 4) {
				++discarded;
				return;
			}
		}

		if (material == 1) {
			// Fine checker board living on the focus plane.
			const int cx = (int)std::floor(vx * 42.0f);
			const int cy = (int)std::floor(vy * 42.0f);
			float shade = ((cx + cy) & 1) ? 1.0f : 0.30f;
			if ((cx & 7) == 0 || (cy & 7) == 0)
				shade *= 0.35f;
			r *= shade;
			g *= shade;
			b *= shade;
		}

		// p.w is the perspective-correct, right-handed view depth (-z).
		buffer->submit(p.x, p.y, r, g, b, p.w, p.z);
	}
};

DepthOfFieldBuffer *PreviewPixelShader::buffer = nullptr;
long PreviewPixelShader::discarded = 0;

struct PreviewVertexShader : VertexShaderBase<PreviewVertexShader> {
	static const int AttribCount = 1;
	static const int AVarCount = 0;
	static const int PVarCount = 7;

	static void processVertex(VertexShaderInput in, VertexShaderOutput *out)
	{
		const Vertex *v = static_cast<const Vertex *>(in[0]);
		const float t = std::tan(kFovY * 0.5f);
		const float depth = -v->z;

		// Standard perspective projection. The clip w is exactly the positive
		// right-handed view depth, so the pixel shader can use p.w directly.
		out->x = v->x / (t * kAspect);
		out->y = v->y / t;
		out->z = ((kFar + kNear) / (kNear - kFar)) * v->z +
		         (2.0f * kFar * kNear) / (kNear - kFar);
		out->w = depth;

		out->pvar[0] = v->x;
		out->pvar[1] = v->y;
		out->pvar[2] = v->z;
		out->pvar[3] = v->r;
		out->pvar[4] = v->g;
		out->pvar[5] = v->b;
		out->pvar[6] = (float)v->material;
	}
};

void addQuad(std::vector<Triangle> &triangles, float z, float cx, float cy,
             float halfX, float halfY, float r, float g, float b, int material)
{
	// Screen winding matches the examples: facing > 0 is visible.
	Vertex v0{cx - halfX, cy - halfY, z, r, g, b, material};
	Vertex v1{cx + halfX, cy - halfY, z, r, g, b, material};
	Vertex v2{cx + halfX, cy + halfY, z, r, g, b, material};
	Vertex v3{cx - halfX, cy + halfY, z, r, g, b, material};
	triangles.push_back({{v0, v1, v2}});
	triangles.push_back({{v0, v2, v3}});
}

std::vector<Triangle> buildScene()
{
	std::vector<Triangle> triangles;

	// Fine grid on the focal plane (view depth 1.2). It covers the centre of
	// the frame and stays sharp.
	addQuad(triangles, -1.2f, 0.0f, 0.0f, 0.50f, 0.40f, 0.85f, 0.86f, 0.90f, 1);

	// Bright, HDR blocks behind the focus plane (view depth 2.6). They sit
	// above the grid in screen space so they stay visible.
	for (int i = 0; i < 5; ++i) {
		const float x = -0.80f + 0.40f * (float)i;
		addQuad(triangles, -2.6f, x, 1.02f, 0.13f, 0.12f, 2.6f, 1.35f, 0.45f, 0);
	}

	// Near occluder (view depth 0.62), in front of the lower-left of the grid.
	addQuad(triangles, -0.62f, -0.20f, -0.15f, 0.15f, 0.16f, 1.0f, 0.22f, 0.08f, 2);

	return triangles;
}

void renderScene(const std::vector<Triangle> &triangles, RasterMode mode, bool reverse,
                 DepthOfFieldBuffer &buffer)
{
	buffer.resize(kWidth, kHeight);
	buffer.clear();

	Rasterizer rasterizer;
	VertexProcessor processor(&rasterizer);

	rasterizer.setRasterMode(mode);
	rasterizer.setScissorRect(0, 0, kWidth, kHeight);
	rasterizer.setPixelShader<PreviewPixelShader>();

	processor.setViewport(0, 0, kWidth, kHeight);
	processor.setCullMode(CullMode::None);
	processor.setVertexShader<PreviewVertexShader>();
	processor.setVertexAttribPointer(0, sizeof(Vertex), triangles.data()->v);

	PreviewPixelShader::buffer = &buffer;
	PreviewPixelShader::discarded = 0;

	int indices[3];
	const int count = (int)triangles.size();
	for (int i = 0; i < count; ++i) {
		const int index = reverse ? (count - 1 - i) : i;
		// Each triangle is a contiguous run of three vertices.
		indices[0] = index * 3;
		indices[1] = index * 3 + 1;
		indices[2] = index * 3 + 2;
		processor.drawElements(DrawMode::Triangle, 3, indices);
	}

	PreviewPixelShader::buffer = nullptr;
}

const float kBackground[3] = {0.025f, 0.035f, 0.07f};

DepthOfFieldCamera makeCamera(float aperture)
{
	DepthOfFieldCamera camera;
	camera.focalLength = 0.05f;
	camera.apertureRadius = aperture;
	camera.focusDistance = 1.2f;
	camera.sensorHeight = 0.024f;
	return camera;
}

void writePPM(const char *path, int width, int height, const std::vector<float> &rgb)
{
	FILE *file = std::fopen(path, "wb");
	if (!file) {
		std::fprintf(stderr, "could not open %s\n", path);
		return;
	}
	std::fprintf(file, "P6\n%d %d\n255\n", width, height);
	for (size_t i = 0; i < (size_t)width * height; ++i) {
		unsigned char pixel[3];
		for (int c = 0; c < 3; ++c) {
			float v = rgb[i * 3 + c];
			v = std::max(0.0f, std::min(1.0f, v));
			pixel[c] = (unsigned char)(v * 255.0f + 0.5f);
		}
		std::fwrite(pixel, 1, 3, file);
	}
	std::fclose(file);
	std::printf("PPM %s\n", path);
}

std::vector<float> extractColors(const DepthOfFieldBuffer &buffer)
{
	std::vector<float> rgb((size_t)buffer.width() * buffer.height() * 3);
	for (int y = 0; y < buffer.height(); ++y) {
		for (int x = 0; x < buffer.width(); ++x) {
			const size_t i = ((size_t)y * buffer.width() + x) * 3;
			const DepthOfFieldSample &s = buffer.sample(x, y);
			rgb[i + 0] = s.valid ? s.r : kBackground[0];
			rgb[i + 1] = s.valid ? s.g : kBackground[1];
			rgb[i + 2] = s.valid ? s.b : kBackground[2];
		}
	}
	return rgb;
}

std::vector<float> makeRadiusImage(const DepthOfFieldResult &result, float maxRadius)
{
	std::vector<float> rgb((size_t)result.width * result.height * 3, 0.0f);
	for (size_t i = 0; i < result.radius.size(); ++i) {
		const float radius = result.radius[i];
		const float amount = std::min(std::fabs(radius) / maxRadius, 1.0f);
		if (radius > 0.0f)
			rgb[i * 3 + 0] = amount; // far layer: red
		else if (radius < 0.0f)
			rgb[i * 3 + 2] = amount; // near layer: blue
	}
	return rgb;
}

void projectToScreen(const Vertex &v, float &sx, float &sy)
{
	const float t = std::tan(kFovY * 0.5f);
	const float clipX = v.x / (t * kAspect);
	const float clipY = v.y / t;
	const float w = -v.z;
	const float ndcX = clipX / w;
	const float ndcY = clipY / w;
	sx = kWidth * 0.5f * ndcX + kWidth * 0.5f;
	sy = kHeight * 0.5f * -ndcY + kHeight * 0.5f;
}

float segmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
	const float vx = bx - ax;
	const float vy = by - ay;
	const float lengthSquared = vx * vx + vy * vy;
	float t = 0.0f;
	if (lengthSquared > 0.0f)
		t = ((px - ax) * vx + (py - ay) * vy) / lengthSquared;
	t = std::max(0.0f, std::min(1.0f, t));
	const float dx = px - (ax + t * vx);
	const float dy = py - (ay + t * vy);
	return std::sqrt(dx * dx + dy * dy);
}

std::vector<char> makeBoundaryMask(const std::vector<Triangle> &triangles, float thickness)
{
	std::vector<char> mask((size_t)kWidth * kHeight, 0);
	for (const Triangle &triangle : triangles) {
		float sx[3], sy[3];
		for (int i = 0; i < 3; ++i)
			projectToScreen(triangle.v[i], sx[i], sy[i]);

		for (int edge = 0; edge < 3; ++edge) {
			const int next = (edge + 1) % 3;
			const float ax = sx[edge], ay = sy[edge];
			const float bx = sx[next], by = sy[next];
			const int minX = std::max(0, (int)std::floor(std::min(ax, bx) - thickness - 1.0f));
			const int maxX = std::min(kWidth - 1, (int)std::ceil(std::max(ax, bx) + thickness + 1.0f));
			const int minY = std::max(0, (int)std::floor(std::min(ay, by) - thickness - 1.0f));
			const int maxY = std::min(kHeight - 1, (int)std::ceil(std::max(ay, by) + thickness + 1.0f));
			for (int y = minY; y <= maxY; ++y) {
				for (int x = minX; x <= maxX; ++x) {
					if (segmentDistance(x + 0.5f, y + 0.5f, ax, ay, bx, by) <= thickness)
						mask[(size_t)y * kWidth + x] = 1;
				}
			}
		}
	}
	return mask;
}

float compareInterior(const std::vector<float> &a, const std::vector<float> &b,
                      const std::vector<char> &boundary)
{
	// Compare shape interiors only. Raster mode coverage at the screen border
	// and along triangle edges is allowed to differ by a pixel.
	const int inset = 4;
	float maxError = 0.0f;
	for (int y = inset; y < kHeight - inset; ++y) {
		for (int x = inset; x < kWidth - inset; ++x) {
			if (boundary[(size_t)y * kWidth + x])
				continue;
			const size_t i = ((size_t)y * kWidth + x) * 3;
			for (int c = 0; c < 3; ++c)
				maxError = std::max(maxError, std::fabs(a[i + c] - b[i + c]));
		}
	}
	return maxError;
}

float maxColorError(const std::vector<float> &a, const std::vector<float> &b)
{
	float maxError = 0.0f;
	for (size_t i = 0; i < a.size(); ++i)
		maxError = std::max(maxError, std::fabs(a[i] - b[i]));
	return maxError;
}

// Returns true when the sample at (x, y) has the given view depth and is
// inside the screen-space projection of one of the scene quads.
bool insideProjectedTriangle(const Triangle &triangle, int x, int y, float margin)
{
	float sx[3], sy[3];
	for (int i = 0; i < 3; ++i)
		projectToScreen(triangle.v[i], sx[i], sy[i]);

	const float px = x + 0.5f;
	const float py = y + 0.5f;
	const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
	if (std::fabs(area) < 1e-6f)
		return false;

	const float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
	const float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
	const float w2 = 1.0f - w0 - w1;
	return w0 > margin && w1 > margin && w2 > margin;
}

} // namespace

int main()
{
	const std::vector<Triangle> scene = buildScene();
	const float maxRadius = 14.0f;

	DepthOfFieldBuffer spanBuffer;
	renderScene(scene, RasterMode::Span, false, spanBuffer);
	const long nearDiscarded = PreviewPixelShader::discarded;

	DepthOfFieldBuffer blockBuffer;
	renderScene(scene, RasterMode::Block, false, blockBuffer);

	DepthOfFieldBuffer reversedBuffer;
	renderScene(scene, RasterMode::Span, true, reversedBuffer);

	const std::vector<float> original = extractColors(spanBuffer);
	const std::vector<char> boundary = makeBoundaryMask(scene, 2.0f);

	DepthOfFieldResolveParams params;
	params.camera = makeCamera(0.05f);
	params.imageHeight = (float)kHeight;
	params.maxRadius = maxRadius;
	params.background[0] = kBackground[0];
	params.background[1] = kBackground[1];
	params.background[2] = kBackground[2];

	DepthOfFieldResult result;
	resolveDepthOfField(spanBuffer, params, result);
	const std::vector<float> blurred = result.rgb;

	DepthOfFieldResolveParams sharpParams = params;
	sharpParams.camera.apertureRadius = 0.0f;
	DepthOfFieldResult sharpResult;
	resolveDepthOfField(spanBuffer, sharpParams, sharpResult);

	// Every hole column of the near occluder was rejected by the pixel shader,
	// so the occluder must never own those samples. Non-hole fragments must
	// own their pixels at the occluder's view depth.
	int holesChecked = 0;
	int holesCorrect = 0;
	int occluderPixels = 0;
	int occluderCorrect = 0;
	for (int y = 0; y < kHeight; ++y) {
		for (int x = 0; x < kWidth; ++x) {
			const DepthOfFieldSample &s = spanBuffer.sample(x, y);
			for (size_t t = 0; t < scene.size(); ++t) {
				const int material = scene[t].v[0].material;
				if (material != 2 || !insideProjectedTriangle(scene[t], x, y, 0.02f))
					continue;
				const bool hole = (x % 11) < 4;
				const bool ownsOccluder = s.valid && std::fabs(s.viewDepth - 0.62f) < 0.01f;
				if (hole) {
					++holesChecked;
					if (!ownsOccluder)
						++holesCorrect;
				} else {
					++occluderPixels;
					if (ownsOccluder)
						++occluderCorrect;
				}
				break;
			}
		}
	}

	// Radius signs must follow the view depth layers: the near occluder is
	// negative, the far blocks positive, and the focused grid is close to zero.
	int nearRadiusPixels = 0;
	int farRadiusPixels = 0;
	int focusRadiusPixels = 0;
	int focusRadiusCorrect = 0;
	for (int y = 0; y < kHeight; ++y) {
		for (int x = 0; x < kWidth; ++x) {
			const DepthOfFieldSample &s = spanBuffer.sample(x, y);
			if (!s.valid)
				continue;
			const float r = result.radius[(size_t)y * kWidth + x];
			if (std::fabs(s.viewDepth - 0.62f) < 0.01f) {
				++nearRadiusPixels;
				if (!(r < 0.0f))
					std::printf("PREVIEW unexpected near radius=%g at %d,%d\n", r, x, y);
			} else if (std::fabs(s.viewDepth - 2.6f) < 0.01f) {
				++farRadiusPixels;
				if (!(r >= 0.0f))
					std::printf("PREVIEW unexpected far radius=%g at %d,%d\n", r, x, y);
			} else if (std::fabs(s.viewDepth - 1.2f) < 0.01f) {
				++focusRadiusPixels;
				// Focused samples land a hair off 1.2 from float rounding, so
				// their radius is tiny rather than exactly zero.
				if (std::fabs(r) < 0.05f) {
					++focusRadiusCorrect;
				}
			}
		}
	}

	// With a zero aperture every visible pixel must retain its exact linear
	// color instead of being averaged with anything.
	int validPixels = 0;
	float zeroApertureError = 0.0f;
	for (int y = 0; y < kHeight; ++y) {
		for (int x = 0; x < kWidth; ++x) {
			const DepthOfFieldSample &s = spanBuffer.sample(x, y);
			if (!s.valid)
				continue;
			++validPixels;
			const size_t i = ((size_t)y * kWidth + x) * 3;
			zeroApertureError = std::max(zeroApertureError, std::fabs(sharpResult.rgb[i + 0] - s.r));
			zeroApertureError = std::max(zeroApertureError, std::fabs(sharpResult.rgb[i + 1] - s.g));
			zeroApertureError = std::max(zeroApertureError, std::fabs(sharpResult.rgb[i + 2] - s.b));
		}
	}

	const float spanBlockError = compareInterior(original, extractColors(blockBuffer), boundary);
	const float orderError = compareInterior(original, extractColors(reversedBuffer), boundary);
	const float blurDifference = maxColorError(original, blurred);
	const float maxNumericError = std::max(std::max(spanBlockError, orderError), zeroApertureError);

	std::printf("PREVIEW span_vs_block interior_max_error=%.9g\n", spanBlockError);
	std::printf("PREVIEW normal_vs_reversed interior_max_error=%.9g\n", orderError);
	std::printf("PREVIEW zero_aperture_color_max_error=%.9g\n", zeroApertureError);
	std::printf("PREVIEW blur_vs_original_max_difference=%.9g\n", blurDifference);
	std::printf("PREVIEW max_numeric_error=%.9g\n", maxNumericError);
	std::printf("PREVIEW discarded_near_fragments=%ld\n", nearDiscarded);
	std::printf("PREVIEW discarded_hole_pixels=%d/%d correct\n", holesCorrect, holesChecked);
	std::printf("PREVIEW occluder_owner_pixels=%d/%d correct\n", occluderCorrect, occluderPixels);
	std::printf("PREVIEW radius_sign near=%d far=%d focus_zero=%d/%d valid_pixels=%d\n",
	            nearRadiusPixels, farRadiusPixels, focusRadiusCorrect, focusRadiusPixels, validPixels);

	writePPM("dof_original.ppm", kWidth, kHeight, original);
	writePPM("dof_blurred.ppm", kWidth, kHeight, blurred);
	writePPM("dof_radius.ppm", kWidth, kHeight, makeRadiusImage(result, maxRadius));

	const bool ok = spanBlockError < 1e-5f &&
	                orderError < 1e-5f &&
	                zeroApertureError < 1e-5f &&
	                blurDifference > 0.05f &&
	                nearDiscarded > 0 &&
	                holesChecked > 0 && holesCorrect == holesChecked &&
	                occluderPixels > 0 && occluderCorrect == occluderPixels &&
	                nearRadiusPixels > 0 && farRadiusPixels > 0 &&
	                focusRadiusPixels > 0 && focusRadiusCorrect == focusRadiusPixels;
	std::printf("PREVIEW %s\n", ok ? "OK" : "FAILED");
	return ok ? 0 : 1;
}

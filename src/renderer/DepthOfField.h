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

#pragma once

/** @file */

#include <cstddef>
#include <vector>

namespace swr {

/// A single resolved surface sample stored in a DepthOfFieldBuffer.
/** Colors are linear RGB. viewDepth is the positive right handed view space
  depth (-z). windowDepth is the depth used for the visibility test. */
struct DepthOfFieldSample {
	float r;
	float g;
	float b;
	float viewDepth;
	float windowDepth;
	bool valid;
};

/// Independent CPU color/depth buffer used as depth-of-field input.
/** A pixel shader can write to this buffer exactly like to any other render
  target. Only the front-most surface is retained per pixel. */
class DepthOfFieldBuffer {
public:
	DepthOfFieldBuffer() = default;
	DepthOfFieldBuffer(int width, int height);

	void resize(int width, int height);
	void clear();

	int width() const { return m_width; }
	int height() const { return m_height; }
	bool empty() const { return m_width <= 0 || m_height <= 0; }
	bool inBounds(int x, int y) const
	{
		return x >= 0 && y >= 0 && x < m_width && y < m_height;
	}

	/// Submit a fragment.
	/** A smaller window depth wins. Equal window depths keep the later
	  submission. Returns true when the fragment became the visible surface. */
	bool submit(int x, int y, float r, float g, float b, float viewDepth, float windowDepth);

	const DepthOfFieldSample &sample(int x, int y) const { return m_samples[(size_t)y * m_width + x]; }
	DepthOfFieldSample &sample(int x, int y) { return m_samples[(size_t)y * m_width + x]; }

	const DepthOfFieldSample *data() const { return m_samples.data(); }
	DepthOfFieldSample *data() { return m_samples.data(); }

private:
	int m_width = 0;
	int m_height = 0;
	std::vector<DepthOfFieldSample> m_samples;
};

/// Thin lens camera parameters. All lengths use the same unit.
struct DepthOfFieldCamera {
	float focalLength = 1.0f;    ///< f, must be > 0.
	float apertureRadius = 0.0f; ///< a, must be >= 0.
	float focusDistance = 2.0f;  ///< s, must be > f.
	float sensorHeight = 1.0f;   ///< h, must be > 0.

	bool valid() const;
};

/// Signed blur radius in pixels for a positive view depth.
/** Near layers are negative, far layers are non-negative. The magnitude is
  clamped to maxRadius. */
float signedBlurRadius(float viewDepth, const DepthOfFieldCamera &camera, float imageHeight, float maxRadius);

struct DepthOfFieldResolveParams {
	DepthOfFieldCamera camera;
	float imageHeight = 1.0f;
	float maxRadius = 0.0f;
	float background[3] = {0.0f, 0.0f, 0.0f};
};

/// Resolved linear RGB image and the signed radius of every input pixel.
struct DepthOfFieldResult {
	int width = 0;
	int height = 0;
	std::vector<float> rgb;    ///< Three floats per pixel.
	std::vector<float> radius; ///< One float per pixel. Zero for invalid pixels.
};

/// Resolve a depth-of-field buffer into independent storage.
/** The input is never modified. Every source pixel is scattered into a
  uniform weight disc. Near and far layers are reconstructed independently and
  composited with the near coverage over the far layer. */
void resolveDepthOfField(const DepthOfFieldBuffer &input, const DepthOfFieldResolveParams &params, DepthOfFieldResult &result);

} // end namespace swr

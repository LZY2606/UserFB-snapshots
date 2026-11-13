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

#include "DepthOfField.h"

#include <algorithm>
#include <cmath>

namespace swr {

DepthOfFieldBuffer::DepthOfFieldBuffer(int width, int height)
{
	resize(width, height);
}

void DepthOfFieldBuffer::resize(int width, int height)
{
	m_width = std::max(width, 0);
	m_height = std::max(height, 0);
	m_samples.assign((size_t)m_width * (size_t)m_height, DepthOfFieldSample{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
}

void DepthOfFieldBuffer::clear()
{
	m_samples.assign((size_t)m_width * (size_t)m_height, DepthOfFieldSample{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false});
}

bool DepthOfFieldBuffer::submit(int x, int y, float r, float g, float b, float viewDepth, float windowDepth)
{
	if (!inBounds(x, y))
		return false;

	DepthOfFieldSample &dst = m_samples[(size_t)y * m_width + x];

	// A smaller window depth wins. Equal depths keep the later fragment.
	if (dst.valid && windowDepth > dst.windowDepth)
		return false;

	dst.r = r;
	dst.g = g;
	dst.b = b;
	dst.viewDepth = viewDepth;
	dst.windowDepth = windowDepth;
	dst.valid = true;
	return true;
}

bool DepthOfFieldCamera::valid() const
{
	return std::isfinite(focalLength) && std::isfinite(apertureRadius) &&
	       std::isfinite(focusDistance) && std::isfinite(sensorHeight) &&
	       focalLength > 0.0f && apertureRadius >= 0.0f &&
	       focusDistance > focalLength && sensorHeight > 0.0f;
}

float signedBlurRadius(float viewDepth, const DepthOfFieldCamera &camera, float imageHeight, float maxRadius)
{
	if (!(viewDepth > 0.0f) || !camera.valid() || !(imageHeight > 0.0f))
		return 0.0f;

	const float radius = (imageHeight / camera.sensorHeight) * camera.apertureRadius *
	                     (camera.focalLength / (camera.focusDistance - camera.focalLength)) *
	                     ((viewDepth - camera.focusDistance) / viewDepth);

	const float limit = std::fabs(maxRadius);
	if (!std::isfinite(radius))
		return radius < 0.0f ? -limit : limit;

	return std::max(-limit, std::min(limit, radius));
}

namespace {

// A uniform weight disc of integer pixel offsets around the center.
struct Disc {
	int count;  // Number of cells in the complete disc.
	int extent; // Half size of the integer bounding box.
};

Disc makeDisc(float radius)
{
	Disc disc;
	if (!(radius >= 1.0f)) {
		// A radius below one pixel only keeps the center cell.
		disc.count = 1;
		disc.extent = 0;
		return disc;
	}

	const int extent = (int)std::floor(radius);
	const float radiusSquared = radius * radius;
	int count = 0;
	for (int dy = -extent; dy <= extent; ++dy) {
		for (int dx = -extent; dx <= extent; ++dx) {
			if ((float)(dx * dx + dy * dy) <= radiusSquared)
				++count;
		}
	}

	disc.count = count > 0 ? count : 1;
	disc.extent = extent;
	return disc;
}

} // namespace

void resolveDepthOfField(const DepthOfFieldBuffer &input, const DepthOfFieldResolveParams &params, DepthOfFieldResult &result)
{
	const int width = input.width();
	const int height = input.height();

	result.width = width;
	result.height = height;
	result.rgb.assign((size_t)std::max(width, 0) * (size_t)std::max(height, 0) * 3, 0.0f);
	result.radius.assign((size_t)std::max(width, 0) * (size_t)std::max(height, 0), 0.0f);

	if (width <= 0 || height <= 0)
		return;

	const size_t pixelCount = (size_t)width * (size_t)height;

	std::vector<float> nearColor(pixelCount * 3, 0.0f);
	std::vector<float> farColor(pixelCount * 3, 0.0f);
	std::vector<float> nearWeight(pixelCount, 0.0f);
	std::vector<float> farWeight(pixelCount, 0.0f);

	// Scatter every visible source pixel into its own layer. All reads use the
	// untouched input frame; the accumulation buffers are separate storage.
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const DepthOfFieldSample &src = input.sample(x, y);
			if (!src.valid)
				continue;

			const float radius = signedBlurRadius(src.viewDepth, params.camera, params.imageHeight, params.maxRadius);
			result.radius[(size_t)y * width + x] = radius;

			const float magnitude = std::fabs(radius);
			const Disc disc = makeDisc(magnitude);
			const float weight = 1.0f / (float)disc.count;
			const int extent = disc.extent;
			const float magnitudeSquared = magnitude * magnitude;

			const bool nearLayer = radius < 0.0f;
			std::vector<float> &colorSum = nearLayer ? nearColor : farColor;
			std::vector<float> &weightSum = nearLayer ? nearWeight : farWeight;

			for (int dy = -extent; dy <= extent; ++dy) {
				const int ty = y + dy;
				for (int dx = -extent; dx <= extent; ++dx) {
					if ((float)(dx * dx + dy * dy) > magnitudeSquared)
						continue;

					const int tx = x + dx;
					if (tx < 0 || tx >= width || ty < 0 || ty >= height)
						continue; // The full disc still counts in the normalisation.

					const size_t index = (size_t)ty * width + tx;
					colorSum[index * 3 + 0] += src.r * weight;
					colorSum[index * 3 + 1] += src.g * weight;
					colorSum[index * 3 + 2] += src.b * weight;
					weightSum[index] += weight;
				}
			}
		}
	}

	// Composite the near layer over the far layer. The two layers are never
	// normalised together, so a bright far layer cannot wash out the near
	// silhouette.
	for (size_t index = 0; index < pixelCount; ++index) {
		float far[3] = {params.background[0], params.background[1], params.background[2]};
		if (farWeight[index] > 0.0f) {
			const float inv = 1.0f / farWeight[index];
			far[0] = farColor[index * 3 + 0] * inv;
			far[1] = farColor[index * 3 + 1] * inv;
			far[2] = farColor[index * 3 + 2] * inv;
		}

		float near[3] = {0.0f, 0.0f, 0.0f};
		float coverage = 0.0f;
		if (nearWeight[index] > 0.0f) {
			coverage = std::min(nearWeight[index], 1.0f);
			const float inv = 1.0f / nearWeight[index];
			near[0] = nearColor[index * 3 + 0] * inv * coverage;
			near[1] = nearColor[index * 3 + 1] * inv * coverage;
			near[2] = nearColor[index * 3 + 2] * inv * coverage;
		}

		const float farCoverage = 1.0f - coverage;
		result.rgb[index * 3 + 0] = near[0] + far[0] * farCoverage;
		result.rgb[index * 3 + 1] = near[1] + far[1] * farCoverage;
		result.rgb[index * 3 + 2] = near[2] + far[2] * farCoverage;
	}
}

} // end namespace swr

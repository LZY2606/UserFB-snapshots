// Ray-traced render of the continuous Perlin height field
// y = noise2D(x, z) without baking a high-resolution mesh.
//
// Usage: raytrace <output-directory>
// Writes depth.pgm and status.pgm (64 x 64). Hit pixels carry the real
// intersection depth; Miss and Uncertain are encoded separately and
// Uncertain is never folded into the background.

#include "PerlinNoise.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
	using siv::PerlinNoise;
	using siv::RayHit2D;
	using Status = RayHit2D::Status;

	constexpr int kSize = 64;
	constexpr double kExtent = 8.0;   // sampled patch is [-8, 8] on x and z
	constexpr double kTop = 0.0;      // eye plane cuts through the [-1, 1] field
	constexpr double kMaxT = 8.0;
	constexpr double kTolerance = 1e-7;
	constexpr unsigned kBudget = 1'000'000u;

	// PGM gray codes for the status image.
	constexpr int gHit = 255;
	constexpr int gMiss = 0;
	constexpr int gUncertain = 128;
	constexpr int gInvalid = 64;

	int statusCode(Status s)
	{
		switch (s)
		{
		case Status::Hit: return gHit;
		case Status::Miss: return gMiss;
		case Status::Uncertain: return gUncertain;
		default: return gInvalid;
		}
	}

	const char* statusName(Status s)
	{
		switch (s)
		{
		case Status::Hit: return "Hit";
		case Status::Miss: return "Miss";
		case Status::Uncertain: return "Uncertain";
		default: return "Invalid";
		}
	}

	bool writePgm(const std::string& path, const std::vector<int>& gray)
	{
		if (std::FILE* f = std::fopen(path.c_str(), "wb"))
		{
			std::fprintf(f, "P2\n%d %d\n255\n", kSize, kSize);
			for (int row = 0; row < kSize; ++row)
			{
				for (int col = 0; col < kSize; ++col)
				{
					std::fprintf(f, "%d ", gray[row * kSize + col]);
				}
				std::fputc('\n', f);
			}
			std::fclose(f);
			return true;
		}
		return false;
	}
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
		return 2;
	}

	const std::filesystem::path outDir = argv[1];
	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);
	if (ec)
	{
		std::fprintf(stderr, "cannot create output directory: %s\n", ec.message().c_str());
		return 1;
	}

	const PerlinNoise noise(12345u);

	std::vector<RayHit2D> hits(kSize * kSize);
	std::vector<int> statusGray(kSize * kSize);
	std::vector<double> hitDepths;
	hitDepths.reserve(kSize * kSize);

	unsigned counts[4] = { 0, 0, 0, 0 };

	// Primary rays point straight down; each column samples one (x, z) cell
	// column of the height field.
	for (int row = 0; row < kSize; ++row)
	{
		for (int col = 0; col < kSize; ++col)
		{
			const double x = -kExtent + 2.0 * kExtent * col / (kSize - 1);
			const double z = -kExtent + 2.0 * kExtent * row / (kSize - 1);
			const RayHit2D r = noise.intersectHeight(
				x, kTop, z, 0.0, -1.0, 0.0, 0.0, kMaxT, kTolerance, kBudget);

			const int idx = row * kSize + col;
			hits[idx] = r;
			statusGray[idx] = statusCode(r.status);
			++counts[static_cast<int>(r.status)];
			if (r.status == Status::Hit)
			{
				hitDepths.push_back(0.5 * (r.t_lo + r.t_hi));
			}
		}
	}

	// Map Hit depths into [1, 254]; reserve 0 (Miss) and 255 (Uncertain).
	std::vector<int> depthGray(kSize * kSize, gMiss);
	if (!hitDepths.empty())
	{
		const double dMin = *std::min_element(hitDepths.begin(), hitDepths.end());
		const double dMax = *std::max_element(hitDepths.begin(), hitDepths.end());
		const double span = (dMax > dMin) ? (dMax - dMin) : 1.0;
		for (int i = 0; i < kSize * kSize; ++i)
		{
			const RayHit2D& r = hits[i];
			if (r.status == Status::Hit)
			{
				const double d = 0.5 * (r.t_lo + r.t_hi);
				depthGray[i] = 1 + static_cast<int>(253.0 * (d - dMin) / span);
			}
			else if (r.status == Status::Uncertain)
			{
				depthGray[i] = 255; // kept distinct from every Hit depth
			}
			else
			{
				depthGray[i] = gMiss; // background
			}
		}
	}

	const std::string depthPath = (outDir / "depth.pgm").string();
	const std::string statusPath = (outDir / "status.pgm").string();
	if (!writePgm(depthPath, depthGray) || !writePgm(statusPath, statusGray))
	{
		std::fprintf(stderr, "failed to write output images\n");
		return 1;
	}

	for (int s = 0; s < 4; ++s)
	{
		std::printf("%-9s %u\n", statusName(static_cast<Status>(s)), counts[s]);
	}
	std::printf("wrote %s and %s (%dx%d)\n",
		depthPath.c_str(), statusPath.c_str(), kSize, kSize);
	return 0;
}

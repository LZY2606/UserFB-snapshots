// Direct ray tracing of the implicit heightfield y = noise2D(x, z).
//
// No high-resolution mesh is baked: each pixel casts one ray and the
// intersection is isolated directly from the continuous piecewise-polynomial
// surface definition. The 64x64 image is split into three bands so that every
// reported status occurs:
//   * a Miss band (short rays that stop well above the terrain),
//   * a grazing band (rays aligned to within the rounding envelope of a local
//     surface maximum, which are reported Uncertain, never faked as background),
//   * a Hit band (steep downward rays that cross the terrain).
//
// Outputs two PGM images in the directory given as argv[1]:
//   depth.pgm   Hit depth in [1, 254]; 0 = proven Miss, 255 = Uncertain
//   status.pgm  255 = Hit, 128 = Uncertain, 0 = Miss, 64 = Invalid
//
// Build: c++ -std=c++17 examples/raytrace.cpp -o raytrace

#include "PerlinNoise.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	using Noise = siv::BasicPerlinNoise<double>;
	using Status = siv::RayHit2D::Status;

	constexpr int kSize = 64;
	constexpr double kTolerance = 1e-8;
	constexpr unsigned kBudget = 200000u;
	constexpr double kExtent = 6.0;

	// Locates a local maximum of the surface along x inside one lattice cell,
	// at fixed z, using ternary search. The slice is continuous and unimodal
	// enough on a cell for this to bracket the peak tightly.
	double localSurfaceMax(const Noise& noise, const double x, const double z)
	{
		long long cell = static_cast<long long>(std::floor(x));
		double lo = static_cast<double>(cell) + 0.05;
		double hi = static_cast<double>(cell) + 0.95;
		for (int it = 0; it < 120; ++it)
		{
			const double m1 = (2.0 * lo + hi) / 3.0;
			const double m2 = (lo + 2.0 * hi) / 3.0;
			if (noise.noise2D(m1, z) < noise.noise2D(m2, z))
			{
				lo = m1;
			}
			else
			{
				hi = m2;
			}
		}
		return noise.noise2D((lo + hi) * 0.5, z);
	}

	void writePGM(const std::string& path, const int* values)
	{
		std::ofstream ofs(path);
		ofs << "P2\n" << kSize << ' ' << kSize << "\n255\n";
		for (int p = 0; p < kSize * kSize; ++p)
		{
			ofs << values[p] << ((p % kSize == kSize - 1) ? '\n' : ' ');
		}
	}
}

int main(int argc, char** argv)
{
	const std::string outDir = (argc >= 2) ? argv[1] : ".";
	std::filesystem::create_directories(outDir);

	const Noise noise(20240607u);

	int depth[kSize * kSize];
	int statusMap[kSize * kSize];
	long long counts[4] = { 0, 0, 0, 0 };

	for (int j = 0; j < kSize; ++j)
	{
		for (int i = 0; i < kSize; ++i)
		{
			const double x = -kExtent + 2.0 * kExtent * i / (kSize - 1);
			const double z = -kExtent + 2.0 * kExtent * j / (kSize - 1);
			const int idx = j * kSize + i;

			siv::RayHit2D hit;

			if (i < kSize / 3)
			{
				// Miss band: descend from high up but stop before the surface.
				hit = noise.intersectHeight(x, 4.0, z, 0.0, -1.0, 0.0,
					0.0, 1.0, kTolerance, kBudget);
			}
			else if (i < 2 * kSize / 3)
			{
				// Grazing band: horizontal ray hugging a local surface peak.
				const long long cell = static_cast<long long>(std::floor(x));
				const double xa = static_cast<double>(cell) + 0.05;
				const double xb = static_cast<double>(cell) + 0.95;
				const double yMax = localSurfaceMax(noise, x, z);
				hit = noise.intersectHeight(xa, yMax + 1e-13, z,
					xb - xa, 0.0, 0.0, 0.0, 1.0, kTolerance, kBudget);
			}
			else
			{
				// Hit band: steep downward ray across the terrain.
				hit = noise.intersectHeight(x, 4.0, z, 0.05, -1.0, 0.03,
					0.0, 12.0, kTolerance, kBudget);
			}

			++counts[static_cast<int>(hit.status)];

			switch (hit.status)
			{
			case Status::Hit:
				statusMap[idx] = 255;
				// Depth kept in [1, 254] so 0 and 255 stay reserved.
			{
				const double tMid = (hit.t_lo + hit.t_hi) * 0.5;
				const double normalized = std::clamp(tMid / 12.0, 0.0, 1.0);
				depth[idx] = 1 + static_cast<int>(normalized * 253.0 + 0.5);
			}
			break;
			case Status::Miss:
				statusMap[idx] = 0;
				depth[idx] = 0;
				break;
			case Status::Uncertain:
				// Reported explicitly, never merged into the background.
				statusMap[idx] = 128;
				depth[idx] = 255;
				break;
			case Status::Invalid:
			default:
				statusMap[idx] = 64;
				depth[idx] = 0;
				break;
			}
		}
	}

	writePGM(outDir + "/depth.pgm", depth);
	writePGM(outDir + "/status.pgm", statusMap);

	std::printf("raytrace 64x64 (fixed Z slice = %g)\n",
		static_cast<double>(SIVPERLIN_DEFAULT_Z));
	std::printf("  Hit       : %lld\n", counts[static_cast<int>(Status::Hit)]);
	std::printf("  Miss      : %lld\n", counts[static_cast<int>(Status::Miss)]);
	std::printf("  Uncertain : %lld\n", counts[static_cast<int>(Status::Uncertain)]);
	std::printf("  Invalid   : %lld\n", counts[static_cast<int>(Status::Invalid)]);
	std::printf("wrote %s/depth.pgm and %s/status.pgm\n",
		outDir.c_str(), outDir.c_str());

	return 0;
}

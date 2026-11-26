//----------------------------------------------------------------------------------------
//
//	siv::PerlinNoise
//	Perlin noise library for modern C++
//
//	Copyright (C) 2013-2021 Ryo Suzuki <reputeless@gmail.com>
//
//	Permission is hereby granted, free of charge, to any person obtaining a copy
//	of this software and associated documentation files(the "Software"), to deal
//	in the Software without restriction, including without limitation the rights
//	to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
//	copies of the Software, and to permit persons to whom the Software is
//	furnished to do so, subject to the following conditions :
//	
//	The above copyright notice and this permission notice shall be included in
//	all copies or substantial portions of the Software.
//	
//	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
//	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//	THE SOFTWARE.
//
//----------------------------------------------------------------------------------------

# pragma once
# include <cstdint>
# include <algorithm>
# include <array>
# include <cmath>
# include <cstdlib>
# include <iterator>
# include <numeric>
# include <random>
# include <type_traits>
# include <vector>

# if __has_include(<concepts>) && defined(__cpp_concepts)
#	include <concepts>
# endif


// Library major version
# define SIVPERLIN_VERSION_MAJOR			3

// Library minor version
# define SIVPERLIN_VERSION_MINOR			0

// Library revision version
# define SIVPERLIN_VERSION_REVISION			0

// Library version
# define SIVPERLIN_VERSION			((SIVPERLIN_VERSION_MAJOR * 100 * 100) + (SIVPERLIN_VERSION_MINOR * 100) + (SIVPERLIN_VERSION_REVISION))


// [[nodiscard]] for constructors
# if (201907L <= __has_cpp_attribute(nodiscard))
#	define SIVPERLIN_NODISCARD_CXX20 [[nodiscard]]
# else
#	define SIVPERLIN_NODISCARD_CXX20
# endif


// std::uniform_random_bit_generator concept
# if __cpp_lib_concepts
#	define SIVPERLIN_CONCEPT_URBG  template <std::uniform_random_bit_generator URBG>
#	define SIVPERLIN_CONCEPT_URBG_ template <std::uniform_random_bit_generator URBG>
# else
#	define SIVPERLIN_CONCEPT_URBG  template <class URBG, std::enable_if_t<std::conjunction_v<std::is_invocable<URBG&>, std::is_unsigned<std::invoke_result_t<URBG&>>>>* = nullptr>
#	define SIVPERLIN_CONCEPT_URBG_ template <class URBG, std::enable_if_t<std::conjunction_v<std::is_invocable<URBG&>, std::is_unsigned<std::invoke_result_t<URBG&>>>>*>
# endif


// arbitrary value for increasing entropy
# ifndef SIVPERLIN_DEFAULT_Y
#	define SIVPERLIN_DEFAULT_Y (0.12345)
# endif

// arbitrary value for increasing entropy
# ifndef SIVPERLIN_DEFAULT_Z
#	define SIVPERLIN_DEFAULT_Z (0.34567)
# endif


namespace siv
{
	// Result of a ray-vs-heightfield intersection query (see
	// BasicPerlinNoise::intersectHeight). t_lo / t_hi are given in the
	// parameter scale of the input ray P(t) = O + tD.
	struct RayHit2D
	{
		enum Status
		{
			Hit,
			Miss,
			Uncertain,
			Invalid,
		};

		Status status = Invalid;

		double t_lo = 0.0;

		double t_hi = 0.0;
	};

	template <class Float>
	class BasicPerlinNoise
	{
	public:

		static_assert(std::is_floating_point_v<Float>);

		///////////////////////////////////////
		//
		//	Typedefs
		//

		using state_type = std::array<std::uint8_t, 256>;

		using value_type = Float;

		using default_random_engine = std::mt19937;

		using seed_type = typename default_random_engine::result_type;

		///////////////////////////////////////
		//
		//	Constructors
		//

		SIVPERLIN_NODISCARD_CXX20
		constexpr BasicPerlinNoise() noexcept;

		SIVPERLIN_NODISCARD_CXX20
		explicit BasicPerlinNoise(seed_type seed);

		SIVPERLIN_CONCEPT_URBG
		SIVPERLIN_NODISCARD_CXX20
		explicit BasicPerlinNoise(URBG&& urbg);

		///////////////////////////////////////
		//
		//	Reseed
		//

		void reseed(seed_type seed);

		SIVPERLIN_CONCEPT_URBG
		void reseed(URBG&& urbg);

		///////////////////////////////////////
		//
		//	Serialization
		//

		[[nodiscard]]
		constexpr const state_type& serialize() const noexcept;

		constexpr void deserialize(const state_type& state) noexcept;

		///////////////////////////////////////
		//
		//	Noise (The result is in the range [-1, 1])
		//

		[[nodiscard]]
		value_type noise1D(value_type x) const noexcept;

		[[nodiscard]]
		value_type noise2D(value_type x, value_type y) const noexcept;

		[[nodiscard]]
		value_type noise3D(value_type x, value_type y, value_type z) const noexcept;

		///////////////////////////////////////
		//
		//	Ray intersection against the heightfield y = noise2D(x, z),
		//	using the fixed z slice SIVPERLIN_DEFAULT_Z that noise2D uses.
		//	Only available for BasicPerlinNoise<double>.
		//
		//	Finds the smallest t in the closed interval [t0, t1] at which
		//	the ray P(t) = (ox, oy, oz) + t * (dx, dy, dz) meets the
		//	continuous piecewise-polynomial surface defined by noise2D with
		//	real (double) arithmetic. The instance is never modified.
		//
		//	- Hit:       a bracketed interval of width <= tolerance containing
		//	             the earliest root is returned (a zero-width interval
		//	             is allowed when a point lies exactly on the surface).
		//	- Miss:      the whole interval is proven root-free.
		//	- Uncertain: the node budget was exhausted, or the earliest
		//	             candidate is a near-multiple (e.g. tangential) root
		//	             that cannot be isolated reliably; t_lo / t_hi bracket
		//	             the earliest not-yet-ruled-out candidate interval.
		//	- Invalid:   an argument fails the documented preconditions.
		//
		//	Tangential / near-multiple roots may therefore be reported as
		//	Uncertain even inside the budget.

		[[nodiscard]]
		RayHit2D intersectHeight(double ox, double oy, double oz,
			double dx, double dy, double dz,
			double t0, double t1,
			double tolerance, unsigned max_nodes) const;

		///////////////////////////////////////
		//
		//	Noise (The result is remapped to the range [0, 1])
		//

		[[nodiscard]]
		value_type noise1D_01(value_type x) const noexcept;

		[[nodiscard]]
		value_type noise2D_01(value_type x, value_type y) const noexcept;

		[[nodiscard]]
		value_type noise3D_01(value_type x, value_type y, value_type z) const noexcept;

		///////////////////////////////////////
		//
		//	Octave noise (The result can be out of the range [-1, 1])
		//

		[[nodiscard]]
		value_type octave1D(value_type x, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type octave2D(value_type x, value_type y, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type octave3D(value_type x, value_type y, value_type z, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		///////////////////////////////////////
		//
		//	Octave noise (The result is clamped to the range [-1, 1])
		//

		[[nodiscard]]
		value_type octave1D_11(value_type x, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type octave2D_11(value_type x, value_type y, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type octave3D_11(value_type x, value_type y, value_type z, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		///////////////////////////////////////
		//
		//	Octave noise (The result is clamped and remapped to the range [0, 1])
		//

		[[nodiscard]]
		value_type octave1D_01(value_type x, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type octave2D_01(value_type x, value_type y, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type octave3D_01(value_type x, value_type y, value_type z, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		///////////////////////////////////////
		//
		//	Octave noise (The result is normalized to the range [-1, 1])
		//

		[[nodiscard]]
		value_type normalizedOctave1D(value_type x, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type normalizedOctave2D(value_type x, value_type y, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type normalizedOctave3D(value_type x, value_type y, value_type z, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		///////////////////////////////////////
		//
		//	Octave noise (The result is normalized and remapped to the range [0, 1])
		//

		[[nodiscard]]
		value_type normalizedOctave1D_01(value_type x, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type normalizedOctave2D_01(value_type x, value_type y, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

		[[nodiscard]]
		value_type normalizedOctave3D_01(value_type x, value_type y, value_type z, std::int32_t octaves, value_type persistence = value_type(0.5)) const noexcept;

	private:

		state_type m_permutation;
	};

	using PerlinNoise = BasicPerlinNoise<double>;

	namespace perlin_detail
	{
		////////////////////////////////////////////////
		//
		//	These functions are provided for consistency.
		//	You may get different results from std::shuffle() with different standard library implementations.
		//
		SIVPERLIN_CONCEPT_URBG
		[[nodiscard]]
		inline std::uint64_t Random(const std::uint64_t max, URBG&& urbg)
		{
			return (urbg() % (max + 1));
		}

		template <class RandomIt, class URBG>
		inline void Shuffle(RandomIt first, RandomIt last, URBG&& urbg)
		{
			if (first == last)
			{
				return;
			}

			using difference_type = typename std::iterator_traits<RandomIt>::difference_type;

			for (RandomIt it = first + 1; it < last; ++it)
			{
				const std::uint64_t n = static_cast<std::uint64_t>(it - first);
				std::iter_swap(it, first + static_cast<difference_type>(Random(n, std::forward<URBG>(urbg))));
			}
		}
		//
		////////////////////////////////////////////////

		template <class Float>
		[[nodiscard]]
		inline constexpr Float Fade(const Float t) noexcept
		{
			return t * t * t * (t * (t * 6 - 15) + 10);
		}

		template <class Float>
		[[nodiscard]]
		inline constexpr Float Lerp(const Float a, const Float b, const Float t) noexcept
		{
			return (a + (b - a) * t);
		}

		template <class Float>
		[[nodiscard]]
		inline constexpr Float Grad(const std::uint8_t hash, const Float x, const Float y, const Float z) noexcept
		{
			const std::uint8_t h = hash & 15;
			const Float u = h < 8 ? x : y;
			const Float v = h < 4 ? y : h == 12 || h == 14 ? x : z;
			return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
		}

		template <class Float>
		[[nodiscard]]
		inline constexpr Float Remap_01(const Float x) noexcept
		{
			return (x * Float(0.5) + Float(0.5));
		}

		template <class Float>
		[[nodiscard]]
		inline constexpr Float Clamp_11(const Float x) noexcept
		{
			return std::clamp(x, Float(-1.0), Float(1.0));
		}

		template <class Float>
		[[nodiscard]]
		inline constexpr Float RemapClamp_01(const Float x) noexcept
		{
			if (x <= Float(-1.0))
			{
				return Float(0.0);
			}
			else if (Float(1.0) <= x)
			{
				return Float(1.0);
			}

			return (x * Float(0.5) + Float(0.5));
		}

		template <class Noise, class Float>
		[[nodiscard]]
		inline auto Octave1D(const Noise& noise, Float x, const std::int32_t octaves, const Float persistence) noexcept
		{
			using value_type = Float;
			value_type result = 0;
			value_type amplitude = 1;

			for (std::int32_t i = 0; i < octaves; ++i)
			{
				result += (noise.noise1D(x) * amplitude);
				x *= 2;
				amplitude *= persistence;
			}

			return result;
		}

		template <class Noise, class Float>
		[[nodiscard]]
		inline auto Octave2D(const Noise& noise, Float x, Float y, const std::int32_t octaves, const Float persistence) noexcept
		{
			using value_type = Float;
			value_type result = 0;
			value_type amplitude = 1;

			for (std::int32_t i = 0; i < octaves; ++i)
			{
				result += (noise.noise2D(x, y) * amplitude);
				x *= 2;
				y *= 2;
				amplitude *= persistence;
			}

			return result;
		}

		template <class Noise, class Float>
		[[nodiscard]]
		inline auto Octave3D(const Noise& noise, Float x, Float y, Float z, const std::int32_t octaves, const Float persistence) noexcept
		{
			using value_type = Float;
			value_type result = 0;
			value_type amplitude = 1;

			for (std::int32_t i = 0; i < octaves; ++i)
			{
				result += (noise.noise3D(x, y, z) * amplitude);
				x *= 2;
				y *= 2;
				z *= 2;
				amplitude *= persistence;
			}

			return result;
		}

		template <class Float>
		[[nodiscard]]
		inline constexpr Float MaxAmplitude(const std::int32_t octaves, const Float persistence) noexcept
		{
			using value_type = Float;
			value_type result = 0;
			value_type amplitude = 1;

			for (std::int32_t i = 0; i < octaves; ++i)
			{
				result += amplitude;
				amplitude *= persistence;
			}

			return result;
		}
	}

	///////////////////////////////////////

	template <class Float>
	inline constexpr BasicPerlinNoise<Float>::BasicPerlinNoise() noexcept
		: m_permutation{ 151,160,137,91,90,15,
				131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
				190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
				88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
				77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
				102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208, 89,18,169,200,196,
				135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
				5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
				223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
				129,22,39,253, 19,98,108,110,79,113,224,232,178,185, 112,104,218,246,97,228,
				251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
				49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
				138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180 } {}

	template <class Float>
	inline BasicPerlinNoise<Float>::BasicPerlinNoise(const seed_type seed)
	{
		reseed(seed);
	}

	template <class Float>
	SIVPERLIN_CONCEPT_URBG_
	inline BasicPerlinNoise<Float>::BasicPerlinNoise(URBG&& urbg)
	{
		reseed(std::forward<URBG>(urbg));
	}

	///////////////////////////////////////

	template <class Float>
	inline void BasicPerlinNoise<Float>::reseed(const seed_type seed)
	{
		reseed(default_random_engine{ seed });
	}

	template <class Float>
	SIVPERLIN_CONCEPT_URBG_
	inline void BasicPerlinNoise<Float>::reseed(URBG&& urbg)
	{
		std::iota(m_permutation.begin(), m_permutation.end(), uint8_t{ 0 });

		perlin_detail::Shuffle(m_permutation.begin(), m_permutation.end(), std::forward<URBG>(urbg));
	}

	///////////////////////////////////////

	template <class Float>
	inline constexpr const typename BasicPerlinNoise<Float>::state_type& BasicPerlinNoise<Float>::serialize() const noexcept
	{
		return m_permutation;
	}

	template <class Float>
	inline constexpr void BasicPerlinNoise<Float>::deserialize(const state_type& state) noexcept
	{
		m_permutation = state;
	}

	///////////////////////////////////////

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::noise1D(const value_type x) const noexcept
	{
		return noise3D(x,
			static_cast<value_type>(SIVPERLIN_DEFAULT_Y),
			static_cast<value_type>(SIVPERLIN_DEFAULT_Z));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::noise2D(const value_type x, const value_type y) const noexcept
	{
		return noise3D(x,
			y,
			static_cast<value_type>(SIVPERLIN_DEFAULT_Z));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::noise3D(const value_type x, const value_type y, const value_type z) const noexcept
	{
		const value_type _x = std::floor(x);
		const value_type _y = std::floor(y);
		const value_type _z = std::floor(z);

		const std::int32_t ix = static_cast<std::int32_t>(_x) & 255;
		const std::int32_t iy = static_cast<std::int32_t>(_y) & 255;
		const std::int32_t iz = static_cast<std::int32_t>(_z) & 255;

		const value_type fx = (x - _x);
		const value_type fy = (y - _y);
		const value_type fz = (z - _z);

		const value_type u = perlin_detail::Fade(fx);
		const value_type v = perlin_detail::Fade(fy);
		const value_type w = perlin_detail::Fade(fz);

		const std::uint8_t A = (m_permutation[ix & 255] + iy) & 255;
		const std::uint8_t B = (m_permutation[(ix + 1) & 255] + iy) & 255;

		const std::uint8_t AA = (m_permutation[A] + iz) & 255;
		const std::uint8_t AB = (m_permutation[(A + 1) & 255] + iz) & 255;

		const std::uint8_t BA = (m_permutation[B] + iz) & 255;
		const std::uint8_t BB = (m_permutation[(B + 1) & 255] + iz) & 255;

		const value_type p0 = perlin_detail::Grad(m_permutation[AA], fx, fy, fz);
		const value_type p1 = perlin_detail::Grad(m_permutation[BA], fx - 1, fy, fz);
		const value_type p2 = perlin_detail::Grad(m_permutation[AB], fx, fy - 1, fz);
		const value_type p3 = perlin_detail::Grad(m_permutation[BB], fx - 1, fy - 1, fz);
		const value_type p4 = perlin_detail::Grad(m_permutation[(AA + 1) & 255], fx, fy, fz - 1);
		const value_type p5 = perlin_detail::Grad(m_permutation[(BA + 1) & 255], fx - 1, fy, fz - 1);
		const value_type p6 = perlin_detail::Grad(m_permutation[(AB + 1) & 255], fx, fy - 1, fz - 1);
		const value_type p7 = perlin_detail::Grad(m_permutation[(BB + 1) & 255], fx - 1, fy - 1, fz - 1);

		const value_type q0 = perlin_detail::Lerp(p0, p1, u);
		const value_type q1 = perlin_detail::Lerp(p2, p3, u);
		const value_type q2 = perlin_detail::Lerp(p4, p5, u);
		const value_type q3 = perlin_detail::Lerp(p6, p7, u);

		const value_type r0 = perlin_detail::Lerp(q0, q1, v);
		const value_type r1 = perlin_detail::Lerp(q2, q3, v);

		return perlin_detail::Lerp(r0, r1, w);
	}

	///////////////////////////////////////

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::noise1D_01(const value_type x) const noexcept
	{
		return perlin_detail::Remap_01(noise1D(x));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::noise2D_01(const value_type x, const value_type y) const noexcept
	{
		return perlin_detail::Remap_01(noise2D(x, y));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::noise3D_01(const value_type x, const value_type y, const value_type z) const noexcept
	{
		return perlin_detail::Remap_01(noise3D(x, y, z));
	}

	///////////////////////////////////////

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave1D(const value_type x, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Octave1D(*this, x, octaves, persistence);
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave2D(const value_type x, const value_type y, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Octave2D(*this, x, y, octaves, persistence);
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave3D(const value_type x, const value_type y, const value_type z, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Octave3D(*this, x, y, z, octaves, persistence);
	}

	///////////////////////////////////////

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave1D_11(const value_type x, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Clamp_11(octave1D(x, octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave2D_11(const value_type x, const value_type y, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Clamp_11(octave2D(x, y, octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave3D_11(const value_type x, const value_type y, const value_type z, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Clamp_11(octave3D(x, y, z, octaves, persistence));
	}

	///////////////////////////////////////

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave1D_01(const value_type x, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::RemapClamp_01(octave1D(x, octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave2D_01(const value_type x, const value_type y, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::RemapClamp_01(octave2D(x, y, octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::octave3D_01(const value_type x, const value_type y, const value_type z, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::RemapClamp_01(octave3D(x, y, z, octaves, persistence));
	}

	///////////////////////////////////////

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::normalizedOctave1D(const value_type x, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return (octave1D(x, octaves, persistence) / perlin_detail::MaxAmplitude(octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::normalizedOctave2D(const value_type x, const value_type y, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return (octave2D(x, y, octaves, persistence) / perlin_detail::MaxAmplitude(octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::normalizedOctave3D(const value_type x, const value_type y, const value_type z, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return (octave3D(x, y, z, octaves, persistence) / perlin_detail::MaxAmplitude(octaves, persistence));
	}

	///////////////////////////////////////

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::normalizedOctave1D_01(const value_type x, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Remap_01(normalizedOctave1D(x, octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::normalizedOctave2D_01(const value_type x, const value_type y, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Remap_01(normalizedOctave2D(x, y, octaves, persistence));
	}

	template <class Float>
	inline typename BasicPerlinNoise<Float>::value_type BasicPerlinNoise<Float>::normalizedOctave3D_01(const value_type x, const value_type y, const value_type z, const std::int32_t octaves, const value_type persistence) const noexcept
	{
		return perlin_detail::Remap_01(normalizedOctave3D(x, y, z, octaves, persistence));
	}

	///////////////////////////////////////
	//
	//	Ray / heightfield intersection
	//
	//	The heightfield y = noise2D(x, z) is a continuous piecewise
	//	polynomial function: inside every integer lattice cell the eight
	//	hashed gradients, the quintic fade and the lerps combine into one
	//	bivariate polynomial. Restricted to a ray it becomes a univariate
	//	polynomial in t of degree at most 11. Roots are isolated with
	//	conservative Bernstein-coefficient enclosures, so an interval is
	//	only ever discarded when a zero is numerically impossible.
	//
	namespace perlin_ray
	{
		constexpr int kMaxDegree = 11;

		// A dense monomial polynomial sum_k c[k] * t^k.
		struct Poly
		{
			double c[kMaxDegree + 1] = {};
			int n = 0;
		};

		inline void polyTrim(Poly& p)
		{
			while (0 < p.n && p.c[p.n] == 0.0)
			{
				--p.n;
			}
		}

		inline Poly polyAdd(const Poly& a, const Poly& b)
		{
			Poly r;
			r.n = (a.n < b.n) ? b.n : a.n;
			for (int i = 0; i <= r.n; ++i)
			{
				r.c[i] = a.c[i] + b.c[i];
			}
			polyTrim(r);
			return r;
		}

		inline Poly polySub(const Poly& a, const Poly& b)
		{
			Poly r;
			r.n = (a.n < b.n) ? b.n : a.n;
			for (int i = 0; i <= r.n; ++i)
			{
				r.c[i] = a.c[i] - b.c[i];
			}
			polyTrim(r);
			return r;
		}

		inline Poly polyScale(const Poly& a, const double s)
		{
			Poly r = a;
			for (int i = 0; i <= r.n; ++i)
			{
				r.c[i] *= s;
			}
			polyTrim(r);
			return r;
		}

		inline Poly polyMul(const Poly& a, const Poly& b)
		{
			Poly r;
			r.n = a.n + b.n;
			for (int i = 0; i <= a.n; ++i)
			{
				for (int j = 0; j <= b.n; ++j)
				{
					r.c[i + j] += a.c[i] * b.c[j];
				}
			}
			polyTrim(r);
			return r;
		}

		// Returns p(t) + k for a constant k (keeps the argument fixed).
		inline Poly polyAddConst(const Poly& a, const double k)
		{
			Poly r = a;
			r.c[0] += k;
			polyTrim(r);
			return r;
		}

		// Taylor shift: returns q(s) = p(s + a), used when reparameterizing
		// a polynomial from one interval to another.
		inline Poly polyTaylorShift(const Poly& p, const double a)
		{
			const int n = p.n;
			double binom[kMaxDegree + 1][kMaxDegree + 1] = {};
			for (int i = 0; i <= n; ++i)
			{
				binom[i][0] = binom[i][i] = 1.0;
				for (int k = 1; k < i; ++k)
				{
					binom[i][k] = binom[i - 1][k - 1] + binom[i - 1][k];
				}
			}
			double power[kMaxDegree + 1];
			power[0] = 1.0;
			for (int k = 1; k <= n; ++k)
			{
				power[k] = power[k - 1] * a;
			}
			Poly r;
			r.n = n;
			for (int k = 0; k <= n; ++k)
			{
				double sum = 0.0;
				for (int j = k; j <= n; ++j)
				{
					sum += p.c[j] * binom[j][k] * power[j - k];
				}
				r.c[k] = sum;
			}
			polyTrim(r);
			return r;
		}

		// Fade(a + b*t) = 10 q^3 - 15 q^4 + 6 q^5, q = a + b*t.
		inline Poly polyFade(const double a, const double b)
		{
			Poly q;
			q.n = (b == 0.0) ? 0 : 1;
			q.c[0] = a;
			q.c[1] = b;

			const Poly q2 = polyMul(q, q);
			const Poly q3 = polyMul(q2, q);
			const Poly q4 = polyMul(q3, q);
			const Poly q5 = polyMul(q4, q);

			Poly r = polyAdd(polyScale(q3, 10.0), polyScale(q4, -15.0));
			r = polyAdd(r, polyScale(q5, 6.0));
			return r;
		}

		// Lerp(a, b, u) = a + (b - a) * u for polynomial arguments.
		inline Poly polyLerp(const Poly& a, const Poly& b, const Poly& u)
		{
			return polyAdd(a, polyMul(polySub(b, a), u));
		}

		// Real-arithmetic version of perlin_detail::Grad for a fixed slice,
		// with the local coordinates given as affine polynomials in t.
		inline Poly polyGrad(const std::uint8_t hash,
			const Poly& lx, const Poly& ly, const Poly& lz)
		{
			const int h = hash & 15;
			Poly u = (h < 8) ? lx : ly;
			Poly v;
			if (h < 4)
			{
				v = ly;
			}
			else if (h == 12 || h == 14)
			{
				v = lx;
			}
			else
			{
				v = lz;
			}
			if (h & 1) u = polyScale(u, -1.0);
			if (h & 2) v = polyScale(v, -1.0);
			return polyAdd(u, v);
		}

		inline double binomRatio(const int n, const int k, const int i)
		{
			// C(k, i) / C(n, i), computed as a product to stay well behaved.
			double r = 1.0;
			for (int j = 0; j < i; ++j)
			{
				r *= static_cast<double>(k - j) / static_cast<double>(n - j);
			}
			return r;
		}

		// Monomial coefficients of p on [lo, hi] -> Bernstein coefficients
		// on the same interval. First reparameterize to s in [0, 1] with
		// t = lo + (hi - lo) * s, then apply the standard monomial-to-
		// Bernstein identity b_k = sum_{i<=k} g_i * C(k, i) / C(n, i).
		inline void toBernstein(const Poly& p, const double lo, const double hi,
			double* b)
		{
			const int n = p.n;
			const Poly g0 = polyTaylorShift(p, -lo);
			const double length = hi - lo;
			Poly g;
			g.n = n;
			double power = 1.0;
			for (int i = 0; i <= n; ++i)
			{
				g.c[i] = g0.c[i] * power;
				power *= length;
			}
			for (int k = 0; k <= n; ++k)
			{
				double sum = 0.0;
				for (int i = 0; i <= k; ++i)
				{
					sum += g.c[i] * binomRatio(n, k, i);
				}
				b[k] = sum;
			}
		}

		// Conservative rounding envelope for a Bernstein coefficient set.
		// Stress measurements over many cells, seeds and both fixed Z slices
		// bound the cell-polynomial construction error near 3e-14, and deep
		// de Casteljau subdivision adds only ~2e-15; the floor below keeps
		// roughly an order of magnitude of headroom. Coefficients genuinely
		// indistinguishable from zero (inside the envelope) are never
		// excluded or claimed to straddle it.
		inline double bernsteinMargin(const double* b, const int n)
		{
			double sumAbs = 0.0;
			for (int i = 0; i <= n; ++i)
			{
				sumAbs += std::fabs(b[i]);
			}
			return 2e-13 * (1.0 + sumAbs);
		}

		// de Casteljau subdivision at u; writes the two new coefficient sets.
		inline void bernsteinSplit(const double* b, const int n, const double u,
			double* left, double* right)
		{
			double tmp[kMaxDegree + 1];
			for (int i = 0; i <= n; ++i)
			{
				tmp[i] = b[i];
			}
			left[0] = tmp[0];
			right[n] = tmp[n];
			for (int j = 1; j <= n; ++j)
			{
				for (int i = 0; i <= n - j; ++i)
				{
					tmp[i] = (1.0 - u) * tmp[i] + u * tmp[i + 1];
				}
				left[j] = tmp[0];
				right[n - j] = tmp[n - j];
			}
		}

		struct Node
		{
			double b[kMaxDegree + 1];
			double lo;
			double hi;
		};

		// Builds f(u) = y(u) - noise2D(x(u), z(u)) for u in [0, 1] on the
		// lattice cell containing the segment midpoint. The segment is
		// expressed in the normalized local parameter u on purpose: every
		// coefficient then stays O(1) regardless of the global t scale, so
		// no catastrophic cancellation can arise from long rays. The hash
		// chain is identical to BasicPerlinNoise::noise3D (same permutation
		// table and gradient rule) - this is not an independent field.
		inline Poly buildCellPolynomial(const std::array<std::uint8_t, 256>& perm,
			const double zSlice,
			const double xLo, const double xHi,
			const double zLo, const double zHi,
			const double yLo, const double yHi)
		{
			const double xMid = (xLo + xHi) * 0.5;
			const double zMid = (zLo + zHi) * 0.5;

			const long long cx = static_cast<long long>(std::floor(xMid));
			const long long cz = static_cast<long long>(std::floor(zMid));
			const long long cSlice = static_cast<long long>(std::floor(zSlice));

			const int ix = static_cast<int>(cx & 255);
			const int iy = static_cast<int>(cz & 255);
			const int iz = static_cast<int>(cSlice & 255);

			const std::uint8_t A = static_cast<std::uint8_t>((perm[ix & 255] + iy) & 255);
			const std::uint8_t B = static_cast<std::uint8_t>((perm[(ix + 1) & 255] + iy) & 255);

			const std::uint8_t AA = static_cast<std::uint8_t>((perm[A] + iz) & 255);
			const std::uint8_t AB = static_cast<std::uint8_t>((perm[(A + 1) & 255] + iz) & 255);
			const std::uint8_t BA = static_cast<std::uint8_t>((perm[B] + iz) & 255);
			const std::uint8_t BB = static_cast<std::uint8_t>((perm[(B + 1) & 255] + iz) & 255);

			// Affine local coordinates in u in [0, 1]:
			// fx(u) = (xLo - cx) + (xHi - xLo) * u.
			Poly fx;
			fx.n = 1;
			fx.c[0] = xLo - static_cast<double>(cx);
			fx.c[1] = xHi - xLo;
			polyTrim(fx);

			Poly fz;
			fz.n = 1;
			fz.c[0] = zLo - static_cast<double>(cz);
			fz.c[1] = zHi - zLo;
			polyTrim(fz);

			const Poly fx1 = polyAddConst(fx, -1.0);	// fx - 1
			const Poly fz1 = polyAddConst(fz, -1.0);	// fz - 1

			const double fw = zSlice - std::floor(zSlice);
			Poly fwc; fwc.n = 0; fwc.c[0] = fw;			// fade input, fixed slice
			Poly fwc1; fwc1.n = 0; fwc1.c[0] = fw - 1.0;

			const Poly p0 = polyGrad(perm[AA], fx, fz, fwc);
			const Poly p1 = polyGrad(perm[BA], fx1, fz, fwc);
			const Poly p2 = polyGrad(perm[AB], fx, fz1, fwc);
			const Poly p3 = polyGrad(perm[BB], fx1, fz1, fwc);
			const Poly p4 = polyGrad(perm[(AA + 1) & 255], fx, fz, fwc1);
			const Poly p5 = polyGrad(perm[(BA + 1) & 255], fx1, fz, fwc1);
			const Poly p6 = polyGrad(perm[(AB + 1) & 255], fx, fz1, fwc1);
			const Poly p7 = polyGrad(perm[(BB + 1) & 255], fx1, fz1, fwc1);

			const Poly u = polyFade(fx.c[0], fx.c[1]);
			const Poly v = polyFade(fz.c[0], fz.c[1]);
			const Poly w = polyFade(fw, 0.0);

			const Poly q0 = polyLerp(p0, p1, u);
			const Poly q1 = polyLerp(p2, p3, u);
			const Poly q2 = polyLerp(p4, p5, u);
			const Poly q3 = polyLerp(p6, p7, u);

			const Poly r0 = polyLerp(q0, q1, v);
			const Poly r1 = polyLerp(q2, q3, v);

			const Poly surface = polyLerp(r0, r1, w);

			Poly rayY;
			rayY.n = 1;
			rayY.c[0] = yLo;
			rayY.c[1] = yHi - yLo;
			polyTrim(rayY);

			return polySub(rayY, surface);
		}

		// A point whose signed height is below this is treated as lying on
		// the surface (exact-root / zero-width hit allowance).
		constexpr double kPointEps = 1e-12;
	}

	template <class Float>
	inline RayHit2D BasicPerlinNoise<Float>::intersectHeight(
		const double ox, const double oy, const double oz,
		const double dx, const double dy, const double dz,
		const double t0, const double t1,
		const double tolerance, const unsigned max_nodes) const
	{
		static_assert(std::is_same_v<Float, double>,
			"intersectHeight is only provided for BasicPerlinNoise<double>");

		RayHit2D invalid;
		invalid.status = RayHit2D::Status::Invalid;

		//
		//	Input validation
		//
		const double inputs[10] = { ox, oy, oz, dx, dy, dz, t0, t1, tolerance, static_cast<double>(max_nodes) };
		for (const double v : inputs)
		{
			if (!std::isfinite(v))
			{
				return invalid;
			}
		}
		if (!(t0 <= t1))
		{
			return invalid;
		}
		if (!(tolerance >= 1e-10 && tolerance <= 1e-3))
		{
			return invalid;
		}
		if (max_nodes < 1u || max_nodes > 1000000u)
		{
			return invalid;
		}
		if (dx == 0.0 && dy == 0.0 && dz == 0.0)
		{
			return invalid;
		}

		const double x0 = ox + dx * t0, x1 = ox + dx * t1;
		const double z0 = oz + dz * t0, z1 = oz + dz * t1;
		if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(z0) || !std::isfinite(z1))
		{
			return invalid;
		}
		if (std::fabs(x0) > 10000.0 || std::fabs(x1) > 10000.0 ||
			std::fabs(z0) > 10000.0 || std::fabs(z1) > 10000.0)
		{
			return invalid;
		}

		// Number of integer lattice lines crossed (floor differences count
		// negative-direction crossings correctly as well).
		const long long crossX = std::llabs(static_cast<long long>(std::floor(x1)) - static_cast<long long>(std::floor(x0)));
		const long long crossZ = std::llabs(static_cast<long long>(std::floor(z1)) - static_cast<long long>(std::floor(z0)));
		if (crossX + crossZ > 512)
		{
			return invalid;
		}

		//
		//	Collect the t parameters of every x / z lattice boundary inside
		//	(t0, t1). Boundaries exactly at the endpoints are not duplicated.
		//
		std::vector<double> cuts;
		cuts.reserve(static_cast<std::size_t>(crossX + crossZ + 2));
		cuts.push_back(t0);

		const auto collectAxis = [&](const double o, const double d,
			const double a, const double b)
		{
			if (d == 0.0)
			{
				return;
			}
			const long long ka = static_cast<long long>(std::floor(a));
			const long long kb = static_cast<long long>(std::floor(b));
			if (d > 0.0)
			{
				for (long long k = ka + 1; k <= kb; ++k)
				{
					const double tc = (static_cast<double>(k) - o) / d;
					if (tc > t0 && tc < t1)
					{
						cuts.push_back(tc);
					}
				}
			}
			else
			{
				for (long long k = ka; k > kb; --k)
				{
					const double tc = (static_cast<double>(k) - o) / d;
					if (tc > t0 && tc < t1)
					{
						cuts.push_back(tc);
					}
				}
			}
		};

		collectAxis(ox, dx, x0, x1);
		collectAxis(oz, dz, z0, z1);

		cuts.push_back(t1);
		std::sort(cuts.begin(), cuts.end());
		cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

		const double zSlice = static_cast<double>(SIVPERLIN_DEFAULT_Z);

		// Signed height used for the exact-endpoint allowance.
		const auto signedHeight = [&](const double t)
		{
			return (oy + dy * t) - this->noise2D(ox + dx * t, oz + dz * t);
		};

		// A ray starting exactly on the surface is a zero-width Hit.
		if (std::fabs(signedHeight(t0)) <= perlin_ray::kPointEps)
		{
			RayHit2D hit;
			hit.status = RayHit2D::Status::Hit;
			hit.t_lo = hit.t_hi = t0;
			return hit;
		}

		unsigned used = 0;
		std::vector<perlin_ray::Node> stack;
		stack.reserve(128);

		for (std::size_t seg = 0; seg + 1 < cuts.size(); ++seg)
		{
			const double sLo = cuts[seg];
			const double sHi = cuts[seg + 1];
			if (!(sHi > sLo))
			{
				continue;
			}

			// Segment endpoints in world coordinates.
			const double xLo = ox + dx * sLo, xHi = ox + dx * sHi;
			const double zLo = oz + dz * sLo, zHi = oz + dz * sHi;
			const double yLo = oy + dy * sLo, yHi = oy + dy * sHi;

			// Polynomial in the normalized local parameter u in [0, 1],
			// where t = sLo + (sHi - sLo) * u.
			const perlin_ray::Poly f = perlin_ray::buildCellPolynomial(
				m_permutation, zSlice, xLo, xHi, zLo, zHi, yLo, yHi);

			perlin_ray::Node root;
			root.lo = sLo;
			root.hi = sHi;
			perlin_ray::toBernstein(f, 0.0, 1.0, root.b);
			const int n = f.n;

			stack.clear();
			stack.push_back(root);

			// Transverse roots are isolated below the requested tolerance if
			// need be, down to this representability floor: a transverse root
			// keeps producing a strict sign straddle under subdivision, while
			// a true tangency does not and is reported Uncertain there.
			const double microTol = 1e-12 * (1.0 + (std::max)(std::fabs(sLo), std::fabs(sHi)));

			while (!stack.empty())
			{
				const perlin_ray::Node node = stack.back();
				stack.pop_back();

				++used;		// every polynomial interval check counts
				if (used > max_nodes)
				{
					// Nodes are examined near-to-far, so the node currently in
					// hand is the earliest candidate that has not been ruled
					// out. Report it rather than masking it as a Miss.
					RayHit2D unc;
					unc.status = RayHit2D::Status::Uncertain;
					unc.t_lo = node.lo;
					unc.t_hi = node.hi;
					return unc;
				}

				const double margin = perlin_ray::bernsteinMargin(node.b, n);
				double bMin = node.b[0], bMax = node.b[0];
				for (int i = 1; i <= n; ++i)
				{
					bMin = (std::min)(bMin, node.b[i]);
					bMax = (std::max)(bMax, node.b[i]);
				}

				// Conservative one-sided exclusion: coefficients hugging
				// zero (inside the rounding margin) are never excluded.
				if (bMin > margin || bMax < -margin)
				{
					continue;
				}

				const double width = node.hi - node.lo;

				if (width <= tolerance)
				{
					// b[0] and b[n] are the exact (degree-preserving)
					// endpoint values of the polynomial on this node.
					const double fLoB = node.b[0];
					const double fHiB = node.b[n];

					// A strict, rounding-margin-free sign straddle proves a
					// root by the intermediate value theorem. Nodes are
					// visited near-to-far and every earlier node was either
					// excluded or subdivided, so this is the earliest root;
					// a second root inside the same tolerance-wide node is
					// immaterial because the interval already contains it.
					if ((fLoB > margin && fHiB < -margin) ||
						(fLoB < -margin && fHiB > margin))
					{
						RayHit2D hit;
						hit.status = RayHit2D::Status::Hit;
						hit.t_lo = node.lo;
						hit.t_hi = node.hi;
						return hit;
					}

					// Otherwise try to prove a unique transverse root with
					// the derivative envelope (covers straddles softened by
					// the rounding margin).
					double dMin = 0.0, dMax = 0.0;
					double dMargin = 0.0;
					if (n >= 1)
					{
						const double scale = static_cast<double>(n) / width;
						dMin = dMax = scale * (node.b[1] - node.b[0]);
						for (int i = 1; i < n; ++i)
						{
							const double d = scale * (node.b[i + 1] - node.b[i]);
							dMin = (std::min)(dMin, d);
							dMax = (std::max)(dMax, d);
						}
						dMargin = perlin_ray::bernsteinMargin(node.b, n) * scale;
					}

					if ((dMin > dMargin || dMax < -dMargin) &&
						bMin <= 0.0 && bMax >= 0.0)
					{
						// Strictly monotone (so the Bernstein coefficients
						// are monotone and b[0] / b[n] are the endpoint
						// values) while genuinely straddling zero: exactly
						// one transverse root lies inside this interval.
						RayHit2D hit;
						hit.status = RayHit2D::Status::Hit;
						hit.t_lo = node.lo;
						hit.t_hi = node.hi;
						return hit;
					}

					// No proof at this width. A transverse root would show a
					// strict straddle on further subdivision; only keep
					// splitting while the interval is representably wider
					// than the micro floor, otherwise the candidate is a
					// near-tangent / possible multiple root and must be
					// reported Uncertain, never silently turned into a Miss.
					if (width <= microTol)
					{
						RayHit2D unc;
						unc.status = RayHit2D::Status::Uncertain;
						unc.t_lo = node.lo;
						unc.t_hi = node.hi;
						return unc;
					}
				}

				// Split at the parametric midpoint; push the far half first
				// so the near half is examined first (LIFO order).
				const double mid = (node.lo + node.hi) * 0.5;
				if (!(mid > node.lo) || !(node.hi > mid))
				{
					// Interval no longer splittable in double precision.
					RayHit2D unc;
					unc.status = RayHit2D::Status::Uncertain;
					unc.t_lo = node.lo;
					unc.t_hi = node.hi;
					return unc;
				}
				const double u = (mid - node.lo) / (node.hi - node.lo);
				perlin_ray::Node left, right;
				left.lo = node.lo;
				left.hi = mid;
				right.lo = mid;
				right.hi = node.hi;
				perlin_ray::bernsteinSplit(node.b, n, u, left.b, right.b);
				stack.push_back(right);
				stack.push_back(left);
			}
		}

		// Every cell segment was proven root-free.
		RayHit2D miss;
		miss.status = RayHit2D::Status::Miss;
		miss.t_lo = t0;
		miss.t_hi = t1;
		return miss;
	}
}

# undef SIVPERLIN_NODISCARD_CXX20
# undef SIVPERLIN_CONCEPT_URBG
# undef SIVPERLIN_CONCEPT_URBG_

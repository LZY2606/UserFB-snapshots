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
# include <iterator>
# include <limits>
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
	//
	//	Result of a ray / height-field intersection query.
	//
	//	The height field is the continuous piecewise polynomial surface
	//	y = noise2D(x, z), evaluated at the fixed slice SIVPERLIN_DEFAULT_Z.
	//
	//	Status:
	//	  Hit       - [t_lo, t_hi] is a width <= tolerance interval that contains
	//	              the earliest root in [t0, t1]. A point exactly on the surface
	//	              may be reported as a zero-width Hit (t_lo == t_hi).
	//	  Miss      - the whole query interval is proven to contain no root.
	//	  Uncertain - the earliest candidate could not be resolved within the
	//	              node budget, or is a near multiple / tangential root that
	//	              cannot be isolated reliably. [t_lo, t_hi] is the earliest
	//	              interval still containing a possible root; it is never used
	//	              to mask a Miss.
	//	  Invalid   - the arguments fail the documented preconditions.
	//
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
		//	Ray / height-field intersection (BasicPerlinNoise<double> only)
		//
		//	Finds the smallest t in the closed interval [t0, t1] at which the
		//	ray P(t) = (ox, oy, oz) + t * (dx, dy, dz) meets the continuous
		//	piecewise-polynomial surface y = noise2D(x, z) of the fixed
		//	SIVPERLIN_DEFAULT_Z slice. Does not modify the instance.
		//

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
	//	Ray / height-field intersection
	//
	//	The surface y = noise2D(x, z) is, inside every integer (x, z) cell,
	//	one fixed polynomial: the permutation-table gradients are affine,
	//	Fade is the quintic 6t^5 - 15t^4 + 10t^3 and Lerp is linear. With the
	//	fixed z slice the cell polynomial has degree at most 6 in each local
	//	coordinate (quintic weight times the affine gradient), hence degree at
	//	most 11 along a ray. All arithmetic below is promoted to double and
	//	treated as real arithmetic; the float rounding steps of the scalar
	//	noise2D path are not replayed. Root exclusion uses conservative
	//	Bernstein-coefficient bounds with an explicit rounding margin, so an
	//	interval whose bound touches zero is never discarded as root-free.
	//

	namespace perlin_raytrace
	{
		constexpr int kDegree = 11;
		constexpr int kAxis = 7; // exponents 0..6 on each cell axis

		// Univariate polynomial in the power basis on [0, 1].
		struct Poly
		{
			std::array<double, kDegree + 1> a{};
		};

		inline Poly polyConst(double c) noexcept
		{
			Poly p;
			p.a[0] = c;
			return p;
		}

		inline Poly polyLinear(double c, double b) noexcept
		{
			Poly p;
			p.a[0] = c;
			p.a[1] = b;
			return p;
		}

		inline Poly polyAdd(const Poly& x, const Poly& y) noexcept
		{
			Poly r;
			for (int i = 0; i <= kDegree; ++i) r.a[i] = x.a[i] + y.a[i];
			return r;
		}

		inline Poly polyScale(const Poly& x, double s) noexcept
		{
			Poly r;
			for (int i = 0; i <= kDegree; ++i) r.a[i] = x.a[i] * s;
			return r;
		}

		inline Poly polyMul(const Poly& x, const Poly& y) noexcept
		{
			Poly r;
			for (int i = 0; i <= kDegree; ++i)
			{
				if (x.a[i] == 0.0) continue;
				for (int j = 0; i + j <= kDegree; ++j)
				{
					r.a[i + j] += x.a[i] * y.a[j];
				}
			}
			return r;
		}

		inline double polyEval(const Poly& p, double t) noexcept
		{
			double r = p.a[kDegree];
			for (int i = kDegree; i-- > 0;)
			{
				r = r * t + p.a[i];
			}
			return r;
		}

		// q(s) = p(l + (r - l) * s), i.e. p restricted to [l, r] expressed
		// on a fresh [0, 1] variable.
		inline Poly polyRestrict(const Poly& p, double l, double r) noexcept
		{
			const double w = r - l;
			Poly q;
			double lpow[kDegree + 1] = { 1.0 };
			double wpow[kDegree + 1] = { 1.0 };
			for (int i = 1; i <= kDegree; ++i)
			{
				lpow[i] = lpow[i - 1] * l;
				wpow[i] = wpow[i - 1] * w;
			}
			for (int i = 0; i <= kDegree; ++i)
			{
				if (p.a[i] == 0.0) continue;
				double binom = 1.0; // C(i, k)
				for (int k = 0; k <= i; ++k)
				{
					q.a[k] += p.a[i] * binom * lpow[i - k] * wpow[k];
					binom *= static_cast<double>(i - k) / static_cast<double>(k + 1);
				}
			}
			return q;
		}

		struct Range
		{
			double lo;
			double hi;
		};

		// Conservative range of p on [0, 1] from its Bernstein coefficients
		// (the convex-hull property), widened by an explicit rounding margin.
		inline Range polyRange(const Poly& p) noexcept
		{
			static constexpr double Cn[kDegree + 1] = {
				1.0, 11.0, 55.0, 165.0, 330.0, 462.0,
				462.0, 330.0, 165.0, 55.0, 11.0, 1.0
			};
			double b[kDegree + 1] = {};
			for (int k = 0; k <= kDegree; ++k)
			{
				double binom = 1.0; // C(k, i)
				for (int i = 0; i <= k; ++i)
				{
					b[k] += p.a[i] * (binom / Cn[i]);
					binom *= static_cast<double>(k - i) / static_cast<double>(i + 1);
				}
			}
			double lo = b[0], hi = b[0], absSum = 0.0;
			for (int k = 1; k <= kDegree; ++k)
			{
				lo = std::min(lo, b[k]);
				hi = std::max(hi, b[k]);
			}
			for (int i = 0; i <= kDegree; ++i) absSum += std::abs(p.a[i]);
			const double margin = 1e-12 * (1.0 + absSum) + 1e-14;
			return { lo - margin, hi + margin };
		}

		// Fade(t) = 6t^5 - 15t^4 + 10t^3
		inline Poly fadePoly() noexcept
		{
			Poly p;
			p.a[3] = 10.0;
			p.a[4] = -15.0;
			p.a[5] = 6.0;
			return p;
		}

		// 1 - Fade(t): the complementary corner weight of the Lerp chain.
		inline Poly oneMinusFadePoly() noexcept
		{
			Poly p;
			p.a[0] = 1.0;
			p.a[3] = -10.0;
			p.a[4] = 15.0;
			p.a[5] = -6.0;
			return p;
		}

		// Bivariate cell polynomial H(sx, sz), term c[i][j] * sx^i * sz^j.
		struct BivPoly
		{
			std::array<std::array<double, kAxis>, kAxis> c{};
		};

		inline BivPoly bivScale(const BivPoly& x, double s) noexcept
		{
			BivPoly r;
			for (int i = 0; i < kAxis; ++i)
				for (int j = 0; j < kAxis; ++j)
					r.c[i][j] = x.c[i][j] * s;
			return r;
		}

		inline BivPoly bivAdd(const BivPoly& x, const BivPoly& y) noexcept
		{
			BivPoly r;
			for (int i = 0; i < kAxis; ++i)
				for (int j = 0; j < kAxis; ++j)
					r.c[i][j] = x.c[i][j] + y.c[i][j];
			return r;
		}

		inline BivPoly bivMul(const BivPoly& x, const BivPoly& y) noexcept
		{
			BivPoly r;
			for (int i = 0; i < kAxis; ++i)
				for (int j = 0; j < kAxis; ++j)
				{
					if (x.c[i][j] == 0.0) continue;
					for (int u = 0; i + u < kAxis; ++u)
						for (int v = 0; j + v < kAxis; ++v)
							r.c[i + u][j + v] += x.c[i][j] * y.c[u][v];
				}
			return r;
		}

		// Integer gradient of Grad(hash, X, Y, Z). Mirrors
		// perlin_detail::Grad exactly so no second random field is created:
		//   u = (h < 8) ? X : Y
		//   v = (h < 4) ? Y : (h == 12 || h == 14 ? X : Z)
		// On the fixed z slice the gradient dot product becomes the affine
		// expression gx*(sx - i) + gy*(sz - j) + gz*(fz - l), where fz is
		// constant within a cell and the (i, j, l) are the corner offsets.
		struct Grad3
		{
			double gx;
			double gy;
			double gz;
		};

		inline Grad3 grad3(std::uint8_t hash) noexcept
		{
			const unsigned h = static_cast<unsigned>(hash) & 15u;
			Grad3 g{ 0.0, 0.0, 0.0 };
			if (h < 8u)
			{
				g.gx = (h & 1u) ? -1.0 : 1.0;
			}
			else
			{
				g.gy = (h & 1u) ? -1.0 : 1.0;
			}
			if (h < 4u)
			{
				g.gy += (h & 2u) ? -1.0 : 1.0;
			}
			else if (h == 12u || h == 14u)
			{
				g.gx += (h & 2u) ? -1.0 : 1.0;
			}
			else
			{
				g.gz = (h & 2u) ? -1.0 : 1.0;
			}
			return g;
		}

		// Exact cell polynomial of noise2D(x0 + sx, z0 + sz) for
		// 0 <= sx, sz <= 1, sharing the instance permutation table. The full
		// 3D Lerp chain is retained; the z coordinate is frozen at the fixed
		// slice so Fade(fz) and every z-offset gradient term are constants.
		inline BivPoly buildCell(const std::array<std::uint8_t, 256>& perm,
			std::int64_t x0, std::int64_t z0, double zslice)
		{
			const int ix = static_cast<int>(x0 & 255);
			const int iy = static_cast<int>(z0 & 255);
			const double fz = zslice - std::floor(zslice);
			const int iz = static_cast<int>(std::floor(zslice)) & 255;
			const double w = perlin_detail::Fade(fz);

			const std::uint8_t A = static_cast<std::uint8_t>((perm[ix & 255] + iy) & 255);
			const std::uint8_t B = static_cast<std::uint8_t>((perm[(ix + 1) & 255] + iy) & 255);
			const std::uint8_t AA = static_cast<std::uint8_t>((perm[A] + iz) & 255);
			const std::uint8_t AB = static_cast<std::uint8_t>((perm[(A + 1) & 255] + iz) & 255);
			const std::uint8_t BA = static_cast<std::uint8_t>((perm[B] + iz) & 255);
			const std::uint8_t BB = static_cast<std::uint8_t>((perm[(B + 1) & 255] + iz) & 255);

			const std::uint8_t h0 = perm[AA];
			const std::uint8_t h1 = perm[BA];
			const std::uint8_t h2 = perm[AB];
			const std::uint8_t h3 = perm[BB];
			const std::uint8_t h4 = perm[(AA + 1) & 255];
			const std::uint8_t h5 = perm[(BA + 1) & 255];
			const std::uint8_t h6 = perm[(AB + 1) & 255];
			const std::uint8_t h7 = perm[(BB + 1) & 255];

			const Poly fade = fadePoly();
			const Poly oneMinus = oneMinusFadePoly(); // 1 - Fade

			auto onX = [](const Poly& p) {
				BivPoly b;
				for (int i = 0; i <= 6; ++i) b.c[i][0] = p.a[i];
				return b;
			};
			auto onZ = [](const Poly& p) {
				BivPoly b;
				for (int j = 0; j <= 6; ++j) b.c[0][j] = p.a[j];
				return b;
			};

			// One Lerp-chain corner: axis weight * affine slice gradient.
			auto corner = [&](std::uint8_t hash, int i, int j, int l,
				const BivPoly& weight) {
				const Grad3 g = grad3(hash);
				BivPoly gp;
				gp.c[0][0] = g.gx * (-static_cast<double>(i))
					+ g.gy * (-static_cast<double>(j))
					+ g.gz * (fz - static_cast<double>(l));
				gp.c[1][0] = g.gx;
				gp.c[0][1] = g.gy;
				return bivMul(weight, gp);
			};

			const BivPoly wx0 = onX(oneMinus); // 1 - u
			const BivPoly wx1 = onX(fade);     // u
			const BivPoly wz0 = onZ(oneMinus); // 1 - v
			const BivPoly wz1 = onZ(fade);     // v

			BivPoly h{};
			// z = iz layer, scaled by the constant (1 - w).
			const double zw0 = 1.0 - w;
			h = bivAdd(h, bivScale(corner(h0, 0, 0, 0, bivMul(wx0, wz0)), zw0));
			h = bivAdd(h, bivScale(corner(h1, 1, 0, 0, bivMul(wx1, wz0)), zw0));
			h = bivAdd(h, bivScale(corner(h2, 0, 1, 0, bivMul(wx0, wz1)), zw0));
			h = bivAdd(h, bivScale(corner(h3, 1, 1, 0, bivMul(wx1, wz1)), zw0));
			// z = iz + 1 layer, scaled by the constant w.
			h = bivAdd(h, bivScale(corner(h4, 0, 0, 1, bivMul(wx0, wz0)), w));
			h = bivAdd(h, bivScale(corner(h5, 1, 0, 1, bivMul(wx1, wz0)), w));
			h = bivAdd(h, bivScale(corner(h6, 0, 1, 1, bivMul(wx0, wz1)), w));
			h = bivAdd(h, bivScale(corner(h7, 1, 1, 1, bivMul(wx1, wz1)), w));
			return h;
		}

		// Compose H(sx(s), sz(s)) with axis polynomials linear in the ray
		// local parameter s in [0, 1]; the result degree is at most kDegree.
		inline Poly composeCell(const BivPoly& h, const Poly& sx, const Poly& sz) noexcept
		{
			Poly xpow[kAxis];
			Poly zpow[kAxis];
			xpow[0] = polyConst(1.0);
			zpow[0] = polyConst(1.0);
			for (int i = 1; i < kAxis; ++i)
			{
				xpow[i] = polyMul(xpow[i - 1], sx);
				zpow[i] = polyMul(zpow[i - 1], sz);
			}
			Poly r;
			for (int i = 0; i < kAxis; ++i)
				for (int j = 0; j < kAxis; ++j)
				{
					const double c = h.c[i][j];
					if (c == 0.0) continue;
					r = polyAdd(r, polyScale(polyMul(xpow[i], zpow[j]), c));
				}
			return r;
		}

		struct Segment
		{
			double t0;
			double t1;
			std::int64_t ix;
			std::int64_t iz;
		};

		inline bool finiteAll(std::initializer_list<double> vs) noexcept
		{
			for (const double v : vs)
			{
				if (!std::isfinite(v)) return false;
			}
			return true;
		}
	}

	template <class Float>
	RayHit2D BasicPerlinNoise<Float>::intersectHeight(
		const double ox, const double oy, const double oz,
		const double dx, const double dy, const double dz,
		const double t0, const double t1,
		const double tolerance, const unsigned max_nodes) const
	{
		using namespace perlin_raytrace;

		//
		// Argument validation.
		//
		if (!finiteAll({ ox, oy, oz, dx, dy, dz, t0, t1, tolerance }))
		{
			return { RayHit2D::Status::Invalid, 0.0, 0.0 };
		}
		if (!(t0 <= t1)
			|| tolerance < 1e-10 || tolerance > 1e-3
			|| max_nodes < 1u || max_nodes > 1000000u
			|| (dx == 0.0 && dy == 0.0 && dz == 0.0))
		{
			return { RayHit2D::Status::Invalid, 0.0, 0.0 };
		}

		const double xEnd0 = ox + dx * t0;
		const double zEnd0 = oz + dz * t0;
		const double xEnd1 = ox + dx * t1;
		const double zEnd1 = oz + dz * t1;
		if (!finiteAll({ xEnd0, zEnd0, xEnd1, zEnd1 })
			|| std::abs(xEnd0) > 10000.0 || std::abs(zEnd0) > 10000.0
			|| std::abs(xEnd1) > 10000.0 || std::abs(zEnd1) > 10000.0)
		{
			return { RayHit2D::Status::Invalid, 0.0, 0.0 };
		}

		//
		// Split [t0, t1] at every integer x/z grid line crossed by the ray.
		// The same (k - o) / d formula handles negative directions and rays
		// starting exactly on a grid line.
		//
		std::vector<double> cuts;
		cuts.reserve(514);
		std::size_t crossedLines = 0;

		const auto collectAxis = [&](double o, double d, double e0, double e1)
		{
			if (d == 0.0) return;
			const double lo = std::min(e0, e1);
			const double hi = std::max(e0, e1);
			const std::int64_t kLo = static_cast<std::int64_t>(std::floor(lo));
			const std::int64_t kHi = static_cast<std::int64_t>(std::floor(hi));
			for (std::int64_t k = kLo; k <= kHi + 1; ++k)
			{
				const double tc = (static_cast<double>(k) - o) / d;
				if (tc > t0 && tc <= t1)
				{
					cuts.push_back(tc);
					++crossedLines;
				}
			}
		};
		collectAxis(ox, dx, xEnd0, xEnd1);
		collectAxis(oz, dz, zEnd0, zEnd1);

		if (crossedLines > 512)
		{
			return { RayHit2D::Status::Invalid, 0.0, 0.0 };
		}
		std::sort(cuts.begin(), cuts.end());

		std::vector<Segment> segments;
		segments.reserve(cuts.size() + 1);
		{
			double prev = t0;
			for (double tc : cuts)
			{
				if (tc <= prev) continue; // merge coincident x/z crossings
				segments.push_back({ prev, tc, 0, 0 });
				prev = tc;
			}
			segments.push_back({ prev, t1, 0, 0 });
			// Each segment's cell is read at its own midpoint, which lies
			// strictly inside the segment and so is never on a grid line.
			for (Segment& seg : segments)
			{
				const double tm = 0.5 * (seg.t0 + seg.t1);
				seg.ix = static_cast<std::int64_t>(std::floor(ox + dx * tm));
				seg.iz = static_cast<std::int64_t>(std::floor(oz + dz * tm));
			}
		}

		const double zslice = static_cast<double>(SIVPERLIN_DEFAULT_Z);
		unsigned nodes = 0;

		//
		// Cells are processed near to far; an unresolved candidate in an
		// earlier cell always shadows every later cell.
		//
		for (const Segment& seg : segments)
		{
			const BivPoly cell = buildCell(m_permutation, seg.ix, seg.iz, zslice);
			const double len = seg.t1 - seg.t0;

			// Local parameter s in [0, 1] maps back to t = seg.t0 + len*s.
			const Poly sx = polyLinear(
				ox + dx * seg.t0 - static_cast<double>(seg.ix), dx * len);
			const Poly sz = polyLinear(
				oz + dz * seg.t0 - static_cast<double>(seg.iz), dz * len);
			const Poly height = composeCell(cell, sx, sz);

			// f(s) = y(t(s)) - height(s); a root places the ray on the surface.
			Poly f;
			const double y0 = oy + dy * seg.t0;
			f.a[0] = y0 - height.a[0];
			f.a[1] = dy * len - height.a[1];
			for (int i = 2; i <= kDegree; ++i) f.a[i] = -height.a[i];

			// A point exactly on the surface may be a zero-width Hit.
			const double f0 = polyEval(f, 0.0);
			const double onEps = 1e-11
				+ 1e-15 * (std::abs(y0) + std::abs(ox + dx * seg.t0)
					+ std::abs(oz + dz * seg.t0));
			if (std::abs(f0) <= onEps)
			{
				return { RayHit2D::Status::Hit, seg.t0, seg.t0 };
			}
			if (len == 0.0)
			{
				continue; // single-point query proven off the surface
			}

			struct Interval
			{
				double l, r;
			};
			std::vector<Interval> pending;
			pending.reserve(128);
			pending.push_back({ 0.0, 1.0 });

			while (!pending.empty())
			{
				const Interval iv = pending.back();
				pending.pop_back();

				const Poly q = polyRestrict(f, iv.l, iv.r);
				const Range r = polyRange(q);
				++nodes; // every polynomial interval check counts
				if (nodes > max_nodes)
				{
					return { RayHit2D::Status::Uncertain,
						seg.t0 + iv.l * len, seg.t0 + iv.r * len };
				}

				// Proven root-free: strictly one side of zero (margin included).
				if (r.hi < 0.0 || r.lo > 0.0)
				{
					continue;
				}

				const double widthT = (iv.r - iv.l) * len;
				const double fl = polyEval(q, 0.0);
				const double fr = polyEval(q, 1.0);
				const bool signChange = (fl < 0.0 && fr > 0.0)
					|| (fl > 0.0 && fr < 0.0)
					|| fl == 0.0 || fr == 0.0;

				if (widthT <= tolerance)
				{
					const double loT = seg.t0 + iv.l * len;
					const double hiT = seg.t0 + iv.r * len;
					if (signChange)
					{
						// Contains the earliest root of this cell.
						return { RayHit2D::Status::Hit, loT, hiT };
					}
					// Same sign at both ends while the conservative bound
					// touches zero: a tangential / near-multiple root cannot
					// be isolated reliably - never downgrade this to Miss.
					return { RayHit2D::Status::Uncertain, loT, hiT };
				}

				// Bisect; push the far half first so the near half is taken
				// first by the LIFO queue.
				const double m = 0.5 * (iv.l + iv.r);
				pending.push_back({ m, iv.r });
				pending.push_back({ iv.l, m });
			}
		}

		// Every crossed cell was proven root-free over the query interval.
		return { RayHit2D::Status::Miss, t0, t1 };
	}
}

# undef SIVPERLIN_NODISCARD_CXX20
# undef SIVPERLIN_CONCEPT_URBG
# undef SIVPERLIN_CONCEPT_URBG_

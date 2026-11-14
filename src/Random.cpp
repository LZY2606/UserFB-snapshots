/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <time.h>
#include <stdio.h>

#include "Random.hpp"


using namespace hxa7241_general;




/// constants ------------------------------------------------------------------

namespace
{
   // default seed
   const int32u SEED = 987654321;

   // minimum seeds
   //const int32u SEED_MINS[4] = { 2, 8, 16, 128 };
}




/// standard object services ---------------------------------------------------

/*Random::Random()
{
   // try to get 'non-determinate' seed
   int32u seed = 0;
   {
      // probably Unix time -- signed 32-bit, seconds since 1970
      const time_t t = ::time(0);

      // check time sufficient (probably) and available
      if( (sizeof(time_t) >= 4) && (t != -1) )
      {
         // rotate to make frequently changing bits more significant
         seed = (static_cast<int32u>(t) << 8) | (static_cast<int32u>(t) >> 24);
      }
   }

   // init state from seed
   // *** VERY IMPORTANT ***
      The initial seeds z1, z2, z3, z4  MUST be larger
      than 1, 7, 15, and 127 respectively. //
   for( int i = 4;  i--;  z_m[i] = (seed >= SEED_MINS[i]) ? seed : SEED ) {}

   // store seed/id as 8 digit hex number string
   ::sprintf( id_m, "%08X", z_m[3] & 0xFFFFFFFFu );
}*/


Random::Random()
{
   /* *** VERY IMPORTANT ***
      The initial seeds z1, z2, z3, z4  MUST be larger
      than 1, 7, 15, and 127 respectively. */

   z_m[0] = z_m[1] = z_m[2] = z_m[3] = SEED;
}


Random::Random
(
   const int32u state[4]
)
{
   z_m[0] = state[0];
   z_m[1] = state[1];
   z_m[2] = state[2];
   z_m[3] = state[3];
}


Random Random::makeIdentified
(
   const int32u baseSeed,
   const int32u pixelX,
   const int32u pixelY,
   const int32u sampleIndex,
   const int32u streamKind
)
{
   // Mix the full sampling identity into four independent 32-bit LFSR113
   // state words. SplitMix64 (Steele/Lea) gives independent, well-mixed words
   // for every distinct identity tuple, so a path can be replayed byte-for-byte
   // and neighbouring identities do not share state.
   int32u h = baseSeed;
   h = static_cast<int32u>(h * 2246822519u) ^ pixelX;
   h = static_cast<int32u>(h * 3266489917u) ^ pixelY;
   h = static_cast<int32u>(h * 668265263u)  ^ sampleIndex;
   h = static_cast<int32u>(h * 374761393u)  ^ streamKind;

   int32u state[4];
   for( int i = 0;  i < 4;  ++i )
   {
      unsigned long long z = (static_cast<unsigned long long>(h) << 1) |
         static_cast<unsigned long long>(i + 1u);
      z += 0x9E3779B97F4A7C15ull;
      z  = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z  = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      z  =  z ^ (z >> 31);
      state[i] = static_cast<int32u>(z & 0xFFFFFFFFull);
   }

   /* *** VERY IMPORTANT ***
      The initial seeds z1, z2, z3, z4 MUST be larger
      than 1, 7, 15, and 127 respectively. */
   state[0] |= 2u;
   state[1] |= 8u;
   state[2] |= 16u;
   state[3] |= 128u;

   return Random( state );
}




/// queries --------------------------------------------------------------------

int32u Random::getInt32u() const
{
   z_m[0] = ((z_m[0] & 0xFFFFFFFEu) << 18) ^ (((z_m[0] <<  6) ^ z_m[0]) >> 13);
   z_m[1] = ((z_m[1] & 0xFFFFFFF8u) <<  2) ^ (((z_m[1] <<  2) ^ z_m[1]) >> 27);
   z_m[2] = ((z_m[2] & 0xFFFFFFF0u) <<  7) ^ (((z_m[2] << 13) ^ z_m[2]) >> 21);
   z_m[3] = ((z_m[3] & 0xFFFFFF80u) << 13) ^ (((z_m[3] <<  3) ^ z_m[3]) >> 12);

   return z_m[0] ^ z_m[1] ^ z_m[2] ^ z_m[3];
}


/*real32 Random::getReal32() const
{
   return static_cast<real32>(static_cast<int32>(getInt32u() & 0xFFFFFF00u)) *
      (1.0f / 4294967296.0f) + 0.5f;
}*/


real64 Random::getReal64() const
{
   return static_cast<real64>(static_cast<int32>(getInt32u())) *
      (1.0 / 4294967296.0) + 0.5 +
      static_cast<real64>(static_cast<int32>(getInt32u() & 0x001FFFFFu)) *
      (1.0 / 9007199254740992.0);
}


/*real64 Random::getReal64_() const
{
   return static_cast<real64>(static_cast<int32>(getInt32u())) *
      (1.0 / 4294967296.0) +
      (0.5 + (1.0  / 4503599627370496.0) * 0.5) +
      static_cast<real64>(static_cast<int32>(getInt32u() & 0x000FFFFFu)) *
      (1.0  / 4503599627370496.0);
}*/


/*const char* Random::getId() const
{
   return id_m;
}*/

/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef Random_h
#define Random_h




#include "hxa7241_general.hpp"
namespace hxa7241_general
{


/**
 * Simple, fast, good random number generator.<br/><br/>
 *
 * @implementation
 *
 * 'Maximally Equidistributed Combined Tausworthe Generators'; L'Ecuyer; 1996.
 * http://www.iro.umontreal.ca/~lecuyer/myftp/papers/tausme2.ps
 * http://www.iro.umontreal.ca/~simardr/rng/lfsr113.c
 *
 * 'Conversion of High-Period Random Numbers to Floating Point'; Doornik; 2006.
 * http://www.doornik.com/research/randomdouble.pdf
 */
class Random
{
/// standard object services ---------------------------------------------------
public:
            /**
             * Create Random object.
             */
            Random();

            /**
             * Create a replayable stream from a full sampling identity.
             *
             * The generator state is derived solely from the base seed, the
             * full-image pixel coordinates, the pixel's own sample serial and
             * the stream kind (pilot/production). It is built afresh from that
             * identity, never copied from another live stream, so the number
             * of random draws made by one path cannot change another pixel's
             * sequence.
             */
   static  Random makeIdentified( int32u baseSeed,
                                  int32u pixelX,
                                  int32u pixelY,
                                  int32u sampleIndex,
                                  int32u streamKind );

// use defaults
//         ~Random();
//          Random( const Random& );
// Random& operator=( const Random& );


/// commands -------------------------------------------------------------------


/// queries --------------------------------------------------------------------
           int32u getInt32u()                                             const;

           /**
            * Single precision, [0,1) interval (never returns 1).
            */
//         real32 getReal32()                                             const;
//
           /**
            * Souble precision, [0,1) interval (never returns 1).
            */
           real64 getReal64()                                             const;

           /**
            * Souble precision, (0,1) interval (never returns 0 or 1).
            */
//         real64 getReal64_()                                            const;

           /**
            * Get original seed / id, as 8 digit/char hex number string.
            */
//         const char* getId()                                            const;


/// fields ---------------------------------------------------------------------
private:
   explicit Random( const int32u state[4] );

   mutable int32u z_m[4];
// char           id_m[9];
};


}//namespace




#endif//Random_h

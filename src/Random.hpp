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

// use defaults
//         ~Random();
//          Random( const Random& );
// Random& operator=( const Random& );


/// commands -------------------------------------------------------------------


/// queries --------------------------------------------------------------------
           int32u getInt32u()                                             const;

           /**
            * Copy another generator's state into this one.<br/><br/>
            *
            * Const, like the sampling queries: the state is mutable, so this
            * lets code with a `const Random&` restore the caller's stream to a
            * particular point in the sequence.
            */
           void  setState( const Random& other )                      const;

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
   mutable int32u z_m[4];
// char           id_m[9];
};


}//namespace




#endif//Random_h

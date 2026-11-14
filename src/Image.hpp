/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef Image_h
#define Image_h


#include <iosfwd>
#include <vector>

#include "Vector3f.hpp"




#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Pixel sheet with simple tone-mapping and file formatting.<br/><br/>
 *
 * Uses PPM image format:
 * <cite>http://netpbm.sourceforge.net/doc/ppm.html</cite><br/><br/>
 *
 * Uses Ward simple tonemapper:
 * <cite>'A Contrast Based Scalefactor For Luminance Display'
 * Ward;
 * Graphics Gems 4, AP 1994.</cite><br/><br/>
 *
 * Uses RGBE image format:
 * <cite>http://radsite.lbl.gov/radiance/refer/filefmts.pdf</cite>
 * <cite>'Real Pixels' Ward; Graphics Gems 2, AP 1991;</cite><br/><br/>
 *
 * @invariants
 * * width_m  >= 1 and <= DIMENSION_MAX
 * * height_m >= 1 and <= DIMENSION_MAX
 * * pixels_m.size() == (width_m * height_m)
 */
class Image
{
/// standard object services ---------------------------------------------------
public:
   explicit Image( std::istream& );

   /**
    * Create an image that accumulates adaptive (per-pixel) production samples.
    * Such an image must never be mixed with the legacy addToPixel/getFrame
    * accumulation; legacy images keep their uniform-iteration behaviour.
    */
   static Image createAdaptive( int32 width, int32 height );

// use defaults
//         ~Image();
//          Image( const Image& );
// Image& operator=( const Image& );


/// commands -------------------------------------------------------------------
           void  addToPixel( int32           x,
                             int32           y,
                             const Vector3f& radiance );

           /**
            * Accumulate one adaptive production sample, updating the pixel's
            * running linear mean and its production count. Adaptive-mode only.
            */
           void  addAdaptiveSample( int32           x,
                                    int32           y,
                                    const Vector3f& radiance );

           /** Clear all adaptive means and production counts. */
           void  resetAdaptive();


/// queries --------------------------------------------------------------------
           int32 getWidth()                                               const;
           int32 getHeight()                                              const;

           void  getFormatted( std::ostream& out,
                               int32         iteration )                  const;

           /** Whether this image uses the adaptive accumulation mode. */
           bool  isAdaptive()                                             const;

           /** Number of production samples accumulated for a pixel. */
           int32u getPixelSampleCount( int32 x, int32 y )                 const;

           /**
            * Running linear RGB mean of a pixel; exactly zero when the pixel
            * has not been sampled yet.
            */
           Vector3f getPixelMean( int32 x, int32 y )                      const;

           /**
            * PPM P6 output for adaptive renders: the whole-frame adaptation
            * luminance and every channel are taken from the per-pixel means,
            * not from a uniform per-iteration divider.
            */
           void  getAdaptiveFormatted( std::ostream& out )                const;


/// constants ------------------------------------------------------------------
   static  int32 DIMENSION_MAX();


/// implementation -------------------------------------------------------------
protected:
   static  real64 calculateToneMapping( const std::vector<Vector3f>& pixels,
                                        real64                       divider );
   static  real64 calculateToneMappingFromMeans(
                                        const std::vector<Vector3f>& means );
//   static  int32u toRgbe( const Vector3f& rgb );   // HDRI


/// fields ---------------------------------------------------------------------
private:
           Image( int32 width, int32 height, bool adaptive );

           int32                 width_m;
           int32                 height_m;
           bool                  adaptive_m;
           std::vector<Vector3f> pixels_m;
           std::vector<int32u>   sampleCounts_m;
};


}//namespace




#endif//Image_h

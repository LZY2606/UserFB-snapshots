/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef Camera_h
#define Camera_h


#include <iosfwd>

#include "Vector3f.hpp"
#include "AdaptiveSampling.hpp"




#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * View definition and rasterizer.<br/><br/>
 *
 * getFrame() accumulates a frame of samples to the image.<br/><br/>
 *
 * Constant.
 *
 * @invariants
 * * viewAngle_m is >= VIEW_ANGLE_MIN and <= VIEW_ANGLE_MAX degrees in radians
 * * viewDirection_m is unitized
 * * right_m is unitized
 * * up_m is unitized
 * * above three form a coordinate frame
 */
class Camera
{
/// standard object services ---------------------------------------------------
public:
   explicit Camera( std::istream& );

// use defaults
//         ~Camera();
//          Camera( const Camera& );
// Camera& operator=( const Camera& );


/// commands -------------------------------------------------------------------


/// queries --------------------------------------------------------------------
           Vector3f getEyePoint()                                         const;

           void  getFrame( const Scene&,
                           const Random&,
                           Image& )                                       const;

           /**
            * Render one adaptive frame. Every pixel still active receives one
            * pilot path (luminance only feeds the Welford statistics) and one
            * production path (accumulated to the adaptive image), each on its
            * own replayable random stream. Active flags for the next frame are
            * then updated from the scheduling criterion.
            */
           void  getAdaptiveFrame( const Scene&  scene,
                                   AdaptiveState& state,
                                   Image&         image )                 const;

           /**
            * Replay a single identified camera path through the full RayTracer
            * transport, without touching any sampling state or image.
            */
           Vector3f replayPath( const Scene& scene,
                                int32        imageWidth,
                                int32        imageHeight,
                                int32u       seed,
                                int32        pixelX,
                                int32        pixelY,
                                int32u       sampleIndex,
                                AdaptiveStream streamKind )                const;

           /**
            * Reset an AdaptiveState together with its bound adaptive image, so
            * the statistics and the film they drive always stay in step.
            */
           void  resetAdaptive( AdaptiveState& state,
                                Image&          image )                   const;


/// implementation -------------------------------------------------------------
protected:
           /** Camera ray direction for a pixel with explicit sub-pixel jitter. */
           Vector3f makeRayDirection( int32 pixelX,
                                      int32 pixelY,
                                      int32 imageWidth,
                                      int32 imageHeight,
                                      real64 jitterX,
                                      real64 jitterY )                     const;


/// constants ------------------------------------------------------------------
   static  real64 VIEW_ANGLE_MIN();
   static  real64 VIEW_ANGLE_MAX();


/// fields ---------------------------------------------------------------------
private:
   Vector3f viewPosition_m;
   real64   viewAngle_m;

   // view frame
   Vector3f viewDirection_m;
   Vector3f right_m;
   Vector3f up_m;
};


}//namespace




#endif//Camera_h

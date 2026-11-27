/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef Camera_h
#define Camera_h


#include <iosfwd>

#include "Vector3f.hpp"
#include "GradientState.hpp"




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
   /**
    * Render one progressive round into a caller-owned gradient state.<br/><br/>
    *
    * For each pixel, the base and every existing right/up neighbour start from
    * the same copy of the incoming random state, so they share sub-pixel jitter
    * and the following random sequence. Each then traces its own camera ray
    * with the existing RayTracer transport. Gradients are neighbour radiance
    * minus base radiance. When a pixel is finished, the caller's random
    * parameter is advanced only to the state at the end of the base evaluation.
    *
    * Only genuine interior edges are accumulated: there are no synthetic
    * border edges and no wraparound, and each edge is recorded once, at its
    * left/bottom endpoint.
    */
           void  getGradientFrame( const Scene&,
                                   const Random&,
                                   GradientState& )                    const;


/// queries --------------------------------------------------------------------
           Vector3f getEyePoint()                                         const;

           void  getFrame( const Scene&,
                           const Random&,
                           Image& )                                       const;

           /**
            * Make the sampling ray direction for pixel (x,y).<br/><br/>
            *
            * Consumes the two jitter values from the supplied random state
            * exactly as getFrame does; callers that need the same sample
            * elsewhere pass a copy of the state.
            */
           Vector3f getSampleDirection( real64            x,
                                        real64            y,
                                        int32             width,
                                        int32             height,
                                        Random&           random )         const;


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

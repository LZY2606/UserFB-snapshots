/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef Camera_h
#define Camera_h


#include <iosfwd>

#include "Vector3f.hpp"




#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * View definition and rasterizer.<br/><br/>
 *
 * getFrame() accumulates a frame of samples to the image.<br/><br/>
 *
 * getFrameGradient() accumulates one gradient-domain round -- primal
 * estimates and paired neighbour differences -- to a GradientState.<br/><br/>
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
            * Accumulate one gradient-domain round.<br/><br/>
            *
            * For each pixel, the caller's Random state is copied; the base
            * pixel and its existing right/up neighbours are all evaluated
            * from that same copied state -- identical sub-pixel jitter and
            * identical subsequent random sequence -- each along its own
            * camera ray. The neighbour-minus-base linear radiance difference
            * is recorded per edge, and the base radiance per pixel. After
            * each pixel, the caller's Random is advanced only to the state
            * at the end of the base evaluation.<br/><br/>
            */
           void  getFrameGradient( const Scene&,
                                   Random&,
                                   GradientState& )                       const;


/// constants ------------------------------------------------------------------
   static  real64 VIEW_ANGLE_MIN();
   static  real64 VIEW_ANGLE_MAX();


/// implementation -------------------------------------------------------------
private:
           /**
            * Make the sampling ray direction for pixel (x,y), with sub-pixel
            * jitter drawn from random. Shared by both rasterizing queries.
            */
           Vector3f makeSampleDirection( real64        x,
                                         real64        y,
                                         real64        width,
                                         real64        height,
                                         const Random& random )           const;


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

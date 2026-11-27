/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <math.h>
#include <istream>

#include "Random.hpp"
#include "RayTracer.hpp"
#include "Image.hpp"

#include "Camera.hpp"


using namespace hxa7241_minilight;




/// standard object services ---------------------------------------------------

Camera::Camera
(
   std::istream& in
)
{
   // read and condition view definition
   {
      in >> viewPosition_m >> viewDirection_m >> viewAngle_m;

      viewDirection_m = viewDirection_m.unitize();
      if( viewDirection_m.isZero() )
      {
         viewDirection_m = Vector3f( 0.0, 0.0, 1.0 );
      }

      viewAngle_m = (viewAngle_m < VIEW_ANGLE_MIN() ? VIEW_ANGLE_MIN() :
         (viewAngle_m > VIEW_ANGLE_MAX() ? VIEW_ANGLE_MAX() : viewAngle_m)) *
         (3.14159265358979 / 180.0);
   }

   // make other directions of frame
   {
      // make trial 'right', using viewDirection and assuming 'up' is Y
      up_m    = Vector3f( 0.0, 1.0, 0.0 );
      right_m = up_m.cross( viewDirection_m ).unitize();

      // check 'right' is valid
      // -- i.e. viewDirection was not co-linear with 'up'
      if( !right_m.isZero() )
      {
         // use 'right', and make 'up' properly orthogonal
         up_m = viewDirection_m.cross( right_m ).unitize();
      }
      // else, assume a different 'up' and redo
      else
      {
         // 'up' is Z if viewDirection is down, otherwise -Z
         up_m = Vector3f( 0.0, 0.0, viewDirection_m[1] < 0.0 ? 1.0 : -1.0);
         // remake 'right'
         right_m = up_m.cross( viewDirection_m ).unitize();
      }
   }
}




/// queries --------------------------------------------------------------------

Vector3f Camera::getEyePoint() const
{
   return viewPosition_m;
}


void Camera::getFrame
(
   const Scene&  scene,
   const Random& random,
   Image&        image_o
) const
{
   const RayTracer rayTracer( scene );

   const int32  width  = image_o.getWidth();
   const int32  height = image_o.getHeight();

   // step through image pixels, sampling them
   for( real64 y = static_cast<real64>(height);  y-- > 0.0; )
   {
      for( real64 x = static_cast<real64>(width);  x-- > 0.0; )
      {
         // make sample ray direction, stratified by pixels
         Random sampleRandom;
         sampleRandom.setState( random );

         const Vector3f sampleDirection( getSampleDirection( x, y,
            width, height, sampleRandom ) );

         // get radiance from RayTracer
         const Vector3f radiance( rayTracer.getRadiance(
            viewPosition_m, sampleDirection, sampleRandom ) );

         // the caller's random advances exactly with the sample
         const_cast<Random&>( random ).setState( sampleRandom );

         // add radiance to image
         image_o.addToPixel( static_cast<int32>(x), static_cast<int32>(y),
            radiance );
      }
   }
}


Vector3f Camera::getSampleDirection
(
   const real64  x,
   const real64  y,
   const int32   width,
   const int32   height,
   Random&       random
) const
{
   // make image plane XY displacement vector [-1,+1) coefficients,
   // with sub-pixel jitter
   const real64 xF = ((x + random.getReal64()) * 2.0 /
      static_cast<real64>(width) ) - 1.0;
   const real64 yF = ((y + random.getReal64()) * 2.0 /
      static_cast<real64>(height)) - 1.0;

   // make image plane offset vector,
   // by scaling the view definition by the coefficients
   const Vector3f offset( (right_m * xF) +
      (up_m * yF * (static_cast<real64>(height) /
         static_cast<real64>(width))) );

   // add image offset vector to view direction
   return (viewDirection_m +
      (offset * ::tan(viewAngle_m * 0.5))).unitize();
}


void Camera::getGradientFrame
(
   const Scene&  scene,
   const Random& random,
   GradientState& gradient_o
) const
{
   const RayTracer rayTracer( scene );

   const int32 width  = gradient_o.getWidth();
   const int32 height = gradient_o.getHeight();

   if( (width < 1) || (height < 1) )
   {
      return;
   }

   for( int32 y = height - 1;  y >= 0;  --y )
   {
      for( int32 x = width - 1;  x >= 0;  --x )
      {
         // the base and any neighbours all begin from the same copy of the
         // caller's random state, so their sub-pixel jitter and subsequent
         // random sequences correspond
         Random startRandom;
         startRandom.setState( random );

         Random baseRandom( startRandom );
         const Vector3f baseRadiance( rayTracer.getRadiance( viewPosition_m,
            getSampleDirection( static_cast<real64>(x), static_cast<real64>(y),
               width, height, baseRandom ),
            baseRandom ) );

         gradient_o.accumulatePrimal( x, y, baseRadiance );

         // right neighbour edge
         if( gradient_o.hasRightEdge( x, y ) )
         {
            Random neighbourRandom( startRandom );
            const Vector3f neighbourRadiance( rayTracer.getRadiance(
               viewPosition_m,
               getSampleDirection( static_cast<real64>(x + 1),
                  static_cast<real64>(y), width, height, neighbourRandom ),
               neighbourRandom ) );

            gradient_o.accumulateRightGradient( x, y,
               neighbourRadiance - baseRadiance );
         }

         // up neighbour edge
         if( gradient_o.hasUpEdge( x, y ) )
         {
            Random neighbourRandom( startRandom );
            const Vector3f neighbourRadiance( rayTracer.getRadiance(
               viewPosition_m,
               getSampleDirection( static_cast<real64>(x),
                  static_cast<real64>(y + 1), width, height, neighbourRandom ),
               neighbourRandom ) );

            gradient_o.accumulateUpGradient( x, y,
               neighbourRadiance - baseRadiance );
         }

         // The caller's random advances only to the end of the base eval:
         // neighbouring paths are never allowed to feed their state onward.
         const_cast<Random&>( random ).setState( baseRandom );
      }
   }

   gradient_o.completeRound();
}




/// constants ------------------------------------------------------------------

real64 Camera::VIEW_ANGLE_MIN()
{
   return 10.0;
}


real64 Camera::VIEW_ANGLE_MAX()
{
   return 160.0;
}

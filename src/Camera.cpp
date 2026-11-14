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
#include "GradientState.hpp"

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

   const real64 width  = static_cast<real64>(image_o.getWidth());
   const real64 height = static_cast<real64>(image_o.getHeight());

   // step through image pixels, sampling them
   for( real64 y = height;  y-- > 0.0; )
   {
      for( real64 x = width;  x-- > 0.0; )
      {
         // make sample ray direction, stratified by pixels
         const Vector3f sampleDirection( makeSampleDirection( x, y, width,
            height, random ) );

         // get radiance from RayTracer
         const Vector3f radiance( rayTracer.getRadiance(
            viewPosition_m, sampleDirection, random ) );

         // add radiance to image
         image_o.addToPixel( static_cast<int32>(x), static_cast<int32>(y),
            radiance );
      }
   }
}


void Camera::getFrameGradient
(
   const Scene&   scene,
   Random&        random,
   GradientState& state_o
) const
{
   const RayTracer rayTracer( scene );

   const int32  width  = state_o.getWidth();
   const int32  height = state_o.getHeight();
   const real64 widthF  = static_cast<real64>(width);
   const real64 heightF = static_cast<real64>(height);

   state_o.beginRound();

   // step through image pixels, sampling them
   for( int32 y = height;  y-- > 0; )
   {
      for( int32 x = width;  x-- > 0; )
      {
         // copy the caller's random state for this pixel
         Random baseRandom( random );

         // base sample
         const Vector3f baseDirection( makeSampleDirection(
            static_cast<real64>(x), static_cast<real64>(y), widthF, heightF,
            baseRandom ) );
         const Vector3f baseRadiance( rayTracer.getRadiance(
            viewPosition_m, baseDirection, baseRandom ) );
         state_o.addPrimal( x, y, baseRadiance );

         // right neighbour, evaluated from the same random state --
         // same sub-pixel jitter, same subsequent random sequence
         if( state_o.hasRight( x, y ) )
         {
            Random rightRandom( random );

            const Vector3f rightDirection( makeSampleDirection(
               static_cast<real64>(x + 1), static_cast<real64>(y), widthF,
               heightF, rightRandom ) );
            const Vector3f rightRadiance( rayTracer.getRadiance(
               viewPosition_m, rightDirection, rightRandom ) );

            state_o.addGradientRight( x, y, rightRadiance - baseRadiance );
         }

         // up neighbour, evaluated from the same random state
         if( state_o.hasUp( x, y ) )
         {
            Random upRandom( random );

            const Vector3f upDirection( makeSampleDirection(
               static_cast<real64>(x), static_cast<real64>(y + 1), widthF,
               heightF, upRandom ) );
            const Vector3f upRadiance( rayTracer.getRadiance(
               viewPosition_m, upDirection, upRandom ) );

            state_o.addGradientUp( x, y, upRadiance - baseRadiance );
         }

         // the caller's random advances only by the base evaluation
         random = baseRandom;
      }
   }

   state_o.completeRound();
}




/// implementation -------------------------------------------------------------

Vector3f Camera::makeSampleDirection
(
   const real64  x,
   const real64  y,
   const real64  width,
   const real64  height,
   const Random& random
) const
{
   // make image plane XY displacement vector [-1,+1) coefficients,
   // with sub-pixel jitter
   const real64 xF = ((x + random.getReal64()) * 2.0 / width ) - 1.0;
   const real64 yF = ((y + random.getReal64()) * 2.0 / height) - 1.0;

   // make image plane offset vector,
   // by scaling the view definition by the coefficients
   const Vector3f offset( (right_m * xF) +
      (up_m * yF * (height / width)) );

   // add image offset vector to view direction
   return (viewDirection_m +
      (offset * ::tan(viewAngle_m * 0.5))).unitize();
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

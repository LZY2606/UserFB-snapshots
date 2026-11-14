/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <math.h>
#include <istream>
#include <stdexcept>

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

   const real64 width  = static_cast<real64>(image_o.getWidth());
   const real64 height = static_cast<real64>(image_o.getHeight());

   // step through image pixels, sampling them
   for( real64 y = height;  y-- > 0.0; )
   {
      for( real64 x = width;  x-- > 0.0; )
      {
         // sub-pixel jitter from the frame's shared random stream
         const real64 jitterX = random.getReal64();
         const real64 jitterY = random.getReal64();

         // make sample ray direction, stratified by pixels
         const Vector3f sampleDirection( makeRayDirection(
            static_cast<int32>(x), static_cast<int32>(y),
            image_o.getWidth(), image_o.getHeight(), jitterX, jitterY ) );

         // get radiance from RayTracer
         const Vector3f radiance( rayTracer.getRadiance(
            viewPosition_m, sampleDirection, random ) );

         // add radiance to image
         image_o.addToPixel( static_cast<int32>(x), static_cast<int32>(y),
            radiance );
      }
   }
}


Vector3f Camera::makeRayDirection
(
   const int32  pixelX,
   const int32  pixelY,
   const int32  imageWidth,
   const int32  imageHeight,
   const real64 jitterX,
   const real64 jitterY
) const
{
   const real64 width  = static_cast<real64>(imageWidth);
   const real64 height = static_cast<real64>(imageHeight);

   // make image plane XY displacement vector [-1,+1) coefficients,
   // with the supplied sub-pixel jitter
   const real64 xF = ((static_cast<real64>(pixelX) + jitterX) * 2.0 /
      width ) - 1.0;
   const real64 yF = ((static_cast<real64>(pixelY) + jitterY) * 2.0 /
      height) - 1.0;

   // make image plane offset vector,
   // by scaling the view definition by the coefficients
   const Vector3f offset( (right_m * xF) +
      (up_m * yF * (height / width)) );

   // add image offset vector to view direction
   return (viewDirection_m +
      (offset * ::tan(viewAngle_m * 0.5))).unitize();
}


void Camera::getAdaptiveFrame
(
   const Scene&   scene,
   AdaptiveState& state,
   Image&         image
) const
{
   if( !image.isAdaptive() )
   {
      throw std::logic_error(
         "Camera::getAdaptiveFrame: image must be an adaptive image" );
   }
   if( (image.getWidth() != state.getWidth()) ||
      (image.getHeight() != state.getHeight()) )
   {
      throw std::invalid_argument(
         "Camera::getAdaptiveFrame: image size does not match state size" );
   }

   // bind (or verify) the Camera/Scene/size triple; re-binding differently
   // requires state.reset() first
   state.bind( *this, scene, image.getWidth(), image.getHeight() );

   const RayTracer rayTracer( scene );

   const int32 width  = state.getWidth();
   const int32 height = state.getHeight();

   // step through image pixels, each active pixel gets one pilot and one
   // production sample from independent, identity-derived random streams
   for( int32 y = 0;  y < height;  ++y )
   {
      for( int32 x = 0;  x < width;  ++x )
      {
         const std::size_t index =
            static_cast<std::size_t>(x + (y * width));
         if( !state.activeAt( index ) )
         {
            continue;
         }

         // the pixel's own sample serial: continuing a render resumes here,
         // and one path's draw count never shifts another pixel's stream
         const int32u sampleIndex = state.sampleSerialAt( index );

         // pilot path -- its radiance only contributes to pilot statistics
         const Random pilotRandom = Random::makeIdentified( state.getSeed(),
            static_cast<int32u>(x), static_cast<int32u>(y), sampleIndex,
            PILOT_STREAM );
         const Vector3f pilotRadiance( rayTracer.getRadiance(
            viewPosition_m,
            makeRayDirection( x, y, width, height,
               pilotRandom.getReal64(), pilotRandom.getReal64() ),
            pilotRandom ) );

         PilotStatistic& statistic = state.statisticAt( index );
         statistic.addSample( linearLuminance( pilotRadiance ) );

         // production path -- a separate stream, accumulated only to the film
         const Random productionRandom = Random::makeIdentified(
            state.getSeed(), static_cast<int32u>(x),
            static_cast<int32u>(y), sampleIndex, PRODUCTION_STREAM );
         const Vector3f productionRadiance( rayTracer.getRadiance(
            viewPosition_m,
            makeRayDirection( x, y, width, height,
               productionRandom.getReal64(), productionRandom.getReal64() ),
            productionRandom ) );

         image.addAdaptiveSample( x, y, productionRadiance );
         state.incrementProductionCount( index );

         // schedule the pixel for the next frame (or retire it)
         const bool stillActive = !statistic.shouldStop( state.getConfig() );
         state.setActiveAt( index, stillActive );
         state.setSampleSerialAt( index, sampleIndex + 1u );
      }
   }

   state.setFrameCount( state.getFrameCount() + 1u );
}


Vector3f Camera::replayPath
(
   const Scene&          scene,
   const int32           imageWidth,
   const int32           imageHeight,
   const int32u          seed,
   const int32           pixelX,
   const int32           pixelY,
   const int32u          sampleIndex,
   const AdaptiveStream  streamKind
) const
{
   // reconstruct the exact identified stream used by getAdaptiveFrame and
   // run the same full RayTracer transport, with no state or film side effect
   const RayTracer rayTracer( scene );
   const Random random = Random::makeIdentified( seed,
      static_cast<int32u>(pixelX), static_cast<int32u>(pixelY), sampleIndex,
      streamKind );

   const Vector3f sampleDirection( makeRayDirection( pixelX, pixelY,
      imageWidth, imageHeight, random.getReal64(), random.getReal64() ) );

   return rayTracer.getRadiance( viewPosition_m, sampleDirection, random );
}


void Camera::resetAdaptive
(
   AdaptiveState& state,
   Image&         image
) const
{
   if( !image.isAdaptive() )
   {
      throw std::logic_error(
         "Camera::resetAdaptive: image must be an adaptive image" );
   }

   // statistics and the film that records their production samples are
   // cleared together, so the two can never describe different runs
   state.reset();
   image.resetAdaptive();
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

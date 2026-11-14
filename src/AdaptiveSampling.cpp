/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Adaptive sampling extension.

------------------------------------------------------------------------------*/


#include <cmath>
#include <limits>
#include <stdexcept>

#include "AdaptiveSampling.hpp"


using namespace hxa7241_minilight;


namespace
{
   std::size_t indexOf
   (
      const int32 width,
      const int32 x,
      const int32 y
   )
   {
      return static_cast<std::size_t>(x + (y * width));
   }
}




/// free functions -------------------------------------------------------------

real64 hxa7241_minilight::linearLuminance
(
   const Vector3f& linearRgb
)
{
   return (linearRgb[0] * 0.2126) +
          (linearRgb[1] * 0.7152) +
          (linearRgb[2] * 0.0722);
}




/// AdaptiveConfig -------------------------------------------------------------

AdaptiveConfig::AdaptiveConfig
(
   const int32  minSamples,
   const int32  maxSamples,
   const real64 absError,
   const real64 relError
)
{
   if( minSamples < 2 )
   {
      throw std::invalid_argument(
         "AdaptiveConfig: minSamples must be at least 2" );
   }
   if( maxSamples < minSamples )
   {
      throw std::invalid_argument(
         "AdaptiveConfig: maxSamples must be >= minSamples" );
   }
   if( !std::isfinite( absError ) || (absError < 0.0) )
   {
      throw std::invalid_argument(
         "AdaptiveConfig: absError must be finite and non-negative" );
   }
   if( !std::isfinite( relError ) || (relError < 0.0) )
   {
      throw std::invalid_argument(
         "AdaptiveConfig: relError must be finite and non-negative" );
   }

   minSamples_m = minSamples;
   maxSamples_m = maxSamples;
   absError_m   = absError;
   relError_m   = relError;
}


int32 AdaptiveConfig::getMinSamples() const
{
   return minSamples_m;
}


int32 AdaptiveConfig::getMaxSamples() const
{
   return maxSamples_m;
}


real64 AdaptiveConfig::getAbsError() const
{
   return absError_m;
}


real64 AdaptiveConfig::getRelError() const
{
   return relError_m;
}




/// PilotStatistic -------------------------------------------------------------

PilotStatistic::PilotStatistic()
 : count_m( 0 )
 , mean_m ( 0.0 )
 , m2_m   ( 0.0 )
{
}


void PilotStatistic::addSample
(
   const real64 luminance
)
{
   // Welford's online mean/variance update.
   ++count_m;
   const real64 delta  = luminance - mean_m;
   mean_m += delta / static_cast<real64>(count_m);
   const real64 delta2 = luminance - mean_m;
   m2_m   += delta * delta2;
}


void PilotStatistic::reset()
{
   count_m = 0;
   mean_m  = 0.0;
   m2_m    = 0.0;
}


int32u PilotStatistic::getCount() const
{
   return count_m;
}


real64 PilotStatistic::getMean() const
{
   return mean_m;
}


real64 PilotStatistic::getM2() const
{
   return m2_m;
}


real64 PilotStatistic::getVariance() const
{
   return count_m > 1u ? (m2_m / static_cast<real64>(count_m - 1u)) : 0.0;
}


real64 PilotStatistic::getConfidenceHalfWidth() const
{
   if( count_m < 2u )
   {
      return std::numeric_limits<real64>::infinity();
   }

   // standard error of the mean, times the 95% normal quantile 1.96
   const real64 n = static_cast<real64>(count_m);
   return 1.96 * ::sqrt( m2_m / ((n - 1.0) * n) );
}


bool PilotStatistic::shouldStop
(
   const AdaptiveConfig& config
) const
{
   const int32 n = static_cast<int32>(count_m);

   // the hard upper bound is mandatory
   if( n >= config.getMaxSamples() )
   {
      return true;
   }

   // the criterion is only consulted after the minimum effort
   if( n < config.getMinSamples() )
   {
      return false;
   }

   const real64 halfWidth = getConfidenceHalfWidth();
   const real64 threshold = config.getAbsError() +
      (config.getRelError() * ::fabs( mean_m ));

   return halfWidth <= threshold;
}




/// AdaptiveState --------------------------------------------------------------

AdaptiveState::AdaptiveState
(
   const int32          width,
   const int32          height,
   const int32u         seed,
   const AdaptiveConfig& config
)
 : width_m ( width )
 , height_m( height )
 , seed_m  ( seed )
 , config_m( config )
 , frameCount_m( 0 )
 , boundCamera_m( 0 )
 , boundScene_m ( 0 )
 , boundWidth_m ( 0 )
 , boundHeight_m( 0 )
{
   if( (width < 1) || (height < 1) )
   {
      throw std::invalid_argument(
         "AdaptiveState: width and height must be positive" );
   }

   const std::size_t pixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
   statistics_m.resize( pixelCount );
   productionCounts_m.resize( pixelCount, 0u );
   sampleSerials_m.resize( pixelCount, 0u );
   active_m.assign( pixelCount, 1u );
}


void AdaptiveState::reset()
{
   for( std::size_t i = 0;  i < statistics_m.size();  ++i )
   {
      statistics_m[i].reset();
      productionCounts_m[i] = 0u;
      sampleSerials_m[i]    = 0u;
      active_m[i]           = 1u;
   }
   frameCount_m = 0u;

   // statistics are cleared, so any previous Camera/Scene/size binding ends
   clearBinding();
}


void AdaptiveState::bind
(
   const Camera& camera,
   const Scene&  scene,
   const int32   imageWidth,
   const int32   imageHeight
)
{
   if( boundCamera_m &&
      ((boundCamera_m != &camera) || (boundScene_m != &scene) ||
      (boundWidth_m != imageWidth) || (boundHeight_m != imageHeight)) )
   {
      throw std::logic_error(
         "AdaptiveState::bind: re-binding to a different Camera, Scene or "
         "size requires reset() first" );
   }

   boundCamera_m = &camera;
   boundScene_m  = &scene;
   boundWidth_m  = imageWidth;
   boundHeight_m = imageHeight;
}


void AdaptiveState::clearBinding()
{
   boundCamera_m = 0;
   boundScene_m  = 0;
   boundWidth_m  = 0;
   boundHeight_m = 0;
}


int32 AdaptiveState::getWidth() const
{
   return width_m;
}


int32 AdaptiveState::getHeight() const
{
   return height_m;
}


int32u AdaptiveState::getSeed() const
{
   return seed_m;
}


const AdaptiveConfig& AdaptiveState::getConfig() const
{
   return config_m;
}


bool AdaptiveState::isBound() const
{
   return boundCamera_m != 0;
}


const Camera* AdaptiveState::getBoundCamera() const
{
   return boundCamera_m;
}


const Scene* AdaptiveState::getBoundScene() const
{
   return boundScene_m;
}


const PilotStatistic& AdaptiveState::getStatistic
(
   const int32 x,
   const int32 y
) const
{
   return statistics_m[ indexOf( width_m, x, y ) ];
}


int32u AdaptiveState::getProductionCount
(
   const int32 x,
   const int32 y
) const
{
   return productionCounts_m[ indexOf( width_m, x, y ) ];
}


bool AdaptiveState::isActive
(
   const int32 x,
   const int32 y
) const
{
   return active_m[ indexOf( width_m, x, y ) ] != 0u;
}


int32u AdaptiveState::getActiveCount() const
{
   int32u count = 0u;
   for( std::size_t i = 0;  i < active_m.size();  ++i )
   {
      count += static_cast<int32u>(active_m[i] != 0u);
   }
   return count;
}


int32u AdaptiveState::getFrameCount() const
{
   return frameCount_m;
}


PilotStatistic& AdaptiveState::statisticAt
(
   const std::size_t index
)
{
   return statistics_m[index];
}


int32u AdaptiveState::sampleSerialAt
(
   const std::size_t index
) const
{
   return sampleSerials_m[index];
}


void AdaptiveState::setSampleSerialAt
(
   const std::size_t index,
   const int32u      n
)
{
   sampleSerials_m[index] = n;
}


int32u AdaptiveState::productionCountAt
(
   const std::size_t index
) const
{
   return productionCounts_m[index];
}


void AdaptiveState::incrementProductionCount
(
   const std::size_t index
)
{
   ++productionCounts_m[index];
}


bool AdaptiveState::activeAt
(
   const std::size_t index
) const
{
   return active_m[index] != 0u;
}


void AdaptiveState::setActiveAt
(
   const std::size_t index,
   const bool        active
)
{
   active_m[index] = active ? 1u : 0u;
}


void AdaptiveState::setFrameCount
(
   const int32u frameCount
)
{
   frameCount_m = frameCount;
}

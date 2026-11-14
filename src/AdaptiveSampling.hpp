/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Adaptive sampling extension.

   Caller-held adaptive sampling state: a pilot path is traced for every still
   active pixel, its linear-RGB luminance is tracked with Welford's online
   algorithm, and pixels are retired once a sampling-based stopping criterion
   is met. Only production paths are accumulated to the image; pilot samples
   drive scheduling alone and are never presented as an error guarantee.

------------------------------------------------------------------------------*/


#ifndef AdaptiveSampling_h
#define AdaptiveSampling_h


#include <cstddef>
#include <iosfwd>
#include <vector>

#include "Vector3f.hpp"
#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/// stream identity tags, mixed into the Random replay identity ---------------
enum AdaptiveStream
{
   PILOT_STREAM      = 0,
   PRODUCTION_STREAM = 1
};


/** Linear RGB luminance used for pilot statistics: Y = 0.2126R+0.7152G+0.0722B. */
real64 linearLuminance( const Vector3f& linearRgb );


/**
 * Adaptive sampling configuration.
 *
 * @invariants
 * * minSamples_m >= 2
 * * maxSamples_m >= minSamples_m
 * * absError_m and relError_m are finite and >= 0
 */
class AdaptiveConfig
{
/// standard object services ---------------------------------------------------
public:
   AdaptiveConfig( int32  minSamples,
                   int32  maxSamples,
                   real64 absError,
                   real64 relError );

// use defaults
//         ~AdaptiveConfig();
//          AdaptiveConfig( const AdaptiveConfig& );
// AdaptiveConfig& operator=( const AdaptiveConfig& );


/// queries --------------------------------------------------------------------
           int32  getMinSamples()                                          const;
           int32  getMaxSamples()                                          const;
           real64 getAbsError()                                            const;
           real64 getRelError()                                            const;


/// fields ---------------------------------------------------------------------
private:
   int32  minSamples_m;
   int32  maxSamples_m;
   real64 absError_m;
   real64 relError_m;
};


/**
 * Welford online statistics for one pixel's pilot luminance, usable on its
 * own, independently of any AdaptiveState.
 *
 * Tracks n, the running mean and M2 (sum of squared deviations) for samples
 * Y = 0.2126 R + 0.7152 G + 0.0722 B of linear RGB radiance.
 */
class PilotStatistic
{
/// standard object services ---------------------------------------------------
public:
   PilotStatistic();


/// commands -------------------------------------------------------------------
           /** Add one pilot luminance sample. */
           void   addSample( real64 luminance );

           void   reset();


/// queries --------------------------------------------------------------------
           int32u getCount()                                               const;
           real64 getMean()                                                const;
           real64 getM2()                                                  const;

           /** Sample variance M2 / (n - 1); zero when fewer than 2 samples. */
           real64 getVariance()                                            const;

           /**
            * Half-width of the 95% normal confidence interval for the mean:
            * 1.96 * sqrt( M2 / ((n - 1) n) ).
            */
           real64 getConfidenceHalfWidth()                                 const;

           /**
            * Scheduling criterion (not an actual error guarantee): the pixel
            * may stop once n >= minSamples and
            * 1.96*sqrt(M2/((n-1)n)) <= absError + relError*|mean|, or once
            * n >= maxSamples, when stopping is mandatory.
            */
           bool   shouldStop( const AdaptiveConfig& config )               const;


/// fields ---------------------------------------------------------------------
private:
   int32u count_m;
   real64 mean_m;
   real64 m2_m;
};


/**
 * Caller-held state for one adaptive render over one fixed-size film.
 *
 * Bound, via reset(), to one Camera/Scene/size triple. Holds the base seed,
 * the configuration, the per-pixel pilot statistics, the per-pixel production
 * sample counts, the per-pixel active flags and the per-pixel next sample
 * serial. Intermediate queries never mutate it; continuing to call
 * Camera::getAdaptiveFrame resumes with the existing serial numbers.
 *
 * @invariants
 * * width_m * height_m == statistics_m.size() when active
 */
class AdaptiveState
{
/// standard object services ---------------------------------------------------
public:
   AdaptiveState( int32               width,
                  int32               height,
                  int32u              seed,
                  const AdaptiveConfig& config );

// use defaults
//         ~AdaptiveState();
//          AdaptiveState( const AdaptiveState& );
// AdaptiveState& operator=( const AdaptiveState& );


/// commands -------------------------------------------------------------------
           /** Clear all statistics, counts, flags and serial numbers. */
           void   reset();

           /**
            * Bind the state to the Camera/Scene/size triple it will render.
            * Re-binding to a different camera, scene or size requires reset()
            * first.
            */
           void   bind( const Camera& camera,
                        const Scene&  scene,
                        int32         imageWidth,
                        int32         imageHeight );

           /** Release the current Camera/Scene/size binding. */
           void   clearBinding();


/// queries --------------------------------------------------------------------
           int32  getWidth()                                                const;
           int32  getHeight()                                               const;
           int32u getSeed()                                                 const;
           const AdaptiveConfig& getConfig()                                const;

           bool   isBound()                                                 const;
           const Camera* getBoundCamera()                                   const;
           const Scene*  getBoundScene()                                    const;

           /** Per-pixel pilot statistic (read-only). */
           const PilotStatistic& getStatistic( int32 x, int32 y )          const;

           /** Number of production samples already accumulated for a pixel. */
           int32u getProductionCount( int32 x, int32 y )                   const;

           /** Whether a pixel still receives pilot/production samples. */
           bool   isActive( int32 x, int32 y )                             const;

           /** Number of pixels still active for the next frame. */
           int32u getActiveCount()                                         const;

           /** Highest sample serial reached by any pixel (0 before any run). */
           int32u getFrameCount()                                          const;


/// implementation -- Camera::getAdaptiveFrame drives the sampling state ------
private:
   friend class Camera;

           PilotStatistic& statisticAt( std::size_t index );
           int32u          sampleSerialAt( std::size_t index )             const;
           void           setSampleSerialAt( std::size_t index, int32u n );
           int32u         productionCountAt( std::size_t index )           const;
           void           incrementProductionCount( std::size_t index );
           bool           activeAt( std::size_t index )                    const;
           void           setActiveAt( std::size_t index, bool active );
           void           setFrameCount( int32u frameCount );


/// fields ---------------------------------------------------------------------
private:
   int32                       width_m;
   int32                       height_m;
   int32u                      seed_m;
   AdaptiveConfig              config_m;

   std::vector<PilotStatistic> statistics_m;
   std::vector<int32u>         productionCounts_m;
   std::vector<int32u>         sampleSerials_m;
   std::vector<unsigned char>  active_m;
   int32u                      frameCount_m;

   const Camera*               boundCamera_m;
   const Scene*                boundScene_m;
   int32                       boundWidth_m;
   int32                       boundHeight_m;
};


}//namespace




#endif//AdaptiveSampling_h

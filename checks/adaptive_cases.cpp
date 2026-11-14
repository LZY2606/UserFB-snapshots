/*------------------------------------------------------------------------------

   Adaptive sampling regression cases.

   Registered through task_support/check.h in four groups:
     statistics - Welford updates, luminance and config validation
     scheduling - stopping boundaries and a real constant/noisy render
     streams    - identity RNG separation and single-path replay
     film       - adaptive accumulation, means, PPM output, reset/continue

   No main(): task_support/driver.c drives the registered cases.

------------------------------------------------------------------------------*/


#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>

// driver.c is translated by the C++ compiler in task_support/build.sh, so its
// symbols share C++ linkage with this file.
#include "task_support/check.h"

#include "task_support/fixture.hpp"
#include "AdaptiveSampling.hpp"


using namespace hxa7241_minilight;


namespace
{
   using hxa7241_general::Random;


   real64 absDiff( const real64 a, const real64 b )
   {
      return ::fabs( a - b );
   }


   bool near( const real64 a, const real64 b, const real64 tol = 1e-12 )
   {
      return absDiff( a, b ) <= tol;
   }


   bool nearVec( const Vector3f& a, const Vector3f& b,
      const real64 tol = 1e-12 )
   {
      return near( a[0], b[0], tol ) && near( a[1], b[1], tol ) &&
         near( a[2], b[2], tol );
   }


   bool throwsInvalid( const std::function<void()>& f )
   {
      try { f(); }
      catch( const std::invalid_argument& ) { return true; }
      catch( ... ) { return false; }
      return false;
   }


   bool throwsLogic( const std::function<void()>& f )
   {
      try { f(); }
      catch( const std::logic_error& ) { return true; }
      catch( ... ) { return false; }
      return false;
   }


   Camera makeCamera()
   {
      std::istringstream in( author_fixture::camera_text() );
      author_fixture::input( in );
      return Camera( in );
   }


   Scene makeScene( const char* text, const Camera& camera )
   {
      std::istringstream in( text );
      author_fixture::input( in );
      return Scene( in, camera.getEyePoint() );
   }


   uint64_t fnv1a( const std::string& s )
   {
      uint64_t h = 14695981039346656037ull;
      for( unsigned char c : s ) { h ^= c; h *= 1099511628211ull; }
      return h;
   }


   /// statistics --------------------------------------------------------------

   int statisticWelford()
   {
      PilotStatistic stat;
      const real64 samples[] = { 1.0, 2.0, 3.0, 4.0 };
      for( real64 v : samples ) stat.addSample( v );

      // independently computed: mean 2.5, M2 = sum of squared deviations = 5
      TASK_CHECK( stat.getCount() == 4u );
      TASK_CHECK( near( stat.getMean(), 2.5 ) );
      TASK_CHECK( near( stat.getM2(), 5.0 ) );
      TASK_CHECK( near( stat.getVariance(), 5.0 / 3.0 ) );

      // 1.96 * sqrt(M2 / ((n-1) n)) = 1.96 * sqrt(5/12)
      const real64 expected = 1.96 * ::sqrt( 5.0 / 12.0 );
      TASK_CHECK( near( stat.getConfidenceHalfWidth(), expected, 1e-12 ) );

      // the standalone statistic also resets cleanly
      stat.reset();
      TASK_CHECK( stat.getCount() == 0u && stat.getMean() == 0.0 &&
         stat.getM2() == 0.0 );
      TASK_CHECK( std::isinf( stat.getConfidenceHalfWidth() ) );
      return 0;
   }


   int statisticLuminance()
   {
      const Vector3f rgb( 0.1, 0.2, 0.3 );
      const real64 y = linearLuminance( rgb );
      TASK_CHECK( near( y, 0.2126 * 0.1 + 0.7152 * 0.2 + 0.0722 * 0.3 ) );

      // a fresh state exposes zeroed, publicly readable per-pixel statistics
      AdaptiveState state( 2, 2, 11u, AdaptiveConfig( 2, 4, 0.1, 0.1 ) );
      const PilotStatistic& pixel = state.getStatistic( 1, 0 );
      TASK_CHECK( pixel.getCount() == 0u && pixel.getMean() == 0.0 );
      TASK_CHECK( state.getProductionCount( 1, 0 ) == 0u );
      TASK_CHECK( state.isActive( 1, 0 ) );
      return 0;
   }


   int statisticConfigValidation()
   {
      TASK_CHECK( throwsInvalid( []{ AdaptiveConfig( 1, 4, 0.1, 0.1 ); } ) );
      TASK_CHECK( throwsInvalid( []{ AdaptiveConfig( 3, 2, 0.1, 0.1 ); } ) );
      TASK_CHECK( throwsInvalid( []{ AdaptiveConfig( 2, 4, -0.1, 0.1 ); } ) );
      TASK_CHECK( throwsInvalid( []{ AdaptiveConfig( 2, 4, 0.1,
         std::numeric_limits<real64>::infinity() ); } ) );
      TASK_CHECK( throwsInvalid( []{ AdaptiveConfig( 0, 0, 0.0, 0.0 ); } ) );

      // a valid configuration is stored and read back
      const AdaptiveConfig config( 2, 9, 0.25, 0.5 );
      TASK_CHECK( config.getMinSamples() == 2 && config.getMaxSamples() == 9 );
      TASK_CHECK( near( config.getAbsError(), 0.25 ) &&
         near( config.getRelError(), 0.5 ) );
      return 0;
   }


   /// scheduling --------------------------------------------------------------

   int scheduleConstantStopsAtMinimum()
   {
      const AdaptiveConfig config( 2, 8, 0.01, 0.01 );
      PilotStatistic stat;

      stat.addSample( 5.0 );
      TASK_CHECK( !stat.shouldStop( config ) );   // below minSamples
      stat.addSample( 5.0 );
      TASK_CHECK( stat.getM2() == 0.0 );
      TASK_CHECK( stat.shouldStop( config ) );    // zero variance, at minimum
      return 0;
   }


   int scheduleForcesMaximum()
   {
      const AdaptiveConfig config( 2, 4, 1e-12, 1e-12 );
      PilotStatistic stat;

      const real64 samples[] = { 0.0, 10.0, 0.0, 10.0 };
      for( int i = 0; i < 3; ++i )
      {
         stat.addSample( samples[i] );
         TASK_CHECK( !stat.shouldStop( config ) );
      }
      stat.addSample( samples[3] );
      TASK_CHECK( stat.shouldStop( config ) );    // maxSamples is mandatory
      TASK_CHECK( stat.getCount() == 4u );
      return 0;
   }


   int scheduleExactBoundary()
   {
      // two samples 0 and 10: mean 5, M2 50 -> half width 1.96*sqrt(25) = 9.8
      PilotStatistic stat;
      stat.addSample( 0.0 );
      stat.addSample( 10.0 );
      TASK_CHECK( near( stat.getMean(), 5.0 ) && near( stat.getM2(), 50.0 ) );
      TASK_CHECK( near( stat.getConfidenceHalfWidth(), 9.8 ) );

      const AdaptiveConfig atBoundary( 2, 100, 9.8, 0.0 );
      const AdaptiveConfig belowBoundary( 2, 100, 9.799, 0.0 );
      TASK_CHECK( stat.shouldStop( atBoundary ) );
      TASK_CHECK( !stat.shouldStop( belowBoundary ) );

      // relative term widens the threshold by relError * |mean|
      const AdaptiveConfig relative( 2, 100, 4.0, 1.2 );
      TASK_CHECK( near( 4.0 + 1.2 * 5.0, 10.0 ) );   // strictly above 9.8
      TASK_CHECK( stat.shouldStop( relative ) );
      return 0;
   }


   // scene with a constant sky background and a small randomly reflecting
   // triangle, so both flat and high-variance pixels are present
   const char* mixedSceneText()
   {
      return
         "(1 1 1) (0 0 0)\n"
         "(-0.5 -0.5 3) (0 0.5 3) (0.5 -0.5 3) (0.5 0.4 0.3) (0 0 0)\n";
   }


   int scheduleRealRender()
   {
      Camera camera = makeCamera();
      Scene scene = makeScene( mixedSceneText(), camera );
      Image image = Image::createAdaptive( 6, 5 );

      // tight thresholds: constant background retires at the minimum, any
      // pixel whose paths vary is forced all the way to maxSamples
      AdaptiveState state( 6, 5, 424242u, AdaptiveConfig( 2, 8, 1e-9, 1e-9 ) );

      for( int frame = 0; frame < 8; ++frame )
      {
         camera.getAdaptiveFrame( scene, state, image );
      }

      TASK_CHECK( state.getActiveCount() == 0u );
      TASK_CHECK( state.getFrameCount() == 8u );

      int minSeen = 0, maxSeen = 0;
      for( int y = 0; y < 5; ++y )
      {
         for( int x = 0; x < 6; ++x )
         {
            const int32u n = state.getProductionCount( x, y );
            TASK_CHECK( n >= 2u && n <= 8u );
            if( n == 2u ) ++minSeen;
            if( n == 8u ) ++maxSeen;
         }
      }
      // flat background pixels stopped early, noisy reflection pixels did not
      TASK_CHECK( minSeen > 0 && maxSeen > 0 );

      // once retired, further frames change nothing
      camera.getAdaptiveFrame( scene, state, image );
      TASK_CHECK( state.getFrameCount() == 9u && state.getActiveCount() == 0u );
      for( int y = 0; y < 5; ++y )
         for( int x = 0; x < 6; ++x )
            TASK_CHECK( state.getProductionCount( x, y ) <= 8u );
      return 0;
   }


   /// streams -----------------------------------------------------------------

   int streamIdentityReplay()
   {
      // same identity -> identical sequence; any changed tuple -> different
      Random a = Random::makeIdentified( 7u, 2u, 3u, 5u, PILOT_STREAM );
      Random b = Random::makeIdentified( 7u, 2u, 3u, 5u, PILOT_STREAM );
      Random production = Random::makeIdentified( 7u, 2u, 3u, 5u,
         PRODUCTION_STREAM );
      Random otherPixel = Random::makeIdentified( 7u, 1u, 3u, 5u,
         PILOT_STREAM );

      bool same = true, kindDiffers = false, pixelDiffers = false;
      for( int i = 0; i < 16; ++i )
      {
         const int32u va = a.getInt32u();
         if( va != b.getInt32u() ) same = false;
         if( va != production.getInt32u() ) kindDiffers = true;
         if( va != otherPixel.getInt32u() ) pixelDiffers = true;
      }
      TASK_CHECK( same && kindDiffers && pixelDiffers );

      // the default-constructed sequence is unchanged
      Random legacy;
      const int32u expected[4] =
         { 3952563604u, 1192989748u, 2423800670u, 1230242343u };
      for( int i = 0; i < 4; ++i )
         TASK_CHECK( legacy.getInt32u() == expected[i] );
      return 0;
   }


   int streamPathReplayAndSeparation()
   {
      Camera camera = makeCamera();
      Scene scene = makeScene( mixedSceneText(), camera );
      Image image = Image::createAdaptive( 6, 5 );
      AdaptiveState state( 6, 5, 99u, AdaptiveConfig( 2, 8, 1e-9, 1e-9 ) );

      const int px = 3, py = 2;

      // expected pilot and production radiances for serial 0, replayed with
      // the public single-path entry before any frame exists
      const Vector3f pilot0 = camera.replayPath( scene, 6, 5, 99u, px, py, 0u,
         PILOT_STREAM );
      const Vector3f production0 = camera.replayPath( scene, 6, 5, 99u, px, py,
         0u, PRODUCTION_STREAM );

      // independent pinhole ray formula for the known identity frame
      // (eye origin, view +z, 45 degrees -> right +x, up +y)
      const Random pilotRng = Random::makeIdentified( 99u,
         static_cast<int32u>(px), static_cast<int32u>(py), 0u, PILOT_STREAM );
      const real64 jx = pilotRng.getReal64();
      const real64 jy = pilotRng.getReal64();
      const real64 xF = ((px + jx) * 2.0 / 6.0) - 1.0;
      const real64 yF = ((py + jy) * 2.0 / 5.0) - 1.0;
      const Vector3f direction = (Vector3f( 0.0, 0.0, 1.0 ) +
         (Vector3f( xF, yF * (5.0 / 6.0), 0.0 ) *
         ::tan( 45.0 * 3.14159265358979 / 180.0 * 0.5 ))).unitize();
      RayTracer tracer( scene );
      const Vector3f independent = tracer.getRadiance(
         camera.getEyePoint(), direction, pilotRng );
      TASK_CHECK( nearVec( independent, pilot0 ) );

      // pilot and production streams are distinct even for the same serial
      Random productionRng = Random::makeIdentified( 99u,
         static_cast<int32u>(px), static_cast<int32u>(py), 0u,
         PRODUCTION_STREAM );
      TASK_CHECK( pilotRng.getReal64() != productionRng.getReal64() );

      // one frame: the stored pilot statistic and production mean must match
      // the replayed paths exactly -- pilot values never reach the film and
      // production values never backfill the pilot statistic
      camera.getAdaptiveFrame( scene, state, image );
      const PilotStatistic& stat = state.getStatistic( px, py );
      TASK_CHECK( stat.getCount() == 1u );
      TASK_CHECK( near( stat.getMean(), linearLuminance( pilot0 ) ) );
      TASK_CHECK( nearVec( image.getPixelMean( px, py ), production0 ) );
      TASK_CHECK( image.getPixelSampleCount( px, py ) == 1u );

      // replay is order-independent: a full frame's path lengths cannot shift
      // another pixel's identity stream
      const Vector3f pilot0Again = camera.replayPath( scene, 6, 5, 99u, px, py,
         0u, PILOT_STREAM );
      TASK_CHECK( nearVec( pilot0Again, pilot0 ) );

      // the emitter fixture gives an exact, transport-independent radiance
      Scene emitter = makeScene( author_fixture::emitter_text(), camera );
      const Vector3f emitted = camera.replayPath( emitter, 6, 5, 99u, px, py,
         0u, PILOT_STREAM );
      TASK_CHECK( nearVec( emitted, Vector3f( 2.0, 3.0, 4.0 ) ) );
      return 0;
   }


   /// film --------------------------------------------------------------------

   int filmMeansAndCounts()
   {
      Image image = Image::createAdaptive( 2, 2 );
      TASK_CHECK( image.isAdaptive() );

      // unsampled pixels read as exactly zero
      TASK_CHECK( image.getPixelSampleCount( 0, 0 ) == 0u );
      TASK_CHECK( nearVec( image.getPixelMean( 0, 0 ), Vector3f::ZERO() ) );

      const Vector3f v1( 0.2, 0.4, 0.8 );
      const Vector3f v2( 0.4, 0.6, 1.0 );
      image.addAdaptiveSample( 0, 0, v1 );
      image.addAdaptiveSample( 0, 0, v2 );
      TASK_CHECK( image.getPixelSampleCount( 0, 0 ) == 2u );
      TASK_CHECK( nearVec( image.getPixelMean( 0, 0 ),
         Vector3f( 0.3, 0.5, 0.9 ) ) );

      // the two accumulation schemes must never be mixed on one image
      TASK_CHECK( throwsLogic( [&]{ image.addToPixel( 0, 0, v1 ); } ) );
      std::istringstream legacyIn( "2 2 " );
      Image legacy( legacyIn );
      TASK_CHECK( !legacy.isAdaptive() );
      TASK_CHECK( throwsLogic( [&]{ legacy.addAdaptiveSample( 0, 0, v1 ); } ) );
      std::ostringstream legacyFormatOut;
      TASK_CHECK( throwsLogic( [&]{ image.getFormatted(
         legacyFormatOut, 1 ); } ) );
      std::ostringstream adaptiveFormatOut;
      TASK_CHECK( throwsLogic( [&]{ legacy.getAdaptiveFormatted(
         adaptiveFormatOut ); } ) );
      return 0;
   }


   int filmAdaptivePpm()
   {
      // two images with identical per-pixel means but unequal production
      // counts (one vs three samples per pixel): since formatting is driven by
      // means rather than a uniform iteration count, the PPMs must be equal
      const Vector3f v0( 0.6, 0.3, 0.15 );
      const Vector3f v1( 0.05, 0.05, 0.05 );

      Image one = Image::createAdaptive( 2, 1 );
      Image three = Image::createAdaptive( 2, 1 );
      one.addAdaptiveSample( 0, 0, v0 );
      one.addAdaptiveSample( 1, 0, v1 );
      for( int i = 0; i < 3; ++i )
      {
         three.addAdaptiveSample( 0, 0, v0 );
         three.addAdaptiveSample( 1, 0, v1 );
      }
      TASK_CHECK( one.getPixelSampleCount( 0, 0 ) == 1u );
      TASK_CHECK( three.getPixelSampleCount( 0, 0 ) == 3u );

      std::ostringstream outOne, outThree;
      one.getAdaptiveFormatted( outOne );
      three.getAdaptiveFormatted( outThree );
      const std::string ppmOne = outOne.str();
      const std::string ppmThree = outThree.str();

      TASK_CHECK( ppmOne.rfind( "P6", 0 ) == 0u );
      const std::string expectedHeaderEnd = "2 1\n255\n";
      TASK_CHECK( ppmOne.find( expectedHeaderEnd ) != std::string::npos );
      const std::size_t headerLen =
         ppmOne.find( expectedHeaderEnd ) + expectedHeaderEnd.size();
      TASK_CHECK( ppmOne.size() == headerLen + 2u * 1u * 3u );
      TASK_CHECK( ppmOne == ppmThree );

      // a changed mean changes the encoded bytes (no count-based cancellation)
      Image changed = Image::createAdaptive( 2, 1 );
      changed.addAdaptiveSample( 0, 0, v0 );
      changed.addAdaptiveSample( 1, 0, Vector3f( 0.9, 0.9, 0.9 ) );
      std::ostringstream outChanged;
      changed.getAdaptiveFormatted( outChanged );
      const std::string ppmChanged = outChanged.str();
      TASK_CHECK( ppmChanged.compare( headerLen + 3, 3, ppmOne,
         headerLen + 3, 3 ) != 0 );

      // save a small image for inspection
      Image small = Image::createAdaptive( 4, 3 );
      for( int y = 0; y < 3; ++y )
         for( int x = 0; x < 4; ++x )
            small.addAdaptiveSample( x, y,
               Vector3f( 0.1 * x, 0.2 * y, 0.05 * (x + y) ) );
      std::ofstream file( ".task-build/adaptive.ppm", std::ios::binary );
      TASK_CHECK( file.good() );
      small.getAdaptiveFormatted( file );
      file.close();
      TASK_CHECK( file.good() );
      return 0;
   }


   int filmContinueSnapshotReset()
   {
      Camera camera = makeCamera();
      Scene scene = makeScene( mixedSceneText(), camera );
      Image image = Image::createAdaptive( 4, 3 );
      AdaptiveState state( 4, 3, 7u, AdaptiveConfig( 2, 6, 1e-9, 1e-9 ) );

      camera.getAdaptiveFrame( scene, state, image );
      TASK_CHECK( state.getFrameCount() == 1u );

      // snapshotting leaves the sampling state completely untouched
      const int32u activeBefore = state.getActiveCount();
      std::ostringstream snapshot;
      image.getAdaptiveFormatted( snapshot );
      TASK_CHECK( state.getFrameCount() == 1u );
      TASK_CHECK( state.getActiveCount() == activeBefore );
      TASK_CHECK( snapshot.str().rfind( "P6", 0 ) == 0u );

      // continuing resumes with the existing per-pixel serial numbers
      int32u totalCounts = 0;
      for( int y = 0; y < 3; ++y )
         for( int x = 0; x < 4; ++x )
         {
            TASK_CHECK( state.getProductionCount( x, y ) == 1u );
            totalCounts += image.getPixelSampleCount( x, y );
         }
      TASK_CHECK( totalCounts == 12u );
      camera.getAdaptiveFrame( scene, state, image );
      TASK_CHECK( state.getFrameCount() == 2u );
      for( int y = 0; y < 3; ++y )
         for( int x = 0; x < 4; ++x )
            TASK_CHECK( state.getProductionCount( x, y ) >= 1u );

      // size and binding rules
      Image wrong = Image::createAdaptive( 5, 3 );
      TASK_CHECK( throwsLogic( [&]{ camera.getAdaptiveFrame(
         scene, state, wrong ); } ) );
      TASK_CHECK( state.isBound() );
      Camera other = makeCamera();
      TASK_CHECK( throwsLogic( [&]{ state.bind( other, scene, 4, 3 ); } ) );

      // reset clears statistics and the adaptive film together, and ends
      // the binding so a different Camera/Scene/size can be bound afterwards
      camera.resetAdaptive( state, image );
      TASK_CHECK( state.getFrameCount() == 0u && !state.isBound() );
      for( int y = 0; y < 3; ++y )
         for( int x = 0; x < 4; ++x )
         {
            TASK_CHECK( state.getProductionCount( x, y ) == 0u );
            TASK_CHECK( state.isActive( x, y ) );
            TASK_CHECK( nearVec( image.getPixelMean( x, y ),
               Vector3f::ZERO() ) );
         }
      state.bind( other, scene, 4, 3 );
      TASK_CHECK( state.isBound() );

      // legacy getFrame/getFormatted behaviour is unchanged: the exact FNV
      // hash of two frames over the diffuse fixture is still reproduced
      std::istringstream diffuseIn( author_fixture::diffuse_text() );
      author_fixture::input( diffuseIn );
      Scene diffuse( diffuseIn, camera.getEyePoint() );
      std::istringstream imageIn( "4 3 " );
      Image legacy( imageIn );
      Random frames;
      camera.getFrame( diffuse, frames, legacy );
      camera.getFrame( diffuse, frames, legacy );
      std::ostringstream legacyOut;
      legacy.getFormatted( legacyOut, 2 );
      TASK_CHECK( fnv1a( legacyOut.str() ) == 0x775b0c358af0f98cull );
      return 0;
   }
}


const struct task_case task_cases[] =
{
   { "statistics", "welford",            &statisticWelford },
   { "statistics", "luminance",          &statisticLuminance },
   { "statistics", "config_validation",  &statisticConfigValidation },

   { "scheduling", "constant_minimum",   &scheduleConstantStopsAtMinimum },
   { "scheduling", "forced_maximum",     &scheduleForcesMaximum },
   { "scheduling", "exact_boundary",     &scheduleExactBoundary },
   { "scheduling", "real_render",        &scheduleRealRender },

   { "streams", "identity_replay",       &streamIdentityReplay },
   { "streams", "path_replay_separation",&streamPathReplayAndSeparation },

   { "film", "means_and_counts",         &filmMeansAndCounts },
   { "film", "adaptive_ppm",             &filmAdaptivePpm },
   { "film", "continue_snapshot_reset",  &filmContinueSnapshotReset },
};

const size_t task_case_count =
   sizeof( task_cases ) / sizeof( task_cases[0] );

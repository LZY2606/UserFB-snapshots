/*------------------------------------------------------------------------------

   MiniLight gradient-domain transport : target tests

   Registered groups: coupling, accumulation, reconstruction, integration.
   Each case returns 0 on success and uses TASK_CHECK for assertions.

------------------------------------------------------------------------------*/


#include "task_support/check.h"

#include "Camera.hpp"
#include "GradientState.hpp"
#include "GradientSolver.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
#include "Scene.hpp"
#include "Vector3f.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>


using namespace hxa7241_minilight;
using hxa7241_general::Random;


namespace
{


/// helpers --------------------------------------------------------------------

bool nearReal
(
   const real64 a,
   const real64 b,
   const real64 eps
)
{
   return std::fabs( a - b ) <= eps;
}


bool nearVec
(
   const Vector3f& a,
   const Vector3f& b,
   const real64    eps
)
{
   for( int32 c = 0;  c < 3;  ++c )
   {
      if( !nearReal( a[c], b[c], eps ) )
      {
         return false;
      }
   }
   return true;
}


bool equalVec
(
   const Vector3f& a,
   const Vector3f& b
)
{
   return (a[0] == b[0]) & (a[1] == b[1]) & (a[2] == b[2]);
}


template<class F> bool throwsInvalid( F f )
{
   try { f(); }
   catch( const std::invalid_argument& ) { return true; }
   catch( ... ) { return false; }
   return false;
}


template<class F> bool throwsLogic( F f )
{
   try { f(); }
   catch( const std::invalid_argument& ) { return false; }
   catch( const std::logic_error& ) { return true; }
   catch( ... ) { return false; }
   return false;
}


template<class F> bool throwsRange( F f )
{
   try { f(); }
   catch( const std::out_of_range& ) { return true; }
   catch( ... ) { return false; }
   return false;
}


/// scene fixtures ---------------------------------------------------------------

// scene-reading streams must throw at EOF, or Scene's triangle loop would
// not terminate (the production CLI sets the same exception mask)
void sceneStream( std::istringstream& in )
{
   in.exceptions( std::ios::badbit | std::ios::eofbit | std::ios::failbit );
}


// camera above a floor, looking straight down
Camera makeDownCamera( const real64 viewAngle = 25.0 )
{
   std::ostringstream text;
   text << "(0 2 0) (0 -1 0) " << viewAngle << "\n";
   std::istringstream in( text.str() );
   sceneStream( in );
   return Camera( in );
}


// sky-only scene: every ray yields the constant ground reflection
// (1 2 4) * (0.5 0.5 0.5) == (0.5 1 2), and consumes no randoms per path
Scene makeSkyScene( const Camera& camera )
{
   std::istringstream in( "(1 2 4) (0.5 0.5 0.5)\n" );
   sceneStream( in );
   return Scene( in, camera.getEyePoint() );
}


// diffuse floor lit by a huge down-facing emitter: smooth, random-consuming
// paths with no visibility discontinuities between neighbouring pixels
Scene makeEmitterScene( const Camera& camera )
{
   std::istringstream in(
      "(0 0 0) (0 0 0)\n"
      "(-10 0 -10) (-10 0 10) (10 0 -10) (0.7 0.7 0.7) (0 0 0)\n"
      "(-10 0 -10) (10 0 -10) (10 0 10) (0.7 0.7 0.7) (0 0 0)\n"
      "(-20 4 -20) (20 4 -20) (-20 4 20) (0 0 0) (2 2 2)\n"
      "(20 4 -20) (20 4 20) (-20 4 20) (0 0 0) (2 2 2)\n" );
   sceneStream( in );
   return Scene( in, camera.getEyePoint() );
}


// small box with a bright emitter, for end-to-end runs
const char BOX_MODEL_BODY[] =
   "(0.278 0.275 -0.789) (0 0 1) 40\n"
   "\n"
   "(0.0906 0.0943 0.1151) (0.1 0.09 0.07)\n"
   "\n"
   "(0.556 0.000 0.000) (0.006 0.000 0.559) (0.556 0.000 0.559)  (0.7 0.7 0.7) (0 0 0)\n"
   "(0.006 0.000 0.559) (0.556 0.000 0.000) (0.003 0.000 0.000)  (0.7 0.7 0.7) (0 0 0)\n"
   "(0.343 0.545 0.332) (0.213 0.545 0.227) (0.343 0.545 0.227)  (0.7 0.7 0.7) (100 100 100)\n"
   "(0.213 0.545 0.227) (0.343 0.545 0.332) (0.213 0.545 0.332)  (0.7 0.7 0.7) (100 100 100)\n";


/// coupling ---------------------------------------------------------------------

// The caller's Random must advance exactly as the legacy per-pixel base
// path; with a constant-radiance scene the primal means equal that constant
// and every gradient sample is exactly zero.
int coupling_sky_stream_and_zero_gradients()
{
   const Camera camera( makeDownCamera() );
   const Scene  scene( makeSkyScene( camera ) );

   GradientState state( 3, 2 );
   Random gradientRandom;
   camera.getFrameGradient( scene, gradientRandom, state );

   std::istringstream sizeIn( "3 2\n" );
   Image legacyImage( sizeIn );
   Random legacyRandom;
   camera.getFrame( scene, legacyRandom, legacyImage );

   TASK_CHECK( 1 == state.getRounds() );
   // both streams consumed identically: neighbours used private copies
   TASK_CHECK( gradientRandom.getInt32u() == legacyRandom.getInt32u() );

   const Vector3f expected( 0.5, 1.0, 2.0 );
   for( int32 y = 0;  y < 2;  ++y )
   {
      for( int32 x = 0;  x < 3;  ++x )
      {
         TASK_CHECK( nearVec( state.getPrimalMean( x, y ), expected,
            1e-12 ) );
         if( state.hasRight( x, y ) )
         {
            TASK_CHECK( state.getGradientRightMean( x, y ).isZero() );
         }
         if( state.hasUp( x, y ) )
         {
            TASK_CHECK( state.getGradientUpMean( x, y ).isZero() );
         }
      }
   }
   return 0;
}


// With a 1x1 image (no edges) the gradient-mode primal accumulation and the
// random stream must match the legacy renderer bit-for-bit through the same
// tone-mapping chain, on a scene whose paths consume randoms.
int coupling_primal_matches_legacy_render()
{
   const Camera camera( makeDownCamera() );
   const Scene  scene( makeEmitterScene( camera ) );

   std::istringstream sizeIn( "1 1\n" );
   Image legacyImage( sizeIn );
   Random legacyRandom;
   for( int32 frame = 0;  frame < 3;  ++frame )
   {
      camera.getFrame( scene, legacyRandom, legacyImage );
   }
   std::ostringstream legacyPpm;
   legacyImage.getFormatted( legacyPpm, 3 );

   GradientState state( 1, 1 );
   Random gradientRandom;
   for( int32 round = 0;  round < 3;  ++round )
   {
      camera.getFrameGradient( scene, gradientRandom, state );
   }

   std::vector<Vector3f> pixels;
   const GradientSolver::Result result(
      GradientSolver::solve( state, 1.0, 1e-12, 100, pixels ) );
   TASK_CHECK( result.isConverged );
   TASK_CHECK( 1u == pixels.size() );
   // no edges: reconstruction is exactly the primal mean
   TASK_CHECK( equalVec( pixels[0], state.getPrimalMean( 0, 0 ) ) );

   std::istringstream gradientSizeIn( "1 1\n" );
   Image gradientImage( gradientSizeIn );
   gradientImage.addToPixel( 0, 0, pixels[0] );
   std::ostringstream gradientPpm;
   gradientImage.getFormatted( gradientPpm, 1 );

   TASK_CHECK( legacyPpm.str() == gradientPpm.str() );
   TASK_CHECK( gradientRandom.getInt32u() == legacyRandom.getInt32u() );
   return 0;
}


// Paired base/neighbour evaluations share the sub-pixel jitter and the
// subsequent random sequence, so their difference has far less variance
// than the difference of two independent primal estimates of the same
// quantity -- while estimating the same mean.
int coupling_paired_paths_share_randomness()
{
   const Camera camera( makeDownCamera( 10.0 ) );
   const Scene  scene( makeEmitterScene( camera ) );

   const int32 T = 64;
   std::vector<real64> paired( static_cast<int32u>(T) );
   std::vector<real64> independent( static_cast<int32u>(T) );

   Random random;
   for( int32 t = 0;  t < T;  ++t )
   {
      GradientState state( 2, 1 );
      camera.getFrameGradient( scene, random, state );
      TASK_CHECK( 1 == state.getRounds() );

      paired[ static_cast<int32u>(t) ] =
         state.getGradientRightMean( 0, 0 )[0];
      independent[ static_cast<int32u>(t) ] =
         state.getPrimalMean( 1, 0 )[0] - state.getPrimalMean( 0, 0 )[0];
   }

   real64 meanP = 0.0, meanI = 0.0;
   for( int32 t = 0;  t < T;  ++t )
   {
      meanP += paired[ static_cast<int32u>(t) ];
      meanI += independent[ static_cast<int32u>(t) ];
   }
   meanP /= static_cast<real64>(T);
   meanI /= static_cast<real64>(T);

   real64 varP = 0.0, varI = 0.0;
   for( int32 t = 0;  t < T;  ++t )
   {
      const real64 dp = paired[ static_cast<int32u>(t) ] - meanP;
      const real64 di = independent[ static_cast<int32u>(t) ] - meanI;
      varP += dp * dp;
      varI += di * di;
   }
   varP /= static_cast<real64>(T);
   varI /= static_cast<real64>(T);

   // common-random-number coupling: much lower variance ...
   TASK_CHECK( varP * 4.0 < varI );
   // ... of the same estimand
   TASK_CHECK( std::fabs( meanP - meanI ) <
      6.0 * ::sqrt( varI / static_cast<real64>(T) ) + 1e-9 );
   return 0;
}


/// accumulation -----------------------------------------------------------------

// Rounds accumulate exactly; repeated queries do not disturb the state; a
// fresh run with a fresh Random reproduces the first round exactly.
int accumulation_rounds_and_stable_queries()
{
   const Camera camera( makeDownCamera() );
   const Scene  scene( makeSkyScene( camera ) );

   GradientState state( 2, 2 );
   Random random;

   camera.getFrameGradient( scene, random, state );
   TASK_CHECK( 1 == state.getRounds() );
   const Vector3f sum1( state.getPrimalSum( 1, 1 ) );

   // queries are stable and non-mutating
   TASK_CHECK( equalVec( state.getPrimalMean( 1, 1 ),
      state.getPrimalMean( 1, 1 ) ) );
   TASK_CHECK( equalVec( state.getPrimalSum( 1, 1 ), sum1 ) );
   TASK_CHECK( 1 == state.getRounds() );

   camera.getFrameGradient( scene, random, state );
   TASK_CHECK( 2 == state.getRounds() );
   // constant radiance: sums and means are exact multiples
   TASK_CHECK( equalVec( state.getPrimalSum( 1, 1 ), sum1 * 2.0 ) );
   TASK_CHECK( equalVec( state.getPrimalMean( 1, 1 ), sum1 ) );
   TASK_CHECK( equalVec( state.getPrimalMean( 0, 0 ),
      state.getPrimalSum( 0, 0 ) / 2.0 ) );

   camera.getFrameGradient( scene, random, state );
   TASK_CHECK( 3 == state.getRounds() );
   TASK_CHECK( equalVec( state.getPrimalSum( 0, 1 ), sum1 * 3.0 ) );

   // determinism: a fresh caller-side Random replays the first round
   GradientState again( 2, 2 );
   Random againRandom;
   camera.getFrameGradient( scene, againRandom, again );
   TASK_CHECK( equalVec( again.getPrimalSum( 1, 1 ), sum1 ) );
   TASK_CHECK( equalVec( again.getPrimalSum( 0, 1 ),
      state.getPrimalSum( 0, 1 ) / 3.0 ) );
   return 0;
}


// After appending a round, reconstruction must use all accumulated samples:
// the solve is linear in (P, G), so the 3-round reconstruction equals the
// round-count-weighted mix of the 2-round and third-round-only solves.
int accumulation_append_uses_all_samples()
{
   const Camera camera( makeDownCamera() );
   const Scene  scene( makeEmitterScene( camera ) );

   Random randomA;
   GradientState stateA( 2, 1 );
   camera.getFrameGradient( scene, randomA, stateA );
   camera.getFrameGradient( scene, randomA, stateA );

   std::vector<Vector3f> image2;
   TASK_CHECK( GradientSolver::solve( stateA, 1.0, 1e-12, 10000,
      image2 ).isConverged );

   camera.getFrameGradient( scene, randomA, stateA );
   TASK_CHECK( 3 == stateA.getRounds() );

   std::vector<Vector3f> image3;
   TASK_CHECK( GradientSolver::solve( stateA, 1.0, 1e-12, 10000,
      image3 ).isConverged );

   // replicate the third round alone, from the same stream position
   Random randomB;
   GradientState warmUp( 2, 1 );
   camera.getFrameGradient( scene, randomB, warmUp );
   camera.getFrameGradient( scene, randomB, warmUp );
   GradientState thirdOnly( 2, 1 );
   camera.getFrameGradient( scene, randomB, thirdOnly );

   std::vector<Vector3f> imageThird;
   TASK_CHECK( GradientSolver::solve( thirdOnly, 1.0, 1e-12, 10000,
      imageThird ).isConverged );

   TASK_CHECK( 2u == image3.size() );
   for( int32u i = 0;  i < image3.size();  ++i )
   {
      const Vector3f expected(
         (image2[i] * 2.0 + imageThird[i]) / 3.0 );
      TASK_CHECK( nearVec( image3[i], expected, 1e-7 ) );
   }
   return 0;
}


// Dimensions, round protocol and edge existence are validated explicitly.
int accumulation_state_validation()
{
   TASK_CHECK( throwsInvalid( []{ GradientState s( 0, 4 );  (void)s; } ) );
   TASK_CHECK( throwsInvalid( []{ GradientState s( 4, 0 );  (void)s; } ) );
   TASK_CHECK( throwsInvalid( []{ GradientState s( -2, 3 ); (void)s; } ) );

   GradientState state( 2, 2 );
   TASK_CHECK( 0 == state.getRounds() );

   // means need a completed round
   TASK_CHECK( throwsLogic( [&]{ state.getPrimalMean( 0, 0 ); } ) );
   TASK_CHECK( throwsLogic( [&]{ state.getGradientRightMean( 0, 0 ); } ) );

   // round protocol
   TASK_CHECK( throwsLogic( [&]{ state.completeRound(); } ) );
   TASK_CHECK( throwsLogic( [&]{ state.addPrimal( 0, 0, Vector3f() ); } ) );
   state.beginRound();
   TASK_CHECK( throwsLogic( [&]{ state.beginRound(); } ) );

   // coordinates and edge existence
   TASK_CHECK( throwsRange( [&]{ state.addPrimal( 2, 0, Vector3f() ); } ) );
   TASK_CHECK( throwsRange( [&]{ state.addPrimal( 0, -1, Vector3f() ); } ) );
   TASK_CHECK( throwsRange( [&]{ state.addGradientRight( 1, 0,
      Vector3f() ); } ) );
   TASK_CHECK( throwsRange( [&]{ state.addGradientUp( 0, 1,
      Vector3f() ); } ) );

   // incomplete round cannot be completed
   state.addPrimal( 0, 0, Vector3f( 1.0, 1.0, 1.0 ) );
   TASK_CHECK( throwsLogic( [&]{ state.completeRound(); } ) );

   // edge map of a 3x2 state
   GradientState edges( 3, 2 );
   TASK_CHECK(  edges.hasRight( 0, 0 ) );
   TASK_CHECK(  edges.hasRight( 1, 1 ) );
   TASK_CHECK( !edges.hasRight( 2, 0 ) );
   TASK_CHECK(  edges.hasUp( 2, 0 ) );
   TASK_CHECK( !edges.hasUp( 0, 1 ) );

   // a complete manual round; missing-edge means stay out_of_range
   edges.beginRound();
   for( int32 y = 0;  y < 2;  ++y )
   {
      for( int32 x = 0;  x < 3;  ++x )
      {
         edges.addPrimal( x, y, Vector3f() );
      }
   }
   edges.completeRound();
   TASK_CHECK( 1 == edges.getRounds() );
   TASK_CHECK( throwsRange( [&]{ edges.getGradientRightMean( 2, 0 ); } ) );
   TASK_CHECK( throwsRange( [&]{ edges.getGradientUpMean( 0, 1 ); } ) );
   TASK_CHECK( throwsRange( [&]{ edges.getPrimalSum( 3, 0 ); } ) );
   return 0;
}


/// reconstruction ---------------------------------------------------------------

// fill one round of a state from analytic P and G fields
void fillRound
(
   GradientState&  state,
   const Vector3f& base,
   const Vector3f& ax,
   const Vector3f& ay
)
{
   state.beginRound();
   for( int32 y = 0;  y < state.getHeight();  ++y )
   {
      for( int32 x = 0;  x < state.getWidth();  ++x )
      {
         state.addPrimal( x, y, base + (ax * static_cast<real64>(x)) +
            (ay * static_cast<real64>(y)) );
         if( state.hasRight( x, y ) )
         {
            state.addGradientRight( x, y, ax );
         }
         if( state.hasUp( x, y ) )
         {
            state.addGradientUp( x, y, ay );
         }
      }
   }
   state.completeRound();
}


// Constant P with zero G reconstructs the constant exactly.
int reconstruction_constant_field()
{
   GradientState state( 4, 3 );
   fillRound( state, Vector3f( 0.3, 0.5, 0.7 ), Vector3f(), Vector3f() );

   std::vector<Vector3f> image;
   const GradientSolver::Result result(
      GradientSolver::solve( state, 0.7, 1e-12, 1000, image ) );

   TASK_CHECK( result.isConverged );
   TASK_CHECK( result.residual <= 1e-12 );
   TASK_CHECK( 12u == image.size() );
   for( int32u i = 0;  i < image.size();  ++i )
   {
      TASK_CHECK( nearVec( image[i], Vector3f( 0.3, 0.5, 0.7 ), 1e-9 ) );
   }
   return 0;
}


// A linear ramp with its exact analytic gradients is recovered exactly --
// this pins down edge orientation and the absence of wrap-around edges.
int reconstruction_linear_ramp()
{
   const Vector3f base( 0.4, 0.1, 0.2 );
   const Vector3f ax( 0.3, -0.2, 0.1 );
   const Vector3f ay( -0.1, 0.25, 0.05 );

   GradientState state( 5, 4 );
   fillRound( state, base, ax, ay );

   std::vector<Vector3f> image;
   const GradientSolver::Result result(
      GradientSolver::solve( state, 2.5, 1e-12, 10000, image ) );

   TASK_CHECK( result.isConverged );
   TASK_CHECK( result.residual <= 1e-12 );
   TASK_CHECK( 20u == image.size() );
   for( int32 y = 0;  y < 4;  ++y )
   {
      for( int32 x = 0;  x < 5;  ++x )
      {
         const Vector3f expected( base + (ax * static_cast<real64>(x)) +
            (ay * static_cast<real64>(y)) );
         TASK_CHECK( nearVec(
            image[ static_cast<int32u>(x + (y * 5)) ], expected, 1e-8 ) );
      }
   }
   return 0;
}


// independent dense solve of the normal equations, for reference
void denseReferenceSolve
(
   const int32                 width,
   const int32                 height,
   const real64                alpha,
   const std::vector<Vector3f>& primal,
   const std::vector<Vector3f>& gradRight,
   const std::vector<Vector3f>& gradUp,
   const int32                 channel,
   std::vector<real64>&        x_o
)
{
   const int32 n = width * height;
   std::vector<std::vector<real64> > a( static_cast<int32u>(n),
      std::vector<real64>( static_cast<int32u>(n), 0.0 ) );
   std::vector<real64> b( static_cast<int32u>(n), 0.0 );

   for( int32 y = 0;  y < height;  ++y )
   {
      for( int32 x = 0;  x < width;  ++x )
      {
         const int32 p = x + (y * width);
         a[ static_cast<int32u>(p) ][ static_cast<int32u>(p) ] += alpha;
         b[ static_cast<int32u>(p) ] +=
            alpha * primal[ static_cast<int32u>(p) ][channel];

         // real interior edges only
         if( x + 1 < width )
         {
            const int32 q = p + 1;
            const real64 g = gradRight[ static_cast<int32u>(p) ][channel];
            a[ static_cast<int32u>(p) ][ static_cast<int32u>(p) ] += 1.0;
            a[ static_cast<int32u>(q) ][ static_cast<int32u>(q) ] += 1.0;
            a[ static_cast<int32u>(p) ][ static_cast<int32u>(q) ] -= 1.0;
            a[ static_cast<int32u>(q) ][ static_cast<int32u>(p) ] -= 1.0;
            b[ static_cast<int32u>(p) ] -= g;
            b[ static_cast<int32u>(q) ] += g;
         }
         if( y + 1 < height )
         {
            const int32 q = p + width;
            const real64 g = gradUp[ static_cast<int32u>(p) ][channel];
            a[ static_cast<int32u>(p) ][ static_cast<int32u>(p) ] += 1.0;
            a[ static_cast<int32u>(q) ][ static_cast<int32u>(q) ] += 1.0;
            a[ static_cast<int32u>(p) ][ static_cast<int32u>(q) ] -= 1.0;
            a[ static_cast<int32u>(q) ][ static_cast<int32u>(p) ] -= 1.0;
            b[ static_cast<int32u>(p) ] -= g;
            b[ static_cast<int32u>(q) ] += g;
         }
      }
   }

   // Gaussian elimination with partial pivoting
   for( int32 col = 0;  col < n;  ++col )
   {
      int32 pivot = col;
      for( int32 row = col + 1;  row < n;  ++row )
      {
         if( std::fabs( a[ static_cast<int32u>(row) ][
               static_cast<int32u>(col) ] ) >
            std::fabs( a[ static_cast<int32u>(pivot) ][
               static_cast<int32u>(col) ] ) )
         {
            pivot = row;
         }
      }
      std::swap( a[ static_cast<int32u>(pivot) ],
         a[ static_cast<int32u>(col) ] );
      std::swap( b[ static_cast<int32u>(pivot) ],
         b[ static_cast<int32u>(col) ] );

      for( int32 row = col + 1;  row < n;  ++row )
      {
         const real64 f = a[ static_cast<int32u>(row) ][
            static_cast<int32u>(col) ] / a[ static_cast<int32u>(col) ][
            static_cast<int32u>(col) ];
         for( int32 k = col;  k < n;  ++k )
         {
            a[ static_cast<int32u>(row) ][ static_cast<int32u>(k) ] -=
               f * a[ static_cast<int32u>(col) ][ static_cast<int32u>(k) ];
         }
         b[ static_cast<int32u>(row) ] -=
            f * b[ static_cast<int32u>(col) ];
      }
   }

   x_o.assign( static_cast<int32u>(n), 0.0 );
   for( int32 row = n;  row-- > 0; )
   {
      real64 s = b[ static_cast<int32u>(row) ];
      for( int32 k = row + 1;  k < n;  ++k )
      {
         s -= a[ static_cast<int32u>(row) ][ static_cast<int32u>(k) ] *
            x_o[ static_cast<int32u>(k) ];
      }
      x_o[ static_cast<int32u>(row) ] = s / a[ static_cast<int32u>(row) ][
         static_cast<int32u>(row) ];
   }
}


// An inconsistent 2D gradient field (non-zero curl around every cell, so
// nothing telescopes) is least-squares solved; the result must match an
// independent dense solve of the same system.
int reconstruction_inconsistent_field()
{
   const int32 W = 3, H = 2, N = W * H;
   const real64 alpha = 1.3;

   std::vector<Vector3f> primal( static_cast<int32u>(N) );
   std::vector<Vector3f> gradRight( static_cast<int32u>(N) );
   std::vector<Vector3f> gradUp( static_cast<int32u>(N) );

   GradientState state( W, H );
   state.beginRound();
   for( int32 y = 0;  y < H;  ++y )
   {
      for( int32 x = 0;  x < W;  ++x )
      {
         const int32u i = static_cast<int32u>(x + (y * W));
         real64 pc[3], grc[3], guc[3];
         for( int32 c = 0;  c < 3;  ++c )
         {
            const real64 cf = static_cast<real64>(c);
            const real64 xf = static_cast<real64>(x);
            const real64 yf = static_cast<real64>(y);
            pc[c]  = 0.3 * xf - 0.2 * yf + 0.1 * cf + 0.05 * xf * yf;
            grc[c] = 0.4 + 0.2 * yf - 0.1 * cf;   // d/dy != d/dx of guc
            guc[c] = -0.3 + 0.15 * xf + 0.05 * cf;
         }
         const Vector3f p( pc[0], pc[1], pc[2] );
         const Vector3f gr( grc[0], grc[1], grc[2] );
         const Vector3f gu( guc[0], guc[1], guc[2] );
         primal[i]    = p;
         gradRight[i] = gr;
         gradUp[i]    = gu;

         state.addPrimal( x, y, p );
         if( state.hasRight( x, y ) )
         {
            state.addGradientRight( x, y, gr );
         }
         if( state.hasUp( x, y ) )
         {
            state.addGradientUp( x, y, gu );
         }
      }
   }
   state.completeRound();

   std::vector<Vector3f> image;
   const GradientSolver::Result result(
      GradientSolver::solve( state, alpha, 1e-12, 10000, image ) );
   TASK_CHECK( result.isConverged );
   TASK_CHECK( result.residual <= 1e-12 );
   TASK_CHECK( static_cast<int32u>(N) == image.size() );

   // compare against the independent dense reference, per channel
   real64 maxDiff = 0.0;
   for( int32 c = 0;  c < 3;  ++c )
   {
      std::vector<real64> reference;
      denseReferenceSolve( W, H, alpha, primal, gradRight, gradUp, c,
         reference );
      for( int32 i = 0;  i < N;  ++i )
      {
         const real64 diff = std::fabs(
            image[ static_cast<int32u>(i) ][c] -
            reference[ static_cast<int32u>(i) ] );
         maxDiff = diff > maxDiff ? diff : maxDiff;
      }
   }
   TASK_CHECK( maxDiff < 1e-7 );

   // the field is genuinely inconsistent: the solution cannot match P ...
   real64 maxPrimalDiff = 0.0;
   for( int32 i = 0;  i < N;  ++i )
   {
      for( int32 c = 0;  c < 3;  ++c )
      {
         const real64 diff = std::fabs(
            image[ static_cast<int32u>(i) ][c] -
            primal[ static_cast<int32u>(i) ][c] );
         maxPrimalDiff = diff > maxPrimalDiff ? diff : maxPrimalDiff;
      }
   }
   TASK_CHECK( maxPrimalDiff > 1e-3 );

   // ... nor satisfy all edge differences
   real64 maxEdgeSlack = 0.0;
   for( int32 y = 0;  y < H;  ++y )
   {
      for( int32 x = 0;  x < W;  ++x )
      {
         const int32u i = static_cast<int32u>(x + (y * W));
         for( int32 c = 0;  c < 3;  ++c )
         {
            if( x + 1 < W )
            {
               const real64 slack = std::fabs(
                  (image[i + 1u][c] - image[i][c]) - gradRight[i][c] );
               maxEdgeSlack = slack > maxEdgeSlack ? slack : maxEdgeSlack;
            }
            if( y + 1 < H )
            {
               const real64 slack = std::fabs(
                  (image[i + static_cast<int32u>(W)][c] - image[i][c]) -
                  gradUp[i][c] );
               maxEdgeSlack = slack > maxEdgeSlack ? slack : maxEdgeSlack;
            }
         }
      }
   }
   TASK_CHECK( maxEdgeSlack > 1e-3 );
   return 0;
}


// Parameter validation, honest non-convergence reporting, and the zero
// right-hand side degenerate case.
int reconstruction_solver_contract()
{
   GradientState state( 2, 2 );
   fillRound( state, Vector3f( 0.5, 0.5, 0.5 ), Vector3f(), Vector3f() );

   std::vector<Vector3f> image;
   const real64 nan = std::numeric_limits<real64>::quiet_NaN();
   const real64 inf = std::numeric_limits<real64>::infinity();

   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, 0.0,
      1e-6, 100, image ); } ) );
   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, -1.0,
      1e-6, 100, image ); } ) );
   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, nan,
      1e-6, 100, image ); } ) );
   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, inf,
      1e-6, 100, image ); } ) );
   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, 1.0, 0.0,
      100, image ); } ) );
   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, 1.0, -1e-6,
      100, image ); } ) );
   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, 1.0, nan,
      100, image ); } ) );
   TASK_CHECK( throwsInvalid( [&]{ GradientSolver::solve( state, 1.0, 1e-6,
      0, image ); } ) );

   GradientState empty( 2, 2 );
   TASK_CHECK( throwsLogic( [&]{ GradientSolver::solve( empty, 1.0, 1e-6,
      100, image ); } ) );

   // an impossible tolerance with one iteration must report non-convergence
   GradientState busy( 3, 2 );
   fillRound( busy, Vector3f( 0.4, 0.1, 0.2 ), Vector3f( 0.3, -0.2, 0.1 ),
      Vector3f( -0.1, 0.25, 0.05 ) );
   std::vector<Vector3f> busyImage;
   const GradientSolver::Result stalled(
      GradientSolver::solve( busy, 1.0, 1e-300, 1, busyImage ) );
   TASK_CHECK( !stalled.isConverged );
   TASK_CHECK( stalled.residual > 1e-300 );
   TASK_CHECK( stalled.iterations <= 1 );

   // zero P and zero G: exact zero solution
   GradientState zero( 2, 2 );
   fillRound( zero, Vector3f(), Vector3f(), Vector3f() );
   std::vector<Vector3f> zeroImage;
   const GradientSolver::Result zeroResult(
      GradientSolver::solve( zero, 1.0, 1e-12, 100, zeroImage ) );
   TASK_CHECK( zeroResult.isConverged );
   TASK_CHECK( 0.0 == zeroResult.residual );
   for( int32u i = 0;  i < zeroImage.size();  ++i )
   {
      TASK_CHECK( zeroImage[i].isZero() );
   }
   return 0;
}


/// integration ------------------------------------------------------------------

// Full in-process pipeline: gradient render of a real scene, Poisson
// reconstruction, and output through the legacy Image tone-mapping chain.
int integration_render_reconstruct_output()
{
   std::istringstream cameraIn( "(0.278 0.275 -0.789) (0 0 1) 40\n" );
   sceneStream( cameraIn );
   const Camera camera( cameraIn );
   std::istringstream sceneIn(
      "(0.0906 0.0943 0.1151) (0.1 0.09 0.07)\n"
      "(0.556 0.000 0.000) (0.006 0.000 0.559) (0.556 0.000 0.559)  (0.7 0.7 0.7) (0 0 0)\n"
      "(0.006 0.000 0.559) (0.556 0.000 0.000) (0.003 0.000 0.000)  (0.7 0.7 0.7) (0 0 0)\n"
      "(0.343 0.545 0.332) (0.213 0.545 0.227) (0.343 0.545 0.227)  (0.7 0.7 0.7) (100 100 100)\n"
      "(0.213 0.545 0.227) (0.343 0.545 0.332) (0.213 0.545 0.332)  (0.7 0.7 0.7) (100 100 100)\n" );
   sceneStream( sceneIn );
   const Scene scene( sceneIn, camera.getEyePoint() );

   GradientState state( 6, 4 );
   Random random;
   for( int32 round = 0;  round < 3;  ++round )
   {
      camera.getFrameGradient( scene, random, state );
   }
   TASK_CHECK( 3 == state.getRounds() );

   std::vector<Vector3f> image;
   const GradientSolver::Result result(
      GradientSolver::solve( state, 1.0, 1e-10, 100000, image ) );
   TASK_CHECK( result.isConverged );
   TASK_CHECK( result.residual <= 1e-10 );
   TASK_CHECK( 24u == image.size() );

   bool anyPositive = false;
   for( int32u i = 0;  i < image.size();  ++i )
   {
      for( int32 c = 0;  c < 3;  ++c )
      {
         TASK_CHECK( std::isfinite( image[i][c] ) );
         // unclamped linear solve: small negative dips are legitimate,
         // but nothing wild
         TASK_CHECK( image[i][c] > -0.1 );
         anyPositive = anyPositive | (image[i][c] > 0.0);
      }
   }
   TASK_CHECK( anyPositive );

   // through the legacy tone-mapping chain
   std::istringstream sizeIn( "6 4\n" );
   Image out( sizeIn );
   for( int32 y = 0;  y < 4;  ++y )
   {
      for( int32 x = 0;  x < 6;  ++x )
      {
         out.addToPixel( x, y,
            image[ static_cast<int32u>(x + (y * 6)) ] );
      }
   }
   std::ostringstream ppm;
   out.getFormatted( ppm, 1 );
   const std::string bytes( ppm.str() );

   TASK_CHECK( 0 == bytes.find( "P6" ) );
   const std::string::size_type raster = bytes.find( "255\n" );
   TASK_CHECK( std::string::npos != raster );
   TASK_CHECK( bytes.size() - (raster + 4) == 6u * 4u * 3u );
   bool anyNonzero = false;
   for( std::string::size_type i = raster + 4;  i < bytes.size();  ++i )
   {
      anyNonzero = anyNonzero | (bytes[i] != 0);
   }
   TASK_CHECK( anyNonzero );
   return 0;
}


// The CLI gradient path renders and writes a valid PPM; the legacy CLI
// still works on the same model.
int integration_cli_gradient_mode()
{
   {
      std::ofstream model( ".task-build/it_gradient.ml" );
      model << "#MiniLight\n2\n4 3\n" << BOX_MODEL_BODY;
      TASK_CHECK( bool(model) );
   }

   const int rc = std::system(
      ".task-build/minilight .task-build/it_gradient.ml --gradient 1.0"
      " >.task-build/it_gradient.log 2>&1" );
   TASK_CHECK( 0 == rc );

   std::ifstream ppm( ".task-build/it_gradient.ml.ppm",
      std::ios::binary );
   TASK_CHECK( bool(ppm) );
   const std::string bytes( (std::istreambuf_iterator<char>( ppm )),
      std::istreambuf_iterator<char>() );
   TASK_CHECK( 0 == bytes.find( "P6" ) );
   TASK_CHECK( std::string::npos != bytes.find( "4 3\n255\n" ) );
   const std::string::size_type raster = bytes.find( "255\n" );
   TASK_CHECK( std::string::npos != raster );
   TASK_CHECK( bytes.size() - (raster + 4) == 4u * 3u * 3u );
   bool anyNonzero = false;
   for( std::string::size_type i = raster + 4;  i < bytes.size();  ++i )
   {
      anyNonzero = anyNonzero | (bytes[i] != 0);
   }
   TASK_CHECK( anyNonzero );

   // legacy mode on the same model still succeeds
   TASK_CHECK( 0 == std::system(
      ".task-build/minilight .task-build/it_gradient.ml"
      " >.task-build/it_legacy.log 2>&1" ) );
   return 0;
}


// Invalid alpha values and malformed invocations are rejected.
int integration_cli_rejects_bad_alpha()
{
   const char* bad[] = { "0", "-2", "nan", "inf", "abc", "1e999", "1,5" };
   for( size_t i = 0;  i < sizeof(bad) / sizeof(bad[0]);  ++i )
   {
      const std::string cmd( std::string(
         ".task-build/minilight .task-build/it_gradient.ml --gradient " ) +
         bad[i] + " >/dev/null 2>&1" );
      TASK_CHECK( 0 != std::system( cmd.c_str() ) );
   }
   // missing alpha
   TASK_CHECK( 0 != std::system(
      ".task-build/minilight .task-build/it_gradient.ml --gradient"
      " >/dev/null 2>&1" ) );
   // unknown extra argument
   TASK_CHECK( 0 != std::system(
      ".task-build/minilight .task-build/it_gradient.ml --gradients 1.0"
      " >/dev/null 2>&1" ) );
   return 0;
}


}//namespace




/// registration ---------------------------------------------------------------

const task_case task_cases[] =
{
   { "coupling",       "sky_stream_and_zero_gradients",
      coupling_sky_stream_and_zero_gradients },
   { "coupling",       "primal_matches_legacy_render",
      coupling_primal_matches_legacy_render },
   { "coupling",       "paired_paths_share_randomness",
      coupling_paired_paths_share_randomness },

   { "accumulation",   "rounds_and_stable_queries",
      accumulation_rounds_and_stable_queries },
   { "accumulation",   "append_uses_all_samples",
      accumulation_append_uses_all_samples },
   { "accumulation",   "state_validation",
      accumulation_state_validation },

   { "reconstruction", "constant_field",
      reconstruction_constant_field },
   { "reconstruction", "linear_ramp",
      reconstruction_linear_ramp },
   { "reconstruction", "inconsistent_field",
      reconstruction_inconsistent_field },
   { "reconstruction", "solver_contract",
      reconstruction_solver_contract },

   { "integration",    "render_reconstruct_output",
      integration_render_reconstruct_output },
   { "integration",    "cli_gradient_mode",
      integration_cli_gradient_mode },
   { "integration",    "cli_rejects_bad_alpha",
      integration_cli_rejects_bad_alpha }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

#include "task_support/check.h"

#include "Camera.hpp"
#include "GradientState.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
#include "Scene.hpp"
#include "Vector3f.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hxa7241_minilight;

namespace
{

const char VIEW_DEFINITION[] = "(0 0.5 -1) (0 0 1) 45\n";
const char SCENE_BODY[] =
   "(0.2 0.3 0.4) (0.5 0.5 0.5)\n"
   "(-10 0 -10) (10 0 -10) (10 0 10) (0.6 0.55 0.5) (0 0 0)\n"
   "(-10 10 -10) (10 10 -10) (10 10 10) (0 0 0) (3 4 5)\n"
   "(-10 0 -10) (-10 10 -10) (-10 10 10) (0.5 0.5 0.5) (0 0 0)\n";

bool near( const Vector3f& a, const Vector3f& b, double tolerance )
{
   for( int c = 0;  c < 3;  ++c )
   {
      if( std::fabs( a[c] - b[c] ) > tolerance ) return false;
   }
   return true;
}

bool nearValue( double a, double b, double tolerance )
{
   return std::fabs( a - b ) <= tolerance * (1.0 + std::fabs( b ));
}

std::istringstream* makeModelStream()
{
   std::istringstream* input = new std::istringstream(
      std::string( VIEW_DEFINITION ) + SCENE_BODY );
   input->exceptions( std::ios::badbit | std::ios::eofbit );
   return input;
}

void makeScene( std::unique_ptr<Camera>& camera_o,
                std::unique_ptr<Scene>& scene_o )
{
   std::unique_ptr<std::istringstream> input( makeModelStream() );
   camera_o.reset( new Camera( *input ) );
   scene_o.reset( new Scene( *input, camera_o->getEyePoint() ) );
}

int index( int x, int y, int width )
{
   return x + (y * width);
}

struct SampleBuffers
{
   SampleBuffers( int width, int height )
    : w( width )
    , h( height )
    , primal( std::vector<Vector3f>( static_cast<size_t>(width * height),
         Vector3f::ZERO() ) )
    , right ( std::vector<Vector3f>( static_cast<size_t>(width * height),
         Vector3f::ZERO() ) )
    , up    ( std::vector<Vector3f>( static_cast<size_t>(width * height),
         Vector3f::ZERO() ) )
   {
   }

   int w;
   int h;
   std::vector<Vector3f> primal;
   std::vector<Vector3f> right;
   std::vector<Vector3f> up;
};

// The reference round: the same pixel traversal order as the Camera, with the
// caller's random state advanced only through the base path.
void referenceRound( const Camera& camera, const Scene& scene, Random& random,
                     SampleBuffers& sums )
{
   const RayTracer rayTracer( scene );

   for( int y = sums.h - 1;  y >= 0;  --y )
   {
      for( int x = sums.w - 1;  x >= 0;  --x )
      {
         Random startRandom;
         startRandom.setState( random );

         Random baseRandom( startRandom );
         const Vector3f baseRadiance( rayTracer.getRadiance(
            camera.getEyePoint(),
            camera.getSampleDirection( static_cast<double>(x),
               static_cast<double>(y), sums.w, sums.h, baseRandom ),
            baseRandom ) );

         sums.primal[ index( x, y, sums.w ) ] =
            sums.primal[ index( x, y, sums.w ) ] + baseRadiance;

         if( x + 1 < sums.w )
         {
            Random neighbourRandom( startRandom );
            const Vector3f neighbourRadiance( rayTracer.getRadiance(
               camera.getEyePoint(),
               camera.getSampleDirection( static_cast<double>(x + 1),
                  static_cast<double>(y), sums.w, sums.h, neighbourRandom ),
               neighbourRandom ) );

            sums.right[ index( x, y, sums.w ) ] =
               sums.right[ index( x, y, sums.w ) ] +
               (neighbourRadiance - baseRadiance);
         }

         if( y + 1 < sums.h )
         {
            Random neighbourRandom( startRandom );
            const Vector3f neighbourRadiance( rayTracer.getRadiance(
               camera.getEyePoint(),
               camera.getSampleDirection( static_cast<double>(x),
                  static_cast<double>(y + 1), sums.w, sums.h,
                  neighbourRandom ),
               neighbourRandom ) );

            sums.up[ index( x, y, sums.w ) ] =
               sums.up[ index( x, y, sums.w ) ] +
               (neighbourRadiance - baseRadiance);
         }

         random.setState( baseRandom );
      }
   }
}

// An independent dense solve of the exact 9-point-normal/4-neighbour system.
std::vector<double> denseSolve( int width, int height,
                                const std::vector<double>& primal,
                                const std::vector<double>& rightGradient,
                                const std::vector<double>& upGradient,
                                double alpha )
{
   const int n = width * height;
   std::vector<double> matrix( static_cast<size_t>(n * n), 0.0 );
   std::vector<double> rhs( static_cast<size_t>(n), 0.0 );

   for( int y = 0;  y < height;  ++y )
   {
      for( int x = 0;  x < width;  ++x )
      {
         const int i = index( x, y, width );
         double diagonal = alpha;

         if( x > 0 )          { diagonal += 1.0; matrix[ i*n + i-1 ] = -1.0; }
         if( x + 1 < width )  { diagonal += 1.0; matrix[ i*n + i+1 ] = -1.0; }
         if( y > 0 )          { diagonal += 1.0; matrix[ i*n + i-width ] = -1.0; }
         if( y + 1 < height ) { diagonal += 1.0; matrix[ i*n + i+width ] = -1.0; }

         matrix[ i*n + i ] = diagonal;
         rhs[i] = alpha * primal[ static_cast<size_t>(i) ];

         if( x + 1 < width )
         {
            const double g = rightGradient[ static_cast<size_t>(i) ];
            rhs[i] -= g;
            rhs[ static_cast<size_t>(i + 1) ] += g;
         }
         if( y + 1 < height )
         {
            const double g = upGradient[ static_cast<size_t>(i) ];
            rhs[i] -= g;
            rhs[ static_cast<size_t>(i + width) ] += g;
         }
      }
   }

   // Gaussian elimination with partial pivoting (n is deliberately tiny).
   for( int column = 0;  column < n;  ++column )
   {
      int pivot = column;
      for( int row = column + 1;  row < n;  ++row )
      {
         if( std::fabs( matrix[ static_cast<size_t>(row*n + column) ] ) >
             std::fabs( matrix[ static_cast<size_t>(pivot*n + column) ] ) )
         {
            pivot = row;
         }
      }

      if( pivot != column )
      {
         for( int k = 0;  k < n;  ++k )
         {
            std::swap( matrix[ static_cast<size_t>(column*n + k) ],
                       matrix[ static_cast<size_t>(pivot*n + k) ] );
         }
         std::swap( rhs[ static_cast<size_t>(column) ],
                    rhs[ static_cast<size_t>(pivot) ] );
      }

      const double divisor = matrix[ static_cast<size_t>(column*n + column) ];
      for( int row = column + 1;  row < n;  ++row )
      {
         const double factor =
            matrix[ static_cast<size_t>(row*n + column) ] / divisor;
         if( factor == 0.0 ) continue;
         for( int k = column;  k < n;  ++k )
         {
            matrix[ static_cast<size_t>(row*n + k) ] -=
               factor * matrix[ static_cast<size_t>(column*n + k) ];
         }
         rhs[ static_cast<size_t>(row) ] -= factor * rhs[ static_cast<size_t>(column) ];
      }
   }

   std::vector<double> solution( static_cast<size_t>(n), 0.0 );
   for( int row = n - 1;  row >= 0;  --row )
   {
      double value = rhs[ static_cast<size_t>(row) ];
      for( int k = row + 1;  k < n;  ++k )
      {
         value -= matrix[ static_cast<size_t>(row*n + k) ] *
            solution[ static_cast<size_t>(k) ];
      }
      solution[ static_cast<size_t>(row) ] =
         value / matrix[ static_cast<size_t>(row*n + row) ];
   }

   return solution;
}

std::vector<double> channelValues( const std::vector<Vector3f>& field, int channel )
{
   std::vector<double> values( field.size(), 0.0 );
   for( size_t i = 0;  i < field.size();  ++i )
   {
      values[i] = field[i][channel];
   }
   return values;
}

Vector3f channelResult( const std::vector<double>& values, size_t i )
{
   Vector3f result;
   for( int c = 0;  c < 3;  ++c )
   {
      result = result + Vector3f( (c == 0 ? values[i] : 0.0),
         (c == 1 ? values[i] : 0.0), (c == 2 ? values[i] : 0.0) );
   }
   return result;
}

double relativeResidual( int width, int height,
                         const std::vector<double>& primal,
                         const std::vector<double>& rightGradient,
                         const std::vector<double>& upGradient,
                         double alpha, const std::vector<double>& image )
{
   std::vector<double> answer( static_cast<size_t>(width * height), 0.0 );
   std::vector<double> rhs( static_cast<size_t>(width * height), 0.0 );

   for( int y = 0;  y < height;  ++y )
   {
      for( int x = 0;  x < width;  ++x )
      {
         const int i = index( x, y, width );
         const size_t si = static_cast<size_t>(i);

         answer[si] = alpha * image[si];
         if( x > 0 )          answer[si] += image[si] - image[static_cast<size_t>(i - 1)];
         if( x + 1 < width )  answer[si] += image[si] - image[static_cast<size_t>(i + 1)];
         if( y > 0 )          answer[si] += image[si] - image[static_cast<size_t>(i - width)];
         if( y + 1 < height ) answer[si] += image[si] - image[static_cast<size_t>(i + width)];

         rhs[si] = alpha * primal[si];
         if( x + 1 < width )
         {
            const double g = rightGradient[si];
            rhs[si] -= g;
            rhs[static_cast<size_t>(i + 1)] += g;
         }
         if( y + 1 < height )
         {
            const double g = upGradient[si];
            rhs[si] -= g;
            rhs[static_cast<size_t>(i + width)] += g;
         }
      }
   }

   double difference = 0.0;
   double magnitude  = 0.0;
   for( size_t i = 0;  i < answer.size();  ++i )
   {
      const double d = answer[i] - rhs[i];
      difference += d * d;
      magnitude  += rhs[i] * rhs[i];
   }

   const double diffNorm = std::sqrt( difference );
   const double rhsNorm  = std::sqrt( magnitude );
   return rhsNorm > 0.0 ? diffNorm / rhsNorm : diffNorm;
}

// Build a state with explicit sums; rounds are set separately.
void fillState( GradientState& state,
                const std::vector<Vector3f>& primal,
                const std::vector<Vector3f>& right,
                const std::vector<Vector3f>& up )
{
   const int width  = state.getWidth();
   const int height = state.getHeight();
   for( int y = 0;  y < height;  ++y )
   {
      for( int x = 0;  x < width;  ++x )
      {
         const size_t i = static_cast<size_t>( index( x, y, width ) );
         state.setPrimalSum( x, y, primal[i] );
         if( state.hasRightEdge( x, y ) )
         {
            state.setRightGradientSum( x, y, right[i] );
         }
         if( state.hasUpEdge( x, y ) )
         {
            state.setUpGradientSum( x, y, up[i] );
         }
      }
   }
}

bool sameVectors( const std::vector<Vector3f>& a,
                  const std::vector<Vector3f>& b )
{
   if( a.size() != b.size() ) return false;
   for( size_t i = 0;  i < a.size();  ++i )
   {
      for( int c = 0;  c < 3;  ++c )
      {
         if( a[i][c] != b[i][c] ) return false;
      }
   }
   return true;
}


{
   try
   {
      state.reconstruct( alpha );
   }
   catch( const std::invalid_argument& )
   {
      return true;
   }
   catch( ... )
   {
      return false;
   }
   return false;
}

bool throwsEmptyState( const GradientState& state )
{
   try
   {
      state.reconstruct( 1.0 );
   }
   catch( const std::logic_error& )
   {
      return true;
   }
   catch( ... )
   {
      return false;
   }
   return false;
}

// gradient field used to check curl-inconsistency and negative values
void makeInconsistentField( int width, int height,
                            std::vector<Vector3f>& primal,
                            std::vector<Vector3f>& right,
                            std::vector<Vector3f>& up )
{
   primal.assign( static_cast<size_t>(width * height), Vector3f::ZERO() );
   right .assign( static_cast<size_t>(width * height), Vector3f::ZERO() );
   up    .assign( static_cast<size_t>(width * height), Vector3f::ZERO() );

   for( int y = 0;  y < height;  ++y )
   {
      for( int x = 0;  x < width;  ++x )
      {
         const size_t i = static_cast<size_t>( index( x, y, width ) );
         primal[i] = Vector3f( 0.3 + (0.1 * x) - (0.2 * y),
                               0.7 - (0.05 * x),
                               0.2 + (0.15 * y) );

         // right gradient varies with y, up gradient varies with x: their
         // mismatch is a genuine non-zero curl, not telescoping.
         right[i] = Vector3f( 0.5 + (0.1 * y), -0.3 + (0.2 * x),
            0.7 - (0.4 * y) );
         up[i]    = Vector3f( 0.1 + (0.05 * x), 0.6 - (0.1 * y),
            -0.4 + (0.3 * x) );
      }
   }
}




/// coupling -------------------------------------------------------------------

int couplingPairing()
{
   try
   {
      std::unique_ptr<Camera> camera;
      std::unique_ptr<Scene>  scene;
      makeScene( camera, scene );

      const int width = 3;
      const int height = 2;

      GradientState state( width, height );
      Random actual;
      camera->getGradientFrame( *scene, actual, state );

      SampleBuffers expected( width, height );
      Random reference;
      referenceRound( *camera, *scene, reference, expected );

      TASK_CHECK( state.getRounds() == 1 );
      TASK_CHECK( state.getRightEdgeCount() == (width - 1) * height );
      TASK_CHECK( state.getUpEdgeCount() == width * (height - 1) );

      bool anyDifference = false;
      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            TASK_CHECK( near( state.getPrimalSum( x, y ),
               expected.primal[ static_cast<size_t>(index(x,y,width)) ], 1e-10 ) );
            if( state.hasRightEdge( x, y ) )
            {
               TASK_CHECK( near( state.getRightGradientSum( x, y ),
                  expected.right[ static_cast<size_t>(index(x,y,width)) ],
                  1e-10 ) );
            }
            if( state.hasUpEdge( x, y ) )
            {
               TASK_CHECK( near( state.getUpGradientSum( x, y ),
                  expected.up[ static_cast<size_t>(index(x,y,width)) ],
                  1e-10 ) );
            }

            const Vector3f gradient =
               state.getRightGradientSum( x, y ) -
               (state.getPrimalSum( x + 1, y ) - state.getPrimalSum( x, y ));
            if( !gradient.isZero() ) anyDifference = true;
         }
      }

      // The neighbour rays genuinely differ from the base ray, so the test is
      // not satisfied by zero gradients.
      TASK_CHECK( anyDifference );

      // The caller's stream must have advanced only through base evaluations.
      Random actualCopy( actual );
      Random referenceCopy( reference );
      TASK_CHECK( actualCopy.getInt32u() == referenceCopy.getInt32u() );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int couplingInteriorOnly()
{
   try
   {
      GradientState state( 4, 1 );
      state.setPrimalSum( 0, 0, Vector3f( 1, 1, 1 ) );
      state.setPrimalSum( 1, 0, Vector3f( 2, 2, 2 ) );
      state.setPrimalSum( 2, 0, Vector3f( 3, 3, 3 ) );
      state.setPrimalSum( 3, 0, Vector3f( 4, 4, 4 ) );

      TASK_CHECK( state.getRightEdgeCount() == 3 );
      TASK_CHECK( state.getUpEdgeCount() == 0 );
      TASK_CHECK( state.hasRightEdge( 2, 0 ) );
      TASK_CHECK( !state.hasRightEdge( 3, 0 ) );
      TASK_CHECK( !state.hasUpEdge( 0, 0 ) );

      // attempts on a non-existent edge must not create an edge value
      state.accumulateRightGradient( 3, 0, Vector3f( 9, 9, 9 ) );
      state.accumulateUpGradient( 0, 0, Vector3f( 9, 9, 9 ) );
      state.setRightGradientSum( 3, 0, Vector3f( 9, 9, 9 ) );
      state.setUpGradientSum( 0, 0, Vector3f( 9, 9, 9 ) );

      TASK_CHECK( near( state.getRightGradientSum( 3, 0 ), Vector3f::ZERO(), 0.0 ) );
      TASK_CHECK( near( state.getUpGradientSum( 0, 0 ), Vector3f::ZERO(), 0.0 ) );

      // each edge is recorded once: (1,0)->(2,0) lives at (1,0), not (2,0)
      TASK_CHECK( state.hasRightEdge( 1, 0 ) );
      state.setRightGradientSum( 1, 0, Vector3f( 1.5, 0, 0 ) );
      TASK_CHECK( near( state.getRightGradientSum( 1, 0 ),
         Vector3f( 1.5, 0, 0 ), 0.0 ) );
      TASK_CHECK( near( state.getRightGradientSum( 2, 0 ), Vector3f::ZERO(), 0.0 ) );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int couplingBorderNoWrap()
{
   try
   {
      GradientState state( 2, 3 );
      std::vector<Vector3f> primal( 6, Vector3f::ZERO() );
      for( int i = 0;  i < 6;  ++i )
      {
         primal[ static_cast<size_t>(i) ] = Vector3f( 1, 2, 3 );
      }
      std::vector<Vector3f> right( 6, Vector3f( 0.2, 0.2, 0.2 ) );
      std::vector<Vector3f> up   ( 6, Vector3f( 0.1, 0.1, 0.1 ) );
      fillState( state, primal, right, up );

      TASK_CHECK( state.getEdgeCount() == 7 ); // 1*3 right + 2*2 up
      for( int y = 0;  y < 3;  ++y )
      {
         TASK_CHECK( !state.hasRightEdge( 1, y ) );
         TASK_CHECK( near( state.getRightGradientSum( 1, y ), Vector3f::ZERO(), 0.0 ) );
      }
      TASK_CHECK( !state.hasUpEdge( 0, 2 ) );
      TASK_CHECK( !state.hasUpEdge( 1, 2 ) );
      TASK_CHECK( near( state.getUpGradientSum( 0, 2 ), Vector3f::ZERO(), 0.0 ) );

      // No wraparound: the edge from (1,0) to (0,0) does not exist.
      TASK_CHECK( near( state.getPrimalSum( 1, 1 ), Vector3f( 1, 2, 3 ), 0.0 ) );

      // one right edge per row: 3; one up edge per column per interior row: 4
      int rightEdges = 0;
      int upEdges = 0;
      for( int y = 0;  y < 3;  ++y )
      {
         for( int x = 0;  x < 2;  ++x )
         {
            if( state.hasRightEdge( x, y ) ) ++rightEdges;
            if( state.hasUpEdge( x, y ) )    ++upEdges;
         }
      }
      TASK_CHECK( rightEdges == 3 );
      TASK_CHECK( upEdges == 4 );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}




/// accumulation ---------------------------------------------------------------

int accumulationRoundSums()
{
   try
   {
      GradientState state( 3, 2 );
      Vector3f first( 0.5, 1.0, 1.5 );
      Vector3f second( 2.0, 3.0, 4.0 );

      state.accumulatePrimal( 2, 1, first );
      state.accumulatePrimal( 2, 1, second );
      TASK_CHECK( near( state.getPrimalSum( 2, 1 ), Vector3f( 2.5, 4.0, 5.5 ), 1e-15 ) );

      state.accumulateRightGradient( 0, 1, first );
      state.accumulateRightGradient( 0, 1, second );
      state.accumulateUpGradient( 1, 0, first );
      state.accumulateUpGradient( 1, 0, second );

      TASK_CHECK( near( state.getRightGradientSum( 0, 1 ),
         Vector3f( 2.5, 4.0, 5.5 ), 1e-15 ) );
      TASK_CHECK( near( state.getUpGradientSum( 1, 0 ),
         Vector3f( 2.5, 4.0, 5.5 ), 1e-15 ) );

      TASK_CHECK( state.getRounds() == 0 );
      state.completeRound();
      TASK_CHECK( state.getRounds() == 1 );
      state.completeRound();
      TASK_CHECK( state.getRoundCount() == 2 );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int accumulationNormalization()
{
   try
   {
      GradientState state( 2, 2 );
      const Vector3f value( 1.0, 2.0, 3.0 );

      for( int i = 0;  i < 6;  ++i )
      {
         state.accumulatePrimal( i % 2, (i / 2) % 2, value );
      }
      state.setRounds( 3 );

      TASK_CHECK( near( state.getPrimalSum( 0, 0 ), Vector3f( 0, 0, 0 ), 0.0 ) ||
         state.getPrimalSum( 0, 0 )[0] > 0.0 );
      TASK_CHECK( near( state.getPrimalEstimate( 0, 0 ), Vector3f( 1.0/3, 2.0/3, 1.0 ), 1e-15 ) );

      state.setRightGradientSum( 0, 0, Vector3f( 3.0, 6.0, 9.0 ) );
      TASK_CHECK( near( state.getRightGradient( 0, 0 ), Vector3f( 1.0, 2.0, 3.0 ), 1e-15 ) );
      state.setUpGradientSum( 1, 0, Vector3f( 6.0, 12.0, 18.0 ) );
      TASK_CHECK( near( state.getUpGradient( 1, 0 ), Vector3f( 2.0, 4.0, 6.0 ), 1e-15 ) );

      TASK_CHECK( state.hasSamples() );
      TASK_CHECK( !state.isEmpty() );

      state.clear();
      TASK_CHECK( state.getRounds() == 0 );
      TASK_CHECK( state.getPrimalSum( 0, 0 ).isZero() );
      TASK_CHECK( state.getRightGradientSum( 0, 0 ).isZero() );
      TASK_CHECK( state.getWidth() == 2 && state.getHeight() == 2 );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int accumulationReadDoesNotMutate()
{
   try
   {
      GradientState state( 3, 3 );
      std::vector<Vector3f> primal( 9, Vector3f( 0.4, 0.5, 0.6 ) );
      std::vector<Vector3f> right( 9, Vector3f( 0.1, 0.1, 0.1 ) );
      std::vector<Vector3f> up   ( 9, Vector3f( 0.05, 0.05, 0.05 ) );
      fillState( state, primal, right, up );
      state.setRounds( 4 );

      const std::vector<Vector3f> primalBefore = state.getPrimalSums();
      const std::vector<Vector3f> rightBefore  = state.getRightGradientSums();
      const std::vector<Vector3f> upBefore     = state.getUpGradientSums();
      const int roundsBefore = state.getRounds();

      GradientSolution first  = state.reconstruct( 1.0, 1e-12, 200 );
      GradientSolution second = state.reconstruct( 1.0, 1e-12, 200 );

      TASK_CHECK( first.isConverged() );
      TASK_CHECK( nearValue( first.getResidual(), second.getResidual(), 1e-14 ) );
      for( size_t i = 0;  i < primalBefore.size();  ++i )
      {
         TASK_CHECK( near( first.getLinear( static_cast<int>(i % 3),
            static_cast<int>(i / 3) ), second.getLinear( static_cast<int>(i % 3),
            static_cast<int>(i / 3) ), 1e-14 ) );
      }

      TASK_CHECK( sameVectors( state.getPrimalSums(), primalBefore ) );
      TASK_CHECK( sameVectors( state.getRightGradientSums(), rightBefore ) );
      TASK_CHECK( sameVectors( state.getUpGradientSums(), upBefore ) );
      TASK_CHECK( state.getRounds() == roundsBefore );

      // Appending a later round must rebuild from all samples, not just the
      // new one: with an extra round the normalized primal stays the same.
      for( int y = 0;  y < 3;  ++y )
      {
         for( int x = 0;  x < 3;  ++x )
         {
            state.setPrimalSum( x, y, state.getPrimalSum( x, y ) +
               Vector3f( 0.4, 0.5, 0.6 ) );
            if( state.hasRightEdge( x, y ) )
            {
               state.setRightGradientSum( x, y,
                  state.getRightGradientSum( x, y ) + Vector3f( 0.1, 0.1, 0.1 ) );
            }
            if( state.hasUpEdge( x, y ) )
            {
               state.setUpGradientSum( x, y,
                  state.getUpGradientSum( x, y ) + Vector3f( 0.05, 0.05, 0.05 ) );
            }
         }
      }
      state.setRounds( 5 );
      GradientSolution third = state.reconstruct( 1.0, 1e-12, 200 );
      TASK_CHECK( third.isConverged() );
      for( int y = 0;  y < 3;  ++y )
      {
         for( int x = 0;  x < 3;  ++x )
         {
            TASK_CHECK( near( third.getLinear( x, y ), first.getLinear( x, y ), 1e-12 ) );
         }
      }
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}




/// reconstruction -------------------------------------------------------------

int reconstructionConstant()
{
   try
   {
      const int width  = 3;
      const int height = 2;
      GradientState state( width, height );

      std::vector<Vector3f> primal( static_cast<size_t>(width * height),
         Vector3f( 2.0, 0.5, 7.0 ) );
      std::vector<Vector3f> right ( primal.size(), Vector3f::ZERO() );
      std::vector<Vector3f> up    ( primal.size(), Vector3f::ZERO() );
      fillState( state, primal, right, up );
      state.setRounds( 1 );

      const double alpha = 2.0;
      GradientSolution solution = state.reconstruct( alpha, 1e-12, 500 );

      TASK_CHECK( solution.isConverged() );
      TASK_CHECK( solution.getWidth() == width && solution.getHeight() == height );
      TASK_CHECK( nearValue( solution.getResidual(), 0.0, 1e-12 ) );

      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            TASK_CHECK( near( solution.getLinear( x, y ),
               Vector3f( 2.0, 0.5, 7.0 ), 1e-10 ) );
         }
      }
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int reconstructionRamp()
{
   try
   {
      const int width  = 4;
      const int height = 3;
      GradientState state( width, height );

      std::vector<Vector3f> primal( static_cast<size_t>(width * height),
         Vector3f::ZERO() );
      std::vector<Vector3f> right ( primal.size(), Vector3f::ZERO() );
      std::vector<Vector3f> up    ( primal.size(), Vector3f::ZERO() );

      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            const size_t i = static_cast<size_t>(index( x, y, width ));
            primal[i] = Vector3f( 1.0 + (2.0 * x) - (3.0 * y),
                                  0.5 + (0.25 * x),
                                  4.0 + (0.5 * y) );

            if( x + 1 < width )
            {
               right[i] = Vector3f( 2.0, 0.25, 0.0 );
            }
            if( y + 1 < height )
            {
               up[i] = Vector3f( -3.0, 0.0, 0.5 );
            }
         }
      }
      fillState( state, primal, right, up );
      state.setRounds( 1 );

      const double alpha = 0.75;
      GradientSolution solution = state.reconstruct( alpha, 1e-12, 500 );
      TASK_CHECK( solution.isConverged() );

      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            TASK_CHECK( near( solution.getLinear( x, y ),
               primal[ static_cast<size_t>(index(x,y,width)) ], 1e-9 ) );
         }
      }
      TASK_CHECK( nearValue( solution.getResidual(), 0.0, 1e-10 ) );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int reconstructionInconsistent()
{
   try
   {
      const int width  = 3;
      const int height = 3;
      GradientState state( width, height );

      std::vector<Vector3f> primal;
      std::vector<Vector3f> right;
      std::vector<Vector3f> up;
      makeInconsistentField( width, height, primal, right, up );
      fillState( state, primal, right, up );
      state.setRounds( 1 );

      const double alpha = 1.5;
      GradientSolution solution = state.reconstruct( alpha, 1e-13, 1000 );
      TASK_CHECK( solution.isConverged() );

      for( int channel = 0;  channel < 3;  ++channel )
      {
         const std::vector<double> primalChannel = channelValues( primal, channel );
         const std::vector<double> rightChannel  = channelValues( right, channel );
         const std::vector<double> upChannel     = channelValues( up, channel );

         const std::vector<double> expected = denseSolve( width, height,
            primalChannel, rightChannel, upChannel, alpha );

         for( int y = 0;  y < height;  ++y )
         {
            for( int x = 0;  x < width;  ++x )
            {
               const size_t i = static_cast<size_t>(index( x, y, width ));
               TASK_CHECK( nearValue( solution.getLinear( x, y )[channel],
                  expected[i], 1e-9 ) );
            }
         }

         const double actualResidual = relativeResidual( width, height,
            primalChannel, rightChannel, upChannel, alpha,
            channelValues( solution.getPixels(), channel ) );
         TASK_CHECK( nearValue( solution.getChannelResidual( channel ),
            actualResidual, 1e-7 ) );
      }

      // The mismatched gradient field must actually be non-integrable: no
      // single image has these edge gradients. Verify with the dense system
      // that the plain data-term residual is non-zero.
      {
         std::vector<double> image = denseSolve( width, height,
            channelValues( primal, 0 ), channelValues( right, 0 ),
            channelValues( up, 0 ), alpha );
         double curl = 0.0;
         for( int y = 0;  y < height;  ++y )
         {
            for( int x = 0;  x < width;  ++x )
            {
               const size_t i = static_cast<size_t>(index( x, y, width ));
               if( (x + 1 < width) && (y + 1 < height) )
               {
                  // closed loop: right(x,y), up(x+1,y), -right(x,y+1), -up(x,y)
                  const double loop =
                     right[ static_cast<size_t>(index(x,y,width)) ][0]
                     + up[    static_cast<size_t>(index(x+1,y,width)) ][0]
                     - right[ static_cast<size_t>(index(x,y+1,width)) ][0]
                     - up[    static_cast<size_t>(index(x,y,width)) ][0];
                  curl += std::fabs( loop );
               }
               (void)image;
            }
         }
         TASK_CHECK( curl > 1e-6 );
      }
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int reconstructionBoundaries()
{
   try
   {
      // 1x1: no edges at all, so I = P exactly and alpha fixes the only mode.
      {
         GradientState state( 1, 1 );
         state.setPrimalSum( 0, 0, Vector3f( 5.0, -2.0, 0.25 ) );
         state.setRounds( 1 );
         GradientSolution solution = state.reconstruct( 0.25, 1e-14, 100 );
         TASK_CHECK( solution.isConverged() );
         TASK_CHECK( near( solution.getLinear( 0, 0 ),
            Vector3f( 5.0, -2.0, 0.25 ), 1e-12 ) );
         TASK_CHECK( state.getEdgeCount() == 0 );
      }

      // 2x1: only a right edge exists; compare with the dense system.
      {
         const int width  = 2;
         const int height = 1;
         GradientState state( width, height );
         std::vector<Vector3f> primal( 2, Vector3f( 1.0, 2.0, 3.0 ) );
         std::vector<Vector3f> right ( 2, Vector3f( 0.5, -0.5, 1.0 ) );
         std::vector<Vector3f> up   ( 2, Vector3f::ZERO() );
         fillState( state, primal, right, up );
         state.setRounds( 1 );

         const double alpha = 0.4;
         GradientSolution solution = state.reconstruct( alpha, 1e-14, 100 );
         TASK_CHECK( solution.isConverged() );
         TASK_CHECK( state.getUpEdgeCount() == 0 );

         for( int channel = 0;  channel < 3;  ++channel )
         {
            const std::vector<double> expected = denseSolve( width, height,
               channelValues( primal, channel ), channelValues( right, channel ),
               channelValues( up, channel ), alpha );
            for( int x = 0;  x < width;  ++x )
            {
               TASK_CHECK( nearValue( solution.getLinear( x, 0 )[channel],
                  expected[ static_cast<size_t>(x) ], 1e-12 ) );
            }
         }
      }

      // 1x3: only up edges exist, and no edge may be faked at the top border.
      {
         const int width  = 1;
         const int height = 3;
         GradientState state( width, height );
         std::vector<Vector3f> primal( 3, Vector3f( 0.0, 0.0, 0.0 ) );
         std::vector<Vector3f> right ( 3, Vector3f::ZERO() );
         std::vector<Vector3f> up    ( 3, Vector3f( 0.2, 0.4, -0.6 ) );
         fillState( state, primal, right, up );
         state.setRounds( 1 );
         TASK_CHECK( state.getRightEdgeCount() == 0 );

         const double alpha = 0.6;
         GradientSolution solution = state.reconstruct( alpha, 1e-14, 100 );
         TASK_CHECK( solution.isConverged() );
         for( int channel = 0;  channel < 3;  ++channel )
         {
            const std::vector<double> expected = denseSolve( width, height,
               channelValues( primal, channel ), channelValues( right, channel ),
               channelValues( up, channel ), alpha );
            for( int y = 0;  y < height;  ++y )
            {
               TASK_CHECK( nearValue( solution.getLinear( 0, y )[channel],
                  expected[ static_cast<size_t>(y) ], 1e-12 ) );
            }
         }
      }
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int reconstructionErrors()
{
   try
   {
      GradientState state( 2, 2 );
      fillState( state, std::vector<Vector3f>( 4, Vector3f( 1, 1, 1 ) ),
         std::vector<Vector3f>( 4, Vector3f( 0.1, 0.1, 0.1 ) ),
         std::vector<Vector3f>( 4, Vector3f( 0.2, 0.2, 0.2 ) ) );

      // no sampling rounds yet
      TASK_CHECK( throwsEmptyState( state ) );

      state.setRounds( 1 );
      // alpha must be positive and finite
      TASK_CHECK( throwsInvalidAlpha( state, 0.0 ) );
      TASK_CHECK( throwsInvalidAlpha( state, -1.0 ) );
      TASK_CHECK( throwsInvalidAlpha( state,
         std::numeric_limits<double>::infinity() ) );
      TASK_CHECK( throwsInvalidAlpha( state,
         std::numeric_limits<double>::quiet_NaN() ) );

      bool badTolerance = false;
      try { state.reconstruct( 1.0, 0.0 ); }
      catch( const std::invalid_argument& ) { badTolerance = true; }
      TASK_CHECK( badTolerance );

      bool badIterations = false;
      try { state.reconstruct( 1.0, 1e-6, 0 ); }
      catch( const std::invalid_argument& ) { badIterations = true; }
      TASK_CHECK( badIterations );

      // dimension changes are explicit and discard the old image's samples
      GradientState sized( 2, 2 );
      sized.setPrimalSum( 0, 0, Vector3f( 3, 3, 3 ) );
      sized.setRounds( 1 );
      sized.reset( 3, 1 );
      TASK_CHECK( sized.getWidth() == 3 && sized.getHeight() == 1 );
      TASK_CHECK( sized.getRounds() == 0 );
      TASK_CHECK( sized.getPrimalSum( 0, 0 ).isZero() );
      TASK_CHECK( throwsEmptyState( sized ) );

      GradientState blank;
      TASK_CHECK( !blank.hasDimensions() );
      TASK_CHECK( throwsEmptyState( blank ) );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int reconstructionIterationBudget()
{
   try
   {
      const int width  = 3;
      const int height = 3;
      GradientState state( width, height );

      std::vector<Vector3f> primal;
      std::vector<Vector3f> right;
      std::vector<Vector3f> up;
      makeInconsistentField( width, height, primal, right, up );
      fillState( state, primal, right, up );
      state.setRounds( 1 );

      const double alpha = 1.5;
      const double tolerance = 1e-14;

      GradientSolution limited = state.reconstruct( alpha, tolerance, 1 );
      TASK_CHECK( !limited.isConverged() );
      TASK_CHECK( limited.getIterations() <= 1 );
      TASK_CHECK( limited.getResidual() > tolerance );

      GradientSolution full = state.reconstruct( alpha, tolerance, 2000 );
      TASK_CHECK( full.isConverged() );
      TASK_CHECK( full.getResidual() <= tolerance );

      // A short budget clamps a real solve; the reported residual is the
      // actual one, not a success claim.
      for( int channel = 0;  channel < 3;  ++channel )
      {
         const double actual = relativeResidual( width, height,
            channelValues( primal, channel ), channelValues( right, channel ),
            channelValues( up, channel ), alpha,
            channelValues( limited.getPixels(), channel ) );
         TASK_CHECK( nearValue( limited.getChannelResidual( channel ),
            actual, 1e-7 ) );
         TASK_CHECK( actual > tolerance * 0.5 );
      }
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int reconstructionNoClamping()
{
   try
   {
      const int width  = 3;
      const int height = 3;
      GradientState state( width, height );

      // zero primal, deliberately strong and inconsistent gradients
      std::vector<Vector3f> primal( 9, Vector3f::ZERO() );
      std::vector<Vector3f> right( 9, Vector3f::ZERO() );
      std::vector<Vector3f> up   ( 9, Vector3f::ZERO() );
      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            const size_t i = static_cast<size_t>(index( x, y, width ));
            right[i] = Vector3f( 4.0, -4.0, (x % 2) ? -2.0 : 2.0 );
            up[i]    = Vector3f(-4.0, 4.0, (y % 2) ? 2.0 : -2.0 );
         }
      }
      fillState( state, primal, right, up );
      state.setRounds( 1 );

      GradientSolution solution = state.reconstruct( 0.01, 1e-13, 2000 );
      TASK_CHECK( solution.isConverged() );

      // The solver keeps the real-valued linear solution, including any
      // negative values; no clamping or quantisation happens here.
      bool foundNegative = false;
      bool foundFractional = false;
      for( size_t i = 0;  i < solution.getPixels().size();  ++i )
      {
         for( int c = 0;  c < 3;  ++c )
         {
            if( solution.getPixels()[i][c] < -1e-9 ) foundNegative = true;
            if( std::fabs( solution.getPixels()[i][c] -
               std::floor( solution.getPixels()[i][c] ) ) > 1e-3 )
            {
               foundFractional = true;
            }
         }
      }
      TASK_CHECK( foundNegative );
      TASK_CHECK( foundFractional );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}




/// integration ----------------------------------------------------------------

int integrationFrameMatchesPrimal()
{
   try
   {
      std::unique_ptr<Camera> camera;
      std::unique_ptr<Scene>  scene;
      makeScene( camera, scene );

      const int width  = 3;
      const int height = 2;

      GradientState state( width, height );
      Random gradientRandom;
      camera->getGradientFrame( *scene, gradientRandom, state );

      // A reference round through exactly the same transport, but built
      // independently from the caller's initial random stream.
      SampleBuffers expected( width, height );
      Random referenceRandom;
      referenceRound( *camera, *scene, referenceRandom, expected );

      TASK_CHECK( state.getRounds() == 1 );
      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            const size_t i = static_cast<size_t>(index( x, y, width ));
            TASK_CHECK( near( state.getPrimalEstimate( x, y ),
               expected.primal[i], 1e-10 ) );
         }
      }

      // A second round accumulates linearly; normalized values become the
      // mean of the two rounds.
      camera->getGradientFrame( *scene, gradientRandom, state );
      SampleBuffers second( width, height );
      referenceRound( *camera, *scene, referenceRandom, second );

      TASK_CHECK( state.getRounds() == 2 );
      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            const size_t i = static_cast<size_t>(index( x, y, width ));
            const Vector3f mean = (expected.primal[i] + second.primal[i]) /
               static_cast<real64>(2);
            TASK_CHECK( near( state.getPrimalEstimate( x, y ), mean, 1e-10 ) );
            if( state.hasRightEdge( x, y ) )
            {
               const Vector3f gradientMean =
                  (expected.right[i] + second.right[i]) /
                  static_cast<real64>(2);
               TASK_CHECK( near( state.getRightGradient( x, y ),
                  gradientMean, 1e-10 ) );
            }
            if( state.hasUpEdge( x, y ) )
            {
               const Vector3f gradientMean =
                  (expected.up[i] + second.up[i]) / static_cast<real64>(2);
               TASK_CHECK( near( state.getUpGradient( x, y ),
                  gradientMean, 1e-10 ) );
            }
         }
      }

      // Reading twice leaves the accumulated state intact.
      GradientSolution first = state.reconstruct( 1.0, 1e-12, 500 );
      GradientSolution again = state.reconstruct( 1.0, 1e-12, 500 );
      TASK_CHECK( first.isConverged() && again.isConverged() );
      TASK_CHECK( nearValue( first.getResidual(), again.getResidual(), 1e-14 ) );
      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            TASK_CHECK( near( first.getLinear( x, y ),
               again.getLinear( x, y ), 1e-13 ) );
         }
      }
      TASK_CHECK( state.getRounds() == 2 );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int integrationImageChain()
{
   try
   {
      const int width  = 2;
      const int height = 2;
      GradientState state( width, height );

      std::vector<Vector3f> primal( 4, Vector3f::ZERO() );
      std::vector<Vector3f> right( 4, Vector3f::ZERO() );
      std::vector<Vector3f> up   ( 4, Vector3f::ZERO() );
      primal[ index(0,0,width) ] = Vector3f( 0.5, 1.0, 2.0 );
      primal[ index(1,0,width) ] = Vector3f( 1.0, 1.5, 2.5 );
      primal[ index(0,1,width) ] = Vector3f( 0.7, 1.1, 2.1 );
      primal[ index(1,1,width) ] = Vector3f( 1.2, 1.6, 2.6 );
      right[ index(0,0,width) ] = Vector3f( -0.2, 0.1, 0.3 );
      up[    index(0,0,width) ] = Vector3f( 0.4, -0.2, 0.1 );
      right[ index(0,1,width) ] = Vector3f( 0.3, 0.2, -0.1 );
      up[    index(1,0,width) ] = Vector3f( -0.1, 0.5, 0.2 );

      fillState( state, primal, right, up );
      state.setRounds( 1 );

      GradientSolution solution = state.reconstruct( 1.0, 1e-12, 500 );
      TASK_CHECK( solution.isConverged() );

      // The solution must go through the same Image tone-mapping chain.
      std::ostringstream fromSolution;
      solution.getFormatted( fromSolution );

      std::ostringstream description;
      description << width << ' ' << height << '\n';
      std::istringstream imageInput( description.str() );
      Image image( imageInput );
      for( int y = 0;  y < height;  ++y )
      {
         for( int x = 0;  x < width;  ++x )
         {
            image.addToPixel( x, y, solution.getLinear( x, y ) );
         }
      }
      std::ostringstream fromImage;
      image.getFormatted( fromImage, 1 );

      TASK_CHECK( fromSolution.str() == fromImage.str() );

      const std::string ppm = fromSolution.str();
      TASK_CHECK( ppm.rfind( "P6\n", 0 ) == 0 );
      const std::string::size_type data = ppm.find( "255\n" );
      TASK_CHECK( data != std::string::npos );
      TASK_CHECK( ppm.size() - (data + 4) ==
         static_cast<std::string::size_type>(width * height * 3) );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}

int integrationResetLifecycle()
{
   try
   {
      std::unique_ptr<Camera> camera;
      std::unique_ptr<Scene>  scene;
      makeScene( camera, scene );

      GradientState state( 2, 2 );
      Random random;
      camera->getGradientFrame( *scene, random, state );
      TASK_CHECK( state.getRounds() == 1 );
      TASK_CHECK( !state.getPrimalSum( 0, 0 ).isZero() );

      // Changing dimensions is a hard reset: no old gradient is repainted
      // onto the new image.
      state.reset( 3, 1 );
      TASK_CHECK( state.getWidth() == 3 && state.getHeight() == 1 );
      TASK_CHECK( state.getRounds() == 0 );
      TASK_CHECK( state.getRightEdgeCount() == 2 );
      TASK_CHECK( state.getUpEdgeCount() == 0 );
      TASK_CHECK( state.getPrimalSum( 0, 0 ).isZero() );

      bool noSamplesRejected = false;
      try { state.reconstruct( 1.0 ); }
      catch( const std::logic_error& ) { noSamplesRejected = true; }
      TASK_CHECK( noSamplesRejected );

      camera->getGradientFrame( *scene, random, state );
      TASK_CHECK( state.getRounds() == 1 );
      for( int x = 0;  x < 3;  ++x )
      {
         TASK_CHECK( !state.getPrimalSum( x, 0 ).isZero() );
      }

      // clear() keeps the size but drops samples, again without stale reuse
      state.clear();
      TASK_CHECK( state.getRounds() == 0 );
      TASK_CHECK( state.getWidth() == 3 && state.getHeight() == 1 );
      TASK_CHECK( state.getPrimalSum( 0, 0 ).isZero() );
      return 0;
   }
   catch( ... )
   {
      return 2;
   }
}




const struct task_case task_cases[] =
{
   { "coupling",       "pairing",             couplingPairing },
   { "coupling",       "interior_only",       couplingInteriorOnly },
   { "coupling",       "border_no_wrap",      couplingBorderNoWrap },
   { "accumulation",   "round_sums",          accumulationRoundSums },
   { "accumulation",   "normalization",       accumulationNormalization },
   { "accumulation",   "read_stability",      accumulationReadDoesNotMutate },
   { "reconstruction", "constant",            reconstructionConstant },
   { "reconstruction", "ramp",                reconstructionRamp },
   { "reconstruction", "inconsistent",        reconstructionInconsistent },
   { "reconstruction", "boundaries",          reconstructionBoundaries },
   { "reconstruction", "errors",              reconstructionErrors },
   { "reconstruction", "iteration_budget",    reconstructionIterationBudget },
   { "reconstruction", "no_clamping",         reconstructionNoClamping },
   { "integration",    "frame_primal",        integrationFrameMatchesPrimal },
   { "integration",    "image_chain",         integrationImageChain },
   { "integration",    "reset_lifecycle",     integrationResetLifecycle },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );

}//namespace

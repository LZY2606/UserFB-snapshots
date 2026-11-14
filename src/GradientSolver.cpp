/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <math.h>
#include <stdexcept>

#include "GradientState.hpp"

#include "GradientSolver.hpp"


using namespace hxa7241_minilight;




/// implementation -------------------------------------------------------------

namespace
{

/**
 * Apply the system matrix (alpha*I + L), L the 4-neighbour graph Laplacian
 * of the image rectangle (boundary pixels simply have fewer neighbours).
 */
void applyOperator
(
   const int32                 width,
   const int32                 height,
   const real64                alpha,
   const std::vector<real64>&  in,
   std::vector<real64>&        out
)
{
   for( int32 y = 0;  y < height;  ++y )
   {
      for( int32 x = 0;  x < width;  ++x )
      {
         const int32 p = x + (y * width);

         real64 v = alpha * in[ static_cast<int32u>(p) ];
         if( x > 0 )        v += in[ static_cast<int32u>(p) ] -
                                  in[ static_cast<int32u>(p - 1) ];
         if( x < width - 1 )  v += in[ static_cast<int32u>(p) ] -
                                  in[ static_cast<int32u>(p + 1) ];
         if( y > 0 )        v += in[ static_cast<int32u>(p) ] -
                                  in[ static_cast<int32u>(p - width) ];
         if( y < height - 1 ) v += in[ static_cast<int32u>(p) ] -
                                  in[ static_cast<int32u>(p + width) ];

         out[ static_cast<int32u>(p) ] = v;
      }
   }
}


real64 dot
(
   const std::vector<real64>& a,
   const std::vector<real64>& b
)
{
   real64 sum = 0.0;
   for( int32u i = 0;  i < a.size();  ++i )
   {
      sum += a[i] * b[i];
   }
   return sum;
}


/**
 * Conjugate gradients for one channel. Returns the relative residual
 * actually reached; isConverged_o is false if the cap was hit first.
 */
real64 solveChannel
(
   const int32                width,
   const int32                height,
   const real64               alpha,
   const std::vector<real64>& rhs,
   const real64               relativeTolerance,
   const int32                maxIterations,
   std::vector<real64>&       x_io,
   std::vector<real64>&       r,
   std::vector<real64>&       p,
   std::vector<real64>&       ap,
   int32&                     iterations_o,
   bool&                      isConverged_o
)
{
   const real64 bNorm = ::sqrt( dot( rhs, rhs ) );

   // a zero right-hand side has the exact solution zero
   if( !(bNorm > 0.0) )
   {
      for( int32u i = 0;  i < x_io.size();  ++i )
      {
         x_io[i] = 0.0;
      }
      iterations_o  = 0;
      isConverged_o = true;
      return 0.0;
   }

   // r = b - A x   (x starts at the primal estimate)
   applyOperator( width, height, alpha, x_io, ap );
   for( int32u i = 0;  i < r.size();  ++i )
   {
      r[i] = rhs[i] - ap[i];
      p[i] = r[i];
   }
   real64 rs = dot( r, r );

   const real64 threshold = relativeTolerance * bNorm;

   int32 k = 0;
   while( (::sqrt(rs) > threshold) & (k < maxIterations) )
   {
      applyOperator( width, height, alpha, p, ap );

      const real64 pap = dot( p, ap );
      if( !(pap > 0.0) )
      {
         break;   // should not happen for an SPD system
      }
      const real64 step = rs / pap;

      for( int32u i = 0;  i < x_io.size();  ++i )
      {
         x_io[i] += step * p[i];
         r[i]    -= step * ap[i];
      }

      const real64 rsNew = dot( r, r );
      const real64 beta  = rsNew / rs;
      for( int32u i = 0;  i < p.size();  ++i )
      {
         p[i] = r[i] + (beta * p[i]);
      }

      rs = rsNew;
      ++k;
   }

   iterations_o  = k;
   isConverged_o = ::sqrt(rs) <= threshold;
   return ::sqrt(rs) / bNorm;
}

}




/// queries --------------------------------------------------------------------

GradientSolver::Result GradientSolver::solve
(
   const GradientState&   state,
   const real64           alpha,
   const real64           relativeTolerance,
   const int32            maxIterations,
   std::vector<Vector3f>& image_o
)
{
   // validate parameters
   {
      using std::isfinite;
      if( !(alpha > 0.0) || !isfinite(alpha) )
      {
         throw std::invalid_argument(
            "GradientSolver alpha must be positive and finite" );
      }
      if( !(relativeTolerance > 0.0) || !isfinite(relativeTolerance) )
      {
         throw std::invalid_argument(
            "GradientSolver relative tolerance must be positive and finite" );
      }
      if( maxIterations < 1 )
      {
         throw std::invalid_argument(
            "GradientSolver max iterations must be at least 1" );
      }
      if( state.getRounds() < 1 )
      {
         throw std::logic_error(
            "GradientSolver state has no completed rounds" );
      }
   }

   const int32 width  = state.getWidth();
   const int32 height = state.getHeight();
   const int32u size = static_cast<int32u>(width * height);

   // scratch, O(pixels)
   std::vector<real64> rhs( size );
   std::vector<real64> solution( size );
   std::vector<real64> r( size );
   std::vector<real64> p( size );
   std::vector<real64> ap( size );

   image_o.assign( size, Vector3f::ZERO() );

   // solved values, one vector per channel
   std::vector<real64> channels[3];

   Result result;
   result.residual    = 0.0;
   result.iterations  = 0;
   result.isConverged = true;

   // one channel at a time
   for( int32 channel = 0;  channel < 3;  ++channel )
   {
      // right-hand side: alpha*P + div(G)
      for( int32 y = 0;  y < height;  ++y )
      {
         for( int32 x = 0;  x < width;  ++x )
         {
            const int32u i = static_cast<int32u>(x + (y * width));

            real64 b = alpha * state.getPrimalMean( x, y )[channel];
            if( state.hasRight( x, y ) )
            {
               b -= state.getGradientRightMean( x, y )[channel];
            }
            if( state.hasRight( x - 1, y ) )
            {
               b += state.getGradientRightMean( x - 1, y )[channel];
            }
            if( state.hasUp( x, y ) )
            {
               b -= state.getGradientUpMean( x, y )[channel];
            }
            if( state.hasUp( x, y - 1 ) )
            {
               b += state.getGradientUpMean( x, y - 1 )[channel];
            }

            rhs[i] = b;

            // start from the primal estimate
            solution[i] = state.getPrimalMean( x, y )[channel];
         }
      }

      int32 iterations  = 0;
      bool  isConverged = false;
      const real64 residual = solveChannel( width, height, alpha, rhs,
         relativeTolerance, maxIterations, solution, r, p, ap, iterations,
         isConverged );

      result.residual    = residual > result.residual ? residual :
         result.residual;
      result.iterations  = iterations > result.iterations ? iterations :
         result.iterations;
      result.isConverged = result.isConverged & isConverged;

      channels[channel] = solution;
   }

   for( int32u i = 0;  i < size;  ++i )
   {
      image_o[i] = Vector3f( channels[0][i], channels[1][i],
         channels[2][i] );
   }

   return result;
}

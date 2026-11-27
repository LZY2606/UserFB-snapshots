/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   Gradient-domain progressive sampling and Poisson reconstruction.

------------------------------------------------------------------------------*/


#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "Image.hpp"

#include "GradientState.hpp"


using namespace hxa7241_minilight;




/// constants ------------------------------------------------------------------

namespace
{
   const real64 MIN_ALPHA = 0.0;

   int32 clampDimension( const int32 value )
   {
      return value < 1 ? 0 : (value > Image::DIMENSION_MAX() ? 0 : value);
   }
}




/// GradientSolution ------------------------------------------------------------

GradientSolution::GradientSolution()
 : width_m     ( 0 )
 , height_m    ( 0 )
 , residual_m  ( 0.0 )
 , iterations_m( 0 )
 , converged_m ( false )
{
   residuals_m[0] = 0.0;
   residuals_m[1] = 0.0;
   residuals_m[2] = 0.0;
}


int32 GradientSolution::getWidth() const
{
   return width_m;
}


int32 GradientSolution::getHeight() const
{
   return height_m;
}


int32 GradientSolution::getIterations() const
{
   return iterations_m;
}


real64 GradientSolution::getResidual() const
{
   return residual_m;
}


real64 GradientSolution::getChannelResidual( const int32 channel ) const
{
   return (channel >= 0) && (channel < 3) ? residuals_m[channel] : -1.0;
}


bool GradientSolution::isConverged() const
{
   return converged_m;
}


const std::vector<Vector3f>& GradientSolution::getPixels() const
{
   return values_m;
}


Vector3f GradientSolution::getLinear
(
   const int32 x,
   const int32 y
) const
{
   if( (x >= 0) && (x < width_m) && (y >= 0) && (y < height_m) )
   {
      return values_m[ static_cast<int32u>(x + (y * width_m)) ];
   }

   return Vector3f::ZERO();
}


void GradientSolution::getFormatted
(
   std::ostream& out
) const
{
   if( (width_m >= 1) && (height_m >= 1) )
   {
      // re-use the normal Image tone-mapping/formatting chain
      std::ostringstream imageDescription;
      imageDescription << width_m << ' ' << height_m << '\n';

      std::istringstream imageInput( imageDescription.str() );
      Image image( imageInput );

      for( int32 y = 0;  y < height_m;  ++y )
      {
         for( int32 x = 0;  x < width_m;  ++x )
         {
            image.addToPixel( x, y, getLinear( x, y ) );
         }
      }

      // no accumulation happens inside Image: divider of 1
      image.getFormatted( out, 1 );
   }
}




/// GradientState ---------------------------------------------------------------

GradientState::GradientState()
 : width_m      ( 0 )
 , height_m     ( 0 )
 , rounds_m     ( 0 )
 , primal_m     ()
 , rightGradient_m()
 , upGradient_m ()
{
}


GradientState::GradientState
(
   const int32 width,
   const int32 height
)
 : width_m      ( 0 )
 , height_m     ( 0 )
 , rounds_m     ( 0 )
 , primal_m     ()
 , rightGradient_m()
 , upGradient_m ()
{
   reset( width, height );
}




/// commands -------------------------------------------------------------------

void GradientState::reset
(
   const int32 width,
   const int32 height
)
{
   const int32 w = clampDimension( width );
   const int32 h = clampDimension( height );

   width_m  = w;
   height_m = h;
   rounds_m = 0;

   const int32u count = static_cast<int32u>(w) * static_cast<int32u>(h);
   primal_m        .assign( count, Vector3f::ZERO() );
   rightGradient_m .assign( count, Vector3f::ZERO() );
   upGradient_m    .assign( count, Vector3f::ZERO() );
}


void GradientState::clear()
{
   const int32u count = static_cast<int32u>(width_m) *
      static_cast<int32u>(height_m);

   rounds_m = 0;

   primal_m        .assign( count, Vector3f::ZERO() );
   rightGradient_m .assign( count, Vector3f::ZERO() );
   upGradient_m    .assign( count, Vector3f::ZERO() );
}


void GradientState::accumulatePrimal
(
   const int32     x,
   const int32     y,
   const Vector3f& radiance
)
{
   if( (x >= 0) && (x < width_m) && (y >= 0) && (y < height_m) )
   {
      const int32u i = static_cast<int32u>(x + (y * width_m));
      primal_m[i] = primal_m[i] + radiance;
   }
}


void GradientState::accumulateRightGradient
(
   const int32     x,
   const int32     y,
   const Vector3f& gradient
)
{
   if( hasRightEdge( x, y ) )
   {
      const int32u i = static_cast<int32u>(x + (y * width_m));
      rightGradient_m[i] = rightGradient_m[i] + gradient;
   }
}


void GradientState::accumulateUpGradient
(
   const int32     x,
   const int32     y,
   const Vector3f& gradient
)
{
   if( hasUpEdge( x, y ) )
   {
      const int32u i = static_cast<int32u>(x + (y * width_m));
      upGradient_m[i] = upGradient_m[i] + gradient;
   }
}


void GradientState::completeRound()
{
   if( hasDimensions() )
   {
      ++rounds_m;
   }
}


void GradientState::setPrimalSum
(
   const int32     x,
   const int32     y,
   const Vector3f& sum
)
{
   if( (x >= 0) && (x < width_m) && (y >= 0) && (y < height_m) )
   {
      primal_m[ static_cast<int32u>(x + (y * width_m)) ] = sum;
   }
}


void GradientState::setRightGradientSum
(
   const int32     x,
   const int32     y,
   const Vector3f& sum
)
{
   if( hasRightEdge( x, y ) )
   {
      rightGradient_m[ static_cast<int32u>(x + (y * width_m)) ] = sum;
   }
}


void GradientState::setUpGradientSum
(
   const int32     x,
   const int32     y,
   const Vector3f& sum
)
{
   if( hasUpEdge( x, y ) )
   {
      upGradient_m[ static_cast<int32u>(x + (y * width_m)) ] = sum;
   }
}


void GradientState::setRounds
(
   const int32 rounds
)
{
   rounds_m = rounds >= 0 ? rounds : 0;
}




/// queries --------------------------------------------------------------------

int32 GradientState::getWidth() const
{
   return width_m;
}


int32 GradientState::getHeight() const
{
   return height_m;
}


int32 GradientState::getRounds() const
{
   return rounds_m;
}


int32 GradientState::getRoundCount() const
{
   return rounds_m;
}


bool GradientState::hasDimensions() const
{
   return (width_m >= 1) && (height_m >= 1);
}


bool GradientState::isEmpty() const
{
   return (rounds_m <= 0);
}


bool GradientState::hasSamples() const
{
   return hasDimensions() && (rounds_m > 0);
}


const std::vector<Vector3f>& GradientState::getPrimalSums() const
{
   return primal_m;
}


const std::vector<Vector3f>& GradientState::getRightGradientSums() const
{
   return rightGradient_m;
}


const std::vector<Vector3f>& GradientState::getUpGradientSums() const
{
   return upGradient_m;
}


Vector3f GradientState::getPrimalSum
(
   const int32 x,
   const int32 y
) const
{
   if( (x >= 0) && (x < width_m) && (y >= 0) && (y < height_m) )
   {
      return primal_m[ static_cast<int32u>(x + (y * width_m)) ];
   }

   return Vector3f::ZERO();
}


Vector3f GradientState::getRightGradientSum
(
   const int32 x,
   const int32 y
) const
{
   if( hasRightEdge( x, y ) )
   {
      return rightGradient_m[ static_cast<int32u>(x + (y * width_m)) ];
   }

   return Vector3f::ZERO();
}


Vector3f GradientState::getUpGradientSum
(
   const int32 x,
   const int32 y
) const
{
   if( hasUpEdge( x, y ) )
   {
      return upGradient_m[ static_cast<int32u>(x + (y * width_m)) ];
   }

   return Vector3f::ZERO();
}


Vector3f GradientState::getPrimalEstimate
(
   const int32 x,
   const int32 y
) const
{
   if( !hasSamples() )
   {
      return Vector3f::ZERO();
   }

   return getPrimalSum( x, y ) / static_cast<real64>(rounds_m);
}


Vector3f GradientState::getRightGradient
(
   const int32 x,
   const int32 y
) const
{
   if( !hasSamples() )
   {
      return Vector3f::ZERO();
   }

   return getRightGradientSum( x, y ) / static_cast<real64>(rounds_m);
}


Vector3f GradientState::getUpGradient
(
   const int32 x,
   const int32 y
) const
{
   if( !hasSamples() )
   {
      return Vector3f::ZERO();
   }

   return getUpGradientSum( x, y ) / static_cast<real64>(rounds_m);
}


bool GradientState::hasRightEdge
(
   const int32 x,
   const int32 y
) const
{
   return (x >= 0) && (x + 1 < width_m) && (y >= 0) && (y < height_m);
}


bool GradientState::hasUpEdge
(
   const int32 x,
   const int32 y
) const
{
   return (x >= 0) && (x < width_m) && (y >= 0) && (y + 1 < height_m);
}


int32 GradientState::getRightEdgeCount() const
{
   return (width_m >= 1) && (height_m >= 1) ?
      (width_m - 1) * height_m : 0;
}


int32 GradientState::getUpEdgeCount() const
{
   return (width_m >= 1) && (height_m >= 1) ?
      width_m * (height_m - 1) : 0;
}


int32 GradientState::getEdgeCount() const
{
   return getRightEdgeCount() + getUpEdgeCount();
}


GradientSolution GradientState::reconstruct
(
   const real64 alpha,
   const real64 relativeTolerance,
   const int32  maxIterations
) const
{
   if( !hasDimensions() )
   {
      throw std::invalid_argument( "gradient state has no image dimensions" );
   }

   if( rounds_m < 1 )
   {
      throw std::logic_error( "gradient state has no completed sampling round" );
   }

   // round-count normalized arrays
   const real64 divider = 1.0 / static_cast<real64>( rounds_m );

   std::vector<Vector3f> primal       ( primal_m.size(),        Vector3f::ZERO() );
   std::vector<Vector3f> rightGradient( rightGradient_m.size(), Vector3f::ZERO() );
   std::vector<Vector3f> upGradient   ( upGradient_m.size(),    Vector3f::ZERO() );

   for( int32u i = 0;  i < primal_m.size();  ++i )
   {
      primal[i]        = primal_m[i]        * divider;
      rightGradient[i] = rightGradient_m[i] * divider;
      upGradient[i]    = upGradient_m[i]    * divider;
   }

   return solveGradientPoisson( width_m, height_m, primal, rightGradient,
      upGradient, alpha, relativeTolerance, maxIterations );
}


GradientSolution GradientState::getReconstruction
(
   const real64 alpha,
   const real64 relativeTolerance,
   const int32  maxIterations
) const
{
   return reconstruct( alpha, relativeTolerance, maxIterations );
}




/// solver ---------------------------------------------------------------------

namespace
{
   void applyOperator
   (
      const int32             width,
      const int32             height,
      const real64            alpha,
      const std::vector<real64>& x,
      std::vector<real64>&    y_o
   )
   {
      for( int32 py = 0;  py < height;  ++py )
      {
         const int32u row = static_cast<int32u>(py) * static_cast<int32u>(width);

         for( int32 px = 0;  px < width;  ++px )
         {
            const int32u i = row + static_cast<int32u>(px);

            real64 sum = alpha * x[i];

            if( px > 0 )
            {
               sum += x[i] - x[i - 1];
            }
            if( px + 1 < width )
            {
               sum += x[i] - x[i + 1];
            }
            if( py > 0 )
            {
               sum += x[i] - x[i - static_cast<int32u>(width)];
            }
            if( py + 1 < height )
            {
               sum += x[i] - x[i + static_cast<int32u>(width)];
            }

            y_o[i] = sum;
         }
      }
   }


   void applyPreconditioner
   (
      const int32                width,
      const int32                height,
      const real64               alpha,
      const std::vector<real64>& r,
      std::vector<real64>&       z_o
   )
   {
      for( int32 py = 0;  py < height;  ++py )
      {
         const int32u row = static_cast<int32u>(py) * static_cast<int32u>(width);

         for( int32 px = 0;  px < width;  ++px )
         {
            const int32u i = row + static_cast<int32u>(px);

            const real64 degree = static_cast<real64>(
               (px > 0 ? 1 : 0) + (px + 1 < width ? 1 : 0) +
               (py > 0 ? 1 : 0) + (py + 1 < height ? 1 : 0));

            const real64 diagonal = alpha + degree;

            z_o[i] = diagonal > 0.0 ? r[i] / diagonal : r[i];
         }
      }
   }


   real64 dotProduct
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


   real64 norm
   (
      const std::vector<real64>& a
   )
   {
      return ::sqrt( dotProduct( a, a ) );
   }
}




/// free solver ----------------------------------------------------------------

namespace hxa7241_minilight
{

GradientSolution solveGradientPoisson
(
   const int32 width,
   const int32 height,
   const std::vector<Vector3f>& primal,
   const std::vector<Vector3f>& rightGradient,
   const std::vector<Vector3f>& upGradient,
   const real64 alpha,
   const real64 relativeTolerance,
   const int32  maxIterations
)
{
   GradientSolution result;

   if( (width < 1) || (height < 1) ||
      (width > Image::DIMENSION_MAX()) || (height > Image::DIMENSION_MAX()) )
   {
      throw std::invalid_argument( "invalid reconstruction dimensions" );
   }

   const int32u count = static_cast<int32u>(width) * static_cast<int32u>(height);
   if( (primal.size() != count) || (rightGradient.size() != count) ||
      (upGradient.size() != count) )
   {
      throw std::invalid_argument( "reconstruction arrays do not match dimensions" );
   }

   if( !(alpha > MIN_ALPHA) || !std::isfinite( alpha ) )
   {
      throw std::invalid_argument( "alpha must be a positive finite number" );
   }

   if( !(relativeTolerance > 0.0) || !std::isfinite( relativeTolerance ) )
   {
      throw std::invalid_argument(
         "relative tolerance must be positive and finite" );
   }

   if( maxIterations < 1 )
   {
      throw std::invalid_argument( "maximum iterations must be at least one" );
   }

   result.width_m  = width;
   result.height_m = height;
   result.values_m.assign( count, Vector3f::ZERO() );

   real64 worstResidual  = 0.0;
   bool   allConverged   = true;
   int32  totalIterations = 0;

   for( int32 channel = 0;  channel < 3;  ++channel )
   {
      // assemble right-hand side: alpha*P - div(G)
      std::vector<real64> b( count, 0.0 );
      for( int32u i = 0;  i < count;  ++i )
      {
         b[i] = alpha * primal[i][channel];
      }

      for( int32 py = 0;  py < height;  ++py )
      {
         const int32u row = static_cast<int32u>(py) *
            static_cast<int32u>(width);

         for( int32 px = 0;  px < width;  ++px )
         {
            const int32u i = row + static_cast<int32u>(px);

            // edge (px,py)-(px+1,py), gradient stored at (px,py)
            if( px + 1 < width )
            {
               const real64 g = rightGradient[i][channel];
               b[i]     -= g;
               b[i + 1] += g;
            }
            // edge (px,py)-(px,py+1), gradient stored at (px,py)
            if( py + 1 < height )
            {
               const real64 g = upGradient[i][channel];
               b[i] -= g;
               b[i + static_cast<int32u>(width)] += g;
            }
         }
      }

      // initial guess: the untouched primal image
      std::vector<real64> x( count, 0.0 );
      for( int32u i = 0;  i < count;  ++i )
      {
         x[i] = primal[i][channel];
      }

      const real64 bNorm = norm( b );

      std::vector<real64> applied( count, 0.0 );
      applyOperator( width, height, alpha, x, applied );

      std::vector<real64> residual( count, 0.0 );
      for( int32u i = 0;  i < count;  ++i )
      {
         residual[i] = b[i] - applied[i];
      }

      std::vector<real64> z( count, 0.0 );
      applyPreconditioner( width, height, alpha, residual, z );

      std::vector<real64> direction( z );
      real64 rz = dotProduct( residual, z );

      int32 iterations = 0;
      bool  converged  = false;

      for( int32 iteration = 0;  iteration < maxIterations;  ++iteration )
      {
         const real64 residualNorm = norm( residual );
         const real64 relative = (bNorm > 0.0) ?
            (residualNorm / bNorm) : residualNorm;

         if( relative <= relativeTolerance )
         {
            converged = true;
            break;
         }

         applyOperator( width, height, alpha, direction, applied );
         const real64 directionEnergy = dotProduct( direction, applied );

         if( !(directionEnergy > 0.0) || !std::isfinite( directionEnergy ) )
         {
            break;
         }

         const real64 step = rz / directionEnergy;

         for( int32u i = 0;  i < count;  ++i )
         {
            x[i]         += step * direction[i];
            residual[i]  -= step * applied[i];
         }

         applyPreconditioner( width, height, alpha, residual, z );

         const real64 rzNew = dotProduct( residual, z );
         const real64 beta  = (rz > 0.0) ? (rzNew / rz) : 0.0;

         for( int32u i = 0;  i < count;  ++i )
         {
            direction[i] = z[i] + (beta * direction[i]);
         }

         rz = rzNew;
         ++iterations;
      }

      // final residual, including the case where the last step converged
      {
         const real64 residualNorm = norm( residual );
         const real64 relative = (bNorm > 0.0) ?
            (residualNorm / bNorm) : residualNorm;

         if( relative <= relativeTolerance )
         {
            converged = true;
         }

         result.residuals_m[channel] = relative;

         if( relative > worstResidual )
         {
            worstResidual = relative;
         }
      }

      if( !converged )
      {
         allConverged = false;
      }

      if( iterations > totalIterations )
      {
         totalIterations = iterations;
      }

      // write this channel into the packed result
      for( int32u i = 0;  i < count;  ++i )
      {
         result.values_m[i] = result.values_m[i] + Vector3f(
            (0 == channel ? x[i] : 0.0),
            (1 == channel ? x[i] : 0.0),
            (2 == channel ? x[i] : 0.0) );
      }
   }

   result.residual_m   = worstResidual;
   result.iterations_m = totalIterations;
   result.converged_m  = allConverged;

   return result;
}

}//namespace

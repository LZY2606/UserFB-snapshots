/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <stdexcept>

#include "GradientState.hpp"


using namespace hxa7241_minilight;




/// standard object services ---------------------------------------------------

GradientState::GradientState
(
   const int32 width,
   const int32 height
)
 : width_m        ( width )
 , height_m       ( height )
 , rounds_m       ( 0 )
 , pending_m      ( 0 )
 , isRoundActive_m( false )
 , primalSums_m   ()
 , rightSums_m    ()
 , upSums_m       ()
{
   if( (width < 1) | (height < 1) )
   {
      throw std::invalid_argument(
         "GradientState dimensions must be positive" );
   }

   const int32u size = static_cast<int32u>(width_m * height_m);
   primalSums_m.resize( size );
   rightSums_m.resize( size );
   upSums_m.resize( size );
}




/// commands -------------------------------------------------------------------

void GradientState::beginRound()
{
   if( isRoundActive_m )
   {
      throw std::logic_error( "GradientState round already in progress" );
   }

   isRoundActive_m = true;
   pending_m       = 0;
}


void GradientState::addPrimal
(
   const int32     x,
   const int32     y,
   const Vector3f& radiance
)
{
   if( !isRoundActive_m )
   {
      throw std::logic_error( "GradientState has no active round" );
   }

   const int32u i = static_cast<int32u>(index( x, y ));
   primalSums_m[i] = primalSums_m[i] + radiance;
   ++pending_m;
}


void GradientState::addGradientRight
(
   const int32     x,
   const int32     y,
   const Vector3f& gradient
)
{
   if( !isRoundActive_m )
   {
      throw std::logic_error( "GradientState has no active round" );
   }
   if( !hasRight( x, y ) )
   {
      throw std::out_of_range( "GradientState has no right edge there" );
   }

   const int32u i = static_cast<int32u>(index( x, y ));
   rightSums_m[i] = rightSums_m[i] + gradient;
}


void GradientState::addGradientUp
(
   const int32     x,
   const int32     y,
   const Vector3f& gradient
)
{
   if( !isRoundActive_m )
   {
      throw std::logic_error( "GradientState has no active round" );
   }
   if( !hasUp( x, y ) )
   {
      throw std::out_of_range( "GradientState has no up edge there" );
   }

   const int32u i = static_cast<int32u>(index( x, y ));
   upSums_m[i] = upSums_m[i] + gradient;
}


void GradientState::completeRound()
{
   if( !isRoundActive_m )
   {
      throw std::logic_error( "GradientState has no active round" );
   }
   if( pending_m != width_m * height_m )
   {
      throw std::logic_error( "GradientState round is incomplete" );
   }

   isRoundActive_m = false;
   ++rounds_m;
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


bool GradientState::hasRight
(
   const int32 x,
   const int32 y
) const
{
   return (x >= 0) & (x < width_m - 1) & (y >= 0) & (y < height_m);
}


bool GradientState::hasUp
(
   const int32 x,
   const int32 y
) const
{
   return (x >= 0) & (x < width_m) & (y >= 0) & (y < height_m - 1);
}


Vector3f GradientState::getPrimalSum
(
   const int32 x,
   const int32 y
) const
{
   return primalSums_m[ static_cast<int32u>(index( x, y )) ];
}


Vector3f GradientState::getPrimalMean
(
   const int32 x,
   const int32 y
) const
{
   if( rounds_m < 1 )
   {
      throw std::logic_error( "GradientState has no completed rounds" );
   }

   return primalSums_m[ static_cast<int32u>(index( x, y )) ] /
      static_cast<real64>(rounds_m);
}


Vector3f GradientState::getGradientRightSum
(
   const int32 x,
   const int32 y
) const
{
   if( !hasRight( x, y ) )
   {
      throw std::out_of_range( "GradientState has no right edge there" );
   }

   return rightSums_m[ static_cast<int32u>(index( x, y )) ];
}


Vector3f GradientState::getGradientRightMean
(
   const int32 x,
   const int32 y
) const
{
   if( rounds_m < 1 )
   {
      throw std::logic_error( "GradientState has no completed rounds" );
   }

   return getGradientRightSum( x, y ) / static_cast<real64>(rounds_m);
}


Vector3f GradientState::getGradientUpSum
(
   const int32 x,
   const int32 y
) const
{
   if( !hasUp( x, y ) )
   {
      throw std::out_of_range( "GradientState has no up edge there" );
   }

   return upSums_m[ static_cast<int32u>(index( x, y )) ];
}


Vector3f GradientState::getGradientUpMean
(
   const int32 x,
   const int32 y
) const
{
   if( rounds_m < 1 )
   {
      throw std::logic_error( "GradientState has no completed rounds" );
   }

   return getGradientUpSum( x, y ) / static_cast<real64>(rounds_m);
}




/// implementation -------------------------------------------------------------

int32 GradientState::index
(
   const int32 x,
   const int32 y
) const
{
   if( (x < 0) | (x >= width_m) | (y < 0) | (y >= height_m) )
   {
      throw std::out_of_range( "GradientState pixel coordinate" );
   }

   return x + (y * width_m);
}

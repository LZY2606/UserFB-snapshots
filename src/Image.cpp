/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <math.h>
#include <iostream>
#include <stdexcept>

#include "Image.hpp"


using namespace hxa7241_minilight;




/// constants ------------------------------------------------------------------

namespace
{
   // format items
   const char MINILIGHT_URI[] = "http://www.hxa.name/minilight";

   // guess of average screen maximum brightness
   const real64 DISPLAY_LUMINANCE_MAX = 200.0;

   // ITU-R BT.709 standard RGB luminance weighting
   const Vector3f RGB_LUMINANCE( 0.2126, 0.7152, 0.0722 );

   // ITU-R BT.709 standard gamma
   const real64 GAMMA_ENCODE = 0.45;
}




/// standard object services ---------------------------------------------------

Image::Image
(
   std::istream& in
)
 : adaptive_m( false )
{
   // read width and height
   in >> width_m >> height_m;

   // condition width and height
   width_m  = width_m  < 1 ? 1 :
      (width_m  > DIMENSION_MAX() ? DIMENSION_MAX() : width_m );
   height_m = height_m < 1 ? 1 :
      (height_m > DIMENSION_MAX() ? DIMENSION_MAX() : height_m);

   pixels_m.resize( static_cast<int32u>(width_m * height_m) );
}


Image Image::createAdaptive
(
   const int32 width,
   const int32 height
)
{
   return Image( width, height, true );
}


Image::Image
(
   const int32 width,
   const int32 height,
   const bool  adaptive
)
 : width_m  ( width  < 1 ? 1 :
              (width  > DIMENSION_MAX() ? DIMENSION_MAX() : width ) )
 , height_m ( height < 1 ? 1 :
              (height > DIMENSION_MAX() ? DIMENSION_MAX() : height) )
 , adaptive_m( adaptive )
{
   const int32u pixelCount = static_cast<int32u>(width_m * height_m);
   pixels_m.resize( pixelCount );
   if( adaptive_m )
   {
      sampleCounts_m.resize( pixelCount, 0u );
   }
}




/// commands -------------------------------------------------------------------

void Image::addToPixel
(
   const int32     x,
   const int32     y,
   const Vector3f& radiance
)
{
   if( adaptive_m )
   {
      // one Image must not mix the two accumulation schemes
      throw std::logic_error(
         "Image::addToPixel: adaptive images use addAdaptiveSample" );
   }

   if( (x >= 0) & (x < width_m) & (y >= 0) & (y < height_m) )
   {
      const int32u index = static_cast<int32u>(
         x + ((height_m - 1 - y) * width_m));
      pixels_m[ index ] = pixels_m[ index ] + radiance;
   }
}


void Image::addAdaptiveSample
(
   const int32     x,
   const int32     y,
   const Vector3f& radiance
)
{
   if( !adaptive_m )
   {
      throw std::logic_error(
         "Image::addAdaptiveSample: legacy images use addToPixel" );
   }

   if( (x >= 0) & (x < width_m) & (y >= 0) & (y < height_m) )
   {
      const int32u index = static_cast<int32u>(
         x + ((height_m - 1 - y) * width_m));

      // running mean of the production samples; pilot radiance never arrives
      const int32u previous = sampleCounts_m[index];
      const int32u next     = previous + 1u;
      const real64  weight   = 1.0 / static_cast<real64>(next);

      pixels_m[index] = (pixels_m[index] * static_cast<real64>(previous) +
         radiance) * weight;
      sampleCounts_m[index] = next;
   }
}


void Image::resetAdaptive()
{
   if( !adaptive_m )
   {
      throw std::logic_error(
         "Image::resetAdaptive: legacy images have no adaptive accumulator" );
   }

   for( int32u i = 0;  i < pixels_m.size();  ++i )
   {
      pixels_m[i]       = Vector3f::ZERO();
      sampleCounts_m[i] = 0u;
   }
}




/// queries --------------------------------------------------------------------

int32 Image::getWidth() const
{
   return width_m;
}


int32 Image::getHeight() const
{
   return height_m;
}


bool Image::isAdaptive() const
{
   return adaptive_m;
}


int32u Image::getPixelSampleCount
(
   const int32 x,
   const int32 y
) const
{
   if( !adaptive_m || (x < 0) || (x >= width_m) || (y < 0) ||
      (y >= height_m) )
   {
      return 0u;
   }

   const int32u index = static_cast<int32u>(
      x + ((height_m - 1 - y) * width_m));
   return sampleCounts_m[index];
}


Vector3f Image::getPixelMean
(
   const int32 x,
   const int32 y
) const
{
   if( !adaptive_m || (x < 0) || (x >= width_m) || (y < 0) ||
      (y >= height_m) )
   {
      return Vector3f::ZERO();
   }

   // the accumulator already holds the running mean (zero when unsampled)
   const int32u index = static_cast<int32u>(
      x + ((height_m - 1 - y) * width_m));
   return pixels_m[index];
}


void Image::getFormatted
(
   std::ostream& out,
   const int32   iteration
) const
{
   if( adaptive_m )
   {
      throw std::logic_error(
         "Image::getFormatted: adaptive images use getAdaptiveFormatted" );
   }

   // make pixel value accumulation divider
   const real64 divider = 1.0 / static_cast<real64>(
      iteration >= 1 ? iteration : 1);

   const real64 tonemapScaling = calculateToneMapping( pixels_m, divider );

   // write PPM P6 format

   // write header
   {
      // write ID and comment
      out << "P6" << '\n' << "# " << MINILIGHT_URI << "\n\n";

      // write width, height, maxval
      out << width_m <<  ' ' << height_m << '\n' << 255 << '\n';
   }

   // write pixels
   for( int32u i = 0;  i < pixels_m.size();  ++i )
   {
      for( int c = 0;  c < 3;  ++c )
      {
         // tonemap
         const real64 mapped = pixels_m[i][c] * divider * tonemapScaling;

         // gamma encode
         const real64 gammaed = ::pow( (mapped > 0.0 ? mapped : 0.0),
            GAMMA_ENCODE );

         // quantize
         const real64 quantized = ::floor( (gammaed * 255.0) + 0.5 );

         // output as byte
         out << static_cast<byteu>( quantized <= 255.0 ? quantized : 255.0 );
      }
   }
}


void Image::getAdaptiveFormatted
(
   std::ostream& out
) const
{
   if( !adaptive_m )
   {
      throw std::logic_error(
         "Image::getAdaptiveFormatted: legacy images use getFormatted" );
   }

   // adaptation and channel values come straight from the per-pixel means,
   // which already incorporate each pixel's own production count -- there is
   // no uniform per-iteration divider here.
   const real64 tonemapScaling =
      calculateToneMappingFromMeans( pixels_m );

   // write PPM P6 format

   // write header
   {
      out << "P6" << '\n' << "# " << MINILIGHT_URI << "\n\n";
      out << width_m <<  ' ' << height_m << '\n' << 255 << '\n';
   }

   // write pixels
   for( int32u i = 0;  i < pixels_m.size();  ++i )
   {
      for( int c = 0;  c < 3;  ++c )
      {
         const real64 mapped = pixels_m[i][c] * tonemapScaling;

         const real64 gammaed = ::pow( (mapped > 0.0 ? mapped : 0.0),
            GAMMA_ENCODE );

         const real64 quantized = ::floor( (gammaed * 255.0) + 0.5 );

         out << static_cast<byteu>( quantized <= 255.0 ? quantized : 255.0 );
      }
   }
}


/**
 * For HDRI output.
 */
/*void Image::getFormatted
(
   std::ostream& out,
   const int32   iteration
) const
{
   // write Radiance RGBE format

   // make pixel value accumulation divider
   const real64 divider = 1.0 / static_cast<real64>(
      iteration >= 1 ? iteration : 1);

   // write header
   {
      // write ID
      out << "#?RADIANCE" << '\n';

      // write other header things
      out << "FORMAT=32-bit_rgbe" << '\n';
      out << "SOFTWARE=" << MINILIGHT_URI << '\n' << '\n';

      // write width, height
      out << "-Y " << height_m << " +X " << width_m << '\n';
   }

   // write pixels
   for( int32u i = 0;  i < pixels_m.size();  ++i )
   {
      const int32u rgbe = toRgbe( pixels_m[i] * divider );

      // write rgbe bytes
      for( int b = 4;  b-- > 0; )
      {
         out << static_cast<byteu>( (rgbe >> (b * 8)) & 0xFFu );
      }
   }
}*/




/// constants ------------------------------------------------------------------

int32 Image::DIMENSION_MAX()
{
   return 4000;
}




/// implementation -------------------------------------------------------------

real64 Image::calculateToneMapping
(
   const std::vector<Vector3f>& pixels,
   const real64                 divider
)
{
   // calculate estimate of world-adaptation luminance
   // as log mean luminance of scene
   real64 adaptLuminance = 1e-4;
   {
      real64 sumOfLogs = 0.0;
      for( int32u i = 0;  i < pixels.size();  ++i )
      {
         const real64 Y = pixels[i].dot( RGB_LUMINANCE ) * divider;
         // clamp luminance to a perceptual minimum
         sumOfLogs += ::log10( Y > 1e-4 ? Y : 1e-4 );
      }

      adaptLuminance = ::pow( 10.0, sumOfLogs /
         static_cast<real64>(pixels.size()) );
   }

   // make scale-factor from:
   // ratio of minimum visible differences in luminance, in display-adapted
   // and world-adapted perception (discluding the constant that cancelled),
   // divided by display max to yield a [0,1] range
   const real64 a = 1.219 + ::pow( DISPLAY_LUMINANCE_MAX * 0.25, 0.4 );
   const real64 b = 1.219 + ::pow( adaptLuminance, 0.4 );

   return ::pow( a / b, 2.5 ) / DISPLAY_LUMINANCE_MAX;
}


real64 Image::calculateToneMappingFromMeans
(
   const std::vector<Vector3f>& means
)
{
   // identical Ward adaptation to calculateToneMapping(), but the pixel
   // entries are already per-pixel means, so no uniform divider is applied.
   real64 adaptLuminance = 1e-4;
   {
      real64 sumOfLogs = 0.0;
      for( int32u i = 0;  i < means.size();  ++i )
      {
         const real64 Y = means[i].dot( RGB_LUMINANCE );
         sumOfLogs += ::log10( Y > 1e-4 ? Y : 1e-4 );
      }

      adaptLuminance = ::pow( 10.0, sumOfLogs /
         static_cast<real64>(means.size()) );
   }

   const real64 a = 1.219 + ::pow( DISPLAY_LUMINANCE_MAX * 0.25, 0.4 );
   const real64 b = 1.219 + ::pow( adaptLuminance, 0.4 );

   return ::pow( a / b, 2.5 ) / DISPLAY_LUMINANCE_MAX;
}


/**
 * For HDRI output.
 */
/*int32u Image::toRgbe
(
   const Vector3f& rgbIn
)
{
   int32u rgbe = 0;

   const Vector3f rgb( rgbIn.getClamped( Vector3f::ZERO(), Vector3f::MAX() ) );
   const real64 rgbLargest = (rgb[0] >= rgb[1]) ? (rgb[0] >= rgb[2] ? rgb[0] :
      rgb[2]) : (rgb[1] >= rgb[2] ? rgb[1] : rgb[2]);

   if( rgbLargest >= 1e-9 )
   {
      int32  exponentLargest = 0;
      real64 mantissaLargest = ::frexp( rgbLargest, &exponentLargest );

      // has been needed in the past, sadly...
      //if( 1.0 == mantissaLargest )
      //{
      //   mantissaLargest = 0.5;
      //   exponentLargest++;
      //}

      const real64 amount = mantissaLargest * 256.0 / rgbLargest;

      for( int i = 3;  i-- > 0; )
      {
         rgbe |= static_cast<int32u>(::floor( rgb[i] * amount )) <<
            ((3 - i) * 8);
      }
      rgbe |= static_cast<int32u>(exponentLargest + 128);
   }

   return rgbe;
}*/

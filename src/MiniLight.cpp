/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <signal.h>
#include <math.h>
#include <iostream>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <sstream>

#include "Primitives.hpp"

#include "Random.hpp"
#include "Image.hpp"
#include "Scene.hpp"
#include "Camera.hpp"
#include "GradientState.hpp"




namespace
{

   using namespace hxa7241;


/// user messages --------------------------------------------------------------

const char BANNER_MESSAGE[] =
"\n  MiniLight 1.6 C++ - http://www.hxa.name/minilight\n"
//"\n  MiniLight 1.6 C++ HDRI - http://www.hxa.name/minilight\n"   // HDRI
"\n";

const char HELP_MESSAGE[] =
"\n"
"----------------------------------------------------------------------\n"
"  MiniLight 1.6 C++\n"
//"  MiniLight 1.6 C++ HDRI\n"   // HDRI
"\n"
"  Harrison Ainsworth / HXA7241 : 2006-2011, 2013\n"
"  http://www.hxa.name/minilight\n"
"\n"
"  2013-05-04\n"
"----------------------------------------------------------------------\n"
"\n"
"MiniLight is a minimal global illumination renderer.\n"
"\n"
"usage:\n"
"  minilight modelFilePathName\n"
"  minilight modelFilePathName --gradient alpha\n"
"\n"
"The optional --gradient mode renders each iteration as a gradient-domain\n"
"round (primal radiance plus right/up edge gradients) and reconstructs the\n"
"image by solving an alpha-constrained Poisson system. alpha must be a\n"
"positive finite number. The output is still a PPM file.\n"
"\n"
"The model text file format is:\n"
"  #MiniLight\n"
"\n"
"  iterations\n"
"\n"
"  imagewidth imageheight\n"
"  viewposition viewdirection viewangle\n"
"\n"
"  skyemission groundreflection\n"
"\n"
"  vertex0 vertex1 vertex2 reflectivity emitivity\n"
"  vertex0 vertex1 vertex2 reflectivity emitivity\n"
"  ...\n"
"\n"
"- where iterations and image values are integers, viewangle is a real,\n"
"and all other values are three parenthised reals. The file must end\n"
"with a newline. E.g.:\n"
"  #MiniLight\n"
"\n"
"  100\n"
"\n"
"  200 150\n"
"  (0 0.75 -2) (0 0 1) 45\n"
"\n"
"  (3626 5572 5802) (0.1 0.09 0.07)\n"
"\n"
"  (0 0 0) (0 1 0) (1 1 0)  (0.7 0.7 0.7) (0 0 0)\n"
"\n";




/// constants ------------------------------------------------------------------

const char MODEL_FORMAT_ID[] = "#MiniLight";




/// gradient option ------------------------------------------------------------

/**
 * Parse the --gradient argument value. Only decimal floating-point forms are
 * accepted (no leading/trailing junk, not empty), and the value must be
 * positive and finite.
 */
bool parseGradientAlpha
(
   const std::string& text,
   real64&            alpha_o
)
{
   if( text.empty() )
   {
      return false;
   }

   const char* begin = text.c_str();
   char*       end   = 0;

   errno = 0;
   const real64 value = ::strtod( begin, &end );

   // whole string consumed, nothing weird like overflow to infinity
   if( (end == begin) || (end == 0) || ('\0' != *end) )
   {
      return false;
   }

   if( !(value > 0.0) || !::isfinite( value ) )
   {
      return false;
   }

   alpha_o = value;
   return true;
}




/// ctrl-c handling ------------------------------------------------------------

void sigintHandler
(
   int arg
)
{
   if( SIGINT == arg )
   {
      std::cout << "\ninterrupted\n";
      ::exit( EXIT_SUCCESS );
   }
}

}




/// entry point ////////////////////////////////////////////////////////////////

int main
(
   int   argc,
   char* argv[]
)
{
   int returnValue = EXIT_FAILURE;

   // catch everything
   try
   {
      // check for help request
      if( (argc <= 1) || (std::string(argv[1]) == "-?") ||
         (std::string(argv[1]) == "--help") )
      {
         std::cout << HELP_MESSAGE;
      }
      // execute
      else
      {
         std::cout << BANNER_MESSAGE;

         // make random generator
         hxa7241_general::Random random;

         // get file names
         const std::string modelFilePathname( argv[1] );
         const std::string imageFilePathname( modelFilePathname + ".ppm" );

         // gradient mode option
         bool   isGradient  = false;
         real64 gradientAlpha = 0.0;
         {
            int argIndex = 2;

            while( argIndex < argc )
            {
               const std::string argument( argv[argIndex] );

               if( "--gradient" == argument )
               {
                  if( argIndex + 1 >= argc )
                  {
                     throw std::invalid_argument(
                        "--gradient requires an alpha value" );
                  }

                  if( !parseGradientAlpha( std::string(argv[argIndex + 1]),
                     gradientAlpha ) )
                  {
                     throw std::invalid_argument(
                        "gradient alpha must be a positive finite number" );
                  }

                  isGradient = true;
                  argIndex += 2;
               }
               else
               {
                  throw std::invalid_argument(
                     std::string("unrecognised argument: ") + argument );
               }
            }
         }

         // open model file
         std::ifstream modelFile( modelFilePathname.c_str() );
         modelFile.exceptions( std::ios::badbit | std::ios::eofbit );

         // check model file format identifier at start of first line
         {
            std::string formatId;
            modelFile >> formatId;
            if( std::string(MODEL_FORMAT_ID) != formatId )
            {
               throw std::runtime_error( "unrecognised model format" );
            }
         }

         // read frame iterations
         int32 iterations = 0;
         modelFile >> iterations;

         // create top-level rendering objects with model file
               hxa7241_minilight::Image  image ( modelFile );
         const hxa7241_minilight::Camera camera( modelFile );
         const hxa7241_minilight::Scene scene( modelFile, camera.getEyePoint());

         modelFile.close();

         // gradient-domain accumulation state, owned by this function
         hxa7241_minilight::GradientState gradientState;
         if( isGradient )
         {
            gradientState.reset( image.getWidth(), image.getHeight() );
         }

         // setup ctrl-c/interruption/sigint handler
         ::signal( SIGINT, sigintHandler );

         // do progressive refinement render loop
         for( int32 frameNo = 1;  frameNo <= iterations;  ++frameNo )
         {
            // display current frame number
            (std::cout << "\riteration: " << frameNo).flush();

            if( isGradient )
            {
               // render one gradient-domain round
               camera.getGradientFrame( scene, random, gradientState );
            }
            else
            {
               // render a frame
               camera.getFrame( scene, random, image );
            }

            // save image at twice error-halving rate, and at start and end
            if( ((frameNo & (frameNo - 1)) == 0) | (iterations == frameNo) )
            {
               if( isGradient )
               {
                  // solve from all rounds accumulated so far
                  const hxa7241_minilight::GradientSolution solution(
                     gradientState.reconstruct( gradientAlpha ) );

                  if( !solution.isConverged() )
                  {
                     std::ostringstream message;
                     message << "gradient reconstruction did not converge"
                        " (relative residual " << solution.getResidual() << ')';
                     throw std::runtime_error( message.str() );
                  }

                  // open image file
                  std::ofstream imageFile( imageFilePathname.c_str(),
                     std::ios::binary );
                  imageFile.exceptions( std::ios::badbit |
                     std::ios::failbit | std::ios::eofbit );

                  solution.getFormatted( imageFile );

                  imageFile.close();
               }
               else
               {
                  // open image file
                  std::ofstream imageFile( imageFilePathname.c_str(),
                     std::ios::binary );
                  imageFile.exceptions( std::ios::badbit |
                     std::ios::failbit | std::ios::eofbit );

                  // write image frame to file
                  image.getFormatted( imageFile, frameNo );

                  imageFile.close();
               }
            }
         }

         std::cout << "\nfinished\n";
      }

      returnValue = EXIT_SUCCESS;
   }
   // print exception message
   catch( const std::exception& e )
   {
      std::cout << '\n' << "*** execution failed:  " << e.what() << '\n';
   }
   catch( ... )
   {
      std::cout << '\n' << "*** execution failed" << '\n';
   }

   return returnValue;
}

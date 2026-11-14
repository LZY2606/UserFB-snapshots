/*------------------------------------------------------------------------------

   MiniLight C++ : bidirectional path connection tests

   numerical checks for src/Bidirectional.hpp/.cpp

------------------------------------------------------------------------------*/


#include "check.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Vector3f.hpp"
#include "Triangle.hpp"
#include "Scene.hpp"
#include "Camera.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "Bidirectional.hpp"


using namespace hxa7241_minilight;
using hxa7241_general::Random;


namespace
{

const double PI_T = 3.14159265358979;


/// make one triangle from its textual description
Triangle makeTri
(
   const char* text
)
{
   std::istringstream in( text );
   in.exceptions( std::ios::badbit | std::ios::failbit );
   return Triangle( in );
}


/// independent area density of an emission edge (front-face cosine/PI,
/// converted to the target surface)
double pdfEmit
(
   const Vector3f& from,
   const Vector3f& to,
   const Vector3f& nFrom,
   const Vector3f& nTo
)
{
   const Vector3f d( to - from );
   const double   dist2 = d.dot( d );
   const Vector3f dir( d.unitize() );
   return (nFrom.dot( dir ) / PI_T) * std::fabs( nTo.dot( dir ) ) / dist2;
}


/// independent area density of a diffuse bounce edge (cosine hemisphere on
/// the incoming side, converted to the target surface); zero on transmission
double pdfBounce
(
   const Vector3f& inDir,
   const Vector3f& at,
   const Vector3f& to,
   const Vector3f& nAt,
   const Vector3f& nTo
)
{
   const Vector3f d( to - at );
   const double   dist2 = d.dot( d );
   const Vector3f outDir( d.unitize() );
   if( (nAt.dot( inDir ) < 0.0) != (nAt.dot( outDir ) < 0.0) )
   {
      return 0.0;
   }
   return (std::fabs( nAt.dot( outDir ) ) / PI_T) *
      std::fabs( nTo.dot( outDir ) ) / dist2;
}


bool near
(
   const double a,
   const double b,
   const double tol = 1e-12
)
{
   return std::fabs( a - b ) <= tol * (1.0 + std::fabs( b ));
}


bool near3
(
   const Vector3f& a,
   const Vector3f& b,
   const double    tol = 1e-12
)
{
   return near( a[0], b[0], tol ) & near( a[1], b[1], tol ) &
      near( a[2], b[2], tol );
}


/// mean/variance accumulator for Vector3f samples
struct Stat
{
   double sum [3];
   double sum2[3];
   int    n;

   Stat() : sum(), sum2(), n( 0 ) {}

   void add( const Vector3f& v )
   {
      for( int c = 0;  c < 3;  ++c )
      {
         sum [c] += v[c];
         sum2[c] += v[c] * v[c];
      }
      ++n;
   }

   double mean( const int c ) const
   {
      return n > 0 ? sum[c] / n : 0.0;
   }

   double se( const int c ) const
   {
      const double m = mean( c );
      const double v = n > 1 ?
         (sum2[c] / n - m * m) / (n - 1.0) : 0.0;
      return std::sqrt( v > 0.0 ? v : 0.0 );
   }
};


/// independent unidirectional random walk: cosine-sampled bounces, no
/// russian roulette, no next-event estimation, truncated at maxVertices
/// surface vertices; per-depth emission is recorded in perK[1..maxK]
Vector3f uniWalk
(
   const Scene&    scene,
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   const Random&   random,
   const int       maxVertices,
   Vector3f*       perK,
   const int       maxK
)
{
   Vector3f total( Vector3f::ZERO() );
   Vector3f tput( 1.0, 1.0, 1.0 );

   Vector3f   origin( rayOrigin );
   Vector3f   dir   ( rayDirection );
   const void* last = 0;

   for( int depth = 1;  depth <= maxVertices;  ++depth )
   {
      const Triangle* pHit = 0;
      Vector3f        pos;
      scene.getIntersection( origin, dir, last, pHit, pos );
      if( 0 == pHit )
      {
         break;
      }

      // single-sided emission
      if( pHit->getNormal().dot( -dir ) > 0.0 )
      {
         const Vector3f e( tput * pHit->getEmitivity() );
         if( depth <= maxK )
         {
            perK[depth] = perK[depth] + e;
         }
         total = total + e;
      }

      // cosine-weighted hemisphere sample on the incoming side
      Vector3f normal( pHit->getNormal() );
      normal = normal.dot( -dir ) >= 0.0 ? normal : -normal;
      const Vector3f tangent( pHit->getTangent() );
      const double a = PI_T * 2.0 * random.getReal64();
      const double r = std::sqrt( random.getReal64() );
      const Vector3f nextDir( (tangent * (std::cos( a ) * r)) +
         (normal.cross( tangent ) * (std::sin( a ) * r)) +
         (normal * std::sqrt( 1.0 - r * r )) );

      tput   = tput * pHit->getReflectivity();
      last   = pHit;
      origin = pos;
      dir    = nextDir;
   }

   return total;
}


}//namespace




/// densities of hand-given, non-coplanar vertex chains ----------------------

CASE(densities)
{
   // geometry: five non-coplanar triangles
   Triangle emitter ( makeTri(
      "(0 0 0) (1 0 0) (0 1 0) (0.5 0.5 0.5) (2 2 2)" ) );   // n +z, A 0.5
   Triangle diffX   ( makeTri(
      "(1 0 0) (1 1 0) (1 0 1) (0.5 0.5 0.5) (0 0 0)" ) );   // n +x
   Triangle diffY   ( makeTri(
      "(0 1 1) (0 1 2) (1 1 1) (0.5 0.5 0.5) (0 0 0)" ) );   // n +y
   Triangle emitter2( makeTri(
      "(0 0 2) (0 1 2) (1 0 2) (0.5 0.5 0.5) (1 1 1)" ) );   // n -z, A 0.5
   Triangle diffX2  ( makeTri(
      "(2 0 0) (2 1 0) (2 0 1) (0.5 0.5 0.5) (0 0 0)" ) );   // n +x
   Triangle diffX3  ( makeTri(
      "(1 1 0) (1 2 0) (1 1 1) (0.5 0.5 0.5) (0 0 0)" ) );   // n +x

   const Vector3f nE ( 0.0, 0.0, 1.0  );
   const Vector3f nX ( 1.0, 0.0, 0.0  );
   const Vector3f nY ( 0.0, 1.0, 0.0  );
   const Vector3f nE2( 0.0, 0.0, -1.0 );

   const Vector3f P0( 0.25, 0.25, 0.0  );   // on emitter
   const Vector3f P1( 1.0,  0.25, 0.25 );   // on diffX
   const Vector3f P2( 0.25, 1.0,  1.25 );   // on diffY
   const Vector3f P3( 0.25, 0.5,  2.0  );   // on emitter2

   const int NE = 2;   // two emitters of area 0.5: pA(start) = 1/(2*0.5)

   std::vector<double> dens, weights;

   // --- three-vertex chain, light -> eye ----------------------------------
   {
      std::vector<Bidirectional::PathVertex> chain;
      chain.push_back( Bidirectional::PathVertex{ P0, &emitter } );
      chain.push_back( Bidirectional::PathVertex{ P1, &diffX   } );
      chain.push_back( Bidirectional::PathVertex{ P2, &diffY   } );
      const Vector3f eyeOut( 0.0, -1.0, 0.0 );

      // independent expectations
      const double pA0    = 1.0 / (NE * 0.5);
      const double eEmit1 = pdfEmit( P0, P1, nE, nX );
      const double eEye21 = pdfBounce( eyeOut, P2, P1, nY, nX );
      const double eEye10 = pdfBounce( (P2 - P1).unitize(), P1, P0, nX, nE );
      const double ex[3] = { eEye21 * eEye10, pA0 * eEye21, pA0 * eEmit1 };

      Bidirectional::getSplitDensities( chain, NE, eyeOut, dens, weights );
      CHECK( dens.size() == 3u && weights.size() == 3u );
      double exSum = 0.0, wSum = 0.0;
      for( int s = 0;  s < 3;  ++s )
      {
         CHECK( near( dens[s], ex[s] ) );
         exSum += ex[s];
         wSum  += weights[s];
      }
      for( int s = 0;  s < 3;  ++s )
      {
         CHECK( near( weights[s], ex[s] / exSum ) );
      }
      CHECK( near( wSum, 1.0 ) );
   }

   // --- four-vertex chain, both directions ---------------------------------
   {
      std::vector<Bidirectional::PathVertex> chain;
      chain.push_back( Bidirectional::PathVertex{ P0, &emitter  } );
      chain.push_back( Bidirectional::PathVertex{ P1, &diffX    } );
      chain.push_back( Bidirectional::PathVertex{ P2, &diffY    } );
      chain.push_back( Bidirectional::PathVertex{ P3, &emitter2 } );
      const Vector3f eyeOut( 0.0, 0.0, -1.0 );

      const double pA0    = 1.0 / (NE * 0.5);
      const double eEmit1 = pdfEmit( P0, P1, nE, nX );
      const double eBnc12 = pdfBounce( (P0 - P1).unitize(), P1, P2, nX, nY );
      const double eEye32 = pdfBounce( eyeOut, P3, P2, nE2, nY );
      const double eEye21 = pdfBounce( (P3 - P2).unitize(), P2, P1, nY, nX );
      const double eEye10 = pdfBounce( (P2 - P1).unitize(), P1, P0, nX, nE );
      const double ex[4] = { eEye32 * eEye21 * eEye10,
                             pA0 * eEye32 * eEye21,
                             pA0 * eEmit1 * eEye32,
                             pA0 * eEmit1 * eBnc12 };

      Bidirectional::getSplitDensities( chain, NE, eyeOut, dens, weights );
      CHECK( dens.size() == 4u );
      double exSum = 0.0, wSum = 0.0;
      for( int s = 0;  s < 4;  ++s )
      {
         CHECK( near( dens[s], ex[s] ) );
         exSum += ex[s];
         wSum  += weights[s];
      }
      for( int s = 0;  s < 4;  ++s )
      {
         CHECK( near( weights[s], ex[s] / exSum ) );
      }
      CHECK( near( wSum, 1.0 ) );

      // same geometry, opposite direction (light and eye ends exchanged)
      std::vector<Bidirectional::PathVertex> chainR;
      chainR.push_back( Bidirectional::PathVertex{ P3, &emitter2 } );
      chainR.push_back( Bidirectional::PathVertex{ P2, &diffY    } );
      chainR.push_back( Bidirectional::PathVertex{ P1, &diffX    } );
      chainR.push_back( Bidirectional::PathVertex{ P0, &emitter  } );
      const Vector3f eyeOutR( 0.0, 0.0, 1.0 );

      const double pA0R    = 1.0 / (NE * 0.5);
      const double eEmit1R = pdfEmit( P3, P2, nE2, nY );
      const double eBnc12R = pdfBounce( (P3 - P2).unitize(), P2, P1, nY, nX );
      const double eEye32R = pdfBounce( eyeOutR, P0, P1, nE, nX );
      const double eEye21R = pdfBounce( (P0 - P1).unitize(), P1, P2, nX, nY );
      const double eEye10R = pdfBounce( (P1 - P2).unitize(), P2, P3, nY, nE2 );
      const double exR[4] = { eEye32R * eEye21R * eEye10R,
                              pA0R * eEye32R * eEye21R,
                              pA0R * eEmit1R * eEye32R,
                              pA0R * eEmit1R * eBnc12R };

      Bidirectional::getSplitDensities( chainR, NE, eyeOutR, dens, weights );
      CHECK( dens.size() == 4u );
      double exSumR = 0.0, wSumR = 0.0;
      for( int s = 0;  s < 4;  ++s )
      {
         CHECK( near( dens[s], exR[s] ) );
         exSumR += exR[s];
         wSumR  += weights[s];
      }
      for( int s = 0;  s < 4;  ++s )
      {
         CHECK( near( weights[s], exR[s] / exSumR ) );
      }
      CHECK( near( wSumR, 1.0 ) );
   }

   // --- five-vertex chain with a transmission vertex -----------------------
   {
      const Vector3f P3b( 1.0, 1.25, 0.25 );   // on diffX3
      std::vector<Bidirectional::PathVertex> chain;
      chain.push_back( Bidirectional::PathVertex{ P0,  &emitter  } );
      chain.push_back( Bidirectional::PathVertex{ P1,  &diffX    } );
      chain.push_back( Bidirectional::PathVertex{ P2,  &diffY    } );
      chain.push_back( Bidirectional::PathVertex{ P3b, &diffX3   } );
      chain.push_back( Bidirectional::PathVertex{ P3,  &emitter2 } );
      const Vector3f eyeOut( 0.0, 0.0, -1.0 );

      // at P2 the P1->P2->P3b turn crosses the surface: every split whose
      // random walk would cross P2 has zero density
      const double pA0    = 1.0 / (NE * 0.5);
      const double eEmit1 = pdfEmit( P0, P1, nE, nX );
      const double eBnc12 = pdfBounce( (P0 - P1).unitize(), P1, P2, nX, nY );
      const double eEye43 = pdfBounce( eyeOut, P3, P3b, nE2, nX );
      const double eEye32 = pdfBounce( (P3 - P3b).unitize(), P3b, P2, nX, nY );

      Bidirectional::getSplitDensities( chain, NE, eyeOut, dens, weights );
      CHECK( dens.size() == 5u );
      CHECK( dens[0] == 0.0 );
      CHECK( dens[1] == 0.0 );
      CHECK( near( dens[2], pA0 * eEmit1 * eEye43 * eEye32 ) );
      CHECK( near( dens[3], pA0 * eEmit1 * eBnc12 * eEye43 ) );
      CHECK( dens[4] == 0.0 );
      CHECK( near( weights[0] + weights[1] + weights[2] + weights[3] +
         weights[4], 1.0 ) );
      CHECK( near( weights[2], dens[2] / (dens[2] + dens[3]) ) );
   }

   // --- chain starting on a non-emitter: no split is feasible --------------
   {
      std::vector<Bidirectional::PathVertex> chain;
      chain.push_back( Bidirectional::PathVertex{ P1, &diffX   } );
      chain.push_back( Bidirectional::PathVertex{ P0, &emitter } );
      chain.push_back( Bidirectional::PathVertex{ P2, &diffY   } );
      Bidirectional::getSplitDensities( chain, NE,
         Vector3f( 0.0, -1.0, 0.0 ), dens, weights );
      CHECK( dens.size() == 3u );
      for( int s = 0;  s < 3;  ++s )
      {
         CHECK( dens[s] == 0.0 );
         CHECK( weights[s] == 0.0 );
      }
   }

   // --- emission from the back face kills exactly the emitting splits ------
   {
      const Vector3f Q1( 1.0, 0.25, -0.5 );   // below the emitter plane
      std::vector<Bidirectional::PathVertex> chain;
      chain.push_back( Bidirectional::PathVertex{ P0, &emitter } );
      chain.push_back( Bidirectional::PathVertex{ Q1, &diffX   } );
      const Vector3f eyeOut( -0.6, 0.0, -0.8 );

      Bidirectional::getSplitDensities( chain, NE, eyeOut, dens, weights );
      CHECK( dens.size() == 2u );
      CHECK( dens[0] == 0.0 );   // eye hit of emitter back face
      CHECK( dens[1] > 0.0 );    // light start + connection is still sampled
      // (two vertices, split (1,1): no random-walk edges, light start only)
      CHECK( near( dens[1], 1.0 / (NE * 0.5) ) );
      CHECK( near( weights[1], 1.0 ) );
   }

   // --- transmission at an interior vertex kills the crossing splits -------
   {
      const Vector3f R2( 2.0, 0.25, 0.25 );   // on diffX2, other side of diffX
      std::vector<Bidirectional::PathVertex> chain;
      chain.push_back( Bidirectional::PathVertex{ P0, &emitter } );
      chain.push_back( Bidirectional::PathVertex{ P1, &diffX   } );
      chain.push_back( Bidirectional::PathVertex{ R2, &diffX2  } );
      const Vector3f eyeOut( -1.0, 0.0, 0.0 );

      Bidirectional::getSplitDensities( chain, NE, eyeOut, dens, weights );
      CHECK( dens.size() == 3u );
      CHECK( dens[0] == 0.0 );   // eye walk would cross diffX
      // split (2,1): P1 is the light endpoint, so the edge past it is the
      // connection edge and is not sampled; density is start + emission
      CHECK( near( dens[2], (1.0 / (NE * 0.5)) *
         pdfEmit( P0, P1, nE, nX ) ) );
      CHECK( near( dens[1], (1.0 / (NE * 0.5)) *
         pdfBounce( eyeOut, R2, P1, nX, nX ) ) );
      CHECK( near( weights[0] + weights[1] + weights[2], 1.0 ) );
      CHECK( near( weights[1], dens[1] / (dens[1] + dens[2]) ) );
   }
}




/// connections through the real spatial index --------------------------------

CASE(connections)
{
   // corner scene: emitter faces wall1 only; wall2 is lit around the corner
   const char* cornerText =
      "(0 0 0) (0 0 0)\n"
      "(-2 -1 0) (0 -1 0) (-2 1 0) (0.7 0.7 0.7) (0 0 0)\n"
      "(0 -1 0) (0 1 0) (0 -1 3) (0.7 0.7 0.7) (0 0 0)\n"
      "(-1.5 -0.25 1) (-1.5 0.25 1) (-0.5 -0.25 1) (0 0 0) (4 4 4)\n";

   const Vector3f cornerEye( -1.5, 0.0, 1.2 );
   const Vector3f cornerDir(
      (Vector3f( 0.0, 0.0, 1.0 ) - cornerEye).unitize() );

   // --- constructor validation ---------------------------------------------
   {
      std::istringstream data( cornerText );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      Scene scene( data, cornerEye );

      bool threw0 = false, threw13 = false;
      try { Bidirectional b( scene, 0 );  (void)b; } catch( ... ) { threw0  = true; }
      try { Bidirectional b( scene, 13 ); (void)b; } catch( ... ) { threw13 = true; }
      CHECK( threw0 );
      CHECK( threw13 );

      Bidirectional ok1( scene, 1 );
      Bidirectional ok12( scene, 12 );
      CHECK( ok1.getMaxVertices() == 1 );
      CHECK( ok12.getMaxVertices() == 12 );
   }
   {
      // non-black sky is rejected
      std::istringstream data(
         "(0.5 0.5 0.5) (0 0 0)\n"
         "(-1 -1 0) (1 -1 0) (0 1 0) (0.5 0.5 0.5) (0 0 0)\n" );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      Scene scene( data, Vector3f( 0.0, 0.0, 2.0 ) );
      bool threw = false;
      try { Bidirectional b( scene, 3 ); (void)b; } catch( ... ) { threw = true; }
      CHECK( threw );
   }
   {
      // no emitters: black, and no division by zero on emitter selection
      std::istringstream data(
         "(0 0 0) (0 0 0)\n"
         "(-1 -1 0) (1 -1 0) (0 1 0) (0.5 0.5 0.5) (0 0 0)\n" );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      Scene scene( data, Vector3f( 0.0, 0.0, 2.0 ) );
      Bidirectional bidir( scene, 4 );
      Random rnd( 5 );
      const Vector3f r( bidir.getRadiance( Vector3f( 0.0, 0.0, 2.0 ),
         Vector3f( 0.0, 0.0, -1.0 ), rnd ) );
      CHECK( r.isZero() );
   }

   // --- direct emitter view, depth limit 1 ----------------------------------
   {
      std::istringstream data(
         "(0 0 0) (0 0 0)\n"
         "(-1 -1 1) (1 -1 1) (-1 1 1) (0.5 0.5 0.5) (2 3 4)\n" );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      const Vector3f eye( 0.0, 0.0, 3.0 );
      Scene scene( data, eye );
      Bidirectional bidir( scene, 1 );
      Random rnd( 11 );
      Bidirectional::PathInfo info;
      const Vector3f r( bidir.getRadiance( eye,
         Vector3f( 0.0, 0.0, -1.0 ), rnd, 0, &info ) );
      CHECK( near3( r, Vector3f( 2.0, 3.0, 4.0 ), 1e-12 ) );
      CHECK( info.paths.size() == 1u );
      CHECK( info.paths[0].splits.size() == 1u );
      CHECK( info.paths[0].splits[0].s == 0 &&
         info.paths[0].splits[0].t == 1 );
      CHECK( near( info.paths[0].splits[0].density, 1.0 ) );
      CHECK( near( info.paths[0].splits[0].weight, 1.0 ) );
      CHECK( near3( info.paths[0].splits[0].contribution,
         Vector3f( 2.0, 3.0, 4.0 ), 1e-12 ) );
   }

   // --- corner indirect: connection around the corner -----------------------
   {
      std::istringstream data( cornerText );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      Scene scene( data, cornerEye );

      // depth limit 3: indirect around the corner must appear
      {
         Bidirectional bidir( scene, 3 );
         Random rnd( 42 );
         Stat total;
         int count12 = 0, count21 = 0;
         bool planesOk = true, depthOk = true, weightsOk = true;
         for( int i = 0;  i < 3000;  ++i )
         {
            Bidirectional::PathInfo info;
            const Vector3f r( bidir.getRadiance( cornerEye, cornerDir,
               rnd, 0, &info ) );
            total.add( r );
            for( int32u p = 0u;  p < info.paths.size();  ++p )
            {
               const Bidirectional::PathRecord& rec = info.paths[p];
               depthOk &= rec.sampledS + rec.sampledT <= 3;
               double wSum = 0.0;
               for( int32u q = 0u;  q < rec.splits.size();  ++q )
               {
                  wSum += rec.splits[q].weight;
               }
               weightsOk &= !rec.contribution.isZero() ? near( wSum, 1.0 ) :
                  true;
               if( rec.contribution.isZero() )
               {
                  continue;
               }
               if( (rec.sampledS == 1) & (rec.sampledT == 2) )
               {
                  ++count12;
                  // light vertex on emitter, middle on wall1 (z=0),
                  // eye vertex on wall2 (x=0)
                  planesOk &= std::fabs( rec.chain[1].position[2] ) < 1e-9;
                  planesOk &= std::fabs( rec.chain[2].position[0] ) < 1e-9;
               }
               if( (rec.sampledS == 2) & (rec.sampledT == 1) )
               {
                  ++count21;
                  planesOk &= std::fabs( rec.chain[1].position[2] ) < 1e-9;
                  planesOk &= std::fabs( rec.chain[2].position[0] ) < 1e-9;
               }
            }
         }
         CHECK( count12 > 0 );
         CHECK( count21 > 0 );
         CHECK( planesOk );
         CHECK( depthOk );
         CHECK( weightsOk );
         CHECK( total.mean( 0 ) > 0.0 );
         std::cout << "INFO corner s1t2 " << count12 << " s2t1 " << count21
            << " mean " << total.mean( 0 ) << "\n";
      }

      // depth limit 2: the corner path needs 3 vertices, and the emitter
      // is edge-on to the receiver, so the estimate is exactly black
      {
         Bidirectional bidir( scene, 2 );
         Random rnd( 42 );
         Vector3f sum( Vector3f::ZERO() );
         for( int i = 0;  i < 500;  ++i )
         {
            sum = sum + bidir.getRadiance( cornerEye, cornerDir, rnd );
         }
         CHECK( sum.isZero() );
      }

      // depth limit 1: no connection is possible at all
      {
         Bidirectional bidir( scene, 1 );
         Random rnd( 42 );
         Vector3f sum( Vector3f::ZERO() );
         for( int i = 0;  i < 100;  ++i )
         {
            sum = sum + bidir.getRadiance( cornerEye, cornerDir, rnd );
         }
         CHECK( sum.isZero() );
      }
   }

   // --- finite occlusion between emitter and receiver -----------------------
   {
      const char* openText =
         "(0 0 0) (0 0 0)\n"
         "(-2 -2 0) (2 -2 0) (0 2 0) (0.8 0.8 0.8) (0 0 0)\n"
         "(-0.5 -0.5 1) (-0.5 0.5 1) (0.5 -0.5 1) (0 0 0) (3 3 3)\n";
      const char* occlText =
         "(0 0 0) (0 0 0)\n"
         "(-2 -2 0) (2 -2 0) (0 2 0) (0.8 0.8 0.8) (0 0 0)\n"
         "(-0.5 -0.5 1) (-0.5 0.5 1) (0.5 -0.5 1) (0 0 0) (3 3 3)\n"
         "(-0.25 -0.25 0.5) (0 -0.25 0.5) (-0.25 0.25 0.5)"
         " (0.5 0.5 0.5) (0 0 0)\n";

      const Vector3f eye( 1.5, 1.5, 3.0 );
      const Vector3f dir( (Vector3f( 0.0, 0.0, 0.0 ) - eye).unitize() );

      Stat openStat, occlStat;
      int openHits = 0, occlHits = 0;
      const int N = 3000;
      {
         std::istringstream data( openText );
         data.exceptions( std::ios::badbit | std::ios::eofbit );
         Scene scene( data, eye );
         Bidirectional bidir( scene, 2 );
         Random rnd( 7 );
         for( int i = 0;  i < N;  ++i )
         {
            Bidirectional::PathInfo info;
            openStat.add( bidir.getRadiance( eye, dir, rnd, 0, &info ) );
            for( int32u p = 0u;  p < info.paths.size();  ++p )
            {
               const Bidirectional::PathRecord& rec = info.paths[p];
               openHits += ((rec.sampledS == 1) & (rec.sampledT == 1) &
                  !rec.contribution.isZero()) ? 1 : 0;
            }
         }
      }
      {
         std::istringstream data( occlText );
         data.exceptions( std::ios::badbit | std::ios::eofbit );
         Scene scene( data, eye );
         Bidirectional bidir( scene, 2 );
         Random rnd( 7 );
         for( int i = 0;  i < N;  ++i )
         {
            Bidirectional::PathInfo info;
            occlStat.add( bidir.getRadiance( eye, dir, rnd, 0, &info ) );
            for( int32u p = 0u;  p < info.paths.size();  ++p )
            {
               const Bidirectional::PathRecord& rec = info.paths[p];
               occlHits += ((rec.sampledS == 1) & (rec.sampledT == 1) &
                  !rec.contribution.isZero()) ? 1 : 0;
            }
         }
      }
      // open: every sample connects; occluded: some are blocked, some pass
      CHECK( openHits == N );
      CHECK( occlHits > 0 );
      CHECK( occlHits < openHits );
      CHECK( occlStat.mean( 0 ) < openStat.mean( 0 ) );
      CHECK( occlStat.mean( 0 ) > 0.0 );
      std::cout << "INFO occlusion open " << openStat.mean( 0 )
         << " occluded " << occlStat.mean( 0 ) << " hits " << occlHits
         << "/" << openHits << "\n";
   }

   // --- reflection on the back side of a surface ----------------------------
   {
      std::istringstream data(
         "(0 0 0) (0 0 0)\n"
         "(-1 -1 0) (1 -1 0) (0 1 0) (0.8 0.8 0.8) (0 0 0)\n"
         "(-0.5 -0.5 -1) (0.5 -0.5 -1) (-0.5 0.5 -1) (0 0 0) (2 2 2)\n" );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      const Vector3f eye( 2.0, 2.0, -3.0 );
      Scene scene( data, eye );
      const Vector3f dir(
         (Vector3f( 0.0, 0.0, 0.0 ) - eye).unitize() );

      // the receiver is seen from behind its geometric normal
      CHECK( Vector3f( 0.0, 0.0, 1.0 ).dot( eye ) < 0.0 );

      Bidirectional bidir( scene, 2 );
      Random rnd( 9 );
      Stat stat;
      int hits = 0;
      const int N = 2000;
      for( int i = 0;  i < N;  ++i )
      {
         Bidirectional::PathInfo info;
         stat.add( bidir.getRadiance( eye, dir, rnd, 0, &info ) );
         for( int32u p = 0u;  p < info.paths.size();  ++p )
         {
            const Bidirectional::PathRecord& rec = info.paths[p];
            hits += ((rec.sampledS == 1) & (rec.sampledT == 1) &
               !rec.contribution.isZero()) ? 1 : 0;
         }
      }
      CHECK( hits == N );
      CHECK( stat.mean( 0 ) > 0.0 );
      std::cout << "INFO backside mean " << stat.mean( 0 ) << "\n";
   }

   // --- emitters of different area and count --------------------------------
   {
      std::istringstream data(
         "(0 0 0) (0 0 0)\n"
         "(-3 -3 0) (3 -3 0) (0 3 0) (0.8 0.8 0.8) (0 0 0)\n"
         "(-0.5 -0.5 1) (-0.5 0.5 1) (0.5 -0.5 1) (0 0 0) (2 2 2)\n"
         "(1 -1 1) (1 1 1) (3 -1 1) (0 0 0) (4 4 4)\n" );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      const Vector3f eye( 1.5, 1.5, 3.0 );
      Scene scene( data, eye );
      CHECK( scene.getEmittersCount() == 2 );
      const Vector3f dir(
         (Vector3f( 0.0, 0.0, 0.0 ) - eye).unitize() );

      Bidirectional bidir( scene, 2 );
      Random rnd( 13 );
      int countA = 0, countB = 0;
      bool densOk = true, weightsOk = true;
      const int N = 4000;
      for( int i = 0;  i < N;  ++i )
      {
         Bidirectional::PathInfo info;
         bidir.getRadiance( eye, dir, rnd, 0, &info );
         for( int32u p = 0u;  p < info.paths.size();  ++p )
         {
            const Bidirectional::PathRecord& rec = info.paths[p];
            if( !((rec.sampledS == 1) & (rec.sampledT == 1)) )
            {
               continue;
            }
            // light start density: 1/(emitters * area)
            if( rec.chain[0].position[0] < 1.0 )
            {
               ++countA;
               densOk &= near( rec.splits[1].density, 1.0 / (2.0 * 0.5) );
            }
            else
            {
               ++countB;
               densOk &= near( rec.splits[1].density, 1.0 / (2.0 * 2.0) );
            }
            weightsOk &= near( rec.splits[0].weight + rec.splits[1].weight,
               1.0 );
         }
      }
      CHECK( countA > 0 );
      CHECK( countB > 0 );
      CHECK( densOk );
      CHECK( weightsOk );
      std::cout << "INFO twolights countA " << countA << " countB "
         << countB << "\n";
   }

   // --- an emitting surface also reflects -----------------------------------
   {
      std::istringstream data(
         "(0 0 0) (0 0 0)\n"
         "(-3 -2 0) (1 -2 0) (-3 2 0) (0.7 0.7 0.7) (0 0 0)\n"
         "(0 -1 0) (0 -1 2) (0 1 0) (0.5 0.5 0.5) (1 1 1)\n"
         "(-2 -0.5 0.5) (-2 0.5 0.5) (-2 -0.5 1.5) (0 0 0) (5 5 5)\n" );
      data.exceptions( std::ios::badbit | std::ios::eofbit );
      const Vector3f eye( -1.0, -0.1, 3.0 );
      Scene scene( data, eye );
      CHECK( scene.getEmittersCount() == 2 );

      Bidirectional bidir( scene, 3 );
      Random rnd( 17 );
      int bounceOnEmitter = 0, startOnEmitter = 0;
      const int N = 4000;
      for( int i = 0;  i < N;  ++i )
      {
         Bidirectional::PathInfo info;
         bidir.getRadiance( eye, Vector3f( 0.0, 0.0, -1.0 ), rnd, 0, &info );
         for( int32u p = 0u;  p < info.paths.size();  ++p )
         {
            const Bidirectional::PathRecord& rec = info.paths[p];
            if( rec.contribution.isZero() )
            {
               continue;
            }
            // light walk bounced on the emitting/reflecting wall (x = 0)
            bounceOnEmitter += ((rec.sampledS == 2) & (rec.sampledT == 1) &
               (std::fabs( rec.chain[1].position[0] ) < 1e-9)) ? 1 : 0;
            // light walk started on it
            startOnEmitter +=
               (std::fabs( rec.chain[0].position[0] ) < 1e-9) ? 1 : 0;
         }
      }
      CHECK( bounceOnEmitter > 0 );
      CHECK( startOnEmitter > 0 );
      std::cout << "INFO emittingreflector bounce " << bounceOnEmitter
         << " start " << startOnEmitter << "\n";
   }
}




/// integral checks against quadrature and unidirectional walks ---------------

CASE(integral)
{
   // one diffuse floor, one emitter above it, one diffuse side wall
   // (so paths with several reflections exist), black environment
   const char* sceneText =
      "(0 0 0) (0 0 0)\n"
      "(-2 -2 0) (2 -2 0) (0 2 0) (0.7 0.7 0.7) (0 0 0)\n"
      "(-0.5 -0.5 1) (-0.5 0.5 1) (0.5 -0.5 1) (0 0 0) (3 2 1)\n"
      "(-2 -2 0) (-2 2 0) (-2 -2 3) (0.6 0.6 0.6) (0 0 0)\n";

   const Vector3f eye   ( 1.5, 1.5, 3.0 );
   const Vector3f target( 0.0, 0.0, 0.0 );
   const Vector3f dir   ( (target - eye).unitize() );

   std::istringstream data( sceneText );
   data.exceptions( std::ios::badbit | std::ios::eofbit );
   Scene scene( data, eye );

   // --- independent quadrature of direct light at the receiver -------------
   double quad[3];
   {
      double integral = 0.0;
      const int    gn = 400;
      const double h  = 1.0 / gn;
      for( int i = 0;  i < gn;  ++i )
      {
         for( int j = 0;  j < gn;  ++j )
         {
            const double px = -0.5 + (i + 0.5) * h;
            const double py = -0.5 + (j + 0.5) * h;
            // emitter triangle: px >= -0.5, py >= -0.5, px + py <= 0
            if( px + py <= 0.0 )
            {
               // cos(receiver) * cos(emitter) / r^2 = 1/r^4 here
               const double r2 = px * px + py * py + 1.0;
               integral += h * h / (r2 * r2);
            }
         }
      }
      CHECK( integral > 0.2 && integral < 0.5 );
      const double em[3] = { 3.0, 2.0, 1.0 };
      for( int c = 0;  c < 3;  ++c )
      {
         quad[c] = em[c] * (0.7 / PI_T) * integral;
      }
      std::cout << "INFO quadrature " << quad[0] << " " << quad[1] << " "
         << quad[2] << "\n";
   }

   // --- bidirectional with vertex limit 2 == direct light only -------------
   {
      Bidirectional bidir( scene, 2 );
      Random rnd( 12345 );
      Stat stat;
      const int N = 40000;
      for( int i = 0;  i < N;  ++i )
      {
         stat.add( bidir.getRadiance( eye, dir, rnd ) );
      }
      for( int c = 0;  c < 3;  ++c )
      {
         std::cout << "INFO direct ch" << c << " mean " << stat.mean( c )
            << " quad " << quad[c] << " se " << stat.se( c ) << "\n";
         CHECK( std::fabs( stat.mean( c ) - quad[c] ) <
            6.0 * stat.se( c ) + 1e-12 );
      }
   }

   // --- bidirectional vs unidirectional, same vertex limit, in batches ------
   {
      const int MAXV   = 4;
      const int BATCHES = 4;
      const int PER    = 15000;

      Bidirectional bidir( scene, MAXV );

      Stat bidirTotal, uniTotal;
      Stat bidirK[5], uniK[5];
      std::map<std::pair<int,int>, Stat> perStrategy;

      Random rndB( 999 );
      for( int b = 0;  b < BATCHES;  ++b )
      {
         Stat batch;
         for( int i = 0;  i < PER;  ++i )
         {
            Bidirectional::PathInfo info;
            const Vector3f r(
               bidir.getRadiance( eye, dir, rndB, 0, &info ) );
            batch.add( r );
            bidirTotal.add( r );
            Vector3f kSum[5];
            for( int32u p = 0u;  p < info.paths.size();  ++p )
            {
               const Bidirectional::PathRecord& rec = info.paths[p];
               perStrategy[std::make_pair( rec.sampledS, rec.sampledT )].
                  add( rec.contribution );
               const int k = rec.sampledS + rec.sampledT;
               if( k <= MAXV )
               {
                  kSum[k] = kSum[k] + rec.contribution;
               }
            }
            for( int k = 1;  k <= MAXV;  ++k )
            {
               bidirK[k].add( kSum[k] );
            }
         }
         std::cout << "INFO bidir batch " << b << " mean "
            << batch.mean( 0 ) << " " << batch.mean( 1 ) << " "
            << batch.mean( 2 ) << "\n";
      }

      Random rndU( 555 );
      for( int b = 0;  b < BATCHES;  ++b )
      {
         Stat batch;
         for( int i = 0;  i < PER;  ++i )
         {
            Vector3f perK[5];
            const Vector3f r(
               uniWalk( scene, eye, dir, rndU, MAXV, perK, MAXV ) );
            batch.add( r );
            uniTotal.add( r );
            for( int k = 1;  k <= MAXV;  ++k )
            {
               uniK[k].add( perK[k] );
            }
         }
         std::cout << "INFO uni batch " << b << " mean " << batch.mean( 0 )
            << " " << batch.mean( 1 ) << " " << batch.mean( 2 ) << "\n";
      }

      // per-strategy contributions and statistical errors
      for( std::map<std::pair<int,int>, Stat>::const_iterator it =
         perStrategy.begin();  it != perStrategy.end();  ++it )
      {
         std::cout << "INFO strategy s" << it->first.first << " t"
            << it->first.second << " mean " << it->second.mean( 0 ) << " "
            << it->second.mean( 1 ) << " " << it->second.mean( 2 )
            << " se " << it->second.se( 0 ) << "\n";
      }

      // compare linear means per path length and in total
      for( int k = 2;  k <= MAXV;  ++k )
      {
         for( int c = 0;  c < 3;  ++c )
         {
            const double comb = std::sqrt( bidirK[k].se( c ) *
               bidirK[k].se( c ) + uniK[k].se( c ) * uniK[k].se( c ) );
            std::cout << "INFO length " << k << " ch" << c << " bidir "
               << bidirK[k].mean( c ) << " uni " << uniK[k].mean( c )
               << " combse " << comb << "\n";
            CHECK( std::fabs( bidirK[k].mean( c ) - uniK[k].mean( c ) ) <
               6.0 * comb + 1e-12 );
         }
      }
      for( int c = 0;  c < 3;  ++c )
      {
         const double comb = std::sqrt( bidirTotal.se( c ) *
            bidirTotal.se( c ) + uniTotal.se( c ) * uniTotal.se( c ) );
         std::cout << "INFO total ch" << c << " bidir "
            << bidirTotal.mean( c ) << " uni " << uniTotal.mean( c )
            << " combse " << comb << "\n";
         CHECK( std::fabs( bidirTotal.mean( c ) - uniTotal.mean( c ) ) <
            6.0 * comb + 1e-12 );
      }
   }

   // --- real small image through the new Camera entry, in batches -----------
   {
      std::istringstream camData( "(1.5 1.5 3) (-0.408 -0.408 -0.816) 45" );
      Camera camera( camData );

      std::istringstream imgData( "4 3" );
      Image image( imgData );

      Random rnd( 2024 );
      const int BATCHES = 3;
      for( int b = 1;  b <= BATCHES;  ++b )
      {
         camera.getFrame( scene, rnd, image, 3 );
      }
      std::ostringstream out;
      image.getFormatted( out, BATCHES );
      const std::string bytes( out.str() );
      CHECK( bytes.find( "P6\n" ) == 0u );
      bool anyNonZero = false, allFinite = true;
      for( int32u i = 0u;  i < bytes.size();  ++i )
      {
         anyNonZero |= bytes[i] != 0;
      }
      (void)allFinite;
      CHECK( anyNonZero );

      // the original entry point still renders, with the unchanged default
      // random sequence: two fresh runs must be identical
      std::istringstream imgData2( "4 3" );
      Image image2( imgData2 );
      std::istringstream imgData3( "4 3" );
      Image image3( imgData3 );
      Random rnd2, rnd3;
      camera.getFrame( scene, rnd2, image2 );
      camera.getFrame( scene, rnd3, image3 );
      std::ostringstream out2, out3;
      image2.getFormatted( out2, 1 );
      image3.getFormatted( out3, 1 );
      CHECK( out2.str() == out3.str() );
      bool anyNonZero2 = false;
      const std::string bytes2( out2.str() );
      for( int32u i = 0u;  i < bytes2.size();  ++i )
      {
         anyNonZero2 |= bytes2[i] != 0;
      }
      CHECK( anyNonZero2 );
   }
}

/*------------------------------------------------------------------------------

   Tests for the finite-depth bidirectional path tracer.

   Three cases:
     densities   - hand-built non-coplanar chains, independently computed
                   area-measure strategy densities and balance weights
     connections - real spatial-index tracing: corners, one-sided surfaces,
                   occlusion, mixed emitters, depth limits, state isolation
     integral    - independent area quadrature at a receiver point, then a
                   truncated unidirectional walk vs the bidirectional tracer,
                   plus the new Camera bidir entry

------------------------------------------------------------------------------*/


#include <sstream>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <string>

#include "check.hpp"

#include "Primitives.hpp"
#include "Random.hpp"
#include "Scene.hpp"
#include "Camera.hpp"
#include "Image.hpp"
#include "Triangle.hpp"
#include "SurfacePoint.hpp"
#include "Bidirectional.hpp"


using namespace hxa7241_minilight;
using hxa7241_general::Random;


namespace
{
   const real64 PI = 3.14159265358979;


   bool close( real64 a, real64 b, real64 rel = 1.0e-10 )
   {
      return ::fabs( a - b ) <= (1.0e-14 + rel *
         (::fabs( a ) > ::fabs( b ) ? ::fabs( a ) : ::fabs( b )));
   }


   Scene makeScene( const std::string& triangles,
                    const Vector3f& eye = Vector3f( 0.0, 0.0, -4.0 ) )
   {
      std::istringstream in( "(0 0 0) (0 0 0)\n" + triangles );
      in.exceptions( std::ios::badbit | std::ios::eofbit );
      return Scene( in, eye );
   }


   /// build a vertex record straight from a real scene triangle
   BidirVertex vertexAt( const Scene& scene, int32 triangleIndex,
                         const Vector3f& position )
   {
      const Triangle& tri = scene.getTriangles()[
         static_cast<size_t>( triangleIndex ) ];
      BidirVertex v;
      v.position  = position;
      v.normal    = tri.getNormal();
      v.albedo    = tri.getReflectivity();
      v.emitivity = tri.getEmitivity();
      v.triangle  = &tri;
      v.area      = tri.getArea();
      v.condPdfW  = 0.0;
      v.isEmitter = !tri.getEmitivity().isZero() && (tri.getArea() > 0.0);
      return v;
   }


   /// independent area-measure edge factor, evaluated light-to-eye
   real64 edgeFactorForward( const BidirVertex& a, const BidirVertex& b )
   {
      const Vector3f ray( b.position - a.position );
      const real64 d2 = ray.dot( ray );
      const real64 cf = ::fabs( ray.unitize().dot( a.normal ) );
      const real64 ct = ::fabs( ray.unitize().dot( b.normal ) );
      return cf * ct / (PI * d2);
   }


   /// same edge factor, derived from the eye end: solid-angle pdf at the
   /// target vertex converted onto the source vertex's area measure
   real64 edgeFactorReverse( const BidirVertex& a, const BidirVertex& b )
   {
      const Vector3f ray( a.position - b.position );
      const real64 d2 = ray.dot( ray );
      const real64 pdfSolidAtB = ::fabs( ray.unitize().dot( b.normal ) ) / PI;
      const real64 cosAtA = ::fabs( (b.position - a.position).unitize().
         dot( a.normal ) );
      return pdfSolidAtB * cosAtA / d2;
   }


   /// fully independent reference densities for an ordered chain
   std::vector<real64> referenceDensities( const std::vector<BidirVertex>& c,
                                           int32 emittersCount )
   {
      const int32 n = static_cast<int32>( c.size() );
      std::vector<real64> ef( static_cast<size_t>( n - 1 ), 0.0 );
      std::vector<real64> efRev( static_cast<size_t>( n - 1 ), 0.0 );
      for( int32 j = 0; j < n - 1; ++j )
      {
         ef[j]    = edgeFactorForward( c[j], c[ j + 1 ] );
         efRev[j] = edgeFactorReverse( c[j], c[ j + 1 ] );
         CHECK( close( ef[j], efRev[j], 1.0e-12 ) );
      }

      std::vector<real64> density( static_cast<size_t>( n + 1 ), 0.0 );

      // strategy 0: whole chain walked from the eye
      real64 all = 1.0;
      for( int32 j = 0; j < n - 1; ++j ) all *= ef[j];
      density[0] = all;

      const real64 startAreaPdf = 1.0 / (static_cast<real64>( emittersCount ) *
         c[0].area);

      // strategies 1..n-1: light-start area density, skipping the connection
      // edge s'-1 (which carries no sampling pdf); the emission cosine pdf
      // is already part of edge 0's factor when that edge is generated
      for( int32 sp = 1; sp < n; ++sp )
      {
         real64 d = startAreaPdf;
         for( int32 j = 0; j < n - 1; ++j )
         {
            if( j != (sp - 1) ) d *= ef[j];
         }
         density[sp] = d;
      }
      // strategy n (light splat onto the pinhole camera) stays zero
      return density;
   }
}




CASE( densities )
{
   // Two emitters of different area plus diffuse triangles, black env.
   const std::string text =
      "(0 0 0) (2 0 0) (0 2 0)     (0.5 0.5 0.5) (10 10 10)\n"
      "(0 0 2) (1 0 2) (0 1 2)     (0.3 0.3 0.3) (5 5 5)\n"
      "(5 0 0) (5 0 2) (5 2 0)     (0.7 0.7 0.7) (0 0 0)\n"
      "(3 3 3) (4 3 3) (3 4 3)     (0.2 0.6 0.4) (0 0 0)\n";
   Scene scene = makeScene( text );
   CHECK( scene.isEnvironmentBlack() );
   CHECK( scene.getEmittersCount() == 2 );
   CHECK( close( scene.getEmitters()[0]->getArea(), 2.0 ) );
   CHECK( close( scene.getEmitters()[1]->getArea(), 0.5 ) );

   Bidirectional bidir( scene, 12 );

   // hand-built, non-coplanar 4-vertex chain; both ends are emitters so the
   // reversed chain is also a valid path
   std::vector<BidirVertex> chain;
   chain.push_back( vertexAt( scene, 0, Vector3f( 0.5, 0.0, 0.3 ) ) );
   chain.push_back( vertexAt( scene, 2, Vector3f( 5.0, 0.7, 0.8 ) ) );
   chain.push_back( vertexAt( scene, 3, Vector3f( 3.2, 3.2, 3.0 ) ) );
   chain.push_back( vertexAt( scene, 1, Vector3f( 0.3, 0.4, 2.0 ) ) );
   const int32 n = 4;

   // eye sits on the +z side of the last vertex (normal +z), so the eye ray
   // meets its front face
   const Vector3f eyeRayDir( Vector3f( 0.0, -1.0, -0.1 ).unitize() );
   const std::vector<real64> ref( referenceDensities( chain, 2 ) );

   // every feasible realized strategy: density per strategy, balance weight,
   // and consistency across calls
   real64 weightSum = 0.0;
   for( int32 realizedS = 0; realizedS < n; ++realizedS )
   {
      const BidirSplit sp = bidir.evaluateTestPath( chain, realizedS, eyeRayDir );
      CHECK( sp.feasible );
      CHECK( sp.s == realizedS );
      CHECK( sp.t == n - realizedS );
      CHECK( static_cast<int32>( sp.strategyDensities.size() ) == n + 1 );

      real64 densitySum = 0.0;
      for( int32 k = 0; k <= n; ++k )
      {
         CHECK( close( sp.strategyDensities[k], ref[k], 1.0e-11 ) );
         if( sp.strategyFeasible[k] ) densitySum += sp.strategyDensities[k];
      }
      // camera splat strategy is outside the support domain
      CHECK( sp.strategyDensities[n] == 0.0 );
      CHECK( !sp.strategyFeasible[n] );
      CHECK( sp.strategyFeasible[realizedS] );

      CHECK( close( sp.realizedDensity, ref[realizedS] ) );
      CHECK( close( sp.weight, ref[realizedS] / densitySum ) );
      CHECK( close( sp.contribution[0],
         sp.unweighted[0] * sp.weight, 1.0e-12 ) );
      CHECK( sp.throughput[0] > 0.0 );
      weightSum += sp.weight;
   }
   // feasible strategy weights partition unity
   CHECK( close( weightSum, 1.0, 1.0e-12 ) );

   // reversed chain (emitter at the other end), independent check again
   std::vector<BidirVertex> reversed;
   for( int32 i = n - 1; i >= 0; --i ) reversed.push_back( chain[i] );
   const Vector3f reversedEyeRay(
      (chain[0].position - chain[1].position).unitize() );
   const std::vector<real64> refRev( referenceDensities( reversed, 2 ) );
   const BidirSplit rev = bidir.evaluateTestPath( reversed, 2, reversedEyeRay );
   CHECK( rev.feasible );
   for( int32 k = 0; k <= n; ++k )
   {
      CHECK( close( rev.strategyDensities[k], refRev[k], 1.0e-11 ) );
   }
   CHECK( close( rev.weight, refRev[2] /
      (refRev[0] + refRev[1] + refRev[2] + refRev[3]) ) );

   // a shorter, non-coplanar three-vertex chain, independent density check
   {
      std::vector<BidirVertex> three;
      three.push_back( vertexAt( scene, 0, Vector3f( 0.4, 0.0, 0.6 ) ) );
      three.push_back( vertexAt( scene, 2, Vector3f( 5.0, 1.1, 1.3 ) ) );
      three.push_back( vertexAt( scene, 1, Vector3f( 0.6, 0.5, 2.0 ) ) );
      const int32 n3 = 3;
      // eye on the -z side of the last vertex, matching the incoming side
      const Vector3f eyeRayDir3( Vector3f( 0.0, -1.0, 0.2 ).unitize() );
      const std::vector<real64> ref3( referenceDensities( three, 2 ) );
      real64 w3 = 0.0;
      for( int32 realizedS = 0; realizedS < n3; ++realizedS )
      {
         const BidirSplit sp3 = bidir.evaluateTestPath( three, realizedS,
            eyeRayDir3 );
         CHECK( sp3.feasible );
         real64 sum3 = 0.0;
         for( int32 k = 0; k <= n3; ++k )
         {
            CHECK( close( sp3.strategyDensities[k], ref3[k], 1.0e-11 ) );
            if( sp3.strategyFeasible[k] ) sum3 += sp3.strategyDensities[k];
         }
         CHECK( sp3.strategyDensities[n3] == 0.0 );
         CHECK( close( sp3.weight, ref3[realizedS] / sum3 ) );
         w3 += sp3.weight;
      }
      CHECK( close( w3, 1.0, 1.0e-12 ) );
   }

   // impossible split: emission leaves the emitter's back face
   std::vector<BidirVertex> back( chain );
   back[0].normal = -back[0].normal;
   const BidirSplit bad = bidir.evaluateTestPath( back, 1, eyeRayDir );
   CHECK( !bad.feasible );
   CHECK( bad.weight == 0.0 );
   CHECK( bad.contribution.isZero() );
   for( int32 k = 0; k <= n; ++k ) CHECK( !bad.strategyFeasible[k] );

   // impossible split: zero-distance connection
   std::vector<BidirVertex> coincident( chain );
   coincident[1].position = coincident[0].position;
   const BidirSplit zero = bidir.evaluateTestPath( coincident, 1, eyeRayDir );
   CHECK( !zero.feasible );
   CHECK( zero.contribution.isZero() );

   // impossible split: two endpoints on the same triangle
   std::vector<BidirVertex> sameTri( chain );
   sameTri[1] = vertexAt( scene, 0, Vector3f( 0.7, 0.0, 0.7 ) );
   const BidirSplit same = bidir.evaluateTestPath( sameTri, 1, eyeRayDir );
   CHECK( !same.feasible );

   // out-of-range realized strategy is rejected
   const BidirSplit oor = bidir.evaluateTestPath( chain, n, eyeRayDir );
   CHECK( !oor.feasible );
}




namespace
{
   const char* CORNER_SCENE =
      // floor (y=0), back wall (z=3), both diffuse
      "(0 0 0) (4 0 0) (0 0 4)        (0.7 0.7 0.7) (0 0 0)\n"
      "(0 0 3) (4 0 3) (4 3 3)        (0.6 0.2 0.2) (0 0 0)\n"
      // emitter strip under the ceiling, front face downward (-y)
      "(0 3 0) (2 3 0) (0 3 2)        (0.4 0.4 0.4) (12 12 12)\n";

   const char* OCCLUDER_SCENE =
      // floor receiver
      "(0 0 0) (6 0 0) (0 0 6)        (0.7 0.7 0.7) (0 0 0)\n"
      // ceiling emitter, front face downward
      "(0 3 0) (3 3 0) (0 3 3)        (0.4 0.4 0.4) (20 20 20)\n"
      // black occluder slab between emitter and part of the floor
      "(1 1.4 1) (3 1.4 1) (3 1.4 3)  (0 0 0)       (0 0 0)\n"
      "(3 1.4 1) (1 1.4 1) (1 1.4 3)  (0 0 0)       (0 0 0)\n";

   const char* MIXED_EMITTER_SCENE =
      // large emitter A (area 2) and small emitter B (area 0.125)
      "(0 3 0) (2 3 0) (0 3 2)        (0.5 0.5 0.5) (8 8 8)\n"
      "(4 3 4) (4.5 3 4) (4 3 4.5)    (0.5 0.5 0.5) (30 30 30)\n"
      // floor and wall
      "(0 0 0) (6 0 0) (0 0 6)        (0.7 0.7 0.7) (0 0 0)\n"
      "(0 0 6) (6 0 6) (6 3 6)        (0.3 0.5 0.7) (0 0 0)\n";


   int countFeasible( const BidirDebug& rec )
   {
      int c = 0;
      for( const BidirSplit& s : rec.splits ) if( s.feasible ) ++c;
      return c;
   }
}


CASE( connections )
{
   // ---- corner: real two-bounce indirect light around the corner ----------
   {
      Scene scene = makeScene( CORNER_SCENE, Vector3f( 0.3, 1.0, -1.0 ) );
      Bidirectional bidir( scene, 4 );

      BidirDebug rec;
      Random rnd( 246813579u );
      const Vector3f value = bidir.getRadiance(
         Vector3f( 0.3, 1.0, -1.0 ), Vector3f( 0.1, -0.2, 1.0 ).unitize(),
         rnd, rec );
      CHECK( !rec.splits.empty() );
      CHECK( countFeasible( rec ) > 0 );
      (void)value;

      bool sawConnection = false;
      bool sawEmitterHit = false;
      for( const BidirSplit& s : rec.splits )
      {
         // depth truncation applies to the whole path
         CHECK( s.s + s.t <= 4 );
         if( !s.feasible )
         {
            CHECK( s.contribution.isZero() );
            continue;
         }
         CHECK( s.realizedDensity > 0.0 );
         CHECK( s.weight > 0.0 && s.weight <= 1.0 );
         CHECK( close( s.contribution[0],
            s.unweighted[0] * s.weight, 1.0e-12 ) );

         // real geometry records
         CHECK( s.vertices.size() ==
            static_cast<size_t>( s.s + s.t ) );
         for( const BidirVertex& v : s.vertices )
         {
            CHECK( v.triangle != 0 );
            CHECK( v.area > 0.0 );
         }
         // a light sub-path that generated its first edge records the
         // cosine conditional (solid-angle) density of that direction
         if( s.s >= 2 && s.feasible )
         {
            CHECK( s.vertices[0].condPdfW > 0.0 );
            CHECK( s.vertices[0].condPdfW <= 1.0 / PI + 1.0e-12 );
         }

         if( s.s == 0 ) sawEmitterHit = true;
         else           sawConnection = true;

         // same-side hemisphere condition on every internal vertex
         for( int32 k = 1; k < static_cast<int32>( s.vertices.size() ); ++k )
         {
            const Vector3f in( (s.vertices[ k - 1 ].position -
               s.vertices[k].position).unitize() );
            const Vector3f out( (k + 1 ==
               static_cast<int32>( s.vertices.size() )) ?
               Vector3f( 0.0, 0.0, 0.0 ) :
               (s.vertices[ k + 1 ].position -
                  s.vertices[k].position).unitize() );
            if( !out.isZero() )
            {
               const real64 a = in.dot( s.vertices[k].normal );
               const real64 b = out.dot( s.vertices[k].normal );
               CHECK( (a < 0.0) == (b < 0.0) );
            }
         }
      }
      CHECK( sawConnection );

      // an eye-side internal vertex of a t >= 2 connection must record the
      // cosine conditional density of the direction generated during the eye
      // random walk (the last eye vertex generates no next edge)
      bool sawEyeCondPdf = false;
      for( int32u seed = 300u; seed < 360u && !sawEyeCondPdf; ++seed )
      {
         BidirDebug r2;
         Random rr( seed * 48611u + 5u );
         bidir.getRadiance( Vector3f( 0.3, 1.0, -1.0 ),
            Vector3f( 0.1, -0.2, 1.0 ).unitize(), rr, r2 );
         for( const BidirSplit& s : r2.splits )
         {
            if( !s.feasible || s.t < 2 ) continue;
            for( int32 k = s.s; k < s.s + s.t - 1; ++k )
            {
               if( s.vertices[ k ].condPdfW > 0.0 )
               {
                  sawEyeCondPdf = true;
                  CHECK( s.vertices[ k ].condPdfW <= 1.0 / PI + 1.0e-12 );
               }
            }
         }
      }
      CHECK( sawEyeCondPdf );

      // an eye ray aimed straight at the emitter produces a direct
      // (s == 0) emitter-hit split
      const Vector3f emitterPoint( 0.7, 3.0, 0.7 );
      BidirDebug hitRec;
      Random hitRnd( 192837465u );
      bidir.getRadiance( Vector3f( 0.3, 1.0, -1.0 ),
         (emitterPoint - Vector3f( 0.3, 1.0, -1.0 )).unitize(),
         hitRnd, hitRec );
      for( const BidirSplit& s : hitRec.splits )
      {
         if( s.s == 0 && s.feasible &&
            s.vertices[0].triangle == scene.getEmitters()[0] )
         {
            sawEmitterHit = true;
         }
      }
      CHECK( sawEmitterHit );
   }

   // ---- finite occlusion: blocked connection contributes nothing ----------
   {
      Scene scene = makeScene( OCCLUDER_SCENE );
      Bidirectional bidir( scene, 3 );

      int unblocked = 0;
      int blockedHere = 0;
      for( int32u seed = 100u; seed < 160u; ++seed )
      {
         BidirDebug rec;
         Random rnd( seed * 7919u + 17u );
         bidir.getRadiance( Vector3f( 3.0, 0.9, -1.0 ),
            Vector3f( -0.4, -0.5, 1.0 ).unitize(), rnd, rec );
         for( const BidirSplit& s : rec.splits )
         {
            if( s.s >= 1 && s.t >= 1 )
            {
               if( s.feasible ) ++unblocked;
               else if( s.occluded ) ++blockedHere;
            }
         }
      }
      CHECK( unblocked > 0 );
      // across the seeds some valid paths are blocked by the slab
      CHECK( blockedHere > 0 );

      // an occluded connection really is occluded by another triangle in
      // the spatial index: aim the eye ray at the floor under the slab and
      // try seeds until a blocked connection shows up
      const Vector3f eyeOrigin( 3.0, 0.9, -1.0 );
      const Vector3f eyeDir(
         (Vector3f( 2.0, 0.0, 2.0 ) - eyeOrigin).unitize() );
      int confirmedOccluders = 0;
      for( int32u seed = 700u; seed < 760u; ++seed )
      {
         BidirDebug rec;
         Random rnd( seed * 104729u + 3u );
         bidir.getRadiance( eyeOrigin, eyeDir, rnd, rec );
         for( const BidirSplit& s : rec.splits )
         {
            if( !s.occluded ) continue;

            const BidirVertex& a = s.vertices[ s.s - 1 ];
            const BidirVertex& b = s.vertices[ s.s ];
            const Vector3f dir( (b.position - a.position).unitize() );
            const Triangle* hit = 0;
            Vector3f hp;
            scene.getIntersection( a.position, dir, a.triangle, hit, hp );
            CHECK( hit != 0 );
            // a genuine middle triangle, not either endpoint, blocks the segment
            CHECK( hit != b.triangle );
            CHECK( hit != a.triangle );
            ++confirmedOccluders;
         }
         if( confirmedOccluders > 0 ) break;
      }
      CHECK( confirmedOccluders > 0 );
   }

   // ---- emitters differing in area and number ------------------------------
   {
      Scene scene = makeScene( MIXED_EMITTER_SCENE );
      CHECK( scene.getEmittersCount() == 2 );
      Bidirectional bidir( scene, 4 );

      BidirDebug rec;
      Random rnd( 135792468u );
      bidir.getRadiance( Vector3f( 3.0, 0.8, -1.0 ),
         Vector3f( 0.0, -0.4, 1.0 ).unitize(), rnd, rec );

      bool checkedA = false;
      bool checkedB = false;
      for( const BidirSplit& s : rec.splits )
      {
         if( !s.feasible || s.s < 1 ) continue;
         const Triangle* first = s.vertices[0].triangle;
         const real64 startPdf = 1.0 / (2.0 * first->getArea());

         // independent density of the s'=1 strategy for this chain:
         // light-start area density, with edge 0 (the connection edge)
         // carrying no sampling pdf
         real64 product = 1.0;
         for( int32 j = 0;
            j < static_cast<int32>( s.vertices.size() ) - 1; ++j )
         {
            if( j != 0 ) product *= edgeFactorForward(
               s.vertices[j], s.vertices[ j + 1 ] );
         }
         const real64 expect = startPdf * product;
         CHECK( close( s.strategyDensities[1], expect, 1.0e-10 ) );

         if( first == scene.getEmitters()[0] ) checkedA = true;
         if( first == scene.getEmitters()[1] ) checkedB = true;
      }
      CHECK( checkedA || checkedB );
   }

   // ---- emitting surface also reflects: direct emitter hit split ----------
   {
      Scene scene = makeScene( MIXED_EMITTER_SCENE );
      Bidirectional bidir( scene, 3 );

      BidirDebug rec;
      Random rnd( 975310864u );
      // aim straight up at the large emitter
      bidir.getRadiance( Vector3f( 0.5, 0.5, 0.5 ),
         Vector3f( -0.1, 1.0, -0.1 ).unitize(), rnd, rec );

      bool direct = false;
      for( const BidirSplit& s : rec.splits )
      {
         if( s.s == 0 && s.feasible &&
            s.vertices[0].triangle == scene.getEmitters()[0] )
         {
            direct = true;
            CHECK( s.vertices[0].isEmitter );
            CHECK( s.contribution[0] > 0.0 );
         }
      }
      CHECK( direct );
   }

   // ---- depth boundary ------------------------------------------------------
   {
      Scene scene = makeScene( CORNER_SCENE );
      Bidirectional bidir1( scene, 1 );
      BidirDebug rec1;
      Random r1( 111111111u );
      bidir1.getRadiance( Vector3f( 0.3, 1.0, -1.0 ),
         Vector3f( 0.1, -0.2, 1.0 ).unitize(), r1, rec1 );
      for( const BidirSplit& s : rec1.splits )
      {
         CHECK( s.s + s.t <= 1 );
         if( s.s >= 1 ) CHECK( s.t == 0 ); // no connections at depth 1
      }

      Bidirectional bidir2( scene, 2 );
      BidirDebug rec2;
      Random r2( 222222222u );
      bidir2.getRadiance( Vector3f( 0.3, 1.0, -1.0 ),
         Vector3f( 0.1, -0.2, 1.0 ).unitize(), r2, rec2 );
      bool sawConnectionAt2 = false;
      for( const BidirSplit& s : rec2.splits )
      {
         CHECK( s.s + s.t <= 2 );
         if( s.s >= 1 && s.t >= 1 ) sawConnectionAt2 = true;
      }
      CHECK( sawConnectionAt2 );

      // construction rejects out-of-range depths
      bool threw = false;
      try { Bidirectional bad( scene, 13 ); }
      catch( const std::exception& ) { threw = true; }
      CHECK( threw );
      threw = false;
      try { Bidirectional bad( scene, 0 ); }
      catch( const std::exception& ) { threw = true; }
      CHECK( threw );
   }

   // ---- non-black environment is rejected ----------------------------------
   {
      std::istringstream in(
         "(0.2 0.2 0.2) (0.1 0.1 0.1)\n"
         "(0 0 0) (1 0 0) (0 1 0) (0.5 0.5 0.5) (0 0 0)\n" );
      in.exceptions( std::ios::badbit | std::ios::eofbit );
      Scene bright( in, Vector3f( 0, 0, -2 ) );
      bool threw = false;
      try { Bidirectional bad( bright, 4 ); }
      catch( const std::exception& ) { threw = true; }
      CHECK( threw );
   }

   // ---- per-call independence: identical seeds give identical records ------
   {
      Scene scene = makeScene( CORNER_SCENE );
      Bidirectional bidir( scene, 4 );
      BidirDebug a, b;
      Random ra( 864209753u );
      Random rb( 864209753u );
      const Vector3f o( 0.3, 1.0, -1.0 );
      const Vector3f d( 0.1, -0.2, 1.0 );
      const Vector3f va = bidir.getRadiance( o, d.unitize(), ra, a );
      const Vector3f vb = bidir.getRadiance( o, d.unitize(), rb, b );
      CHECK( close( va[0], vb[0] ) );
      CHECK( a.splits.size() == b.splits.size() );
      for( size_t i = 0; i < a.splits.size(); ++i )
      {
         CHECK( a.splits[i].s == b.splits[i].s );
         CHECK( a.splits[i].t == b.splits[i].t );
         CHECK( close( a.splits[i].realizedDensity,
            b.splits[i].realizedDensity ) );
         CHECK( close( a.splits[i].weight, b.splits[i].weight ) );
      }

      // a scene without emitters is black and never divides by zero
      std::istringstream in(
         "(0 0 0) (0 0 0)\n"
         "(0 0 0) (1 0 0) (0 1 0) (0.5 0.5 0.5) (0 0 0)\n" );
      in.exceptions( std::ios::badbit | std::ios::eofbit );
      Scene dark( in, Vector3f( 0, 0, -2 ) );
      Bidirectional bd( dark, 4 );
      BidirDebug rec;
      Random rnd( 314159265u );
      const Vector3f black = bd.getRadiance(
         Vector3f( 0, 0, -2 ), Vector3f( 0, 0, 1 ), rnd, rec );
      CHECK( black.isZero() );
      CHECK( rec.splits.empty() );
   }
}




namespace
{
   // minimal closed room: black walls plus a grey floor receiver and one
   // ceiling emitter; everything but the floor has zero albedo, so the
   // floor receives direct light only
   const char* QUAD_SCENE =
      // floor y=0, normal +y
      "(0 0 0) (4 0 0) (0 0 4)        (0.7 0.7 0.7) (0 0 0)\n"
      "(4 0 0) (4 0 4) (0 0 4)        (0.7 0.7 0.7) (0 0 0)\n"
      // single ceiling emitter, front face downward (-y); no overlapping
      // ceiling triangles so an upward ray cannot hit a coplanar occluder
      "(1 3 1) (2 3 1) (1 3 2)        (0 0 0)       (16 16 16)\n"
      // walls, black
      "(0 0 0) (0 0 4) (0 3 4)        (0 0 0)       (0 0 0)\n"
      "(0 0 0) (0 3 4) (0 3 0)        (0 0 0)       (0 0 0)\n"
      "(4 0 0) (4 3 0) (4 3 4)        (0 0 0)       (0 0 0)\n"
      "(4 0 0) (4 0 4) (4 3 4)        (0 0 0)       (0 0 0)\n"
      "(0 0 4) (4 0 4) (4 3 4)        (0 0 0)       (0 0 0)\n"
      "(0 0 4) (0 3 4) (4 3 4)        (0 0 0)       (0 0 0)\n";


   // larger room with a wide ceiling emitter and grey walls: real
   // two-bounce indirect transport exists here, and the big emitter keeps
   // the pure unidirectional walk's variance manageable
   const char* WALK_SCENE =
      // floor y=0, normal +y
      "(0 0 0) (4 0 0) (0 0 4)        (0.7 0.7 0.7) (0 0 0)\n"
      "(4 0 0) (4 0 4) (0 0 4)        (0.7 0.7 0.7) (0 0 0)\n"
      // large ceiling emitter (area ~6.48, front face downward -y) that also
      // reflects, so the pure walk reaches an emitter often (low variance)
      // while grey walls still create two-bounce indirect transport
      "(0.2 3 0.2) (3.8 3 0.2) (0.2 3 3.8)  (0.3 0.3 0.3) (6 6 6)\n"
      // grey walls
      "(0 0 0) (0 0 4) (0 3 4)        (0.5 0.5 0.5) (0 0 0)\n"
      "(0 0 0) (0 3 4) (0 3 0)        (0.5 0.5 0.5) (0 0 0)\n"
      "(4 0 0) (4 3 0) (4 3 4)        (0.5 0.5 0.5) (0 0 0)\n"
      "(4 0 0) (4 0 4) (4 3 4)        (0.5 0.5 0.5) (0 0 0)\n"
      "(0 0 4) (4 0 4) (4 3 4)        (0.5 0.5 0.5) (0 0 0)\n"
      "(0 0 4) (0 3 4) (4 3 4)        (0.5 0.5 0.5) (0 0 0)\n";


   /// stratified area quadrature of direct reflected radiance at a floor
   /// point: uniform area samples over the emitter, each weighted by the
   /// area-measure geometry kernel and the Lambert BRDF
   Vector3f quadrature( const Scene& scene, const Vector3f& p, int32 strata )
   {
      const Triangle& emitter = *scene.getEmitters()[0];
      const real64 area = emitter.getArea();
      Vector3f sum;
      real64 count = 0.0;
      for( int32 i = 0; i < strata; ++i )
      {
         for( int32 j = 0; j < strata; ++j )
         {
            // deterministic uniform-area point via the triangle's own
            // barycentric sampling with a fixed seed
            Random rng( static_cast<int32u>( i * 97u + j * 131u + 7u ) );
            const Vector3f q( emitter.getSamplePoint( rng ) );

            const Vector3f ray( q - p );
            const real64 d2 = ray.dot( ray );
            const Vector3f dir( ray.unitize() );

            const Triangle* hit = 0;
            Vector3f hp;
            // exclude the receiver triangle itself (p lies on it)
            scene.getIntersection( p, dir, &scene.getTriangles()[0],
               hit, hp );
            if( hit != &emitter ) continue;

            const real64 cosReceiver = ::fabs( dir.dot(
               scene.getTriangles()[0].getNormal() ) );
            const real64 cosEmitter = ::fabs( dir.dot( emitter.getNormal() ) );
            const real64 kernel = cosReceiver * cosEmitter / d2;
            sum = sum + emitter.getEmitivity() * kernel;
            count += 1.0;
         }
      }
      // mean of L_e * G over uniform area samples, scaled by emitter area,
      // then the receiver's Lambert BRDF albedo/pi
      const Vector3f irradiance( sum * (area /
         (count > 0.0 ? count : 1.0)) );
      return irradiance * (scene.getTriangles()[0].getReflectivity() / PI);
   }


   /// truncated pure unidirectional random walk (no next-event estimation,
   /// no russian roulette): independent reference for the same path-depth
   /// truncation as the bidirectional tracer
   Vector3f walk( const Scene& scene, const Vector3f& origin,
                  const Vector3f& direction, Random& rnd, int32 verticesLeft,
                  bool firstHit, const void* lastHit )
   {
      const Triangle* hit = 0;
      Vector3f hp;
      scene.getIntersection( origin, direction, lastHit, hit, hp );
      if( 0 == hit ) return Vector3f::ZERO();

      SurfacePoint sp( *hit, hp );
      Vector3f value;
      if( firstHit )
      {
         value = sp.getEmission( origin, -direction, false );
      }
      else if( !hit->getEmitivity().isZero() &&
         ((-direction).dot( hit->getNormal() ) > 0.0) )
      {
         // walk that ends on the emitter's front face: full emission
         // (the area-measure path term); the back face emits nothing
         value = hit->getEmitivity();
      }

      if( verticesLeft <= 1 ) return value;

      // cosine-weighted continuation in the incoming hemisphere
      const Vector3f n( hit->getNormal() );
      Vector3f hemi( n );
      if( (-direction).dot( hemi ) < 0.0 ) hemi = -hemi;

      const real64 phi = 2.0 * PI * rnd.getReal64();
      const real64 sr  = ::sqrt( rnd.getReal64() );
      const Vector3f tang( hit->getTangent() );
      const Vector3f nd( (tang * (::cos( phi ) * sr)) +
         (hemi.cross( tang ) * (::sin( phi ) * sr)) +
         (hemi * ::sqrt( 1.0 - sr * sr )) );

      // cosine sampling cancels the BRDF cosine/pi weight, leaving albedo
      value = value + hit->getReflectivity() *
         walk( scene, hp, nd, rnd, verticesLeft - 1, false, hit );
      return value;
   }


   struct Stats
   {
      Vector3f mean   = Vector3f::ZERO();
      Vector3f stderr = Vector3f::ZERO();
      Vector3f sum    = Vector3f::ZERO();
      Vector3f sum2   = Vector3f::ZERO();
      int32    n      = 0;
   };

   void accumulate( Stats& st, const Vector3f& v )
   {
      st.sum  = st.sum + v;
      st.sum2 = st.sum2 + v * v;
      ++st.n;
   }

   void finish( Stats& st )
   {
      const real64 inv = 1.0 / static_cast<real64>( st.n );
      st.mean  = st.sum * inv;
      for( int32 c = 0; c < 3; ++c )
      {
         const real64 var = st.sum2[c] * inv - st.mean[c] * st.mean[c];
         st.stderr = Vector3f( c == 0 ? ::sqrt( (var > 0.0 ? var : 0.0) /
            st.n ) : st.stderr[0],
            c == 1 ? ::sqrt( (var > 0.0 ? var : 0.0) / st.n ) : st.stderr[1],
            c == 2 ? ::sqrt( (var > 0.0 ? var : 0.0) / st.n ) : st.stderr[2] );
      }
   }
}


CASE( integral )
{
   Scene scene = makeScene( QUAD_SCENE, Vector3f( 2.0, 1.0, -1.0 ) );
   const int32 maxV = 3;
   Bidirectional bidir( scene, maxV );

   // ---- independent quadrature at a visible floor receiver ----------------
   const Vector3f p( 1.5, 0.0, 2.0 );
   const Vector3f eye( 1.5, 1.2, 0.4 );
   const Vector3f rayDir( (p - eye).unitize() );

   const Vector3f quad( quadrature( scene, p, 40 ) );
   CHECK( quad[0] > 0.0 );

   Stats bidirAtP;
   const int32 batches = 160;
   for( int32 b = 0; b < batches; ++b )
   {
      Random rnd( static_cast<int32u>( 424242u + b * 101u ) );
      BidirDebug rec;
      const Vector3f v = bidir.getRadiance( eye, rayDir, rnd, rec );
      accumulate( bidirAtP, v );
   }
   finish( bidirAtP );
   CHECK( bidirAtP.mean[0] > 0.0 );
   // indirect transport is zero here: black surroundings, so the bidir
   // estimate must agree with the direct-only area quadrature
   for( int32 c = 0; c < 3; ++c )
   {
      const real64 sigma = 5.0 * bidirAtP.stderr[c] + 1.0e-6;
      CHECK( ::fabs( bidirAtP.mean[c] - quad[c] ) < sigma );
   }
   std::cout << "quadrature receiver point: quad=" << quad[0]
             << " bidir=" << bidirAtP.mean[0]
             << " stderr=" << bidirAtP.stderr[0] << "\n";

   // ---- truncated unidirectional walk vs bidir on a real small image ------
   // a separate grey-walled scene with a wide emitter so genuine two-bounce
   // indirect transport exists and the pure walk has low enough variance
   Scene walkScene = makeScene( WALK_SCENE, Vector3f( 2.0, 1.0, -1.0 ) );
   Bidirectional walkBidir( walkScene, maxV );

   const int32 width = 8;
   const int32 height = 8;
   const int32 raysPerPixel = 150;

   Stats uni;
   Stats bd;
   // per-strategy statistics for the bidir run
   std::vector<Stats> strat( static_cast<size_t>( maxV + 1 ) );

   const Vector3f walkEye( 2.0, 1.0, -0.5 );
   for( int32 py = 0; py < height; ++py )
   {
      for( int32 px = 0; px < width; ++px )
      {
         const real64 xF = ((static_cast<real64>( px ) + 0.5) * 2.0 /
            width) - 1.0;
         const real64 yF = ((static_cast<real64>( py ) + 0.5) * 2.0 /
            height) - 1.0;
         const Vector3f right( 1.0, 0.0, 0.0 );
         const Vector3f up( 0.0, 1.0, 0.0 );
         const Vector3f dir( (Vector3f( 0.0, -0.15, 1.0 ) +
            (right * xF * 0.7) + (up * yF * 0.7)).unitize() );

         for( int32 r = 0; r < raysPerPixel; ++r )
         {
            const int32u seed = static_cast<int32u>(
               900000u + py * 9973u + px * 131u + r * 17u );

            Random ru( seed );
            accumulate( uni,
               walk( walkScene, walkEye, dir, ru, maxV, true, 0 ) );

            Random rb( seed );
            BidirDebug rec;
            const Vector3f bv = walkBidir.getRadiance( walkEye, dir, rb,
               rec );
            accumulate( bd, bv );

            // per strategy: each ray contributes exactly once per strategy
            // (zero when that split is infeasible), so record every strategy
            for( int32 s = 0; s <= maxV; ++s )
            {
               Vector3f contribution;
               for( const BidirSplit& sp : rec.splits )
               {
                  if( sp.feasible && sp.s == s )
                  {
                     contribution = contribution + sp.contribution;
                  }
               }
               accumulate( strat[ s ], contribution );
            }
         }
      }
   }
   finish( uni );
   finish( bd );
   for( int32 s = 0; s <= maxV; ++s ) finish( strat[ s ] );

   std::cout << "truncated means (maxVertices=" << maxV << "):\n";
   std::cout << "  unidirectional=" << uni.mean[0] << " stderr="
             << uni.stderr[0] << "\n";
   std::cout << "  bidirectional =" << bd.mean[0] << " stderr="
             << bd.stderr[0] << "\n";
   for( int32 s = 0; s <= maxV; ++s )
   {
      std::cout << "  strategy s=" << s << " samples=" << strat[s].n
                << " meanContribution=" << strat[s].mean[0]
                << " stderr=" << strat[s].stderr[0] << "\n";
   }
   // the per-strategy means must partition the bidirectional mean
   Vector3f stratTotal;
   for( int32 s = 0; s <= maxV; ++s ) stratTotal = stratTotal + strat[s].mean;
   CHECK( close( stratTotal[0], bd.mean[0], 1.0e-12 ) );

   CHECK( uni.mean[0] > 0.0 );
   CHECK( bd.mean[0] > 0.0 );
   // same truncated integral: agreement within four combined standard errors
   const real64 combined = 4.0 * ::sqrt(
      uni.stderr[0] * uni.stderr[0] + bd.stderr[0] * bd.stderr[0] );
   CHECK( ::fabs( uni.mean[0] - bd.mean[0] ) < combined );
   // both the eye emitter-hit strategy and a light connection contribute
   CHECK( strat[0].mean[0] > 0.0 );
   int32 nonzeroStrategies = 0;
   for( int32 s = 0; s <= maxV; ++s )
   {
      if( strat[s].mean[0] > 0.0 ) ++nonzeroStrategies;
   }
   CHECK( nonzeroStrategies >= 2 );

   // ---- new Camera entry renders a real small image ------------------------
   std::istringstream camIn( "(2 1 -0.5) (0 -0.15 1) 60" );
   Camera camera( camIn );
   std::istringstream imgIn( "10 8" );
   Image bidirImage( imgIn );
   std::istringstream imgIn2( "10 8" );
   Image oldImage( imgIn2 );
   Random cr1( 271828182u );
   Random cr2( 271828182u );
   camera.getFrame( scene, cr1, bidirImage, maxV );
   camera.getFrame( scene, cr2, oldImage );

   std::ostringstream bo;
   std::ostringstream oo;
   bidirImage.getFormatted( bo, 1 );
   oldImage.getFormatted( oo, 1 );
   const std::string bbytes = bo.str();
   const std::string obytes = oo.str();
   CHECK( bbytes.rfind( "P6", 0 ) == 0 );
   CHECK( obytes.rfind( "P6", 0 ) == 0 );
   CHECK( bbytes.size() == obytes.size() );
   CHECK( bbytes != obytes );
   int32 differing = 0;
   for( size_t i = 0; i < bbytes.size(); ++i )
   {
      if( bbytes[i] != obytes[i] ) ++differing;
   }
   CHECK( differing > 3 );
   std::cout << "camera bidir frame bytes=" << bbytes.size()
             << " differing=" << differing << "\n";
}

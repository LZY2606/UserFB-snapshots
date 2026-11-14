/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#include <math.h>
#include <stdexcept>

#include "Random.hpp"
#include "Scene.hpp"
#include "Triangle.hpp"

#include "Bidirectional.hpp"


using namespace hxa7241_minilight;




/// constants ------------------------------------------------------------------

namespace
{
   const real64 PI       = 3.14159265358979;
   const real64 DIST2MIN = 1e-24;
}




/// helpers --------------------------------------------------------------------

namespace
{

/**
 * Cosine-weighted hemisphere sample around the (already oriented) normal.
 * Same sampling formula as SurfacePoint::getNextDirection, but without
 * russian roulette; directional density is cos/PI.
 */
Vector3f sampleCosineDirection
(
   const Random&   random,
   const Vector3f& normal,
   const Vector3f& tangent
)
{
   const real64 _2pr1 = PI * 2.0 * random.getReal64();
   const real64 sr2   = ::sqrt( random.getReal64() );

   const real64 x = ::cos( _2pr1 ) * sr2;
   const real64 y = ::sin( _2pr1 ) * sr2;
   const real64 z = ::sqrt( 1.0 - (sr2 * sr2) );

   return (tangent * x) + (normal.cross( tangent ) * y) + (normal * z);
}


/**
 * Area density of generating 'next' from 'at', where 'at' is a diffuse
 * bounce vertex arrived at from inDir: cosine hemisphere on the side of
 * the incoming direction, converted to the target surface.
 */
real64 bounceEdgePdfA
(
   const Vector3f&                  inDir,
   const Bidirectional::PathVertex& at,
   const Bidirectional::PathVertex& next
)
{
   const Vector3f d( next.position - at.position );
   const real64   dist2 = d.dot( d );
   if( dist2 <= DIST2MIN )
   {
      return 0.0;
   }
   const Vector3f outDir( d.unitize() );

   const Vector3f normal( at.pTriangle->getNormal() );
   const real64   inDot  = normal.dot( inDir );
   const real64  outDot = normal.dot( outDir );

   // reflection must stay on the incoming side of the surface
   if( ((inDot < 0.0) != (outDot < 0.0)) | (inDot == 0.0) | (outDot == 0.0) )
   {
      return 0.0;
   }

   // directional density cos/PI, to area: * |cos(target)| / distance^2
   return (::fabs( outDot ) / PI) *
      ::fabs( next.pTriangle->getNormal().dot( outDir ) ) / dist2;
}


/**
 * Area density of generating 'next' from the light start vertex 'at':
 * single-sided cosine emission (front face only), converted to area.
 */
real64 emissionEdgePdfA
(
   const Bidirectional::PathVertex& at,
   const Bidirectional::PathVertex& next
)
{
   const Vector3f d( next.position - at.position );
   const real64   dist2 = d.dot( d );
   if( dist2 <= DIST2MIN )
   {
      return 0.0;
   }
   const Vector3f outDir( d.unitize() );

   const real64 cosOut = at.pTriangle->getNormal().dot( outDir );
   if( cosOut <= 0.0 )
   {
      return 0.0;
   }

   return (cosOut / PI) *
      ::fabs( next.pTriangle->getNormal().dot( outDir ) ) / dist2;
}


/**
 * Throughput of a complete path (chain ordered light -> eye): single-sided
 * emission, geometry terms, and Lambert reflectance at interior vertices.
 */
Vector3f chainThroughput
(
   const std::vector<Bidirectional::PathVertex>& vertices,
   const Vector3f&                               eyeOutDir
)
{
   const int32 k = static_cast<int32>( vertices.size() );

   // emission from front face only
   const Vector3f out0( k > 1 ?
      (vertices[1].position - vertices[0].position).unitize() : eyeOutDir );
   if( vertices[0].pTriangle->getNormal().dot( out0 ) <= 0.0 )
   {
      return Vector3f::ZERO();
   }
   Vector3f throughput( vertices[0].pTriangle->getEmitivity() );

   // geometry terms of all edges
   for( int32 i = 0;  i + 1 < k;  ++i )
   {
      const Vector3f d( vertices[i+1].position - vertices[i].position );
      const real64   dist2 = d.dot( d );
      if( dist2 <= DIST2MIN )
      {
         return Vector3f::ZERO();
      }
      const Vector3f dir( d.unitize() );

      const real64 c0 = vertices[i  ].pTriangle->getNormal().dot(  dir );
      const real64 c1 = vertices[i+1].pTriangle->getNormal().dot( -dir );

      throughput = throughput * (::fabs( c0 ) * ::fabs( c1 ) / dist2);
   }

   // Lambert reflectance at every non-emitter vertex (the last one
   // reflects toward the camera), same-side reflection only
   for( int32 i = 1;  i < k;  ++i )
   {
      const Vector3f inDir ( (vertices[i-1].position -
         vertices[i].position).unitize() );
      const Vector3f outDir( i + 1 < k ? (vertices[i+1].position -
         vertices[i].position).unitize() : eyeOutDir );
      const Vector3f normal( vertices[i].pTriangle->getNormal() );

      const real64 inDot  = normal.dot( inDir );
      const real64 outDot = normal.dot( outDir );
      if( ((inDot < 0.0) != (outDot < 0.0)) | (inDot == 0.0) |
         (outDot == 0.0) )
      {
         return Vector3f::ZERO();
      }

      throughput = throughput *
         (vertices[i].pTriangle->getReflectivity() / PI);
   }

   return throughput;
}

}//namespace




/// standard object services ---------------------------------------------------

Bidirectional::Bidirectional
(
   const Scene& scene,
   const int32  maxVertices
)
 : pScene_m     ( &scene )
 , maxVertices_m( maxVertices )
{
   if( (maxVertices < 1) | (maxVertices > 12) )
   {
      throw std::runtime_error(
         "Bidirectional: maxVertices outside 1..12" );
   }

   // only black sky and ground environment are supported
   if( !scene.getDefaultEmission( Vector3f( 0.0, -1.0, 0.0 ) ).isZero() |
      !scene.getDefaultEmission( Vector3f( 0.0, 1.0, 0.0 ) ).isZero() )
   {
      throw std::runtime_error(
         "Bidirectional: sky/ground environment must be black" );
   }
}




/// queries --------------------------------------------------------------------

Vector3f Bidirectional::getRadiance
(
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   const Random&   random,
   const void*     lastHit
) const
{
   return getRadiance( rayOrigin, rayDirection, random, lastHit, 0 );
}


Vector3f Bidirectional::getRadiance
(
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   const Random&   random,
   const void*     lastHit,
   PathInfo*       pInfo
) const
{
   // generate the two subpaths (all state is local to this call)
   std::vector<SubVertex> eye;
   std::vector<SubVertex> light;
   traceEyeSubpath( rayOrigin, rayDirection, lastHit, random, eye );
   traceLightSubpath( random, light );

   if( 0 != pInfo )
   {
      pInfo->paths.clear();
      pInfo->radiance = Vector3f::ZERO();
   }

   Vector3f radiance( Vector3f::ZERO() );

   const int32 emittersCount = pScene_m->getEmittersCount();
   const Vector3f eyeOutDir( !eye.empty() ?
      (rayOrigin - eye[0].position).unitize() : Vector3f::ZERO() );

   // eye subpath hits an emitter (s = 0, t >= 1)
   for( int32u t = 1u;  t <= eye.size();  ++t )
   {
      const SubVertex& xt = eye[t-1u];
      if( xt.pTriangle->getEmitivity().isZero() )
      {
         continue;
      }
      const Vector3f outDir( t > 1u ?
         (eye[t-2u].position - xt.position).unitize() :
         (rayOrigin - xt.position).unitize() );
      // emit from front face only
      if( xt.pTriangle->getNormal().dot( outDir ) <= 0.0 )
      {
         continue;
      }

      std::vector<PathVertex> chain;
      for( int32u j = t;  j-- > 0u; )
      {
         chain.push_back( PathVertex{ eye[j].position, eye[j].pTriangle } );
      }
      accumulate( chain, 0, emittersCount, eyeOutDir, radiance, pInfo );
   }

   // connections between light and eye prefixes (s >= 1, t >= 1)
   for( int32u s = 1u;  s <= light.size();  ++s )
   {
      for( int32u t = 1u;  t <= eye.size();  ++t )
      {
         if( s + t > static_cast<int32u>( maxVertices_m ) )
         {
            continue;
         }

         const SubVertex& ys = light[s-1u];
         const SubVertex& xt = eye[t-1u];

         // no connections on one surface, or of zero length
         if( ys.pTriangle == xt.pTriangle )
         {
            continue;
         }
         const Vector3f d( ys.position - xt.position );
         const real64   dist2 = d.dot( d );
         if( dist2 <= DIST2MIN )
         {
            continue;
         }

         // occlusion is only tested between the two endpoints: the nearest
         // hit from xt toward ys must be ys's own triangle at ys itself
         const Triangle* pHitObject = 0;
         Vector3f        hitPosition;
         pScene_m->getIntersection( xt.position, d.unitize(),
            xt.pTriangle, pHitObject, hitPosition );
         if( pHitObject != ys.pTriangle )
         {
            continue;
         }
         const Vector3f dd( hitPosition - ys.position );
         if( dd.dot( dd ) > dist2 * 1e-9 )
         {
            continue;
         }

         std::vector<PathVertex> chain;
         for( int32u i = 0u;  i < s;  ++i )
         {
            chain.push_back( PathVertex{
               light[i].position, light[i].pTriangle } );
         }
         for( int32u j = t;  j-- > 0u; )
         {
            chain.push_back( PathVertex{ eye[j].position, eye[j].pTriangle } );
         }
         accumulate( chain, static_cast<int32>( s ), emittersCount,
            eyeOutDir, radiance, pInfo );
      }
   }

   if( 0 != pInfo )
   {
      pInfo->radiance = radiance;
   }
   return radiance;
}


real64 Bidirectional::getSplitDensity
(
   const std::vector<PathVertex>& vertices,
   const int32                    s,
   const int32                    emittersCount,
   const Vector3f&                eyeOutDir
)
{
   const int32 k = static_cast<int32>( vertices.size() );
   if( (k < 1) | (s < 0) | (s > k - 1) )
   {
      return 0.0;
   }

   const Triangle& t0 = *vertices[0].pTriangle;

   real64 p = 1.0;

   // light end of the chain
   if( s >= 1 )
   {
      // light start: emitter choice and area probability
      if( (emittersCount <= 0) | t0.getEmitivity().isZero() |
         (t0.getArea() <= 0.0) )
      {
         return 0.0;
      }
      p = 1.0 / (static_cast<real64>( emittersCount ) * t0.getArea());
   }
   else
   {
      // s = 0: the eye walk must end on an emitter front face
      if( t0.getEmitivity().isZero() )
      {
         return 0.0;
      }
      const Vector3f out0( k > 1 ?
         (vertices[1].position - vertices[0].position).unitize() :
         eyeOutDir );
      if( t0.getNormal().dot( out0 ) <= 0.0 )
      {
         return 0.0;
      }
   }

   // light random-walk edges (first edge carries the emission density)
   for( int32 i = 0;  i + 1 <= s - 1;  ++i )
   {
      const real64 e = (0 == i) ?
         emissionEdgePdfA( vertices[0], vertices[1] ) :
         bounceEdgePdfA( (vertices[i-1].position -
            vertices[i].position).unitize(), vertices[i], vertices[i+1] );
      if( e <= 0.0 )
      {
         return 0.0;
      }
      p *= e;
   }

   // eye random-walk edges (first eye segment is the shared condition of
   // all strategies, so it appears in none of them)
   for( int32 j = k - 1;  j >= s + 1;  --j )
   {
      const Vector3f inDir( j == k - 1 ? eyeOutDir :
         (vertices[j+1].position - vertices[j].position).unitize() );
      const real64 e = bounceEdgePdfA( inDir, vertices[j], vertices[j-1] );
      if( e <= 0.0 )
      {
         return 0.0;
      }
      p *= e;
   }

   return p;
}


void Bidirectional::getSplitDensities
(
   const std::vector<PathVertex>& vertices,
   const int32                    emittersCount,
   const Vector3f&                eyeOutDir,
   std::vector<real64>&           densities_o,
   std::vector<real64>&           weights_o
)
{
   const int32 k = static_cast<int32>( vertices.size() );

   densities_o.assign( static_cast<int32u>( k ), 0.0 );
   weights_o.assign( static_cast<int32u>( k ), 0.0 );

   real64 sum = 0.0;
   for( int32 s = 0;  s < k;  ++s )
   {
      densities_o[ static_cast<int32u>( s ) ] =
         getSplitDensity( vertices, s, emittersCount, eyeOutDir );
      sum += densities_o[ static_cast<int32u>( s ) ];
   }

   // balance heuristic: strategy density over sum of all feasible densities
   if( sum > 0.0 )
   {
      for( int32 s = 0;  s < k;  ++s )
      {
         weights_o[ static_cast<int32u>( s ) ] =
            densities_o[ static_cast<int32u>( s ) ] / sum;
      }
   }
}


int32 Bidirectional::getMaxVertices() const
{
   return maxVertices_m;
}




/// implementation -------------------------------------------------------------

void Bidirectional::traceEyeSubpath
(
   const Vector3f&         rayOrigin,
   const Vector3f&         rayDirection,
   const void*             lastHit,
   const Random&           random,
   std::vector<SubVertex>& vertices_o
) const
{
   vertices_o.clear();

   // first hit needs no camera sampling density: the eye ray is the
   // shared condition of every strategy
   const Triangle* pHitObject = 0;
   Vector3f        hitPosition;
   pScene_m->getIntersection( rayOrigin, rayDirection, lastHit,
      pHitObject, hitPosition );
   if( 0 == pHitObject )
   {
      return;
   }

   vertices_o.push_back( SubVertex{ hitPosition, pHitObject } );

   Vector3f position( hitPosition );
   Vector3f inDir   ( -rayDirection );
   const void* last = pHitObject;

   // cosine-weighted continuation, no russian roulette, up to vertex limit
   while( vertices_o.size() < static_cast<int32u>( maxVertices_m ) )
   {
      const Triangle& triangle = *vertices_o.back().pTriangle;

      // hemisphere on the side of the incoming direction
      Vector3f normal( triangle.getNormal() );
      normal = normal.dot( inDir ) >= 0.0 ? normal : -normal;

      const Vector3f direction(
         sampleCosineDirection( random, normal, triangle.getTangent() ) );

      pScene_m->getIntersection( position, direction, last,
         pHitObject, hitPosition );
      if( 0 == pHitObject )
      {
         break;
      }

      vertices_o.push_back( SubVertex{ hitPosition, pHitObject } );
      position = hitPosition;
      inDir    = -direction;
      last     = pHitObject;
   }
}


void Bidirectional::traceLightSubpath
(
   const Random&           random,
   std::vector<SubVertex>& vertices_o
) const
{
   vertices_o.clear();

   // no emitters: the scene is black (and no emitter selection happens)
   const int32 emittersCount = pScene_m->getEmittersCount();
   if( (emittersCount <= 0) | (maxVertices_m < 2) )
   {
      return;
   }

   // light start: uniform emitter choice, uniform point on its area
   Vector3f        position;
   const Triangle* pTriangle = 0;
   pScene_m->getEmitter( random, position, pTriangle );
   if( 0 == pTriangle )
   {
      return;
   }
   vertices_o.push_back( SubVertex{ position, pTriangle } );

   // cosine-weighted continuation, up to vertex limit minus one
   // (at least one eye vertex is always needed); the first edge carries
   // single-sided cosine emission from the front face, later edges are
   // diffuse bounces on the side of the incoming direction
   Vector3f    inDir;
   bool        isFirst = true;
   const void* last    = pTriangle;
   while( vertices_o.size() < static_cast<int32u>( maxVertices_m - 1 ) )
   {
      const Triangle& triangle = *vertices_o.back().pTriangle;

      Vector3f normal( triangle.getNormal() );
      if( isFirst )
      {
         isFirst = false;
      }
      else
      {
         normal = normal.dot( inDir ) >= 0.0 ? normal : -normal;
      }

      const Vector3f direction(
         sampleCosineDirection( random, normal, triangle.getTangent() ) );

      const Triangle* pHitObject = 0;
      Vector3f        hitPosition;
      pScene_m->getIntersection( position, direction, last,
         pHitObject, hitPosition );
      if( 0 == pHitObject )
      {
         break;
      }

      vertices_o.push_back( SubVertex{ hitPosition, pHitObject } );
      position = hitPosition;
      inDir    = -direction;
      last     = pHitObject;
   }
}


void Bidirectional::accumulate
(
   const std::vector<PathVertex>& chain,
   const int32                    sampledS,
   const int32                    emittersCount,
   const Vector3f&                eyeOutDir,
   Vector3f&                      radiance_io,
   PathInfo*                      pInfo
) const
{
   const int32 k = static_cast<int32>( chain.size() );

   const Vector3f throughput( chainThroughput( chain, eyeOutDir ) );

   std::vector<real64> densities;
   std::vector<real64> weights;
   getSplitDensities( chain, emittersCount, eyeOutDir, densities, weights );

   real64 densitySum = 0.0;
   for( int32 s = 0;  s < k;  ++s )
   {
      densitySum += densities[ static_cast<int32u>( s ) ];
   }

   // balance-heuristic estimate: weight * throughput / density, which for
   // every feasible strategy equals throughput / densitySum
   Vector3f contribution( Vector3f::ZERO() );
   if( (densitySum > 0.0) & !throughput.isZero() )
   {
      contribution = throughput / densitySum;
   }
   radiance_io = radiance_io + contribution;

   if( 0 != pInfo )
   {
      PathRecord record;
      record.chain        = chain;
      record.sampledS     = sampledS;
      record.sampledT     = k - sampledS;
      record.contribution = contribution;
      for( int32 s = 0;  s < k;  ++s )
      {
         SplitInfo split;
         split.s       = s;
         split.t       = k - s;
         split.density = densities[ static_cast<int32u>( s ) ];
         split.weight  = weights  [ static_cast<int32u>( s ) ];
         split.contribution = densities[ static_cast<int32u>( s ) ] > 0.0 ?
            contribution : Vector3f( Vector3f::ZERO() );
         record.splits.push_back( split );
      }
      pInfo->paths.push_back( record );
   }
}

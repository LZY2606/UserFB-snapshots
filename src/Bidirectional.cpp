/*------------------------------------------------------------------------------

   MiniLight C++ : bidirectional path tracer

------------------------------------------------------------------------------*/


#include <math.h>
#include <stdexcept>

#include "Random.hpp"
#include "Scene.hpp"
#include "SurfacePoint.hpp"

#include "Bidirectional.hpp"


using namespace hxa7241_minilight;


namespace
{
   const real64 PI  = 3.14159265358979;
   const real64 EPS = 1.0e-12;

   /// cosine-weighted hemisphere sample around the given (unit) hemisphere
   /// normal; consumes two randoms (azimuth and radius), and returns the
   /// solid-angle pdf cos(theta)/pi. No russian roulette: every call yields
   /// a direction.
   Vector3f cosineSample
   (
      const Vector3f& hemisphereNormal,
      const Vector3f& tangent,
      Random&         random,
      real64&         pdfWo
   )
   {
      const real64 _2pr1 = PI * 2.0 * random.getReal64();
      const real64 sr2   = ::sqrt( random.getReal64() );

      const real64 x = ::cos( _2pr1 ) * sr2;
      const real64 y = ::sin( _2pr1 ) * sr2;
      const real64 z = ::sqrt( 1.0 - (sr2 * sr2) );

      const Vector3f direction( (tangent * x) +
         (hemisphereNormal.cross( tangent ) * y) + (hemisphereNormal * z) );

      pdfWo = direction.dot( hemisphereNormal ) / PI;
      return direction;
   }
}




/// standard object services ---------------------------------------------------

Bidirectional::Bidirectional
(
   const Scene& scene,
   int32        maxVertices
)
 : pScene_m( &scene )
 , maxVertices_m( maxVertices )
{
   if( (maxVertices < 1) | (maxVertices > 12) )
   {
      throw std::invalid_argument(
         "Bidirectional: maxVertices must be between 1 and 12" );
   }
   if( !scene.isEnvironmentBlack() )
   {
      throw std::invalid_argument(
         "Bidirectional: sky and ground environment must be black" );
   }
}




/// queries --------------------------------------------------------------------

BidirVertex Bidirectional::makeVertex
(
   const Triangle& triangle,
   const Vector3f& position,
   const real64    condPdfW
)
{
   BidirVertex v;
   v.position   = position;
   v.normal     = triangle.getNormal();
   v.albedo     = triangle.getReflectivity();
   v.emitivity  = triangle.getEmitivity();
   v.triangle   = &triangle;
   v.area       = triangle.getArea();
   v.condPdfW   = condPdfW;
   v.isEmitter  = !triangle.getEmitivity().isZero() &&
                  (triangle.getArea() > 0.0);
   return v;
}


int32 Bidirectional::getMaxVertices() const
{
   return maxVertices_m;
}


BidirSplit Bidirectional::evaluateTestPath
(
   const std::vector<BidirVertex>& chain,
   int32                           realizedS,
   const Vector3f&                 eyeRayDirection
) const
{
   if( (realizedS < 0) |
      (realizedS >= static_cast<int32>(chain.size())) )
   {
      BidirSplit empty;
      empty.s = realizedS;
      empty.vertices = chain;
      empty.feasible = false;
      return empty;
   }
   return evaluateChain( chain, realizedS, eyeRayDirection );
}


Vector3f Bidirectional::getRadiance
(
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   const Random&   random,
   const void*     lastHit
) const
{
   Random localRandom( random );
   return run( rayOrigin, rayDirection, localRandom, lastHit, 0 );
}


Vector3f Bidirectional::getRadiance
(
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   const Random&   random,
   BidirDebug&     record_o,
   const void*     lastHit
) const
{
   record_o = BidirDebug();
   Random localRandom( random );
   const Vector3f result( run( rayOrigin, rayDirection, localRandom, lastHit,
      &record_o ) );
   record_o.result = result;
   return result;
}




/// sub-path generation --------------------------------------------------------

void Bidirectional::generateLightPath
(
   Random&                 random,
   std::vector<SubVertex>& path_o
) const
{
   // same emitter selection as Scene::getEmitter: equal emitter probability,
   // uniform point over the chosen triangle
   Vector3f        startPosition;
   const Triangle* emitter = 0;
   pScene_m->getEmitter( random, startPosition, emitter );
   if( 0 == emitter ) return;

   path_o.push_back( SubVertex() );
   path_o.back().info = makeVertex( *emitter, startPosition, 0.0 );

   while( static_cast<int32>(path_o.size()) < maxVertices_m )
   {
      SubVertex&      from = path_o.back();
      const Vector3f& n    = from.info.normal;

      // first edge: cosine hemisphere over the emitter's front face;
      // later edges: hemisphere on the side the incoming ray arrived from
      Vector3f hemisphereNormal( n );
      if( path_o.size() > 1 )
      {
         const SubVertex& prev = path_o[ path_o.size() - 2u ];
         const Vector3f incoming( (from.info.position -
            prev.info.position).unitize() );
         if( incoming.dot( n ) < 0.0 ) hemisphereNormal = -n;
      }

      real64  pdfW = 0.0;
      const Vector3f direction( cosineSample( hemisphereNormal,
         from.info.triangle->getTangent(), random, pdfW ) );
      from.outDirection = direction;
      from.info.condPdfW = pdfW;

      const Triangle* hitObject = 0;
      Vector3f        hitPosition;
      pScene_m->getIntersection( from.info.position, direction,
         from.info.triangle, hitObject, hitPosition );
      if( 0 == hitObject ) break;

      SubVertex next;
      next.info = makeVertex( *hitObject, hitPosition, 0.0 );
      next.outDirection = Vector3f::ZERO();
      path_o.push_back( next );
   }
}


void Bidirectional::generateEyePath
(
   const Vector3f&         rayOrigin,
   const Vector3f&         rayDirection,
   const void*             lastHit,
   Random&                 random,
   std::vector<SubVertex>& path_o
) const
{
   const Triangle* hitObject = static_cast<const Triangle*>(lastHit);
   Vector3f        hitPosition;
   Vector3f        origin( rayOrigin );
   Vector3f        direction( rayDirection );

   while( static_cast<int32>(path_o.size()) < maxVertices_m )
   {
      const Triangle* nextObject = 0;
      pScene_m->getIntersection( origin, direction, hitObject, nextObject,
         hitPosition );
      if( 0 == nextObject ) break;

      // continue by cosine sampling in the hemisphere the ray arrived from
      const Vector3f incoming( -direction );
      Vector3f hemisphereNormal( nextObject->getNormal() );
      if( incoming.dot( hemisphereNormal ) < 0.0 )
      {
         hemisphereNormal = -hemisphereNormal;
      }

      real64 pdfW = 0.0;
      const Vector3f nextDirection( cosineSample( hemisphereNormal,
         nextObject->getTangent(), random, pdfW ) );

      // store the vertex together with the direction generated at it
      SubVertex vertex;
      vertex.info = makeVertex( *nextObject, hitPosition, pdfW );
      vertex.outDirection = nextDirection;
      path_o.push_back( vertex );

      origin      = hitPosition;
      direction   = nextDirection;
      hitObject   = nextObject;
   }
}




/// full-path evaluation -------------------------------------------------------

/**
 * Evaluate every strategy density (area measure on the internal vertices)
 * for an ordered chain v[0..n-1], v[0] the emitter end, v[n-1] the eye end.
 * realizedS is the light-side vertex count of the split that generated it.
 * The eye ray direction only affects the last vertex's hemisphere check.
 */
BidirSplit Bidirectional::evaluateChain
(
   const std::vector<BidirVertex>& chain,
   int32                            realizedS,
   const Vector3f&                  rayDirectionEye
) const
{
   BidirSplit split;
   split.s = realizedS;
   split.t = static_cast<int32>(chain.size()) - realizedS;
   split.vertices = chain;
   split.feasible = false;
   split.realizedDensity = 0.0;
   split.weight = 0.0;
   split.throughput = Vector3f::ZERO();
   split.unweighted = Vector3f::ZERO();
   split.contribution = Vector3f::ZERO();
   split.occluded = false;

   const int32 n = static_cast<int32>(chain.size());

   split.strategyFirst = 0;
   split.strategyDensities.assign( static_cast<size_t>(n + 1), 0.0 );
   split.strategyFeasible.assign( static_cast<size_t>(n + 1), 0 );

   if( n < 1 ) return split;

   // edge geometry: energy flows v[j] -> v[j+1]
   std::vector<real64> dist2( static_cast<size_t>(n - 1), 0.0 );
   std::vector<real64> cosFrom( static_cast<size_t>(n - 1), 0.0 );
   std::vector<real64> cosTo( static_cast<size_t>(n - 1), 0.0 );
   std::vector<real64> edgeFactor( static_cast<size_t>(n - 1), 0.0 );
   std::vector<char>   edgeValid( static_cast<size_t>(n - 1), 0 );

   bool geometryValid = chain[0].isEmitter;
   for( int32 j = 0;  j < n - 1;  ++j )
   {
      const Vector3f ray( chain[ j + 1 ].position - chain[j].position );
      dist2[j]   = ray.dot( ray );
      cosFrom[j] = ray.unitize().dot( chain[j].normal );
      cosTo[j]   = ray.unitize().dot( chain[ j + 1 ].normal );

      edgeValid[j] = (dist2[j] > EPS) &&
         (::fabs( cosFrom[j] ) > EPS) && (::fabs( cosTo[j] ) > EPS) &&
         (chain[j].triangle != chain[ j + 1 ].triangle);
      geometryValid = geometryValid && edgeValid[j];

      if( edgeValid[j] )
      {
         // area density of a cosine sampled walk edge, from either end:
         // p_w(o) * |cos target| / dist2
         edgeFactor[j] = ::fabs( cosFrom[j] ) * ::fabs( cosTo[j] ) /
            (PI * dist2[j]);
      }
   }

   // one-sided emission at the light end
   bool emissionValid = false;
   if( chain[0].isEmitter )
   {
      if( n > 1 )
      {
         // first edge must leave the emitter's front face
         emissionValid = cosFrom[0] > 0.0;
      }
      else
      {
         // single-vertex emitter hit: the eye ray must meet the front face
         emissionValid = (-rayDirectionEye).dot( chain[0].normal ) > 0.0;
      }
   }

   // reflection hemisphere checks at every internal vertex. The eye ray
   // direction only enters at the last vertex (the one adjacent to the eye).
   bool reflectionValid = true;
   for( int32 k = 1;  k < n;  ++k )
   {
      // incoming energy direction (away from surface, back toward v[k-1])
      const Vector3f incoming( (chain[ k - 1 ].position -
         chain[k].position).unitize() );
      // outgoing energy direction toward the camera side
      const Vector3f outgoing( (k == n - 1) ? -rayDirectionEye :
         (chain[ k + 1 ].position - chain[k].position).unitize() );

      const real64 inDot  = incoming.dot( chain[k].normal );
      const real64 outDot = outgoing.dot( chain[k].normal );
      if( (::fabs( inDot ) <= EPS) | (::fabs( outDot ) <= EPS) |
         ((inDot < 0.0) ^ (outDot < 0.0)) )
      {
         reflectionValid = false;
      }
   }

   // s' = n (light path splatted straight onto the pinhole camera) is not
   // supported; every other strategy shares the common support domain here,
   // since all surfaces are finite-area Lambertian triangles
   const bool commonValid = geometryValid && emissionValid && reflectionValid;

   if( !commonValid ) return split;

   const int32 emittersCount = pScene_m->getEmittersCount();
   if( emittersCount < 1 ) return split;

   const real64 startAreaPdf = 1.0 / (static_cast<real64>(emittersCount) *
      chain[0].area);

   // strategy s' = 0: whole chain walked from the eye
   real64 densityEye = 1.0;
   for( int32 j = 0;  j < n - 1;  ++j ) densityEye *= edgeFactor[j];
   split.strategyDensities[0] = densityEye;
   split.strategyFeasible[0]  = 1;

   // strategies 1 <= s' <= n-1: light-start area density (emitter pick and
   // uniform point), generated light edges (0..s'-2), generated eye edges
   // (s'..n-2). The connection edge s'-1 carries no sampling pdf at all.
   // The one-sided emission cosine pdf is inside edgeFactor[0] whenever
   // edge 0 is a generated light edge (s' >= 2); feasibility above already
   // guarantees it leaves the emitter's front face.
   for( int32 sp = 1;  sp < n;  ++sp )
   {
      real64 density = startAreaPdf;
      for( int32 j = 0;  j < n - 1;  ++j )
      {
         if( j != (sp - 1) ) density *= edgeFactor[j];
      }
      split.strategyDensities[ sp ] = density;
      split.strategyFeasible[ sp ]  = 1;
   }
   // strategy n stays zero: no camera splat

   // path throughput beta: L_e * prod f (albedo/pi) * prod G
   Vector3f beta( chain[0].emitivity );
   for( int32 k = 1;  k < n;  ++k )
   {
      beta = beta * (chain[k].albedo / PI);
   }
   for( int32 j = 0;  j < n - 1;  ++j )
   {
      const real64 geometry = ::fabs( cosFrom[j] ) * ::fabs( cosTo[j] ) /
         dist2[j];
      beta = beta * geometry;
   }

   real64 densitySum = 0.0;
   for( int32 sp = 0;  sp < n;  ++sp )
   {
      if( split.strategyFeasible[sp] )
      {
         densitySum += split.strategyDensities[sp];
      }
   }

   const real64 realizedDensity =
      split.strategyDensities[ realizedS ];
   if( (realizedDensity <= 0.0) | (densitySum <= 0.0) ) return split;

   split.feasible = true;
   split.realizedDensity = realizedDensity;
   split.weight = realizedDensity / densitySum;
   split.throughput = beta;
   split.unweighted = beta / realizedDensity;
   split.contribution = split.unweighted * split.weight;

   return split;
}




/// integration ----------------------------------------------------------------

Vector3f Bidirectional::run
(
   const Vector3f& rayOrigin,
   const Vector3f& rayDirection,
   Random&         random,
   const void*     lastHit,
   BidirDebug*     pRecord
) const
{
   Vector3f radiance;

   // a scene without emitters is black and must never divide by one
   if( pScene_m->getEmittersCount() < 1 ) return radiance;

   std::vector<SubVertex> lightPath;
   std::vector<SubVertex> eyePath;

   generateLightPath( random, lightPath );
   generateEyePath( rayOrigin, rayDirection, lastHit, random, eyePath );
   if( eyePath.empty() ) return radiance;

   // strategy s = 0: eye sub-path directly hits an emitting triangle.
   // the ordered chain is the eye prefix reversed, emitter end first.
   for( int32 k = 0;  k < static_cast<int32>(eyePath.size());  ++k )
   {
      if( !eyePath[k].info.isEmitter ) continue;
      const int32 n = k + 1;
      if( n > maxVertices_m ) break;

      std::vector<BidirVertex> chain;
      chain.reserve( static_cast<size_t>(n) );
      for( int32 i = k;  i >= 0;  --i ) chain.push_back( eyePath[i].info );

      const BidirSplit split( evaluateChain( chain, 0, rayDirection ) );
      if( pRecord ) pRecord->splits.push_back( split );
      if( split.feasible ) radiance = radiance + split.contribution;
   }

   // strategies s >= 1, t >= 1: connect light and eye prefixes.
   const int32 lightCount = static_cast<int32>(lightPath.size());
   const int32 eyeCount   = static_cast<int32>(eyePath.size());

   for( int32 s = 1;  s <= lightCount;  ++s )
   {
      for( int32 t = 1;  t <= eyeCount;  ++t )
      {
         if( (s + t) > maxVertices_m ) break;

         const SubVertex& lightEnd = lightPath[ s - 1 ];
         const SubVertex& eyeEnd   = eyePath[ t - 1 ];

         std::vector<BidirVertex> chain;
         chain.reserve( static_cast<size_t>(s + t) );
         for( int32 i = 0;  i < s;  ++i ) chain.push_back( lightPath[i].info );
         for( int32 i = 0;  i < t;  ++i ) chain.push_back( eyePath[i].info );

         BidirSplit split( evaluateChain( chain, s, rayDirection ) );

         // visibility: only the segment between the two endpoints matters.
         // lastHit suppresses the source triangle's own intersection;
         // anything between the endpoints invalidates the connection.
         bool visible = false;
         if( split.feasible )
         {
            const Vector3f connector( eyeEnd.info.position -
               lightEnd.info.position );
            const real64 connectorDistance2 = connector.dot( connector );
            const Vector3f connectorDirection( connector.unitize() );

            const Triangle* blocker = 0;
            Vector3f        blockerPosition;
            pScene_m->getIntersection( lightEnd.info.position,
               connectorDirection, lightEnd.info.triangle, blocker,
               blockerPosition );

            visible = (0 != blocker) &&
               (blocker == eyeEnd.info.triangle) &&
               (connectorDistance2 > EPS);
            // a middle triangle (not the target endpoint) blocks the segment
            if( !visible && (0 != blocker) &&
               (blocker != eyeEnd.info.triangle) )
            {
               split.occluded = true;
            }
         }
         split.feasible = split.feasible && visible;
         if( !visible ) split.contribution = Vector3f::ZERO();

         if( pRecord ) pRecord->splits.push_back( split );
         if( split.feasible ) radiance = radiance + split.contribution;
      }
   }

   return radiance;
}

/*------------------------------------------------------------------------------

   MiniLight C++ : bidirectional path tracer

   Finite-depth bidirectional path connection model: a real eye sub-path and
   a real light sub-path are independently generated, and every usable pair
   of prefixes is connected. All strategy densities are expressed on the same
   internal-vertex area measure and combined with the balance heuristic.

   Constant apart from the referenced Scene.

------------------------------------------------------------------------------*/


#ifndef Bidirectional_h
#define Bidirectional_h


#include <vector>

#include "Vector3f.hpp"
#include "Triangle.hpp"

#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Read-only description of one vertex of a generated full path.<br/><br/>
 *
 * All quantities come from the actual integration run that produced it.
 */
struct BidirVertex
{
   Vector3f        position;
   Vector3f        normal;
   Vector3f        albedo;
   Vector3f        emitivity;
   const Triangle* triangle;
   real64          area;
   /// solid-angle pdf used to generate the direction toward the next vertex
   /// in the owning sub-path (zero for the sub-path's last vertex)
   real64          condPdfW;
   bool            isEmitter;
};


/**
 * One enumerated full path (a pair of connected prefixes), together with the
 * density of every strategy that could generate the same vertex chain, and
 * the balance-heuristic result.
 */
struct BidirSplit
{
   /// number of light-side surface vertices (s == 0 is an eye emitter hit)
   int32                   s;
   /// number of eye-side surface vertices
   int32                   t;

   /// full path, v[0] is the emitter end, v[size-1] is the eye end
   std::vector<BidirVertex> vertices;

   /// strategy index of the first entry in strategyDensities
   int32                   strategyFirst;
   /// area-measure full-path density of each candidate strategy
   /// (strategyFirst + index is the strategy's s')
   std::vector<real64>     strategyDensities;
   /// whether each candidate strategy is inside its sampling support domain
   std::vector<char>       strategyFeasible;

   /// density of the strategy that actually generated this split
   real64                  realizedDensity;
   /// balance weight of the realized strategy (its share of the unit sum)
   real64                  weight;
   /// unweighted path throughput beta (Lambert/geometry terms only)
   Vector3f                throughput;
   /// unweighted estimator value beta / realizedDensity
   Vector3f                unweighted;
   /// weighted contribution actually added to the pixel radiance
   Vector3f                contribution;
   /// false when this split's connection is outside the support domain
   bool                    feasible;
   /// true when the path itself is valid but a middle triangle blocks the
   /// connecting segment (only meaningful for s >= 1, t >= 1 splits)
   bool                    occluded;
};


/**
 * All splits produced by one getRadiance call, and the summed result.
 */
struct BidirDebug
{
   std::vector<BidirSplit> splits;
   Vector3f                result;
};


/**
 * Bidirectional path tracer with a fixed surface-vertex depth cap.<br/><br/>
 *
 * @invariants
 * * pScene_m points to a Scene
 * * maxVertices_m is in [1,12]
 * * the scene environment (sky and ground) is black
 */
class Bidirectional
{
/// standard object services ---------------------------------------------------
public:
           Bidirectional( const Scene& scene,
                          int32        maxVertices );

// use defaults
//         ~Bidirectional();
//          Bidirectional( const Bidirectional& );
// Bidirectional& operator=( const Bidirectional& );


/// queries --------------------------------------------------------------------
           Vector3f getRadiance( const Vector3f& rayOrigin,
                                 const Vector3f& rayDirection,
                                 const Random&   random,
                                 const void*     lastHit = 0 )          const;

           /// as above, but also exposes every enumerated split for inspection
           Vector3f getRadiance( const Vector3f& rayOrigin,
                                 const Vector3f& rayDirection,
                                 const Random&   random,
                                 BidirDebug&     record_o,
                                 const void*     lastHit = 0 )          const;

           int32    getMaxVertices()                                     const;

           /// test/inspection adapter: evaluate every strategy density and the
           /// balance weight for a hand-specified ordered chain (v[0] is the
           /// emitter end). Does not trace rays; only evaluates the path.
           BidirSplit evaluateTestPath(
                           const std::vector<BidirVertex>& chain,
                           int32                           realizedS,
                           const Vector3f&                 eyeRayDirection ) const;


/// implementation -------------------------------------------------------------
protected:
   struct SubVertex
   {
      BidirVertex info;
      /// direction generated at this vertex toward the next sub-path vertex
      Vector3f    outDirection;
   };

           Vector3f run( const Vector3f& rayOrigin,
                         const Vector3f& rayDirection,
                         Random&         random,
                         const void*     lastHit,
                         BidirDebug*     pRecord )                      const;

           void     generateLightPath( Random&                  random,
                                       std::vector<SubVertex>&  path_o )  const;
           void     generateEyePath( const Vector3f&          rayOrigin,
                                     const Vector3f&          rayDirection,
                                     const void*              lastHit,
                                     Random&                  random,
                                     std::vector<SubVertex>&  path_o )    const;

           static BidirVertex makeVertex( const Triangle& triangle,
                                          const Vector3f& position,
                                          real64          condPdfW );

           BidirSplit evaluateChain( const std::vector<BidirVertex>& chain,
                                     int32                           realizedS,
                                     const Vector3f& rayDirectionEye ) const;

           const Scene* pScene_m;
           int32        maxVertices_m;
};


}//namespace




#endif//Bidirectional_h

/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef Bidirectional_h
#define Bidirectional_h


#include <vector>

#include "Vector3f.hpp"



#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Limited-depth bidirectional path connection estimator.<br/><br/>
 *
 * For each radiance query, one eye subpath and one light subpath are
 * generated, and every usable prefix of the two is connected: direct
 * emitter hits by the eye subpath (s = 0) and connections between s >= 1
 * light-side and t >= 1 eye-side surface vertices, with s + t limited by
 * maxVertices. No Russian roulette: all paths up to the vertex limit take
 * part. Competing splittings of the same complete path are combined with
 * the balance heuristic over area-measure path densities.<br/><br/>
 *
 * Only the existing ideal-diffuse triangles and finite emitting triangles
 * are supported: the sky and ground environment must both be black, and
 * no light-to-camera splatting is performed (pinhole camera).<br/><br/>
 *
 * Constant: each query is independent, no per-ray state is retained.
 *
 * @invariants
 * * pScene_m points to a Scene (is not 0)
 * * maxVertices_m is >= 1 and <= 12
 */
class Bidirectional
{
/// standard object services ---------------------------------------------------
public:
            /**
             * @throws std::runtime_error if maxVertices is outside 1..12,
             * or the scene sky/ground environment is not black
             */
            Bidirectional( const Scene&,
                           int32        maxVertices );

// use defaults
//         ~Bidirectional();
//          Bidirectional( const Bidirectional& );
// Bidirectional& operator=( const Bidirectional& );


/// commands -------------------------------------------------------------------


/// queries --------------------------------------------------------------------
           /**
            * Same role as RayTracer::getRadiance: ray and Random in,
            * linear RGB out.
            */
           Vector3f getRadiance( const Vector3f& rayOrigin,
                                 const Vector3f& rayDirection,
                                 const Random&   random,
                                 const void*     lastHit = 0 )          const;


/// test access ----------------------------------------------------------------
public:
           /**
            * Read-only description of one surface vertex of a complete path.
            */
           struct PathVertex
           {
              Vector3f        position;
              const Triangle* pTriangle;
           };

           /**
            * Per-split accounting of one complete path: the area-measure
            * density of the strategy, its balance-heuristic weight, and its
            * weighted contribution.
            */
           struct SplitInfo
           {
              int32    s;
              int32    t;
              real64   density;
              real64   weight;
              Vector3f contribution;
           };

           /**
            * One complete path assembled during a getRadiance evaluation,
            * with the densities/weights of all of its possible splittings.
            */
           struct PathRecord
           {
              std::vector<PathVertex> chain;       //< ordered light -> eye
              int32                   sampledS;    //< split actually sampled
              int32                   sampledT;
              Vector3f                contribution;
              std::vector<SplitInfo>  splits;      //< all splits, indexed by s
           };

           /**
            * Diagnostics of a single getRadiance evaluation.
            */
           struct PathInfo
           {
              std::vector<PathRecord> paths;
              Vector3f                radiance;
           };

           /**
            * As getRadiance, additionally recording read-only path data.
            */
           Vector3f getRadiance( const Vector3f& rayOrigin,
                                 const Vector3f& rayDirection,
                                 const Random&   random,
                                 const void*     lastHit,
                                 PathInfo*       info_o )               const;

           /**
            * Area-measure density of one splitting of a vertex chain ordered
            * from light (index 0) to eye; zero when the splitting is outside
            * the support of that strategy. The chain holds the surface
            * vertices only (the camera itself is not a vertex); eyeOutDir is
            * the unit direction from the last vertex toward the camera.
            */
   static  real64   getSplitDensity(
                        const std::vector<PathVertex>& vertices,
                        int32                          s,
                        int32                          emittersCount,
                        const Vector3f&                eyeOutDir );

           /**
            * Densities and balance-heuristic weights of all splittings
            * (s = 0..k-1) of a vertex chain; weights sum to 1 whenever any
            * splitting is feasible.
            */
   static  void     getSplitDensities(
                        const std::vector<PathVertex>& vertices,
                        int32                          emittersCount,
                        const Vector3f&                eyeOutDir,
                        std::vector<real64>&           densities_o,
                        std::vector<real64>&           weights_o );

           int32    getMaxVertices()                                      const;


/// implementation -------------------------------------------------------------
private:
           struct SubVertex
           {
              Vector3f        position;
              const Triangle* pTriangle;
           };

           void     traceEyeSubpath( const Vector3f&         rayOrigin,
                                     const Vector3f&         rayDirection,
                                     const void*             lastHit,
                                     const Random&           random,
                                     std::vector<SubVertex>& vertices_o ) const;
           void     traceLightSubpath( const Random&           random,
                                       std::vector<SubVertex>& vertices_o )
                                                                          const;
           void     accumulate( const std::vector<PathVertex>& chain,
                                int32                          sampledS,
                                int32                          emittersCount,
                                const Vector3f&                eyeOutDir,
                                Vector3f&                      radiance_io,
                                PathInfo*                      pInfo )     const;


/// fields ---------------------------------------------------------------------
private:
   const Scene* pScene_m;
   int32        maxVertices_m;
};


}//namespace




#endif//Bidirectional_h

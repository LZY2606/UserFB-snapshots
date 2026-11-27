/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   Gradient-domain progressive sampling and Poisson reconstruction.

------------------------------------------------------------------------------*/


#ifndef GradientState_h
#define GradientState_h


#include <iosfwd>
#include <vector>

#include "Vector3f.hpp"




#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Result of the alpha-constrained Poisson reconstruction.<br/><br/>
 *
 * values_m holds the reconstructed linear radiance, one Vector3f per pixel,
 * indexed as (x + y * width), with y increasing upwards (the same image
 * orientation as Camera/Image).<br/><br/>
 *
 * residual_m is the largest per-channel relative residual
 * ||A I - b|| / ||b|| at the end of the solve (0 if ||b|| is zero and the
 * residual is zero).<br/><br/>
 *
 * No clamping, quantization or gamma encoding is applied here; those belong
 * to the normal Image tone-mapping/output path.
 */
struct GradientSolution
{
   GradientSolution();

   int32  getWidth()                                                const;
   int32  getHeight()                                               const;
   int32  getIterations()                                           const;
   real64 getResidual()                                             const;
   bool   isConverged()                                             const;

   real64 getChannelResidual( int32 channel )                       const;

   const std::vector<Vector3f>& getPixels()                         const;
   Vector3f getLinear( int32 x,
                       int32 y )                                    const;

   /**
    * Write the reconstructed linear image through the normal Image
    * tone-mapping chain as PPM.
    */
   void   getFormatted( std::ostream& )                             const;

   // fields (public for simple value semantics, as the surrounding code style)
   int32                 width_m;
   int32                 height_m;
   std::vector<Vector3f> values_m;
   real64                residuals_m[3];
   real64                residual_m;
   int32                 iterations_m;
   bool                  converged_m;
};


/**
 * Caller-owned progressive gradient-domain state.<br/><br/>
 *
 * Stores, per pixel, the sum of linear primal radiance samples and, for the
 * right and up edges that actually exist inside the image rectangle, the sum
 * of (neighbour radiance - base radiance).<br/><br/>
 *
 * Pixel storage is row-major with y increasing upwards:
 * index = x + y * width. The right edge of (x,y) is the edge to (x+1,y), and
 * the up edge of (x,y) is the edge to (x,y+1). Edges outside the rectangle
 * are never created; each interior edge is accumulated exactly once, from
 * its left/bottom endpoint.<br/><br/>
 *
 * All accumulated gradients are sums of per-round samples. The reconstruction
 * normalizes both the primal and gradient sums by the completed round count.
 */
class GradientState
{
/// standard object services ---------------------------------------------------
public:
            GradientState();
            GradientState( int32 width,
                           int32 height );

// use defaults
//         ~GradientState();
//          GradientState( const GradientState& );
// GradientState& operator=( const GradientState& );


/// commands -------------------------------------------------------------------
   /**
    * Set the image size and discard all accumulated samples.
    */
   void  reset( int32 width,
                int32 height );

   /**
    * Discard all accumulated samples but keep the current image size.
    */
   void  clear();

   // Sampling accumulation. Camera uses these once per round.
   void  accumulatePrimal( int32           x,
                           int32           y,
                           const Vector3f& radiance );
   void  accumulateRightGradient( int32           x,
                                  int32           y,
                                  const Vector3f& gradient );
   void  accumulateUpGradient( int32           x,
                               int32           y,
                               const Vector3f& gradient );
   void  completeRound();

   // Direct accumulation setters, useful for loading or numeric tests.
   void  setPrimalSum( int32           x,
                       int32           y,
                       const Vector3f& sum );
   void  setRightGradientSum( int32           x,
                              int32           y,
                              const Vector3f& sum );
   void  setUpGradientSum( int32           x,
                           int32           y,
                           const Vector3f& sum );
   void  setRounds( int32 rounds );


/// queries --------------------------------------------------------------------
   int32 getWidth()                                                 const;
   int32 getHeight()                                                const;
   int32 getRounds()                                                const;
   int32 getRoundCount()                                            const;

   bool  hasDimensions()                                            const;
   bool  isEmpty()                                                  const;
   bool  hasSamples()                                               const;

   const std::vector<Vector3f>& getPrimalSums()                     const;
   const std::vector<Vector3f>& getRightGradientSums()              const;
   const std::vector<Vector3f>& getUpGradientSums()                 const;

   Vector3f getPrimalSum( int32 x,
                          int32 y )                                 const;
   Vector3f getRightGradientSum( int32 x,
                                 int32 y )                          const;
   Vector3f getUpGradientSum( int32 x,
                              int32 y )                             const;

   /**
    * Round-count normalized samples, as used by the reconstruction.
    */
   Vector3f getPrimalEstimate( int32 x,
                               int32 y )                            const;
   Vector3f getRightGradient( int32 x,
                              int32 y )                             const;
   Vector3f getUpGradient( int32 x,
                           int32 y )                            const;

   bool  hasRightEdge( int32 x,
                       int32 y )                                    const;
   bool  hasUpEdge( int32 x,
                    int32 y )                                       const;

   int32 getRightEdgeCount()                                        const;
   int32 getUpEdgeCount()                                           const;
   int32 getEdgeCount()                                             const;

   /**
    * Minimize, per colour channel:
    *
    *   alpha * sum_p (I_p - P_p)^2
    *     + sum_(p,q) ((I_q - I_p) - G_pq)^2
    *
    * over the real interior 4-neighbour edges of the rectangle.
    *
    * @throws std::logic_error if no round has been completed.
    * @throws std::invalid_argument for an invalid alpha, tolerance or
    * iteration limit.
    */
   GradientSolution reconstruct(
                       real64 alpha,
                       real64 relativeTolerance = 1.0e-6,
                       int32  maxIterations     = 10000 )           const;

   // convenience alias
   GradientSolution getReconstruction(
                       real64 alpha,
                       real64 relativeTolerance = 1.0e-6,
                       int32  maxIterations     = 10000 )           const;


/// fields ---------------------------------------------------------------------
private:
   int32                 width_m;
   int32                 height_m;
   int32                 rounds_m;
   std::vector<Vector3f> primal_m;
   std::vector<Vector3f> rightGradient_m;
   std::vector<Vector3f> upGradient_m;
};


/**
 * Solve the alpha-constrained Poisson least-squares problem on already
 * normalized arrays.<br/><br/>
 *
 * Arrays use row-major indexing with y increasing upwards:
 * index = x + y * width.<br/><br/>
 *
 * The right-gradient array stores the gradient for edge (x,y)-(x+1,y) at
 * (x,y); entries with x+1 == width are ignored. The up-gradient array stores
 * the gradient for edge (x,y)-(x,y+1) at (x,y); entries with y+1 == height are
 * ignored.
 *
 * Storage is O(pixel count). The returned residual is the largest relative
 * per-channel residual. Convergence is reported only when every channel met
 * the tolerance within the iteration limit.
 */
GradientSolution solveGradientPoisson(
                       int32 width,
                       int32 height,
                       const std::vector<Vector3f>& primal,
                       const std::vector<Vector3f>& rightGradient,
                       const std::vector<Vector3f>& upGradient,
                       real64 alpha,
                       real64 relativeTolerance = 1.0e-6,
                       int32  maxIterations     = 10000 );


}//namespace




#endif//GradientState_h

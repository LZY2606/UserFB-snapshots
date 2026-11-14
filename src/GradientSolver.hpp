/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef GradientSolver_h
#define GradientSolver_h


#include <vector>

#include "Vector3f.hpp"



#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Reconstruction of a linear radiance image from the primal and gradient
 * estimates held by a GradientState.<br/><br/>
 *
 * P and G are normalized by the completed round count, then, per colour
 * channel, the following is minimized over the real interior 4-neighbour
 * edges of the whole rectangle:<br/><br/>
 *
 *    alpha * sum_p (I_p - P_p)^2  +  sum_(p,q) ((I_q - I_p) - G_pq)^2<br/><br/>
 *
 * Only edges that exist in the GradientState are used -- no zero padding at
 * the boundary, no wrap-around. With alpha > 0 the constant mode is anchored
 * and the system (alpha*I + L) I = alpha*P + div(G) is symmetric positive
 * definite; it is solved with conjugate gradients, using O(pixels) storage.
 * No clamping, quantization or gamma is applied here -- that is left to the
 * Image output stage.<br/><br/>
 *
 * Constant.
 */
class GradientSolver
{
/// standard object services ---------------------------------------------------
public:
   /**
    * Outcome of one solve.
    * * residual    -- worst relative residual ||r||/||b|| over the channels
    * * iterations  -- most conjugate-gradient iterations used by a channel
    * * isConverged -- true only if every channel met the tolerance
    */
   struct Result
   {
      real64 residual;
      int32  iterations;
      bool   isConverged;
   };

// use defaults
//          GradientSolver();
//         ~GradientSolver();
//          GradientSolver( const GradientSolver& );
// GradientSolver& operator=( const GradientSolver& );


/// queries --------------------------------------------------------------------
           /**
            * Reconstruct the linear radiance image.
            *
            * @param state             source of P and G, normalized by its
            *                          completed round count
            * @param alpha             data-term weight, must be positive and
            *                          finite
            * @param relativeTolerance convergence threshold on ||r||/||b||,
            *                          must be positive and finite
            * @param maxIterations     per-channel iteration cap, must be >= 1
            * @param image_o           receives width*height linear radiances,
            *                          row-major with y upward
            *
            * @throws std::invalid_argument if a parameter is not positive and
            * finite (or maxIterations < 1)
            * @throws std::logic_error if the state has no completed rounds
            */
   static  Result solve( const GradientState&   state,
                         real64                 alpha,
                         real64                 relativeTolerance,
                         int32                  maxIterations,
                         std::vector<Vector3f>& image_o );
};


}//namespace




#endif//GradientSolver_h

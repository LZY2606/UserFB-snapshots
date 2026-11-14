/*------------------------------------------------------------------------------

   MiniLight C++ : minimal global illumination renderer
   Harrison Ainsworth / HXA7241 : 2006-2011, 2013

   http://www.hxa.name/minilight

------------------------------------------------------------------------------*/


#ifndef GradientState_h
#define GradientState_h


#include <vector>

#include "Vector3f.hpp"



#include "hxa7241_minilight.hpp"
namespace hxa7241_minilight
{


/**
 * Progressive accumulation state for gradient-domain rendering.<br/><br/>
 *
 * Owned by the caller. Camera::getFrameGradient() adds one round of samples
 * at a time: a primal (linear radiance) estimate per pixel, and a paired
 * horizontal/vertical finite-difference estimate per existing interior
 * edge. Edges only exist between real neighbouring pixels -- no phantom
 * edges are made at the image boundary, and each edge is recorded once per
 * round, from its left/bottom endpoint only.<br/><br/>
 *
 * Pixel coordinates have y upward, matching Image::addToPixel().<br/><br/>
 *
 * @invariants
 * * width_m  >= 1
 * * height_m >= 1
 * * rounds_m >= 0
 * * primalSums_m.size() == rightSums_m.size() == upSums_m.size() ==
 *   width_m * height_m
 */
class GradientState
{
/// standard object services ---------------------------------------------------
public:
            /**
             * @throws std::invalid_argument if width or height is < 1
             */
            GradientState( int32 width,
                           int32 height );

// use defaults
//         ~GradientState();
//          GradientState( const GradientState& );
// GradientState& operator=( const GradientState& );


/// commands -------------------------------------------------------------------
           /**
            * Begin a new round of accumulation.
            * @throws std::logic_error if a round is already in progress
            */
           void  beginRound();

           /**
            * Add the primal sample of one pixel in the active round.
            * @throws std::logic_error if no round is active
            * @throws std::out_of_range if (x,y) is outside the image
            */
           void  addPrimal( int32           x,
                            int32           y,
                            const Vector3f& radiance );

           /**
            * Add the gradient sample of the edge from (x,y) to (x+1,y).
            * @throws std::logic_error if no round is active
            * @throws std::out_of_range if the edge does not exist
            */
           void  addGradientRight( int32           x,
                                   int32           y,
                                   const Vector3f& gradient );

           /**
            * Add the gradient sample of the edge from (x,y) to (x,y+1).
            * @throws std::logic_error if no round is active
            * @throws std::out_of_range if the edge does not exist
            */
           void  addGradientUp( int32           x,
                                int32           y,
                                const Vector3f& gradient );

           /**
            * End the active round.
            * @throws std::logic_error if no round is active, or the round did
            * not receive exactly one primal sample per pixel
            */
           void  completeRound();


/// queries --------------------------------------------------------------------
           int32 getWidth()                                              const;
           int32 getHeight()                                             const;
           /**
            * Number of completed rounds.
            */
           int32 getRounds()                                             const;

           /**
            * Whether the edge from (x,y) to (x+1,y) exists.
            */
           bool  hasRight( int32 x,
                           int32 y )                                     const;
           /**
            * Whether the edge from (x,y) to (x,y+1) exists.
            */
           bool  hasUp( int32 x,
                        int32 y )                                        const;

           Vector3f getPrimalSum( int32 x,
                                  int32 y )                             const;
           /**
            * @throws std::logic_error if no rounds have been completed
            */
           Vector3f getPrimalMean( int32 x,
                                   int32 y )                            const;

           /**
            * @throws std::out_of_range if the edge does not exist
            */
           Vector3f getGradientRightSum( int32 x,
                                         int32 y )                      const;
           /**
            * @throws std::logic_error if no rounds have been completed
            * @throws std::out_of_range if the edge does not exist
            */
           Vector3f getGradientRightMean( int32 x,
                                          int32 y )                     const;

           /**
            * @throws std::out_of_range if the edge does not exist
            */
           Vector3f getGradientUpSum( int32 x,
                                      int32 y )                         const;
           /**
            * @throws std::logic_error if no rounds have been completed
            * @throws std::out_of_range if the edge does not exist
            */
           Vector3f getGradientUpMean( int32 x,
                                       int32 y )                        const;


/// implementation -------------------------------------------------------------
private:
           int32 index( int32 x,
                        int32 y )                                        const;


/// fields ---------------------------------------------------------------------
private:
   int32 width_m;
   int32 height_m;

   int32 rounds_m;
   int32 pending_m;        // primal samples received in the active round
   bool  isRoundActive_m;

   std::vector<Vector3f> primalSums_m;
   std::vector<Vector3f> rightSums_m;   // valid where hasRight()
   std::vector<Vector3f> upSums_m;      // valid where hasUp()
};


}//namespace




#endif//GradientState_h

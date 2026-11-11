/*
** SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
** Copyright (C) [dates of first publication] Silicon Graphics, Inc.
** All Rights Reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
** of the Software, and to permit persons to whom the Software is furnished to do so,
** subject to the following conditions:
**
** The above copyright notice including the dates of first publication and either this
** permission notice or a reference to http://oss.sgi.com/projects/FreeB/ shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
** INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
** PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL SILICON GRAPHICS, INC.
** BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
** OR OTHER DEALINGS IN THE SOFTWARE.
**
** Except as contained in this notice, the name of Silicon Graphics, Inc. shall not
** be used in advertising or otherwise to promote the sale, use or other dealings in
** this Software without prior written authorization from Silicon Graphics, Inc.
*/
/*
** Author: Mikko Mononen, July 2009.
*/

#ifndef TESSELATOR_H
#define TESSELATOR_H

#ifdef __cplusplus
extern "C" {
#endif

// See OpenGL Red Book for description of the winding rules
// http://www.glprogramming.com/red/chapter11.html
enum TessWindingRule
{
	TESS_WINDING_ODD,
	TESS_WINDING_NONZERO,
	TESS_WINDING_POSITIVE,
	TESS_WINDING_NEGATIVE,
	TESS_WINDING_ABS_GEQ_TWO,
};

// The contents of the tessGetElements() depends on element type being passed to tessTesselate().
// Tesselation result element types:
// TESS_POLYGONS
//   Each element in the element array is polygon defined as 'polySize' number of vertex indices.
//   If a polygon has than 'polySize' vertices, the remaining indices are stored as TESS_UNDEF.
//   Example, drawing a polygon:
//     const int nelems = tessGetElementCount(tess);
//     const TESSindex* elems = tessGetElements(tess);
//     for (int i = 0; i < nelems; i++) {
//         const TESSindex* poly = &elems[i * polySize];
//         glBegin(GL_POLYGON);
//         for (int j = 0; j < polySize; j++) {
//             if (poly[j] == TESS_UNDEF) break;
//             glVertex2fv(&verts[poly[j]*vertexSize]);
//         }
//         glEnd();
//     }
//
// TESS_CONNECTED_POLYGONS
//   Each element in the element array is polygon defined as 'polySize' number of vertex indices,
//   followed by 'polySize' indices to neighour polygons, that is each element is 'polySize' * 2 indices.
//   If a polygon has than 'polySize' vertices, the remaining indices are stored as TESS_UNDEF.
//   If a polygon edge is a boundary, that is, not connected to another polygon, the neighbour index is TESS_UNDEF.
//   Example, flood fill based on seed polygon:
//     const int nelems = tessGetElementCount(tess);
//     const TESSindex* elems = tessGetElements(tess);
//     unsigned char* visited = (unsigned char*)calloc(nelems);
//     TESSindex stack[50];
//     int nstack = 0;
//     stack[nstack++] = seedPoly;
//     visited[startPoly] = 1;
//     while (nstack > 0) {
//         TESSindex idx = stack[--nstack];
//			const TESSindex* poly = &elems[idx * polySize * 2];
//			const TESSindex* nei = &poly[polySize];
//          for (int i = 0; i < polySize; i++) {
//              if (poly[i] == TESS_UNDEF) break;
//              if (nei[i] != TESS_UNDEF && !visited[nei[i]])
//	                stack[nstack++] = nei[i];
//                  visited[nei[i]] = 1;
//              }
//          }
//     }
//
// TESS_BOUNDARY_CONTOURS
//   Each element in the element array is [base index, count] pair defining a range of vertices for a contour.
//   The first value is index to first vertex in contour and the second value is number of vertices in the contour.
//   Example, drawing contours:
//     const int nelems = tessGetElementCount(tess);
//     const TESSindex* elems = tessGetElements(tess);
//     for (int i = 0; i < nelems; i++) {
//         const TESSindex base = elems[i * 2];
//         const TESSindex count = elems[i * 2 + 1];
//         glBegin(GL_LINE_LOOP);
//         for (int j = 0; j < count; j++) {
//             glVertex2fv(&verts[(base+j) * vertexSize]);
//         }
//         glEnd();
//     }

enum TessElementType
{
	TESS_POLYGONS,
	TESS_CONNECTED_POLYGONS,
	TESS_BOUNDARY_CONTOURS,
};


// TESS_CONSTRAINED_DELAUNAY_TRIANGULATION
//   If enabled, the initial triagulation is improved with non-robust Constrained Delayney triangulation.
//   Disable by default.
//
// TESS_REVERSE_CONTOURS
//   If enabled, tessAddContour() will treat CW contours as CCW and vice versa
//   Disabled by default.
//
// TESS_BOUNDARY_SOURCES
//   If enabled together with TESS_BOUNDARY_CONTOURS output, every directed
//   edge of every output contour also reports which original contour edges
//   cover it, and where.  See TESSboundarySource and the
//   tessGetBoundarySource*() queries below.
//   The option must be set with tessSetOption() before the first
//   tessAddContour() call of a run and must not be toggled during the run.
//   When the tesselator object is reused after a successful run, the option
//   value is kept and contour numbering restarts from zero.
//
//   Only finite contours in the XY plane are accepted with this option:
//   tessAddContour() must use size 2, or size 3 with z == 0, and
//   tessTesselate() must be passed an explicit normal of (0,0,1).
//   Contours are numbered from zero in the order of the tessAddContour()
//   calls; edge j inside a contour is the edge from input point j to
//   input point (j+1) % count.
//   No extra storage is allocated while the option is disabled.

enum TessOption
{
	TESS_CONSTRAINED_DELAUNAY_TRIANGULATION,
	TESS_REVERSE_CONTOURS,
	TESS_BOUNDARY_SOURCES
};

typedef float TESSreal;
typedef int TESSindex;
typedef struct TESStesselator TESStesselator;
typedef struct TESSalloc TESSalloc;

#define TESS_UNDEF (~(TESSindex)0)

#define TESS_NOTUSED(v) do { (void)(1 ? (void)0 : ( (void)(v) ) ); } while(0)

// These two constants define the valid input coordinate range the library is
// able to operate on. Tesselation will fail if any of the coordinates are not
// within this range. Clients are responsible for dealing with inputs outside of
// this range (e.g. clamping or filtering invalid points, scaling down the
// coordinate space).
#define TESS_MAX_VALID_INPUT_VALUE ((TESSreal) (1<<23))
#define TESS_MIN_VALID_INPUT_VALUE (-TESS_MAX_VALID_INPUT_VALUE)

// Custom memory allocator interface.
// The internal memory allocator allocates mesh edges, vertices and faces
// as well as dictionary nodes and active regions in buckets and uses simple
// freelist to speed up the allocation. The bucket size should roughly match your
// expected input data. For example if you process only hundreds of vertices,
// a bucket size of 128 might be ok, where as when processing thousands of vertices
// bucket size of 1024 might be approproate. The bucket size is a compromise between
// how often to allocate memory from the system versus how much extra space the system
// should allocate. Reasonable defaults are show in commects below, they will be used if
// the bucket sizes are zero.
//
// The use may left the memrealloc to be null. In that case, the tesselator will not try to
// dynamically grow int's internal arrays. The tesselator only needs the reallocation when it
// has found intersecting segments and needs to add new vertex. This defency can be cured by
// allocating some extra vertices beforehand. The 'extraVertices' variable allows to specify
// number of expected extra vertices.
struct TESSalloc
{
	void *(*memalloc)( void *userData, unsigned int size );
	void *(*memrealloc)( void *userData, void* ptr, unsigned int size );
	void (*memfree)( void *userData, void *ptr );
	void* userData;				// User data passed to the allocator functions.
	int meshEdgeBucketSize;		// 512
	int meshVertexBucketSize;	// 512
	int meshFaceBucketSize;		// 256
	int dictNodeBucketSize;		// 512
	int regionBucketSize;		// 256
	int extraVertices;			// Number of extra vertices allocated for the priority queue.
};


//
// Example use:
//
//
//
//

// tessNewTess() - Creates a new tesselator.
// Use tessDeleteTess() to delete the tesselator.
// Parameters:
//   alloc - pointer to a filled TESSalloc struct or NULL to use default malloc based allocator.
// Returns:
//   new tesselator object.
TESStesselator* tessNewTess( TESSalloc* alloc );

// tessDeleteTess() - Deletes a tesselator.
// Parameters:
//   tess - pointer to tesselator object to be deleted.
void tessDeleteTess( TESStesselator *tess );

// tessAddContour() - Adds a contour to be tesselated.
// The type of the vertex coordinates is assumed to be TESSreal.
// Parameters:
//   tess - pointer to tesselator object.
//   size - number of coordinates per vertex. Must be 2 or 3.
//   pointer - pointer to the first coordinate of the first vertex in the array.
//   stride - defines offset in bytes between consecutive vertices.
//   count - number of vertices in contour.
void tessAddContour( TESStesselator *tess, int size, const void* pointer, int stride, int count );

// tessSetOption() - Toggles optional tessellation parameters
// Parameters:
//  option - one of TessOption
//  value - 1 if enabled, 0 if disabled.
void tessSetOption( TESStesselator *tess, int option, int value );

// tessTesselate() - tesselate contours.
// Parameters:
//   tess - pointer to tesselator object.
//   windingRule - winding rules used for tesselation, must be one of TessWindingRule.
//   elementType - defines the tesselation result element type, must be one of TessElementType.
//   polySize - defines maximum vertices per polygons if output is polygons.
//   vertexSize - defines the number of coordinates in tesselation result vertex, must be 2 or 3.
//   normal - defines the normal of the input contours, of null the normal is calculated automatically.
// Returns:
//   1 if succeed, 0 if failed (tessGetStatus can be used after to get a more
//   specific failure status)
int tessTesselate( TESStesselator *tess, int windingRule, int elementType, int polySize, int vertexSize, const TESSreal* normal );

// tessGetVertexCount() - Returns number of vertices in the tesselated output.
int tessGetVertexCount( TESStesselator *tess );

// tessGetVertices() - Returns pointer to first coordinate of first vertex.
const TESSreal* tessGetVertices( TESStesselator *tess );

// tessGetVertexIndices() - Returns pointer to first vertex index.
// Vertex indices can be used to map the generated vertices to the original vertices.
// Every point added using tessAddContour() will get a new index starting at 0.
// New vertices generated at the intersections of segments are assigned value TESS_UNDEF.
const TESSindex* tessGetVertexIndices( TESStesselator *tess );

// tessGetElementCount() - Returns number of elements in the the tesselated output.
int tessGetElementCount( TESStesselator *tess );

// tessGetElements() - Returns pointer to the first element.
const TESSindex* tessGetElements( TESStesselator *tess );

// TESSboundarySource describes one original contour edge that covers (part
// of) a directed output boundary edge.
//   contour - contour number, assigned from zero in tessAddContour() order.
//   edge    - edge number inside the contour, the edge from input point
//             'edge' to input point (edge+1) % contourPointCount.
//   t0, t1  - parameters along the original edge P(t) = P0 + t*(P1 - P0),
//             so that P(t0) and P(t1) reconstruct the start and end points
//             of the covered portion of the output edge.  Both parameters
//             are in [0,1]; t0 > t1 is allowed when the original edge runs
//             in the opposite direction of the output edge.
// Every original edge identity covering the output edge is retained, even
// when several contours overlap or cancel each other's winding.  Records
// are sorted by contour and edge, and each source edge appears at most
// once per output edge.
typedef struct TESSboundarySource
{
	TESSindex contour;
	TESSindex edge;
	TESSreal t0;
	TESSreal t1;
} TESSboundarySource;

// Boundary source queries, available only for a successful
// TESS_BOUNDARY_CONTOURS run that was performed with the
// TESS_BOUNDARY_SOURCES option enabled.  In every other case (option off,
// other element type, initial state, failed run) the offsets and source
// pointers are NULL and the count is zero.  A successful run with empty
// boundary may also return NULL pointers.
// The returned memory is owned by the tesselator and stays valid until
// the next tessTesselate() call or until the object is deleted.
// On reuse after a successful run, contour numbering restarts from zero.

// tessGetBoundarySourceOffsets() - CSR offsets, an array of
// tessGetVertexCount()+1 entries.  Output vertex k corresponds to the
// directed boundary edge from vertex k to the next vertex of its output
// contour (the last vertex wraps back to the first); the sources of that
// edge are stored in entries offsets[k] .. offsets[k+1]-1.
// The last offset equals tessGetBoundarySourceCount().
const TESSindex* tessGetBoundarySourceOffsets( TESStesselator *tess );

// tessGetBoundarySources() - contiguous array of source records, grouped
// per output edge according to tessGetBoundarySourceOffsets().
const TESSboundarySource* tessGetBoundarySources( TESStesselator *tess );

// tessGetBoundarySourceCount() - total number of source records.
int tessGetBoundarySourceCount( TESStesselator *tess );

typedef enum TESSstatus {
  TESS_STATUS_OK,
  TESS_STATUS_OUT_OF_MEMORY,
  TESS_STATUS_INVALID_INPUT
} TESSstatus;

// Return the success or failure status. If tessTesselate fails (or will fail,
// e.g. after invalid data is passed to tessAddContour), this can indicate
// more specifically why. It can also be checked after tessAddContour to
// see whether to bail out early.
TESSstatus tessGetStatus( TESStesselator *tess );

#ifdef __cplusplus
};
#endif

#endif // TESSELATOR_H

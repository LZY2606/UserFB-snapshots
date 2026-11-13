//
using namespace OpenMesh

// ---------------------------------------- necessary types

// Mesh type
typedef TriMesh_ArrayKernelT<>                      Mesh;

// Decimater type
typedef Decimater::DecimaterT< Mesh >               Decimater;

// Decimation Module Handle types
typedef Decimater::ModQuadricT< Mesh >::Handle HModQuadric;
typedef Decimater::ModNormalFlippingT< Mesh >::Handle HModNormalFlipping;
typedef Decimater::ModAspectRatioT< Mesh >::Handle HModAspectRatio;

// ---------------------------------------- decimater setup

Mesh        mesh;             // a mesh object
Decimater   decimater(mesh);  // a decimater object, connected to a mesh
HModQuadric hModQuadric;      // use a quadric module

decimater.add( hModQuadric ); // register module at the decimater

std::cout << decimater.module( hModQuadric ).name() << std::endl;
                              // the way to access the module

/*
 * since we need exactly one priority module (non-binary)
 * we have to call set_binary(false) for our priority module
 * in the case of HModQuadric, unset_max_err() calls set_binary(false) internally
 */
decimater.module( hModQuadric ).unset_max_err();

decimater.initialize();       // let the decimater initialize the mesh and the
                              // modules

decimater.decimate();         // do decimation


// ---------------------------------------- relocation mode

/*
 * By default a halfedge collapse removes the from-vertex and keeps the
 * to-vertex exactly where it is.  Relocation mode lets the non-binary
 * ModQuadricT choose a better position for the remaining vertex: the
 * unconstrained minimizer of the two merged quadrics.  This is useful for
 * curved surfaces, where an edge's best error position usually lies
 * between the endpoints rather than on one of them.
 *
 * Relocation is off by default and has to be enabled before initialize()
 * (changing it resets the decimater, so initialize() again):
 */
Decimater relocation_decimater(mesh);
HModQuadric             hModQuadric2;
HModNormalFlipping      hModNormalFlipping;
HModAspectRatio         hModAspectRatio;

relocation_decimater.add( hModQuadric2 );
relocation_decimater.add( hModNormalFlipping );
relocation_decimater.add( hModAspectRatio );

relocation_decimater.set_relocation( true );

if ( relocation_decimater.initialize() )
{
  relocation_decimater.decimate_to( 1000 );
}
/*
 * initialize() returns false and leaves the mesh untouched if the module
 * set cannot support relocation.  That is the case if
 *
 *   - the priority module is binary (for example ModQuadricT with
 *     set_max_err(..., true)), or
 *   - any other attached module is not a relocation aware binary module.
 *
 * ModQuadricT (non-binary) is the only built-in module that can place the
 * remaining vertex, and ModNormalFlippingT and ModAspectRatioT are the
 * built-in binary modules that validate a moved vertex on both sides of the
 * edge.  In relocation mode every candidate is checked with the actual
 * target position: the quadric priority is evaluated there, the normal and
 * aspect-ratio tests inspect the faces around both endpoints, and the
 * collapse moves the retained vertex to exactly that position before the
 * next candidate is scored.
 */

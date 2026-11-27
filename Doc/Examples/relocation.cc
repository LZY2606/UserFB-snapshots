#include <iostream>

#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>

// ---------------------------------------- necessary types

typedef OpenMesh::TriMesh_ArrayKernelT<>              Mesh;
typedef OpenMesh::Decimater::DecimaterT<Mesh>         Decimater;
typedef OpenMesh::Decimater::ModQuadricT<Mesh>::Handle HModQuadric;
typedef OpenMesh::Decimater::ModNormalFlippingT<Mesh>::Handle HModFlipping;

int main()
{
  Mesh      mesh;             // a mesh object, loaded elsewhere
  Decimater decimater(mesh);  // a decimater object, connected to the mesh

  HModQuadric  hModQuadric;   // priority module: quadric error
  HModFlipping hModFlipping;  // binary module: reject normal flips

  decimater.add(hModQuadric);
  decimater.add(hModFlipping);

  // Relocation is off by default, which keeps the classic behaviour of
  // collapsing the removed vertex into the retained endpoint.  Turning it
  // on lets the quadric module choose an unconstrained minimizer of the
  // merged quadric for every eligible interior edge.
  decimater.set_relocation(true);

  // Changing the setting resets the decimater, so initialize() must be
  // called afterwards.  It returns false when the configured module
  // combination cannot support relocation; the mesh is left untouched.
  if (!decimater.initialize()) {
    std::cerr << "relocation is not supported by this module combination\n";
    return 1;
  }

  std::cout << decimater.module(hModQuadric).name() << std::endl;

  // Both entry points run the full relocation pipeline.
  size_t removed = decimater.decimate_to_faces(0, mesh.n_faces() / 2);
  std::cout << "performed " << removed << " collapses\n";

  return 0;
}

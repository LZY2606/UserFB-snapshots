/** \file decimation_history.cc
 *
 * Complete C++14 example for local collapse history, undo/redo, and an
 * optional attribute transfer callback.
 */

#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>

#include <iostream>

using Mesh = OpenMesh::TriMesh_ArrayKernelT<>;
using Decimater = OpenMesh::Decimater::DecimaterT<Mesh>;
using CollapseInfo = Decimater::CollapseInfo;

int main()
{
  Mesh mesh;
  mesh.request_vertex_status();
  mesh.request_edge_status();
  mesh.request_face_status();
  mesh.request_face_normals();

  // Optional ordinary attributes. They must be created before initialize()
  // and enable_history().
  OpenMesh::VPropHandleT<double> weight;
  OpenMesh::HPropHandleT<OpenMesh::Vec2f> corner_uv;
  mesh.add_property(weight, "weight");
  mesh.add_property(corner_uv, "corner_uv");

  const int w = 7;
  for (int y = 0; y < w; ++y) {
    for (int x = 0; x < w; ++x) {
      Mesh::VertexHandle vh = mesh.add_vertex(
          Mesh::Point(double(x), double(y),
                      0.02 * x * x + 0.03 * y * y));
      mesh.property(weight, vh) = 1.0;
    }
  }

  for (int y = 0; y < w - 1; ++y) {
    for (int x = 0; x < w - 1; ++x) {
      const int a = y * w + x;
      mesh.add_face(Mesh::VertexHandle(a),
                    Mesh::VertexHandle(a + 1),
                    Mesh::VertexHandle(a + w + 1));
      mesh.add_face(Mesh::VertexHandle(a),
                    Mesh::VertexHandle(a + w + 1),
                    Mesh::VertexHandle(a + w));
    }
  }
  mesh.update_face_normals();

  Decimater decimater(mesh);
  OpenMesh::Decimater::ModQuadricT<Mesh>::Handle quadric;
  decimater.add(quadric);

  // Install before enable_history(). The callback captures the mesh by
  // reference and only changes ordinary V/H/E/F property values.
  unsigned int transfer_calls = 0;
  decimater.set_history_transfer(
      [&](const CollapseInfo& ci)
      {
        ++transfer_calls;

        // Accumulate a vertex weight on the retained vertex.
        mesh.property(weight, ci.v1) += mesh.property(weight, ci.v0);

        // Example corner-attribute transfer for surviving faces around v1.
        for (Mesh::VertexFaceIter vf_it(mesh, ci.v1);
             vf_it.is_valid(); ++vf_it) {
          for (Mesh::FaceHalfedgeIter h_it(mesh, *vf_it);
               h_it.is_valid(); ++h_it) {
            const OpenMesh::Vec2f old_uv = mesh.property(corner_uv, *h_it);
            mesh.property(corner_uv, *h_it) =
                old_uv + OpenMesh::Vec2f(0.01f, 0.01f);
          }
        }
      });

  // Finish all property/module setup, then initialize and enable history.
  decimater.initialize();
  if (!decimater.enable_history()) {
    std::cerr << "History requires an initialized non-binary ModQuadricT "
                 "setup, optionally with ModNormalFlippingT.\n";
    return 1;
  }

  const size_t collapsed = decimater.decimate(5);
  std::cout << "collapsed=" << collapsed
            << " history_size=" << decimater.history_size()
            << " stored_values=" << decimater.history_stored_elements()
            << " transfers=" << transfer_calls << '\n';

  const size_t undone = decimater.undo(2);
  std::cout << "undone=" << undone
            << " position=" << decimater.history_position() << '\n';

  const size_t redone = decimater.redo(1);
  std::cout << "redone=" << redone
            << " position=" << decimater.history_position() << '\n';

  // A new collapse after undo creates a new history branch and discards the
  // old future on its first successful collapse.
  decimater.decimate(1);

  // Disable history before garbage collection/compacting handle slots.
  decimater.disable_history();
  mesh.garbage_collection();

  return 0;
}

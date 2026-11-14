// ============================================================================
//
//  Local, replayable decimater history example
//
//  Shows how to enable the collapse history on a DecimaterT, register a
//  local attribute transfer callback, decimate interactively, and move
//  back and forth with undo()/redo() without replacing the mesh object.
//
// ============================================================================

#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>

#include <functional>
#include <iostream>
#include <string>

using Mesh       = OpenMesh::TriMesh_ArrayKernelT<>;
using Decimater  = OpenMesh::Decimater::DecimaterT<Mesh>;
using QHandle    = OpenMesh::Decimater::ModQuadricT<Mesh>::Handle;
using NFHandle   = OpenMesh::Decimater::ModNormalFlippingT<Mesh>::Handle;
using CollInfo   = OpenMesh::Decimater::CollapseInfoT<Mesh>;

int main()
{
  Mesh mesh;

  // ordinary user properties that the modeling tool wants to keep in sync
  OpenMesh::VPropHandleT<std::string> origin;
  OpenMesh::VPropHandleT<double>      weight;
  OpenMesh::FPropHandleT<std::string> face_tag;
  mesh.add_property(origin,   "origin");
  mesh.add_property(weight,   "weight");
  mesh.add_property(face_tag, "face_tag");

  // ... fill the mesh here (add_vertex/add_face, request status, ...) ...
  mesh.request_vertex_status();
  mesh.request_edge_status();
  mesh.request_face_status();
  mesh.request_face_normals();

  // 1. build the decimater and add ALL modules first
  Decimater decimater(mesh);
  QHandle  quadric;
  NFHandle flipping;
  decimater.add(quadric);
  decimater.add(flipping);          // optional, the only allowed binary add-on

  // 2. initialize before enabling the history
  if (!decimater.initialize())
    return 1;

  // 3. optional local attribute transfer, registered before enable.
  //    It runs once per recorded collapse, after module post-processing,
  //    and may only touch ordinary V/H/E/F properties on the local
  //    support domain reachable through the captured mesh reference.
  int transfer_calls = 0;
  decimater.set_history_transfer(
      [&mesh, &origin, &weight, &face_tag, &transfer_calls](
          const CollInfo& _ci) {
        ++transfer_calls;

        // move vertex data onto the surviving vertex
        mesh.property(weight, _ci.v1) += mesh.property(weight, _ci.v0);
        mesh.property(origin, _ci.v1) =
            mesh.property(origin, _ci.v0) + ">" +
            mesh.property(origin, _ci.v1);

        // re-tag the surviving adjacent faces (deleted slots may be
        // written as well, they are restored by the history on undo)
        for (auto vf = mesh.vf_iter(_ci.v1); vf.is_valid(); ++vf) {
          if (mesh.status(*vf).deleted()) continue;
          mesh.property(face_tag, *vf) += "+";
        }
      });

  // 4. enable the history (fails for unsupported module combinations)
  if (!decimater.enable_history()) {
    std::cerr << "history not supported for this module setup\n";
    return 1;
  }

  // 5. drive decimation interactively; every legal collapse is recorded
  size_t done = decimater.decimate_to(200);
  std::cout << "recorded " << decimater.history_size()
            << " collapses (performed " << done << ")\n";

  // drag the slider back: collapse records are restored directly, the
  // transfer callback and the module callbacks are NOT run again
  size_t undone = decimater.undo(10);
  std::cout << "undid " << undone
            << ", now at position " << decimater.history_position()
            << " of " << decimater.history_size() << "\n";

  // drag forward again
  decimater.redo(10);

  // decimating again from an undone position opens a new branch and drops
  // the old future on the first successful new collapse
  decimater.decimate(1);

  // how many individual property values are retained in all records
  std::cout << "stored property values: "
            << decimater.history_stored_elements() << "\n";

  // 6. finish the session: records are released, garbage collection works
  //    again and the mesh object is the very same one as before
  decimater.disable_history();
  mesh.garbage_collection();

  std::cout << "transfer callback invocations: " << transfer_calls << "\n";
  return 0;
}

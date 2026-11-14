/*===========================================================================*\
 *                                                                           *
 *                               OpenMesh                                    *
 *      Copyright (C) 2001-2013 by Computer Graphics Group, RWTH Aachen      *
 *                           www.openmesh.org                                *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *  This file is part of OpenMesh.                                           *
 *                                                                           *
 *  OpenMesh is free software: you can redistribute it and/or modify         *
 *  it under the terms of the GNU Lesser General Public License as           *
 *  published by the Free Software Foundation, either version 3 of           *
 *  the License, or (at your option) any later version with the              *
 *  following exceptions:                                                    *
 *                                                                           *
 *  If other files instantiate templates or use macros                       *
 *  or inline functions from this file, or you compile this file and         *
 *  link it with other files to produce an executable, this file does        *
 *  not by itself cause the resulting executable to be covered by the        *
 *  GNU Lesser General Public License. This exception does not however       *
 *  invalidate any other reasons why the executable file might be            *
 *  covered by the GNU Lesser General Public License.                        *
 *                                                                           *
 *  OpenMesh is distributed in the hope that it will be useful,              *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenMesh.  If not,                                    *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *
\*===========================================================================*/

/*===========================================================================*\
 *                                                                           *
 *   $Revision$                                                         *
 *   $Date$                   *
 *                                                                           *
\*===========================================================================*/

/** \file DecimaterT.hh
 */

//=============================================================================
//
//  CLASS DecimaterT
//
//=============================================================================

#ifndef OPENMESH_DECIMATER_DECIMATERT_HH
#define OPENMESH_DECIMATER_DECIMATERT_HH


//== INCLUDES =================================================================

#include <functional>
#include <memory>
#include <set>
#include <vector>

#include <OpenMesh/Core/Utils/BaseProperty.hh>
#include <OpenMesh/Core/Utils/Property.hh>
#include <OpenMesh/Core/Mesh/BaseKernel.hh>
#include <OpenMesh/Tools/Utils/HeapT.hh>
#include <OpenMesh/Tools/Decimater/BaseDecimaterT.hh>



//== NAMESPACE ================================================================

namespace OpenMesh  {
namespace Decimater {


//== CLASS DEFINITION =========================================================


/** Decimater framework.
    \see BaseModT, \ref decimater_docu
*/
template < typename MeshT >
class DecimaterT : virtual public BaseDecimaterT<MeshT> //virtual especially for the mixed decimater
{
public: //-------------------------------------------------------- public types

  typedef DecimaterT< MeshT >           Self;
  typedef MeshT                         Mesh;
  typedef CollapseInfoT<MeshT>          CollapseInfo;
  typedef ModBaseT<MeshT>               Module;
  typedef std::vector< Module* >        ModuleList;
  typedef typename ModuleList::iterator ModuleListIterator;

  /** Callback type for the optional local attribute transfer.
   *
   *  The callback is invoked once per recorded collapse, after all module
   *  post-processing has finished.  It receives the CollapseInfo of the
   *  collapse and may transfer ordinary V/H/E/F properties on the local
   *  support domain through the mesh reference captured by the callback.
   */
  typedef std::function<void(const CollapseInfo&)> HistoryTransfer;

public: //------------------------------------------------------ public methods

  /// Constructor
  DecimaterT( Mesh& _mesh );

  /// Destructor
  ~DecimaterT();

public:

  /** Decimate (perform _n_collapses collapses). Return number of
      performed collapses. If _n_collapses is not given reduce as
      much as possible */
  size_t decimate( size_t _n_collapses = 0 );

  /// Decimate to target complexity, returns number of collapses.
  /// The target is compared against the number of currently alive
  /// (non-deleted) vertices, so delayed deletions are not counted.
  size_t decimate_to( size_t  _n_vertices )
  {
    size_t alive = count_alive_vertices();
    return ( (_n_vertices < alive) ? decimate( alive - _n_vertices ) : 0 );
  }

  /** Decimate to target complexity (vertices and faces).
   *  Stops when the number of vertices or the number of faces is reached.
   *  Returns number of performed collapses.
   */
  size_t decimate_to_faces( size_t  _n_vertices=0, size_t _n_faces=0 );

//== HISTORY SUPPORT ==========================================================

  /** \name Local, replayable collapse history
   *
   *  The decimater can record every legal halfedge collapse together with
   *  local before/after images of the affected slots.  The records allow
   *  undoing and redoing collapses on the same mesh object, including all
   *  V/H/E/F properties (points, normals, halfedge UVs, dynamic
   *  properties of any copyable type) and the accumulated ModQuadricT
   *  quadrics.
   *
   *  <b>Supported module setups:</b> a non-binary ModQuadricT used alone
   *  or together with a ModNormalFlippingT.  Every other module
   *  combination makes enable_history() return false.
   *
   *  <b>Usage protocol:</b>
   *  -# Add all properties and modules first.
   *  -# Call initialize().
   *  -# Optionally register a transfer callback with
   *     set_history_transfer().
   *  -# Call enable_history().
   *  -# Drive decimation with decimate(), decimate_to() or
   *     decimate_to_faces(), and navigate with undo()/redo().
   *  -# Call disable_history() (or destroy the decimater) before using
   *     garbage collection.
   *
   *  <b>Preconditions while a history session is enabled:</b>
   *  - do not edit the mesh topology, points, status bits or normals
   *    except through the decimater collapse entries;
   *  - do not edit property values or add/remove properties outside of the
   *    transfer callback (and the callback may only touch ordinary
   *    V/H/E/F properties on the collapse's local support domain);
   *  - do not add/remove modules, change module parameters or call
   *    initialize() again;
   *  - do not call garbage_collection() (both overloads throw
   *    std::logic_error while a session is enabled);
   *  - do not replace the transfer callback.
   *
   *  The mesh must outlive the decimater, and at most one history session
   *  may be bound to a mesh at any time.  When history is disabled the
   *  decimater behaves exactly as without history support.
   */
  //@{

  /** Enable the local collapse history.
   *
   *  Succeeds only if the decimater is initialized, no history session is
   *  active on this decimater or its mesh, and the module setup is one of
   *  the supported combinations.  On failure the mesh and all modules are
   *  left unchanged.
   *
   *  \return true on success, false otherwise.
   */
  bool enable_history();

  /** Disable the history, discard all records and release the session.
   *
   *  Afterwards garbage collection and all other mesh operations behave
   *  normally again.
   */
  void disable_history();

  /** Undo (reverse) up to \p n recorded collapses.
   *
   *  Records are restored directly; priorities, collapses, module
   *  callbacks and the transfer callback are not re-run.  The method stops
   *  at the beginning of the history.  Undoing beyond the available
   *  records, or calling undo(0), simply performs fewer or no steps.
   *
   *  \return Number of records actually moved.
   */
  size_t undo( size_t n = 1 );

  /** Redo (re-apply) up to \p n recorded collapses.
   *
   *  Records are restored directly; priorities, collapses, module
   *  callbacks and the transfer callback are not re-run.  The method stops
   *  at the end of the current history branch.
   *
   *  \return Number of records actually moved.
   */
  size_t redo( size_t n = 1 );

  /// Number of records stored on the current history branch.
  size_t history_size() const { return history_records_.size(); }

  /// Number of records currently applied (0 at the oldest position).
  size_t history_position() const { return history_position_; }

  /** Number of property values stored in all retained records, counting
   *  both before and after images: one value of one property on one slot
   *  counts as one.  Decimater-private heap/candidate cache properties
   *  are not counted.
   */
  size_t history_stored_elements() const;

  /** Register the optional local attribute transfer callback.
   *
   *  The callback is only used while history is enabled and is invoked
   *  exactly once per recorded collapse, after the module post-processing.
   *  It must not throw, must not change the property structure, topology,
   *  points, status bits or normals, and must not have external business
   *  side effects.  Set it before calling enable_history(); the callback
   *  cannot be replaced while a session is enabled.
   */
  void set_history_transfer( HistoryTransfer _transfer )
  { history_transfer_ = std::move(_transfer); }

  //@}

public:

  typedef typename Mesh::VertexHandle    VertexHandle;
  typedef typename Mesh::HalfedgeHandle  HalfedgeHandle;

  /// Heap interface
  class HeapInterface
  {
  public:

    HeapInterface(Mesh&               _mesh,
      VPropHandleT<float> _prio,
      VPropHandleT<int>   _pos)
      : mesh_(_mesh), prio_(_prio), pos_(_pos)
    { }

    inline bool
    less( VertexHandle _vh0, VertexHandle _vh1 )
    { return mesh_.property(prio_, _vh0) < mesh_.property(prio_, _vh1); }

    inline bool
    greater( VertexHandle _vh0, VertexHandle _vh1 )
    { return mesh_.property(prio_, _vh0) > mesh_.property(prio_, _vh1); }

    inline int
    get_heap_position(VertexHandle _vh)
    { return mesh_.property(pos_, _vh); }

    inline void
    set_heap_position(VertexHandle _vh, int _pos)
    { mesh_.property(pos_, _vh) = _pos; }


  private:
    Mesh&                mesh_;
    VPropHandleT<float>  prio_;
    VPropHandleT<int>    pos_;
  };

  typedef Utils::HeapT<VertexHandle, HeapInterface>  DeciHeap;


private: //---------------------------------------------------- private methods

  /// Insert vertex in heap
  void heap_vertex(VertexHandle _vh);

  /// Number of vertices without the deleted status bit.
  size_t count_alive_vertices() const;

  /// Number of faces without the deleted status bit.
  size_t count_alive_faces() const;

//== HISTORY INTERNALS ========================================================

  /** A single stored property value of one slot of one property. */
  struct HistoryPropertyValue
  {
    HistoryPropertyValue(unsigned char _container,
                         int           _property_index,
                         int           _slot_index,
                         PropertyValueT* _value)
      : container(_container), property_index(_property_index),
        slot_index(_slot_index), value(_value)
    {}

    unsigned char    container;        ///< 0=vertex, 1=halfedge, 2=edge, 3=face
    int              property_index;   ///< index within the property container
    int              slot_index;       ///< handle index of the slot
    PropertyValueT*  value;            ///< reference counted type-erased value
  };

  /** Connectivity image of one affected slot. */
  struct HistoryItem
  {
    HistoryItem() : kind(0), index(-1) {}

    unsigned char            kind;     ///< 0=vertex, 1=halfedge, 2=edge, 3=face
    int                      index;    ///< handle index
    typename Mesh::Vertex    vertex;
    typename Mesh::Halfedge  halfedge;
    typename Mesh::Edge      edge;
    typename Mesh::Face      face;
  };

  /** One collapse record: local before/after images of the support domain. */
  struct HistoryRecord
  {
    std::vector<HistoryItem>           items_before;
    std::vector<HistoryItem>           items_after;
    std::vector<HistoryPropertyValue>  before;
    std::vector<HistoryPropertyValue>  after;
    size_t                             stored_elements;

    HistoryRecord() : stored_elements(0) {}
  };

  /// Collect the local support domain of collapse \p _ci (before collapse).
  void history_collect_domain(const CollapseInfo& _ci,
                              std::vector<VertexHandle>&   _vertices,
                              std::vector<HalfedgeHandle>& _halfedges,
                              std::vector<typename Mesh::EdgeHandle>& _edges,
                              std::vector<typename Mesh::FaceHandle>& _faces);

  /// Capture connectivity images of the domain slots.
  void history_capture_items(const std::vector<VertexHandle>&   _vertices,
                             const std::vector<HalfedgeHandle>& _halfedges,
                             const std::vector<typename Mesh::EdgeHandle>& _edges,
                             const std::vector<typename Mesh::FaceHandle>& _faces,
                             std::vector<HistoryItem>& _items) const;

  /// Capture all ordinary property values of the domain slots.
  void history_capture_properties(
                             const std::vector<VertexHandle>&   _vertices,
                             const std::vector<HalfedgeHandle>& _halfedges,
                             const std::vector<typename Mesh::EdgeHandle>& _edges,
                             const std::vector<typename Mesh::FaceHandle>& _faces,
                             std::vector<HistoryPropertyValue>& _images) const;

  /// Restore connectivity from an item image.
  void history_restore_items(const std::vector<HistoryItem>& _items) const;

  /// Restore property values from an image list.
  void history_restore_properties(
      const std::vector<HistoryPropertyValue>& _images) const;

  /// Drop the records after the current position (the old future).
  void history_discard_future();

  /// Release all value references held by the records.
  void history_clear_records();

  /// Apply record \p _record forward (after image) or backward (before image).
  void history_apply_record(const HistoryRecord& _record, bool _forward);

private: //------------------------------------------------------- private data


  // reference to mesh
  Mesh&      mesh_;

  // heap
  std::auto_ptr<DeciHeap> heap_;

  // vertex properties
  VPropHandleT<HalfedgeHandle>  collapse_target_;
  VPropHandleT<float>           priority_;
  VPropHandleT<int>             heap_position_;

  // history state
  bool                          history_enabled_;
  size_t                        history_position_;
  std::vector<HistoryRecord>    history_records_;
  HistoryTransfer               history_transfer_;

  // decimater-private properties excluded from history snapshots
  std::set<const BaseProperty*> excluded_properties_;

};

//=============================================================================
} // END_NS_DECIMATER
} // END_NS_OPENMESH
//=============================================================================
#if defined(OM_INCLUDE_TEMPLATES) && !defined(OPENMESH_DECIMATER_DECIMATERT_CC)
#define OPENMESH_DECIMATER_TEMPLATES
#include "DecimaterT.cc"
#endif
//=============================================================================
#endif // OPENMESH_DECIMATER_DECIMATERT_HH defined
//=============================================================================

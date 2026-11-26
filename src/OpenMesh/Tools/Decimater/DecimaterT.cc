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
 *                                                                           *
 \*===========================================================================*/

/*===========================================================================*\
 *                                                                           *
 *   $Revision$                                                         *
 *   $Date$                   *
 *                                                                           *
 \*===========================================================================*/

/** \file DecimaterT.cc
 */

//=============================================================================
//
//  CLASS DecimaterT - IMPLEMENTATION
//
//=============================================================================
#define OPENMESH_DECIMATER_DECIMATERT_CC

//== INCLUDES =================================================================

#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>

#include <vector>
#include <set>
#include <memory>
#include <functional>
#include <stdexcept>
#include <utility>
#if defined(OM_CC_MIPS)
#  include <float.h>
#else
#  include <cfloat>
#endif

//== NAMESPACE ===============================================================

namespace OpenMesh {
namespace Decimater {

//== History implementation ===================================================

template <class Mesh>
struct DecimaterT<Mesh>::HistoryState
{
  typedef typename Mesh::VertexHandle     VH;
  typedef typename Mesh::HalfedgeHandle   HH;
  typedef typename Mesh::EdgeHandle       EH;
  typedef typename Mesh::FaceHandle       FH;

  struct Support
  {
    std::vector<VH> vertices;
    std::vector<HH> halfedges;
    std::vector<EH> edges;
    std::vector<FH> faces;
    std::set<int> vset, hset, eset, fset;

    void add_vertex(VH _h)
    { if (_h.is_valid() && vset.insert(_h.idx()).second) vertices.push_back(_h); }
    void add_halfedge(HH _h)
    { if (_h.is_valid() && hset.insert(_h.idx()).second) halfedges.push_back(_h); }
    void add_edge(EH _h)
    { if (_h.is_valid() && eset.insert(_h.idx()).second) edges.push_back(_h); }
    void add_face(FH _h)
    { if (_h.is_valid() && fset.insert(_h.idx()).second) faces.push_back(_h); }
  };

  struct PropertyValue
  {
    BaseProperty*                              prop;
    size_t                                     index;
    std::unique_ptr<PropertyValueMemento>      value;
  };

  struct VertexTopology { VH id; HH halfedge; };
  struct HalfedgeTopology
  {
    HH id;
    VH vertex;
    FH face;
    HH next_halfedge;
    HH prev_halfedge;
  };
  struct FaceTopology { FH id; HH halfedge; };

  struct Image
  {
    std::vector<VertexTopology>    vertex_topology;
    std::vector<HalfedgeTopology>  halfedge_topology;
    std::vector<FaceTopology>      face_topology;
    std::vector<PropertyValue>     properties;
    size_t                         value_count;

    Image() : value_count(0) {}

    void capture(Mesh& _mesh,
                 const Support& _support,
                 const std::set<const BaseProperty*>& _skipped)
    {
      vertex_topology.clear();
      halfedge_topology.clear();
      face_topology.clear();
      properties.clear();
      value_count = 0;

      for (VH vh : _support.vertices)
        vertex_topology.push_back({vh, _mesh.halfedge_handle(vh)});

      for (HH hh : _support.halfedges) {
        halfedge_topology.push_back({
          hh,
          _mesh.to_vertex_handle(hh),
          _mesh.face_handle(hh),
          _mesh.next_halfedge_handle(hh),
          _mesh.prev_halfedge_handle(hh)
        });
      }

      for (FH fh : _support.faces)
        face_topology.push_back({fh, _mesh.halfedge_handle(fh)});

      auto capture_properties = [&](auto _begin, auto _end,
                                    const auto& _handles)
      {
        for (auto pit = _begin; pit != _end; ++pit) {
          BaseProperty* prop = *pit;
          if (!prop || _skipped.find(prop) != _skipped.end())
            continue;

          for (const auto& handle : _handles) {
            const size_t index = static_cast<size_t>(handle.idx());
            std::unique_ptr<PropertyValueMemento> memento(
                prop->save_value(index));
            if (!memento)
              continue;

            properties.push_back(
                PropertyValue{prop, index, std::move(memento)});
            ++value_count;
          }
        }
      };

      capture_properties(_mesh.vprops_begin(), _mesh.vprops_end(),
                         _support.vertices);
      capture_properties(_mesh.hprops_begin(), _mesh.hprops_end(),
                         _support.halfedges);
      capture_properties(_mesh.eprops_begin(), _mesh.eprops_end(),
                         _support.edges);
      capture_properties(_mesh.fprops_begin(), _mesh.fprops_end(),
                         _support.faces);
    }

    void restore(Mesh& _mesh) const
    {
      // Connectivity fields are written directly by their original indices.
      // Garbage collection is forbidden while history is enabled, so every
      // recorded slot still exists in its original container.
      for (const VertexTopology& v : vertex_topology)
        _mesh.set_halfedge_handle(v.id, v.halfedge);

      for (const HalfedgeTopology& h : halfedge_topology) {
        _mesh.set_vertex_handle(h.id, h.vertex);
        _mesh.set_face_handle(h.id, h.face);
      }

      for (const HalfedgeTopology& h : halfedge_topology) {
        if (h.next_halfedge.idx() >= 0)
          _mesh.set_next_halfedge_handle(h.id, h.next_halfedge);
      }

      for (const HalfedgeTopology& h : halfedge_topology) {
        if (h.prev_halfedge.idx() >= 0)
          _mesh.set_prev_halfedge_handle(h.id, h.prev_halfedge);
      }

      for (const FaceTopology& f : face_topology)
        _mesh.set_halfedge_handle(f.id, f.halfedge);

      for (const PropertyValue& value : properties)
        value.prop->restore_value(value.index, *value.value);
    }
  };

  struct Record
  {
    Support support;
    Image   before;
    Image   after;
    size_t  element_count;
    Record() : element_count(0) {}
  };

  std::vector<std::unique_ptr<Record> > records;
  size_t                                position;
  std::set<const BaseProperty*>         skipped;
  std::unique_ptr<Record>               pending;

  HistoryState() : position(0) {}

  static void collect_support(Mesh& _mesh,
                              const CollapseInfoT<Mesh>& _ci,
                              Support& _support)
  {
    _support.add_vertex(_ci.v0);
    _support.add_vertex(_ci.v1);
    _support.add_halfedge(_ci.v0v1);
    _support.add_halfedge(_ci.v1v0);
    _support.add_edge(_mesh.edge_handle(_ci.v0v1));

    auto add_face = [&](FH _fh)
    {
      if (!_fh.is_valid())
        return;
      _support.add_face(_fh);

      for (typename Mesh::FaceVertexIter it(_mesh, _fh);
           it.is_valid(); ++it)
        _support.add_vertex(*it);

      for (typename Mesh::FaceEdgeIter it(_mesh, _fh);
           it.is_valid(); ++it)
        _support.add_edge(*it);

      for (typename Mesh::FaceHalfedgeIter it(_mesh, _fh);
           it.is_valid(); ++it) {
        _support.add_halfedge(*it);
        _support.add_edge(_mesh.edge_handle(*it));
      }
    };

    for (typename Mesh::VertexFaceIter it(_mesh, _ci.v0);
         it.is_valid(); ++it)
      add_face(*it);
    for (typename Mesh::VertexFaceIter it(_mesh, _ci.v1);
         it.is_valid(); ++it)
      add_face(*it);

    for (typename Mesh::VertexEdgeIter it(_mesh, _ci.v0);
         it.is_valid(); ++it)
      _support.add_edge(*it);
    for (typename Mesh::VertexEdgeIter it(_mesh, _ci.v1);
         it.is_valid(); ++it)
      _support.add_edge(*it);

    // Every edge owns its two opposite halfedge slots.
    for (EH eh : _support.edges) {
      _support.add_halfedge(_mesh.halfedge_handle(eh, 0));
      _support.add_halfedge(_mesh.halfedge_handle(eh, 1));
    }

    // Explicit endpoint rings make boundary-chain restoration robust.
    for (typename Mesh::VertexOHalfedgeIter it(_mesh, _ci.v0);
         it.is_valid(); ++it)
      _support.add_halfedge(*it);
    for (typename Mesh::VertexIHalfedgeIter it(_mesh, _ci.v0);
         it.is_valid(); ++it)
      _support.add_halfedge(*it);
    for (typename Mesh::VertexOHalfedgeIter it(_mesh, _ci.v1);
         it.is_valid(); ++it)
      _support.add_halfedge(*it);
    for (typename Mesh::VertexIHalfedgeIter it(_mesh, _ci.v1);
         it.is_valid(); ++it)
      _support.add_halfedge(*it);

    // Links written through set_next_halfedge_handle() also touch the
    // previous pointer of the successor. Keep those local slots as well.
    const std::vector<HH> current_halfedges = _support.halfedges;
    for (HH hh : current_halfedges) {
      _support.add_halfedge(_mesh.next_halfedge_handle(hh));
      _support.add_halfedge(_mesh.prev_halfedge_handle(hh));
      _support.add_halfedge(_mesh.opposite_halfedge_handle(hh));
    }
  }

  void begin(Mesh& _mesh, const CollapseInfoT<Mesh>& _ci)
  {
    pending.reset(new Record);
    collect_support(_mesh, _ci, pending->support);
    pending->before.capture(_mesh, pending->support, skipped);
  }

  void commit(Mesh& _mesh)
  {
    if (!pending)
      return;

    pending->after.capture(_mesh, pending->support, skipped);
    pending->element_count =
        pending->before.value_count + pending->after.value_count;

    // A new collapse after undo creates a new branch and drops the old future
    // only when the collapse actually completes.
    if (position < records.size())
      records.resize(position);

    records.push_back(std::move(pending));
    position = records.size();
    pending.reset();
  }

  size_t undo(Mesh& _mesh, size_t _n)
  {
    size_t moved = 0;
    while (_n > 0 && position > 0) {
      --position;
      records[position]->before.restore(_mesh);
      ++moved;
      --_n;
    }
    return moved;
  }

  size_t redo(Mesh& _mesh, size_t _n)
  {
    size_t moved = 0;
    while (_n > 0 && position < records.size()) {
      records[position]->after.restore(_mesh);
      ++position;
      ++moved;
      --_n;
    }
    return moved;
  }

  size_t stored_elements() const
  {
    size_t result = 0;
    for (const std::unique_ptr<Record>& record : records)
      result += record->element_count;
    return result;
  }
};

//== IMPLEMENTATION ==========================================================

template<class Mesh>
DecimaterT<Mesh>::DecimaterT(Mesh& _mesh) :
  BaseDecimaterT<Mesh>(_mesh),
    mesh_(_mesh), heap_(NULL) {

  // private vertex properties
  mesh_.add_property(collapse_target_);
  mesh_.add_property(priority_);
  mesh_.add_property(heap_position_);
}

//-----------------------------------------------------------------------------

template<class Mesh>
DecimaterT<Mesh>::~DecimaterT() {

  disable_history();

  // private vertex properties
  mesh_.remove_property(collapse_target_);
  mesh_.remove_property(priority_);
  mesh_.remove_property(heap_position_);

}

//-----------------------------------------------------------------------------

template<class Mesh>
bool DecimaterT<Mesh>::enable_history()
{
  if (history_)
    return false;

  if (!this->is_initialized())
    return false;

  bool have_quadric = false;
  bool have_normal_flipping = false;

  for (typename ModuleList::const_iterator it = this->all_modules().begin();
       it != this->all_modules().end(); ++it) {
    Module* module = *it;

    if (dynamic_cast<ModQuadricT<Mesh>*>(module) != 0) {
      // History currently reconstructs the non-binary priority quadric path.
      if (module->is_binary() || have_quadric)
        return false;
      have_quadric = true;
    } else if (dynamic_cast<ModNormalFlippingT<Mesh>*>(module) != 0) {
      if (have_normal_flipping)
        return false;
      have_normal_flipping = true;
    } else {
      return false;
    }
  }

  if (!have_quadric)
    return false;

  if (!mesh_.begin_decimation_history_session(this))
    return false;

  history_.reset(new HistoryState);
  history_->skipped.insert(
      static_cast<const BaseProperty*>(&mesh_.property(collapse_target_)));
  history_->skipped.insert(
      static_cast<const BaseProperty*>(&mesh_.property(priority_)));
  history_->skipped.insert(
      static_cast<const BaseProperty*>(&mesh_.property(heap_position_)));

  return true;
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::disable_history()
{
  if (!history_)
    return;

  history_.reset();
  mesh_.end_decimation_history_session(this);
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::undo(size_t _n)
{
  heap_.reset();
  return history_ ? history_->undo(mesh_, _n) : 0;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::redo(size_t _n)
{
  heap_.reset();
  return history_ ? history_->redo(mesh_, _n) : 0;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::history_size() const
{ return history_ ? history_->records.size() : 0; }

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::history_position() const
{ return history_ ? history_->position : 0; }

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::history_stored_elements() const
{ return history_ ? history_->stored_elements() : 0; }

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::count_live_vertices() const
{
  size_t result = 0;
  for (typename Mesh::ConstVertexIter it = mesh_.vertices_begin();
       it != mesh_.vertices_end(); ++it) {
    if (!mesh_.status(*it).deleted())
      ++result;
  }
  return result;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::count_live_faces() const
{
  size_t result = 0;
  for (typename Mesh::ConstFaceIter it = mesh_.faces_begin();
       it != mesh_.faces_end(); ++it) {
    if (!mesh_.status(*it).deleted())
      ++result;
  }
  return result;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::decimate_to(size_t _n_vertices)
{
  const size_t live_vertices = count_live_vertices();
  if (_n_vertices >= live_vertices)
    return 0;
  return decimate(live_vertices - _n_vertices);
}

//-----------------------------------------------------------------------------

template<class Mesh>
bool DecimaterT<Mesh>::collapse_legal_without_tag_leak(
    const CollapseInfo& _ci)
{
  if (!history_)
    return this->is_collapse_legal(_ci);

  std::vector<std::pair<VertexHandle, bool> > tagged_state;
  std::set<int> seen;

  auto save_vertex = [&](VertexHandle _vh)
  {
    if (_vh.is_valid() && seen.insert(_vh.idx()).second)
      tagged_state.push_back(
          std::make_pair(_vh, mesh_.status(_vh).tagged()));
  };

  auto save_ring = [&](VertexHandle _center)
  {
    if (!_center.is_valid())
      return;

    save_vertex(_center);
    for (typename Mesh::VertexVertexIter it(mesh_, _center);
         it.is_valid(); ++it)
      save_vertex(*it);
  };

  save_ring(_ci.v0);
  save_ring(_ci.v1);

  const bool legal = this->is_collapse_legal(_ci);

  for (const auto& state : tagged_state)
    mesh_.status(state.first).set_tagged(state.second);

  return legal;
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::heap_vertex(VertexHandle _vh) {
  //   std::clog << "heap_vertex: " << _vh << std::endl;

  float prio, best_prio(FLT_MAX);
  typename Mesh::HalfedgeHandle heh, collapse_target;

  // find best target in one ring
  typename Mesh::VertexOHalfedgeIter voh_it(mesh_, _vh);
  for (; voh_it.is_valid(); ++voh_it) {
    heh = *voh_it;
    CollapseInfo ci(mesh_, heh);

    if (collapse_legal_without_tag_leak(ci)) {
      prio = this->collapse_priority(ci);
      if (prio >= 0.0 && prio < best_prio) {
        best_prio = prio;
        collapse_target = heh;
      }
    }
  }

  // target found -> put vertex on heap
  if (collapse_target.is_valid()) {
    //     std::clog << "  added|updated" << std::endl;
    mesh_.property(collapse_target_, _vh) = collapse_target;
    mesh_.property(priority_, _vh)        = best_prio;

    if (heap_->is_stored(_vh))
      heap_->update(_vh);
    else
      heap_->insert(_vh);
  }

  // not valid -> remove from heap
  else {
    //     std::clog << "  n/a|removed" << std::endl;
    if (heap_->is_stored(_vh))
      heap_->remove(_vh);

    mesh_.property(collapse_target_, _vh) = collapse_target;
    mesh_.property(priority_, _vh) = -1;
  }
}

//-----------------------------------------------------------------------------
template<class Mesh>
size_t DecimaterT<Mesh>::decimate(size_t _n_collapses) {

  if (!this->is_initialized())
    return 0;

  typename Mesh::VertexIter v_it, v_end(mesh_.vertices_end());
  typename Mesh::VertexHandle vp;
  typename Mesh::HalfedgeHandle v0v1;
  typename Mesh::VertexVertexIter vv_it;
  typename Mesh::VertexFaceIter vf_it;
  unsigned int n_collapses(0);

  typedef std::vector<typename Mesh::VertexHandle> Support;
  typedef typename Support::iterator SupportIterator;

  Support support(15);
  SupportIterator s_it, s_end;

  // check _n_collapses
  if (!_n_collapses)
    _n_collapses = mesh_.n_vertices();

  // initialize heap
  HeapInterface HI(mesh_, priority_, heap_position_);
  heap_ = std::auto_ptr<DeciHeap>(new DeciHeap(HI));
  heap_->reserve(mesh_.n_vertices());

  for (v_it = mesh_.vertices_begin(); v_it != v_end; ++v_it) {
    heap_->reset_heap_position(*v_it);
    if (!mesh_.status(*v_it).deleted())
      heap_vertex(*v_it);
  }

  // process heap
  while ((!heap_->empty()) && (n_collapses < _n_collapses)) {
    // get 1st heap entry
    vp = heap_->front();
    v0v1 = mesh_.property(collapse_target_, vp);
    heap_->pop_front();

    // setup collapse info
    CollapseInfo ci(mesh_, v0v1);

    // check topological correctness AGAIN !
    if (!collapse_legal_without_tag_leak(ci))
      continue;

    // store support (= one ring of *vp)
    vv_it = mesh_.vv_iter(ci.v0);
    support.clear();
    for (; vv_it.is_valid(); ++vv_it)
      support.push_back(*vv_it);

    if (history_)
      history_->begin(mesh_, ci);

    // perform collapse
    mesh_.collapse(v0v1);
    ++n_collapses;

    // update triangle normals
    vf_it = mesh_.vf_iter(ci.v1);
    for (; vf_it.is_valid(); ++vf_it)
      if (!mesh_.status(*vf_it).deleted())
        mesh_.set_normal(*vf_it, mesh_.calc_face_normal(*vf_it));

    // post-process collapse
    this->postprocess_collapse(ci);

    if (history_) {
      if (history_transfer_)
        history_transfer_(ci);
      history_->commit(mesh_);
    }

    // update heap (former one ring of decimated vertex)
    for (s_it = support.begin(), s_end = support.end(); s_it != s_end; ++s_it) {
      assert(!mesh_.status(*s_it).deleted());
      heap_vertex(*s_it);
    }
  }

  // delete heap
  heap_.reset();



  // DON'T do garbage collection here! It's up to the application.
  return n_collapses;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::decimate_to_faces(size_t _nv, size_t _nf) {

  if (!this->is_initialized())
    return 0;

  const size_t initial_live_vertices = count_live_vertices();
  const size_t initial_live_faces = count_live_faces();
  if (_nv >= initial_live_vertices || _nf >= initial_live_faces)
    return 0;

  typename Mesh::VertexIter v_it, v_end(mesh_.vertices_end());
  typename Mesh::VertexHandle vp;
  typename Mesh::HalfedgeHandle v0v1;
  typename Mesh::VertexVertexIter vv_it;
  typename Mesh::VertexFaceIter vf_it;
  size_t nv = initial_live_vertices;
  size_t nf = initial_live_faces;
  unsigned int n_collapses = 0;

  typedef std::vector<typename Mesh::VertexHandle> Support;
  typedef typename Support::iterator SupportIterator;

  Support support(15);
  SupportIterator s_it, s_end;

  // initialize heap
  HeapInterface HI(mesh_, priority_, heap_position_);
  heap_ = std::auto_ptr<DeciHeap>(new DeciHeap(HI));
  heap_->reserve(mesh_.n_vertices());

  for (v_it = mesh_.vertices_begin(); v_it != v_end; ++v_it) {
    heap_->reset_heap_position(*v_it);
    if (!mesh_.status(*v_it).deleted())
      heap_vertex(*v_it);
  }

  // process heap
  while ((!heap_->empty()) && (_nv < nv) && (_nf < nf)) {
    // get 1st heap entry
    vp = heap_->front();
    v0v1 = mesh_.property(collapse_target_, vp);
    heap_->pop_front();

    // setup collapse info
    CollapseInfo ci(mesh_, v0v1);

    // check topological correctness AGAIN !
    if (!collapse_legal_without_tag_leak(ci))
      continue;

    // store support (= one ring of *vp)
    vv_it = mesh_.vv_iter(ci.v0);
    support.clear();
    for (; vv_it.is_valid(); ++vv_it)
      support.push_back(*vv_it);

    if (history_)
      history_->begin(mesh_, ci);

    // adjust complexity in advance (need boundary status)
    ++n_collapses;
    --nv;
    if (mesh_.is_boundary(ci.v0v1) || mesh_.is_boundary(ci.v1v0))
      --nf;
    else
      nf -= 2;

    // pre-processing
    this->preprocess_collapse(ci);

    // perform collapse
    mesh_.collapse(v0v1);

    // update triangle normals
    vf_it = mesh_.vf_iter(ci.v1);
    for (; vf_it.is_valid(); ++vf_it)
      if (!mesh_.status(*vf_it).deleted())
        mesh_.set_normal(*vf_it, mesh_.calc_face_normal(*vf_it));

    // post-process collapse
    this->postprocess_collapse(ci);

    if (history_) {
      if (history_transfer_)
        history_transfer_(ci);
      history_->commit(mesh_);
    }

    // update heap (former one ring of decimated vertex)
    for (s_it = support.begin(), s_end = support.end(); s_it != s_end; ++s_it) {
      assert(!mesh_.status(*s_it).deleted());
      heap_vertex(*s_it);
    }
  }

  // delete heap
  heap_.reset();


  // DON'T do garbage collection here! It's up to the application.
  return n_collapses;
}

//=============================================================================
}// END_NS_DECIMATER
} // END_NS_OPENMESH
//=============================================================================


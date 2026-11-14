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

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#if defined(OM_CC_MIPS)
#  include <float.h>
#else
#  include <cfloat>
#endif

//== NAMESPACE ===============================================================

namespace OpenMesh {
namespace Decimater {

//== IMPLEMENTATION ==========================================================

template<class Mesh>
DecimaterT<Mesh>::DecimaterT(Mesh& _mesh) :
  BaseDecimaterT<Mesh>(_mesh),
    mesh_(_mesh), heap_(NULL),
    history_enabled_(false), history_position_(0) {

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
void DecimaterT<Mesh>::heap_vertex(VertexHandle _vh) {
  //   std::clog << "heap_vertex: " << _vh << std::endl;

  float prio, best_prio(FLT_MAX);
  typename Mesh::HalfedgeHandle heh, collapse_target;

  // find best target in one ring
  typename Mesh::VertexOHalfedgeIter voh_it(mesh_, _vh);
  for (; voh_it.is_valid(); ++voh_it) {
    heh = *voh_it;
    CollapseInfo ci(mesh_, heh);

    if (this->is_collapse_legal(ci)) {
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
size_t DecimaterT<Mesh>::count_alive_vertices() const {
  size_t count(0);
  typename Mesh::ConstVertexIter v_it(mesh_.vertices_begin()),
                                 v_end(mesh_.vertices_end());
  for (; v_it != v_end; ++v_it)
    if (!mesh_.status(*v_it).deleted())
      ++count;
  return count;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::count_alive_faces() const {
  size_t count(0);
  typename Mesh::ConstFaceIter f_it(mesh_.faces_begin()),
                               f_end(mesh_.faces_end());
  for (; f_it != f_end; ++f_it)
    if (!mesh_.status(*f_it).deleted())
      ++count;
  return count;
}

//== HISTORY ==================================================================

template<class Mesh>
bool DecimaterT<Mesh>::enable_history() {

  // only one session per decimater
  if (history_enabled_)
    return false;

  // must be initialized first
  if (!this->is_initialized())
    return false;

  // only the non-binary quadric, optionally with normal flipping, is supported
  Module* prio = this->priority_module();
  if (prio == NULL || prio->name() != "Quadric")
    return false;

  const ModuleList& bmods = this->binary_modules();
  if (bmods.size() > 1)
    return false;
  if (bmods.size() == 1 && bmods[0]->name() != "NormalFlipping")
    return false;

  // at most one history session per mesh at a time
  if (mesh_.has_history_session())
    return false;

  // remember the decimater-private properties so they don't end up in
  // the local snapshots
  excluded_properties_.clear();
  excluded_properties_.insert(&mesh_.property(collapse_target_));
  excluded_properties_.insert(&mesh_.property(priority_));
  excluded_properties_.insert(&mesh_.property(heap_position_));

  mesh_.register_history_session();
  history_enabled_    = true;
  history_position_   = 0;
  return true;
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::disable_history() {
  if (!history_enabled_)
    return;

  history_clear_records();
  history_records_.clear();
  history_position_ = 0;
  excluded_properties_.clear();

  mesh_.unregister_history_session();
  history_enabled_ = false;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::undo(size_t n) {
  size_t done(0);
  if (!history_enabled_)
    return 0;

  while (done < n && history_position_ > 0) {
    --history_position_;
    history_apply_record(history_records_[history_position_], false);
    ++done;
  }
  return done;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::redo(size_t n) {
  size_t done(0);
  if (!history_enabled_)
    return 0;

  while (done < n && history_position_ < history_records_.size()) {
    history_apply_record(history_records_[history_position_], true);
    ++history_position_;
    ++done;
  }
  return done;
}

//-----------------------------------------------------------------------------

template<class Mesh>
size_t DecimaterT<Mesh>::history_stored_elements() const {
  size_t total(0);
  for (size_t i = 0; i < history_records_.size(); ++i)
    total += history_records_[i].stored_elements;
  return total;
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_collect_domain(
    const CollapseInfo& _ci,
    std::vector<VertexHandle>&   _vertices,
    std::vector<HalfedgeHandle>& _halfedges,
    std::vector<typename Mesh::EdgeHandle>& _edges,
    std::vector<typename Mesh::FaceHandle>& _faces) {

  std::set<int> vset, hset, eset, fset;

  vset.insert(_ci.v0.idx());
  vset.insert(_ci.v1.idx());

  typename Mesh::ConstVertexVertexIter vv_it(mesh_.vv_iter(_ci.v0));
  for (; vv_it.is_valid(); ++vv_it)
    vset.insert(vv_it->idx());
  vv_it = mesh_.vv_iter(_ci.v1);
  for (; vv_it.is_valid(); ++vv_it)
    vset.insert(vv_it->idx());

  // faces adjacent to either endpoint (includes fl/fr, even if deleted)
  typename Mesh::ConstVertexFaceIter vf_it(mesh_.vf_iter(_ci.v0));
  for (; vf_it.is_valid(); ++vf_it)
    fset.insert(vf_it->idx());
  vf_it = mesh_.vf_iter(_ci.v1);
  for (; vf_it.is_valid(); ++vf_it)
    fset.insert(vf_it->idx());

  hset.insert(_ci.v0v1.idx());
  hset.insert(_ci.v1v0.idx());

  for (std::set<int>::const_iterator it = vset.begin(); it != vset.end(); ++it) {
    VertexHandle vh(*it);
    typename Mesh::ConstVertexOHalfedgeIter voh(mesh_.voh_iter(vh));
    for (; voh.is_valid(); ++voh)
      hset.insert(voh->idx());
  }
  for (std::set<int>::const_iterator it = fset.begin(); it != fset.end(); ++it) {
    typename Mesh::FaceHandle fh(*it);
    typename Mesh::ConstFaceHalfedgeIter fh_it(mesh_.fh_iter(fh));
    for (; fh_it.is_valid(); ++fh_it)
      hset.insert(fh_it->idx());
  }

  for (std::set<int>::const_iterator it = hset.begin(); it != hset.end(); ++it) {
    HalfedgeHandle hh(*it);
    eset.insert(mesh_.edge_handle(hh).idx());
  }

  _vertices.clear();
  _halfedges.clear();
  _edges.clear();
  _faces.clear();
  _vertices.reserve(vset.size());
  _halfedges.reserve(hset.size());
  _edges.reserve(eset.size());
  _faces.reserve(fset.size());

  for (std::set<int>::const_iterator it = vset.begin(); it != vset.end(); ++it)
    _vertices.push_back(VertexHandle(*it));
  for (std::set<int>::const_iterator it = hset.begin(); it != hset.end(); ++it)
    _halfedges.push_back(HalfedgeHandle(*it));
  for (std::set<int>::const_iterator it = eset.begin(); it != eset.end(); ++it)
    _edges.push_back(typename Mesh::EdgeHandle(*it));
  for (std::set<int>::const_iterator it = fset.begin(); it != fset.end(); ++it)
    _faces.push_back(typename Mesh::FaceHandle(*it));
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_capture_items(
    const std::vector<VertexHandle>&   _vertices,
    const std::vector<HalfedgeHandle>& _halfedges,
    const std::vector<typename Mesh::EdgeHandle>& _edges,
    const std::vector<typename Mesh::FaceHandle>& _faces,
    std::vector<HistoryItem>& _items) const {

  _items.clear();
  _items.reserve(_vertices.size() + _halfedges.size() + _edges.size()
                 + _faces.size());

  for (size_t i = 0; i < _vertices.size(); ++i) {
    HistoryItem item;
    item.kind  = 0;
    item.index = _vertices[i].idx();
    item.vertex = mesh_.vertex(_vertices[i]);
    _items.push_back(item);
  }
  for (size_t i = 0; i < _halfedges.size(); ++i) {
    HistoryItem item;
    item.kind  = 1;
    item.index = _halfedges[i].idx();
    item.halfedge = mesh_.halfedge(_halfedges[i]);
    _items.push_back(item);
  }
  for (size_t i = 0; i < _edges.size(); ++i) {
    HistoryItem item;
    item.kind  = 2;
    item.index = _edges[i].idx();
    item.edge = mesh_.edge(_edges[i]);
    _items.push_back(item);
  }
  for (size_t i = 0; i < _faces.size(); ++i) {
    HistoryItem item;
    item.kind  = 3;
    item.index = _faces[i].idx();
    item.face = mesh_.face(_faces[i]);
    _items.push_back(item);
  }
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_capture_properties(
    const std::vector<VertexHandle>&   _vertices,
    const std::vector<HalfedgeHandle>& _halfedges,
    const std::vector<typename Mesh::EdgeHandle>& _edges,
    const std::vector<typename Mesh::FaceHandle>& _faces,
    std::vector<HistoryPropertyValue>& _images) const {

  _images.clear();

  // one helper per property container
  struct Local {
    static void capture_container(
        const std::set<const BaseProperty*>& _excluded,
        typename BaseKernel::prop_iterator _begin,
        typename BaseKernel::prop_iterator _end,
        unsigned char _kind,
        const std::vector<int>& _slots,
        std::vector<HistoryPropertyValue>& _images) {

      int property_index = 0;
      for (typename BaseKernel::prop_iterator p_it = _begin; p_it != _end;
           ++p_it, ++property_index) {
        BaseProperty* prop = *p_it;
        if (prop == NULL || !prop->can_snapshot())
          continue;
        if (_excluded.count(prop) != 0)
          continue;
        for (size_t i = 0; i < _slots.size(); ++i) {
          int slot = _slots[i];
          _images.push_back(HistoryPropertyValue(
              _kind, property_index, slot, prop->snapshot(slot)));
        }
      }
    }
  };

  std::vector<int> slots;

  slots.clear();
  slots.reserve(_vertices.size());
  for (size_t i = 0; i < _vertices.size(); ++i)
    slots.push_back(_vertices[i].idx());
  Local::capture_container(excluded_properties_,
                           mesh_.vprops_begin(), mesh_.vprops_end(),
                           0, slots, _images);

  slots.clear();
  slots.reserve(_halfedges.size());
  for (size_t i = 0; i < _halfedges.size(); ++i)
    slots.push_back(_halfedges[i].idx());
  Local::capture_container(excluded_properties_,
                           mesh_.hprops_begin(), mesh_.hprops_end(),
                           1, slots, _images);

  slots.clear();
  slots.reserve(_edges.size());
  for (size_t i = 0; i < _edges.size(); ++i)
    slots.push_back(_edges[i].idx());
  Local::capture_container(excluded_properties_,
                           mesh_.eprops_begin(), mesh_.eprops_end(),
                           2, slots, _images);

  slots.clear();
  slots.reserve(_faces.size());
  for (size_t i = 0; i < _faces.size(); ++i)
    slots.push_back(_faces[i].idx());
  Local::capture_container(excluded_properties_,
                           mesh_.fprops_begin(), mesh_.fprops_end(),
                           3, slots, _images);
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_restore_items(
    const std::vector<HistoryItem>& _items) const {

  for (size_t i = 0; i < _items.size(); ++i) {
    const HistoryItem& item = _items[i];
    switch (item.kind) {
      case 0:
        mesh_.vertex(VertexHandle(item.index)) = item.vertex;
        break;
      case 1:
        mesh_.halfedge(HalfedgeHandle(item.index)) = item.halfedge;
        break;
      case 2:
        mesh_.edge(typename Mesh::EdgeHandle(item.index)) = item.edge;
        break;
      case 3:
        mesh_.face(typename Mesh::FaceHandle(item.index)) = item.face;
        break;
    }
  }
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_restore_properties(
    const std::vector<HistoryPropertyValue>& _images) const {

  for (size_t i = 0; i < _images.size(); ++i) {
    const HistoryPropertyValue& image = _images[i];
    BaseProperty* prop = NULL;
    switch (image.container) {
      case 0: prop = &mesh_._vprop(size_t(image.property_index)); break;
      case 1: prop = &mesh_._hprop(size_t(image.property_index)); break;
      case 2: prop = &mesh_._eprop(size_t(image.property_index)); break;
      case 3: prop = &mesh_._fprop(size_t(image.property_index)); break;
    }
    prop->restore(size_t(image.slot_index), image.value);
  }
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_apply_record(const HistoryRecord& _record,
                                            bool _forward) {
  history_restore_items(_forward ? _record.items_after
                                 : _record.items_before);
  if (_forward)
    history_restore_properties(_record.after);
  else
    history_restore_properties(_record.before);
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_discard_future() {
  if (history_position_ >= history_records_.size())
    return;
  for (size_t i = history_position_; i < history_records_.size(); ++i) {
    HistoryRecord& record = history_records_[i];
    for (size_t j = 0; j < record.before.size(); ++j)
      record.before[j].value->release();
    for (size_t j = 0; j < record.after.size(); ++j)
      record.after[j].value->release();
  }
  history_records_.resize(history_position_);
}

//-----------------------------------------------------------------------------

template<class Mesh>
void DecimaterT<Mesh>::history_clear_records() {
  for (size_t i = 0; i < history_records_.size(); ++i) {
    HistoryRecord& record = history_records_[i];
    for (size_t j = 0; j < record.before.size(); ++j)
      record.before[j].value->release();
    for (size_t j = 0; j < record.after.size(); ++j)
      record.after[j].value->release();
  }
}

//-----------------------------------------------------------------------------
template<class Mesh>
size_t DecimaterT<Mesh>::decimate(size_t _n_collapses) {

  if (!this->is_initialized())
    return 0;

  const bool history = history_enabled_;

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

  // local support domain and record storage (history mode only)
  std::vector<VertexHandle>                 domain_vertices;
  std::vector<HalfedgeHandle>               domain_halfedges;
  std::vector<typename Mesh::EdgeHandle>    domain_edges;
  std::vector<typename Mesh::FaceHandle>    domain_faces;

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
    if (!this->is_collapse_legal(ci))
      continue;

    // a legal collapse starts a new branch: the old future is obsolete
    if (history && history_position_ < history_records_.size())
      history_discard_future();

    HistoryRecord record;
    if (history) {
      // store support (= one ring of *vp)
      vv_it = mesh_.vv_iter(ci.v0);
      support.clear();
      for (; vv_it.is_valid(); ++vv_it)
        support.push_back(*vv_it);

      history_collect_domain(ci, domain_vertices, domain_halfedges,
                             domain_edges, domain_faces);
      history_capture_items(domain_vertices, domain_halfedges, domain_edges,
                            domain_faces, record.items_before);
      history_capture_properties(domain_vertices, domain_halfedges,
                                 domain_edges, domain_faces, record.before);
    } else {
      // store support (= one ring of *vp)
      vv_it = mesh_.vv_iter(ci.v0);
      support.clear();
      for (; vv_it.is_valid(); ++vv_it)
        support.push_back(*vv_it);
    }

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

    if (history) {
      // optional local attribute transfer
      if (history_transfer_)
        history_transfer_(ci);

      // after images of the same local domain (connectivity and properties)
      history_capture_items(domain_vertices, domain_halfedges, domain_edges,
                            domain_faces, record.items_after);
      history_capture_properties(domain_vertices, domain_halfedges,
                                 domain_edges, domain_faces, record.after);
      record.stored_elements = record.before.size() + record.after.size();
      history_records_.push_back(std::move(record));
      ++history_position_;
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

  const size_t alive_vertices = count_alive_vertices();
  const size_t alive_faces    = count_alive_faces();
  if (_nv >= alive_vertices || _nf >= alive_faces)
    return 0;

  const bool history = history_enabled_;

  typename Mesh::VertexIter v_it, v_end(mesh_.vertices_end());
  typename Mesh::VertexHandle vp;
  typename Mesh::HalfedgeHandle v0v1;
  typename Mesh::VertexVertexIter vv_it;
  typename Mesh::VertexFaceIter vf_it;
  size_t nv = alive_vertices;
  size_t nf = alive_faces;
  unsigned int n_collapses = 0;

  typedef std::vector<typename Mesh::VertexHandle> Support;
  typedef typename Support::iterator SupportIterator;

  Support support(15);
  SupportIterator s_it, s_end;

  std::vector<VertexHandle>                 domain_vertices;
  std::vector<HalfedgeHandle>               domain_halfedges;
  std::vector<typename Mesh::EdgeHandle>    domain_edges;
  std::vector<typename Mesh::FaceHandle>    domain_faces;

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
    if (!this->is_collapse_legal(ci))
      continue;

    // a legal collapse starts a new branch: the old future is obsolete
    if (history && history_position_ < history_records_.size())
      history_discard_future();

    HistoryRecord record;
    if (history) {
      // store support (= one ring of *vp)
      vv_it = mesh_.vv_iter(ci.v0);
      support.clear();
      for (; vv_it.is_valid(); ++vv_it)
        support.push_back(*vv_it);

      history_collect_domain(ci, domain_vertices, domain_halfedges,
                             domain_edges, domain_faces);
      history_capture_items(domain_vertices, domain_halfedges, domain_edges,
                            domain_faces, record.items_before);
      history_capture_properties(domain_vertices, domain_halfedges,
                                 domain_edges, domain_faces, record.before);
    } else {
      // store support (= one ring of *vp)
      vv_it = mesh_.vv_iter(ci.v0);
      support.clear();
      for (; vv_it.is_valid(); ++vv_it)
        support.push_back(*vv_it);
    }

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

    if (history) {
      // optional local attribute transfer
      if (history_transfer_)
        history_transfer_(ci);

      // after images of the same local domain (connectivity and properties)
      history_capture_items(domain_vertices, domain_halfedges, domain_edges,
                            domain_faces, record.items_after);
      history_capture_properties(domain_vertices, domain_halfedges,
                                 domain_edges, domain_faces, record.after);
      record.stored_elements = record.before.size() + record.after.size();
      history_records_.push_back(std::move(record));
      ++history_position_;
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

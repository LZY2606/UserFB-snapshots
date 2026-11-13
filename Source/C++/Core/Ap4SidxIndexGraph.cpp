/*****************************************************************
|
|    AP4 - Multi-level SIDX index graph
|
|    Copyright 2002-2015 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
|    Bento4|GPL is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with Bento4|GPL; see the file COPYING.  If not, write to the
|    Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    021113-1307, USA.
|
****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4SidxIndexGraph.h"
#include "Ap4Atom.h"
#include "Ap4AtomFactory.h"
#include "Ap4ByteStream.h"

/*----------------------------------------------------------------------
|   helpers
+---------------------------------------------------------------------*/
namespace {

// unsigned 64x64 -> 128 bit product, used for exact rational comparisons
struct U128 {
    AP4_UI64 hi;
    AP4_UI64 lo;
};

static U128
Mul64(AP4_UI64 a, AP4_UI64 b)
{
    AP4_UI64 a0 = a & 0xFFFFFFFFULL;
    AP4_UI64 a1 = a >> 32;
    AP4_UI64 b0 = b & 0xFFFFFFFFULL;
    AP4_UI64 b1 = b >> 32;
    AP4_UI64 p0 = a0*b0;
    AP4_UI64 p1 = a1*b0;
    AP4_UI64 p2 = a0*b1;
    AP4_UI64 p3 = a1*b1;
    AP4_UI64 carry = ((p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL)) >> 32;
    U128 result;
    result.hi = p3 + (p1 >> 32) + (p2 >> 32) + carry;
    result.lo = p0 + ((p1 & 0xFFFFFFFFULL) << 32) + ((p2 & 0xFFFFFFFFULL) << 32);
    return result;
}

static bool
Equal128(U128 a, U128 b)
{
    return a.hi == b.hi && a.lo == b.lo;
}

static bool
AddOverflow(AP4_UI64 a, AP4_UI64 b, AP4_UI64& result)
{
    result = a+b;
    return result < a;
}

// exact equality of a/ta and b/tb (all values are non-negative)
static bool
TimesEqual(AP4_UI64 a, AP4_UI64 ta, AP4_UI64 b, AP4_UI64 tb)
{
    return Equal128(Mul64(a, tb), Mul64(b, ta));
}

} // namespace

/*----------------------------------------------------------------------
|   AP4_SidxIndexGraph::StreamHasSidx
+---------------------------------------------------------------------*/
bool
AP4_SidxIndexGraph::StreamHasSidx(AP4_ByteStream& stream)
{
    AP4_LargeSize stream_size = 0;
    if (AP4_FAILED(stream.GetSize(stream_size))) return false;
    if (AP4_FAILED(stream.Seek(0))) return false;

    AP4_UI64 position = 0;
    for (;;) {
        if (position+AP4_ATOM_HEADER_SIZE > stream_size) return false;
        AP4_UI32 size32 = 0;
        AP4_UI32 type   = 0;
        if (AP4_FAILED(stream.ReadUI32(size32))) return false;
        if (AP4_FAILED(stream.ReadUI32(type)))   return false;

        AP4_UI64 size = size32;
        AP4_UI64 header = AP4_ATOM_HEADER_SIZE;
        if (size32 == 1) {
            AP4_UI64 size64 = 0;
            if (AP4_FAILED(stream.ReadUI64(size64))) return false;
            size = size64;
            header = AP4_ATOM_HEADER_SIZE_64;
        } else if (size32 == 0) {
            size = stream_size-position;
        }
        if (size < header || position+size > stream_size) return false;

        if (type == AP4_ATOM_TYPE_SIDX) return true;
        position += size;
        if (AP4_FAILED(stream.Seek(position))) return false;
    }
}

/*----------------------------------------------------------------------
|   AP4_SidxIndexGraph::Build
+---------------------------------------------------------------------*/
AP4_Result
AP4_SidxIndexGraph::Build(AP4_ByteStream&      stream,
                          AP4_SidxIndexGraph*& graph)
{
    graph = NULL;

    AP4_LargeSize stream_size = 0;
    AP4_Result result = stream.GetSize(stream_size);
    if (AP4_FAILED(result)) return result;

    // all addresses are interpreted relative to the stream's byte 0
    result = stream.Seek(0);
    if (AP4_FAILED(result)) return result;

    AP4_SidxIndexGraph* g = new AP4_SidxIndexGraph();

    // parse every top-level box: truncated boxes are rejected here
    AP4_AtomFactory& factory = AP4_DefaultAtomFactory::Instance_;
    for (;;) {
        AP4_Position before = 0;
        result = stream.Tell(before);
        if (AP4_FAILED(result)) { delete g; return result; }

        AP4_Atom* atom = NULL;
        result = factory.CreateAtomFromStream(stream, atom);
        if (result == AP4_ERROR_EOS) {
            // a clean end-of-stream must consume every byte: 1..7 trailing
            // bytes cannot hold a box header and denote a truncated box
            AP4_Position position = 0;
            AP4_Result tell_result = stream.Tell(position);
            if (AP4_FAILED(tell_result)) { delete g; return tell_result; }
            if (position != stream_size) { delete g; return AP4_ERROR_INVALID_FORMAT; }
            break;
        }
        if (AP4_FAILED(result)) { delete g; return result; }

        AP4_Position after = 0;
        result = stream.Tell(after);
        if (AP4_FAILED(result)) { delete atom; delete g; return result; }
        if (after < before || after > stream_size ||
            after-before != atom->GetSize()) {
            delete atom;
            delete g;
            return AP4_ERROR_INVALID_FORMAT;
        }

        BoxRecord record;
        record.m_Offset = before;
        record.m_Size   = atom->GetSize();
        record.m_Type   = atom->GetType();
        record.m_Atom   = atom;
        record.m_Sidx   = AP4_DYNAMIC_CAST(AP4_SidxAtom, atom);
        record.m_IsSidx = (record.m_Sidx != NULL);
        g->m_Boxes.Append(record);
    }

    // the fragment region starts just after the initialization moov (in a
    // combined stream). In a fragment-only stream there is no moov, so the
    // whole stream belongs to the fragment region.
    g->m_TailIndex = 0;
    for (AP4_Ordinal i=0; i<g->m_Boxes.ItemCount(); i++) {
        if (g->m_Boxes[i].m_Type == AP4_ATOM_TYPE_MOOV) {
            g->m_TailIndex = i+1;
            break;
        }
    }

    // collect sidx boxes in their top-level order
    for (AP4_Ordinal i=0; i<g->m_Boxes.ItemCount(); i++) {
        if (!g->m_Boxes[i].m_IsSidx) continue;
        AP4_SidxAtom* atom = g->m_Boxes[i].m_Sidx;

        // a truncated sidx body parses as a generic atom: reject it
        if (atom == NULL) { delete g; return AP4_ERROR_INVALID_FORMAT; }

        // verify the declared box size matches version + reference count
        AP4_UI64 expected = AP4_FULL_ATOM_HEADER_SIZE + 4 + 4 +
                            (atom->GetVersion() == 0 ? 8ULL : 16ULL) + 2 + 2 +
                            (AP4_UI64)atom->GetReferences().ItemCount()*12;
        if (expected != atom->GetSize()) { delete g; return AP4_ERROR_INVALID_FORMAT; }
        if (atom->GetTimeScale() == 0)  { delete g; return AP4_ERROR_INVALID_FORMAT; }

        SidxRecord node;
        node.m_Atom        = atom;
        node.m_Offset      = g->m_Boxes[i].m_Offset;
        node.m_End         = node.m_Offset + atom->GetSize();
        node.m_BoxIndex    = i;
        node.m_NodeIndex   = g->m_Sidxes.ItemCount();
        node.m_ReferenceId = atom->GetReferenceId();
        node.m_TimeScale   = atom->GetTimeScale();
        node.m_Earliest    = atom->GetEarliestPresentationTime();
        node.m_Version     = atom->GetVersion();
        g->m_Sidxes.Append(node);
    }

    if (g->m_Sidxes.ItemCount() == 0) {
        graph = g;
        return AP4_SUCCESS;
    }

    result = g->ResolveReferences(stream, stream_size);
    if (AP4_FAILED(result)) { delete g; return result; }

    result = g->ValidateHierarchy();
    if (AP4_FAILED(result)) { delete g; return result; }

    graph = g;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SidxIndexGraph::~AP4_SidxIndexGraph
+---------------------------------------------------------------------*/
AP4_SidxIndexGraph::~AP4_SidxIndexGraph()
{
    for (AP4_Ordinal i=0; i<m_Boxes.ItemCount(); i++) {
        delete m_Boxes[i].m_Atom;
    }
}

/*----------------------------------------------------------------------
|   AP4_SidxIndexGraph::ResolveReferences
+---------------------------------------------------------------------*/
AP4_Result
AP4_SidxIndexGraph::ResolveReferences(AP4_ByteStream& /* stream */,
                                      AP4_LargeSize   stream_size)
{
    for (AP4_Ordinal n=0; n<m_Sidxes.ItemCount(); n++) {
        SidxRecord& node = m_Sidxes[n];
        AP4_SidxAtom* atom = node.m_Atom;
        AP4_Cardinal ref_count = atom->GetReferences().ItemCount();
        node.m_Refs.SetItemCount(ref_count);
        if (ref_count == 0) continue; // empty index is kept verbatim

        // first item starts at the box end + first_offset
        AP4_UI64 cursor = 0;
        if (AddOverflow(node.m_End, atom->GetFirstOffset(), cursor)) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        if (cursor > stream_size) return AP4_ERROR_OUT_OF_RANGE;

        for (AP4_Ordinal r=0; r<ref_count; r++) {
            const AP4_SidxAtom::Reference& ref = atom->GetReferences()[r];
            ResolvedRef& resolved = node.m_Refs[r];

            if (ref.m_ReferencedSize == 0)    return AP4_ERROR_INVALID_FORMAT;
            if (ref.m_SubsegmentDuration == 0) return AP4_ERROR_INVALID_FORMAT;

            AP4_UI64 range_end = 0;
            if (AddOverflow(cursor, ref.m_ReferencedSize, range_end)) {
                return AP4_ERROR_OUT_OF_RANGE;
            }
            if (range_end > stream_size) return AP4_ERROR_OUT_OF_RANGE;

            // find the box record that starts the range
            AP4_Ordinal first = m_Boxes.ItemCount();
            for (AP4_Ordinal i=0; i<m_Boxes.ItemCount(); i++) {
                if (m_Boxes[i].m_Offset == cursor) { first = i; break; }
                if (m_Boxes[i].m_Offset > cursor) break;
            }
            if (first == m_Boxes.ItemCount()) return AP4_ERROR_INVALID_FORMAT;

            // find the box record that ends the range
            AP4_Ordinal last = m_Boxes.ItemCount();
            for (AP4_Ordinal i=first; i<m_Boxes.ItemCount(); i++) {
                AP4_UI64 box_end = m_Boxes[i].m_Offset + m_Boxes[i].m_Size;
                if (box_end > range_end) return AP4_ERROR_INVALID_FORMAT;
                if (box_end == range_end) { last = i; break; }
            }
            if (last == m_Boxes.ItemCount()) return AP4_ERROR_INVALID_FORMAT;

            resolved.m_Start    = cursor;
            resolved.m_End      = range_end;
            resolved.m_FirstBox = first;
            resolved.m_LastBox  = last;

            if (ref.m_ReferenceType == 1) {
                // type 1: the range must be exactly one complete sidx box
                if (first != last || !m_Boxes[first].m_IsSidx) {
                    return AP4_ERROR_INVALID_FORMAT;
                }
                resolved.m_IsSidx = true;
                resolved.m_ChildNode = m_Boxes.ItemCount();
                for (AP4_Ordinal c=0; c<m_Sidxes.ItemCount(); c++) {
                    if (m_Sidxes[c].m_BoxIndex == first) {
                        resolved.m_ChildNode = c;
                        m_Sidxes[c].m_IncomingRefs++;
                        break;
                    }
                }
                if (resolved.m_ChildNode == m_Boxes.ItemCount()) {
                    return AP4_ERROR_INVALID_FORMAT;
                }
            } else {
                // type 0: complete boxes from a moof up to and including an mdat,
                // each moof immediately followed by its mdat
                if (m_Boxes[first].m_Type != AP4_ATOM_TYPE_MOOF) {
                    return AP4_ERROR_INVALID_FORMAT;
                }
                if (m_Boxes[last].m_Type != AP4_ATOM_TYPE_MDAT) {
                    return AP4_ERROR_INVALID_FORMAT;
                }
                bool expect_mdat = false;
                bool saw_media   = false;
                for (AP4_Ordinal i=first; i<=last; i++) {
                    AP4_UI32 type = m_Boxes[i].m_Type;
                    if (expect_mdat) {
                        if (type != AP4_ATOM_TYPE_MDAT) {
                            return AP4_ERROR_INVALID_FORMAT;
                        }
                        expect_mdat = false;
                        saw_media   = true;
                    } else if (type == AP4_ATOM_TYPE_MOOF) {
                        expect_mdat = true;
                    } else if (type == AP4_ATOM_TYPE_MDAT ||
                               type == AP4_ATOM_TYPE_SIDX ||
                               type == AP4_ATOM_TYPE_SSIX ||
                               type == AP4_ATOM_TYPE_MFRA) {
                        // an orphan mdat or an index box cannot be part of
                        // a media reference range
                        return AP4_ERROR_INVALID_FORMAT;
                    }
                    // any other box type (free/styp/emsg ...) is a side box
                }
                if (expect_mdat || !saw_media) return AP4_ERROR_INVALID_FORMAT;
            }

            cursor = range_end;
        }
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SidxIndexGraph::ValidateHierarchy
+---------------------------------------------------------------------*/
AP4_Result
AP4_SidxIndexGraph::ValidateHierarchy()
{
    // total media time span of every index, with overflow checks
    AP4_Array<AP4_UI64> total_duration;
    total_duration.SetItemCount(m_Sidxes.ItemCount());
    for (AP4_Ordinal n=0; n<m_Sidxes.ItemCount(); n++) {
        AP4_UI64 total = 0;
        SidxRecord& node = m_Sidxes[n];
        for (AP4_Ordinal r=0; r<node.m_Refs.ItemCount(); r++) {
            AP4_UI64 next = 0;
            if (AddOverflow(total, node.m_Atom->GetReferences()[r].m_SubsegmentDuration, next)) {
                return AP4_ERROR_OUT_OF_RANGE;
            }
            total = next;
        }
        total_duration[n] = total;
    }

    // no shared child indexes, and parent reference time spans must exactly
    // match the child index time span (rational comparison)
    for (AP4_Ordinal n=0; n<m_Sidxes.ItemCount(); n++) {
        SidxRecord& node = m_Sidxes[n];
        AP4_UI64 parent_time = node.m_Earliest;
        for (AP4_Ordinal r=0; r<node.m_Refs.ItemCount(); r++) {
            ResolvedRef& resolved = node.m_Refs[r];
            AP4_UI32 duration = node.m_Atom->GetReferences()[r].m_SubsegmentDuration;
            AP4_UI64 parent_end = 0;
            if (AddOverflow(parent_time, duration, parent_end)) {
                return AP4_ERROR_OUT_OF_RANGE;
            }
            if (resolved.m_IsSidx) {
                SidxRecord& child = m_Sidxes[resolved.m_ChildNode];
                if (child.m_IncomingRefs > 1) return AP4_ERROR_INVALID_FORMAT;

                AP4_UI64 child_end = 0;
                if (AddOverflow(child.m_Earliest, total_duration[resolved.m_ChildNode], child_end)) {
                    return AP4_ERROR_OUT_OF_RANGE;
                }
                if (!TimesEqual(parent_time, node.m_TimeScale,
                                child.m_Earliest, child.m_TimeScale)) {
                    return AP4_ERROR_INVALID_FORMAT;
                }
                if (!TimesEqual(parent_end, node.m_TimeScale,
                                child_end, child.m_TimeScale)) {
                    return AP4_ERROR_INVALID_FORMAT;
                }
            }
            parent_time = parent_end;
        }
    }

    // leaf media ranges must not overlap
    for (AP4_Ordinal n=0; n<m_Sidxes.ItemCount(); n++) {
        SidxRecord& node = m_Sidxes[n];
        for (AP4_Ordinal r=0; r<node.m_Refs.ItemCount(); r++) {
            if (node.m_Refs[r].m_IsSidx) continue;
            const ResolvedRef& a = node.m_Refs[r];
            for (AP4_Ordinal m=0; m<m_Sidxes.ItemCount(); m++) {
                SidxRecord& other = m_Sidxes[m];
                for (AP4_Ordinal q=0; q<other.m_Refs.ItemCount(); q++) {
                    if (other.m_Refs[q].m_IsSidx) continue;
                    const ResolvedRef& b = other.m_Refs[q];
                    if (&a == &b) continue;
                    if (a.m_Start < b.m_End && b.m_Start < a.m_End) {
                        return AP4_ERROR_INVALID_FORMAT;
                    }
                }
            }
        }
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_SidxIndexGraph::Relocate
+---------------------------------------------------------------------*/
AP4_Result
AP4_SidxIndexGraph::Relocate(const AP4_Array<AP4_UI64>& out_offsets,
                             const AP4_Array<AP4_UI64>& out_sizes)
{
    if (out_offsets.ItemCount() != m_Boxes.ItemCount() ||
        out_sizes.ItemCount()   != m_Boxes.ItemCount()) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }

    for (AP4_Ordinal n=0; n<m_Sidxes.ItemCount(); n++) {
        SidxRecord& node = m_Sidxes[n];
        node.m_OutOffset = out_offsets[node.m_BoxIndex];
        node.m_OutEnd    = node.m_OutOffset + node.m_Atom->GetSize();
    }

    for (AP4_Ordinal n=0; n<m_Sidxes.ItemCount(); n++) {
        SidxRecord& node = m_Sidxes[n];
        AP4_SidxAtom* atom = node.m_Atom;
        if (node.m_Refs.ItemCount() == 0) continue; // empty index: untouched

        AP4_UI64 sidx_out_end = node.m_OutEnd;
        for (AP4_Ordinal r=0; r<node.m_Refs.ItemCount(); r++) {
            ResolvedRef& resolved = node.m_Refs[r];
            AP4_UI64 range_start = 0;
            AP4_UI64 range_end   = 0;
            if (resolved.m_IsSidx) {
                SidxRecord& child = m_Sidxes[resolved.m_ChildNode];
                range_start = child.m_OutOffset;
                range_end   = child.m_OutEnd;
            } else {
                range_start = out_offsets[resolved.m_FirstBox];
                AP4_Ordinal last = resolved.m_LastBox;
                range_end = out_offsets[last] + out_sizes[last];
            }
            if (range_end < range_start) return AP4_ERROR_OUT_OF_RANGE;
            AP4_UI64 new_size = range_end-range_start;
            if (new_size > 0x7FFFFFFFULL) return AP4_ERROR_OUT_OF_RANGE;

            if (r == 0) {
                if (range_start < sidx_out_end) return AP4_ERROR_OUT_OF_RANGE;
                AP4_UI64 new_first_offset = range_start - sidx_out_end;

                // the offset may not shrink, and must fit the original version
                if (new_first_offset < atom->GetFirstOffset()) {
                    return AP4_ERROR_OUT_OF_RANGE;
                }
                if (atom->GetVersion() == 0 && new_first_offset > 0xFFFFFFFFULL) {
                    return AP4_ERROR_OUT_OF_RANGE;
                }
                atom->SetFirstOffset(new_first_offset);
            }

            atom->UseReferences()[r].m_ReferencedSize = (AP4_UI32)new_size;
        }
    }

    return AP4_SUCCESS;
}

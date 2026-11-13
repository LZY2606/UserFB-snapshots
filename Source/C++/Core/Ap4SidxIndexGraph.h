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

#ifndef _AP4_SIDX_INDEX_GRAPH_H_
#define _AP4_SIDX_INDEX_GRAPH_H_

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Types.h"
#include "Ap4Results.h"
#include "Ap4Array.h"
#include "Ap4SidxAtom.h"

/*----------------------------------------------------------------------
|   class references
+---------------------------------------------------------------------*/
class AP4_ByteStream;
class AP4_Atom;

/*----------------------------------------------------------------------
|   AP4_SidxIndexGraph
|
|   Validated layout of all top-level boxes and all (possibly nested)
|   sidx boxes in a fragmented MP4 stream. The graph is built before any
|   fragment handler is called or any output byte is written, and it is
|   replayed after the output has been produced to recompute the
|   first_offset / referenced_size fields of every sidx reference.
+---------------------------------------------------------------------*/
class AP4_SidxIndexGraph {
public:
    // one top-level box of the input stream
    struct BoxRecord {
        BoxRecord() :
            m_Offset(0), m_Size(0), m_Type(0), m_Atom(NULL),
            m_IsSidx(false), m_Sidx(NULL) {}
        AP4_UI64      m_Offset;
        AP4_UI64      m_Size;
        AP4_UI32      m_Type;
        AP4_Atom*     m_Atom;
        bool          m_IsSidx;
        AP4_SidxAtom* m_Sidx;
    };

    // one resolved reference of a sidx box
    struct ResolvedRef {
        ResolvedRef() :
            m_IsSidx(false), m_Start(0), m_End(0),
            m_FirstBox(0), m_LastBox(0), m_ChildNode(0) {}
        bool          m_IsSidx;       // reference type: 1 = sidx, 0 = media
        AP4_UI64      m_Start;        // absolute input offset of the range
        AP4_UI64      m_End;          // absolute input offset just past it
        AP4_Ordinal   m_FirstBox;     // first top-level box covered by the range
        AP4_Ordinal   m_LastBox;      // last top-level box covered by the range
        AP4_Ordinal   m_ChildNode;    // sidx node index (m_IsSidx only)
    };

    // one sidx box and its resolved references
    struct SidxRecord {
        SidxRecord() :
            m_Atom(NULL), m_Offset(0), m_End(0),
            m_BoxIndex(0), m_NodeIndex(0),
            m_ReferenceId(0), m_TimeScale(0),
            m_Earliest(0), m_Version(0),
            m_IncomingRefs(0), m_OutOffset(0), m_OutEnd(0) {}
        AP4_SidxAtom*         m_Atom;
        AP4_UI64              m_Offset;
        AP4_UI64              m_End;
        AP4_Ordinal           m_BoxIndex;
        AP4_Ordinal           m_NodeIndex;
        AP4_UI32              m_ReferenceId;
        AP4_UI32              m_TimeScale;
        AP4_UI64              m_Earliest;
        AP4_UI08              m_Version;
        AP4_Cardinal          m_IncomingRefs;
        AP4_Array<ResolvedRef> m_Refs;
        AP4_UI64              m_OutOffset;   // output position of this sidx box
        AP4_UI64              m_OutEnd;      // output position just past it
    };

    // Scan and validate a stream whose byte 0 is the fragment origin.
    // On success, the graph owns the parsed atoms (they are also kept in
    // the box records) and must be destroyed by the caller. On failure,
    // *graph is set to NULL and any parsed atom is destroyed.
    static AP4_Result Build(AP4_ByteStream&      stream,
                            AP4_SidxIndexGraph*& graph);

    // Cheap header-only probe: walk the top-level box headers without
    // instantiating any atom and report whether a 'sidx' box is present.
    // Read failures simply yield false; the authoritative parse in Build
    // or the atom factory reports the real error afterwards.
    static bool StreamHasSidx(AP4_ByteStream& stream);

    ~AP4_SidxIndexGraph();

    bool                HasIndexes() const { return m_Sidxes.ItemCount() != 0; }
    AP4_Cardinal        GetBoxCount() const { return m_Boxes.ItemCount();  }
    AP4_Cardinal        GetIndexCount() const { return m_Sidxes.ItemCount(); }
    // index of the first box that belongs to the fragment region (boxes
    // before it, such as ftyp/moov in a combined stream, are emitted by
    // the regular processing path)
    AP4_Ordinal         GetTailIndex() const { return m_TailIndex; }
    BoxRecord&          GetBox(AP4_Ordinal i) { return m_Boxes[i]; }
    const BoxRecord&    GetBox(AP4_Ordinal i) const { return m_Boxes[i]; }
    SidxRecord&         UseSidx(AP4_Ordinal i) { return m_Sidxes[i]; }
    const SidxRecord&   GetSidx(AP4_Ordinal i) const { return m_Sidxes[i]; }

    // Recompute every first_offset and referenced_size from the output
    // positions and sizes of the top-level boxes, which are in the same
    // order and have the same box types as the input records. Sidx boxes
    // are represented by their (unchanged) on-disk size.
    AP4_Result Relocate(const AP4_Array<AP4_UI64>&  out_offsets,
                        const AP4_Array<AP4_UI64>&  out_sizes);

private:
    AP4_SidxIndexGraph() : m_TailIndex(0) {}
    AP4_SidxIndexGraph(const AP4_SidxIndexGraph&);
    AP4_SidxIndexGraph& operator=(const AP4_SidxIndexGraph&);

    AP4_Result ResolveReferences(AP4_ByteStream& stream,
                                 AP4_LargeSize   stream_size);
    AP4_Result ValidateHierarchy();

    AP4_Array<BoxRecord>  m_Boxes;
    AP4_Array<SidxRecord> m_Sidxes;
    AP4_Ordinal           m_TailIndex;
};

#endif // _AP4_SIDX_INDEX_GRAPH_H_

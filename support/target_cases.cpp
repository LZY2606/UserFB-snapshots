#include "case_harness.hpp"
#include "fixture.hpp"
#include "Ap4.h"
#include "Ap4Processor.h"
#include "Ap4SidxAtom.h"
#include "Ap4TrunAtom.h"
#include "Ap4TfraAtom.h"
#include "Ap4MfroAtom.h"
#include "Ap4MfhdAtom.h"
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

/*----------------------------------------------------------------------
|   local helpers
+---------------------------------------------------------------------*/
namespace tc {

using seed::Bytes;
using seed::box;
using seed::words;
using seed::u32;
using seed::get;
using seed::append;

struct RefSpec {
    bool      indirect;
    AP4_UI32  size;
    AP4_UI32  duration;
};

// a moof/mdat pair with any number of samples, each sample carrying a
// one-byte marker equal to (marker_base + sample index)
struct FragSpec {
    unsigned              seq;
    std::vector<unsigned> sample_sizes;
    AP4_UI08              marker_base;
};

static Bytes
make_fragment(const FragSpec& spec)
{
    // trun flags: data-offset + first-sample-flags + sample-duration +
    // sample-size (each entry is duration then size)
    const AP4_UI32 TRUN_FLAGS = 0x00000305;
    auto build_moof_body = [&](AP4_SI32 data_offset) {
        Bytes trun_body = words({TRUN_FLAGS,
                                 (AP4_UI32)spec.sample_sizes.size(),
                                 (AP4_UI32)data_offset, 0});
        for (size_t i=0; i<spec.sample_sizes.size(); ++i) {
            u32(trun_body, 100);                          // sample duration
            u32(trun_body, spec.sample_sizes[i]);        // sample size
        }
        Bytes traf = box("tfhd", words({0x020000, 1}));
        append(traf, box("trun", trun_body));
        Bytes moof_body = box("mfhd", words({0, spec.seq}));
        append(moof_body, box("traf", traf));
        return moof_body;
    };

    // the trun data offset reaches from the moof start to the mdat payload:
    // whole moof box (8-byte header + body) plus the 8-byte mdat header
    Bytes moof_body = build_moof_body(0);
    AP4_SI32 data_offset = (AP4_SI32)(moof_body.size() + 16);
    moof_body = build_moof_body(data_offset);

    Bytes mdat_payload;
    for (size_t i=0; i<spec.sample_sizes.size(); ++i) {
        AP4_UI08 marker = (AP4_UI08)(spec.marker_base + (AP4_UI08)i);
        for (unsigned k=0; k<spec.sample_sizes[i]; ++k) {
            mdat_payload.push_back(k == 0 ? marker : (AP4_UI08)(0x40 + k));
        }
    }
    Bytes result = box("moof", moof_body);
    append(result, box("mdat", mdat_payload));
    return result;
}

static Bytes
make_sidx(AP4_UI08 version, AP4_UI32 ref_id, AP4_UI32 timescale,
          AP4_UI64 earliest, AP4_UI64 first_offset,
          const std::vector<RefSpec>& refs)
{
    Bytes body;
    u32(body, ref_id);
    u32(body, timescale);
    if (version == 0) {
        u32(body, (AP4_UI32)earliest);
        u32(body, (AP4_UI32)first_offset);
    } else {
        for (int i=7; i>=0; --i) body.push_back((AP4_UI08)(earliest    >> (8*i)));
        for (int i=7; i>=0; --i) body.push_back((AP4_UI08)(first_offset >> (8*i)));
    }
    body.push_back(0); body.push_back(0);            // reserved
    body.push_back((AP4_UI08)(refs.size() >> 8));
    body.push_back((AP4_UI08)(refs.size() & 0xFF));
    for (size_t i=0; i<refs.size(); ++i) {
        u32(body, refs[i].size | (refs[i].indirect ? 0x80000000u : 0));
        u32(body, refs[i].duration);
        u32(body, 0x90000000u);                       // SAP type 1, delta 0
    }
    Bytes hdr;
    u32(hdr, (AP4_UI32)(body.size()+12));
    const char* type = "sidx";
    hdr.insert(hdr.end(), type, type+4);
    hdr.push_back(version);
    hdr.push_back(0); hdr.push_back(0); hdr.push_back(0);
    append(hdr, body);
    return hdr;
}

struct ParsedSidx {
    AP4_UI08              version;
    AP4_UI32              ref_id;
    AP4_UI32              timescale;
    AP4_UI64              earliest;
    AP4_UI64              first_offset;
    std::vector<AP4_UI32> types;
    std::vector<AP4_UI32> sizes;
    std::vector<AP4_UI32> durations;
    std::vector<AP4_UI32> saps;
};

static AP4_UI64 read64(const Bytes& b, size_t p) {
    AP4_UI64 v = 0;
    for (int i=0; i<8; ++i) v = (v<<8) | b[p+i];
    return v;
}

// parse a sidx box directly from raw bytes (no AP4_SidxAtom round-trip)
static ParsedSidx
parse_sidx(const Bytes& b, const seed::Box& sb)
{
    ParsedSidx s;
    size_t p = sb.offset;
    s.version = b[p+8];
    size_t q = p+12;
    s.ref_id    = get(b, q);
    s.timescale = get(b, q+4);
    if (s.version == 0) {
        s.earliest     = get(b, q+8);
        s.first_offset = get(b, q+12);
        q += 16;
    } else {
        s.earliest     = read64(b, q+8);
        s.first_offset = read64(b, q+16);
        q += 24;
    }
    AP4_UI32 count = ((AP4_UI32)b[q+2] << 8) | b[q+3];
    q += 4;
    for (AP4_UI32 i=0; i<count; ++i) {
        AP4_UI32 v0 = get(b, q);
        s.types.push_back((v0>>31)&1);
        s.sizes.push_back(v0 & 0x7FFFFFFF);
        s.durations.push_back(get(b, q+4));
        s.saps.push_back(get(b, q+8));
        q += 12;
    }
    return s;
}

static std::vector<seed::Box>
boxes_of_type(const Bytes& b, AP4_UI32 type)
{
    std::vector<seed::Box> out;
    for (auto& x : seed::scan(b)) if (x.type == type) out.push_back(x);
    return out;
}

// collect the payload marker bytes of every mdat inside a byte range
static std::vector<AP4_UI08>
markers_in_range(const Bytes& b, AP4_UI64 start, AP4_UI64 end)
{
    std::vector<AP4_UI08> markers;
    for (auto& x : seed::scan(b)) {
        if (x.type != AP4_ATOM_TYPE_MDAT) continue;
        AP4_UI64 xs = x.offset, xe = x.offset+x.size;
        if (xs >= start && xe <= end) {
            markers.push_back(b[x.offset+8]);   // first payload byte is the marker
        }
    }
    return markers;
}

/*----------------------------------------------------------------------
|   recording fragment handler
+---------------------------------------------------------------------*/
class RecordingHandler : public AP4_Processor::FragmentHandler {
public:
    RecordingHandler(AP4_ContainerAtom* traf, bool grow_moof,
                     std::vector<char>& log, unsigned& sample_counter,
                     AP4_Result sample_error = AP4_SUCCESS,
                     AP4_Result finish_error = AP4_SUCCESS) :
        m_Traf(traf), m_GrowMoof(grow_moof), m_Log(log),
        m_SampleCounter(sample_counter),
        m_SampleError(sample_error), m_FinishError(finish_error) {}

    AP4_Result ProcessFragment() override {
        m_Log.push_back('g');
        AP4_AtomParent* parent = m_Traf->GetParent();
        AP4_ContainerAtom* moof = AP4_DYNAMIC_CAST(AP4_ContainerAtom, parent);
        if (moof == NULL) return AP4_ERROR_INTERNAL;
        AP4_MfhdAtom* mfhd = AP4_DYNAMIC_CAST(AP4_MfhdAtom,
                                              moof->GetChild(AP4_ATOM_TYPE_MFHD));
        if (mfhd == NULL) return AP4_ERROR_INVALID_FORMAT;
        unsigned seq = mfhd->GetSequenceNumber();
        // only grow fragments 1 and 3; leave 2 and 4 at their input size
        bool grow = m_GrowMoof && (seq == 1 || seq == 3);
        if (grow) {
            // grow the moof: insert a fixed-size 'free' child box, which
            // changes the moof size before the samples are processed
            AP4_UI08 pad[8] = {0};
            moof->AddChild(new AP4_UnknownAtom(AP4_ATOM_TYPE_FREE, pad, sizeof(pad)));
        }
        return AP4_SUCCESS;
    }

    AP4_Result PrepareForSamples(AP4_FragmentSampleTable*) override {
        m_Log.push_back('p');
        return AP4_SUCCESS;
    }

    AP4_Result ProcessSample(AP4_DataBuffer& in, AP4_DataBuffer& out) override {
        m_Log.push_back('s');
        if (m_SampleError != AP4_SUCCESS) return m_SampleError;
        AP4_Size n = in.GetDataSize();
        // unequal size changes: delta cycles 1,2,3 across all samples
        unsigned delta = 1 + (m_SampleCounter % 3);
        AP4_Result r = out.SetDataSize(n+delta);
        if (AP4_FAILED(r)) return r;
        std::memcpy(out.UseData(), in.GetData(), n);
        for (unsigned k=0; k<delta; ++k) {
            out.UseData()[n+k] = (AP4_UI08)(0xA0 + delta);
        }
        ++m_SampleCounter;
        return AP4_SUCCESS;
    }

    AP4_Result FinishFragment() override {
        m_Log.push_back('f');
        return m_FinishError;
    }

private:
    AP4_ContainerAtom*  m_Traf;
    bool                m_GrowMoof;
    std::vector<char>&  m_Log;
    unsigned&           m_SampleCounter;
    AP4_Result          m_SampleError;
    AP4_Result          m_FinishError;
};

class RecordingProcessor : public AP4_Processor {
public:
    RecordingProcessor(bool grow_moof, std::vector<char>& log,
                       unsigned& sample_counter,
                       AP4_Result sample_error = AP4_SUCCESS,
                       AP4_Result finish_error = AP4_SUCCESS) :
        m_GrowMoof(grow_moof), m_Log(log), m_SampleCounter(sample_counter),
        m_SampleError(sample_error), m_FinishError(finish_error),
        m_HandlerCreations(0) {}

    FragmentHandler* CreateFragmentHandler(AP4_TrakAtom*, AP4_TrexAtom*,
                                           AP4_ContainerAtom* traf,
                                           AP4_ByteStream&, AP4_Position) override {
        ++m_HandlerCreations;
        return new RecordingHandler(traf, m_GrowMoof, m_Log, m_SampleCounter,
                                    m_SampleError, m_FinishError);
    }
    unsigned HandlerCreations() const { return m_HandlerCreations; }
private:
    bool                m_GrowMoof;
    std::vector<char>&  m_Log;
    unsigned&           m_SampleCounter;
    AP4_Result          m_SampleError;
    AP4_Result          m_FinishError;
    unsigned            m_HandlerCreations;
};

// byte stream that counts read/write operations and can fail on demand
class FailingStream : public AP4_ByteStream {
public:
    FailingStream(AP4_ByteStream* inner, bool fail_reads,
                  unsigned fail_after) :
        m_Inner(inner), m_FailReads(fail_reads),
        m_FailAfter(fail_after), m_Ops(0), m_Refs(1) {
        m_Inner->AddReference();
    }
    AP4_Result ReadPartial(void* b, AP4_Size n, AP4_Size& read) override {
        if (m_FailReads && m_Ops++ >= m_FailAfter) return AP4_ERROR_READ_FAILED;
        return m_Inner->ReadPartial(b, n, read);
    }
    AP4_Result WritePartial(const void* b, AP4_Size n, AP4_Size& written) override {
        if (!m_FailReads && m_Ops++ >= m_FailAfter) return AP4_ERROR_WRITE_FAILED;
        return m_Inner->WritePartial(b, n, written);
    }
    AP4_Result Seek(AP4_Position p) override { return m_Inner->Seek(p); }
    AP4_Result Tell(AP4_Position& p) override { return m_Inner->Tell(p); }
    AP4_Result GetSize(AP4_LargeSize& s) override { return m_Inner->GetSize(s); }
    void AddReference() override { ++m_Refs; }
    void Release() override { if (--m_Refs == 0) { m_Inner->Release(); delete this; } }
    unsigned Ops() const { return m_Ops; }
private:
    AP4_ByteStream* m_Inner;
    bool            m_FailReads;
    unsigned        m_FailAfter;
    unsigned        m_Ops;
    unsigned        m_Refs;
};

struct RunResult {
    AP4_Result result;
    Bytes      output;
};

template <typename ProcessorT>
static RunResult
run_split(const Bytes& fragment_bytes, ProcessorT&& processor,
          AP4_ByteStream* frag_override = NULL,
          AP4_ByteStream* out_override = NULL,
          AP4_Position   out_start = 0)
{
    Bytes init = box("moov");
    auto init_stream = new AP4_MemoryByteStream(init.data(), (AP4_Size)init.size());
    auto frag_stream = new AP4_MemoryByteStream(fragment_bytes.data(),
                                                (AP4_Size)fragment_bytes.size());
    auto out = new AP4_MemoryByteStream();
    if (out_start) {
        Bytes pad((size_t)out_start, 0xCD);
        out->Write(pad.data(), (AP4_Size)pad.size());
        out->Seek(out_start);
    }
    AP4_ByteStream* frag_in = frag_override ? frag_override : frag_stream;
    AP4_ByteStream* out_in  = out_override  ? out_override  : out;
    AP4_Result r = processor.Process(*frag_in, *out_in, *init_stream);
    RunResult rr;
    rr.result = r;
    if (!out_override) {
        rr.output.assign(out->GetData(), out->GetData()+out->GetDataSize());
    }
    init_stream->Release();
    frag_stream->Release();
    out->Release();
    return rr;
}

template <typename ProcessorT>
static RunResult
run_combined(const Bytes& whole, ProcessorT&& processor)
{
    auto in = new AP4_MemoryByteStream(whole.data(), (AP4_Size)whole.size());
    auto out = new AP4_MemoryByteStream();
    AP4_Result r = processor.Process(*in, *out);
    RunResult rr;
    rr.result = r;
    rr.output.assign(out->GetData(), out->GetData()+out->GetDataSize());
    in->Release();
    out->Release();
    return rr;
}

} // namespace tc

/*======================================================================
|  relocation group
+=====================================================================*/
namespace tc {

struct Item {
    AP4_UI32 type;
    Bytes    bytes;
};

static AP4_UI64 item_offset(const std::vector<Item>& items, size_t index) {
    AP4_UI64 off = 0;
    for (size_t i=0; i<index; ++i) off += items[i].bytes.size();
    return off;
}

static Bytes concat_items(const std::vector<Item>& items) {
    Bytes out;
    for (auto& it : items) append(out, it.bytes);
    return out;
}

static AP4_UI32 fourcc(const char* s) {
    return (AP4_UI32)((AP4_UI08)s[0]<<24 | (AP4_UI08)s[1]<<16 |
                      (AP4_UI08)s[2]<<8  | (AP4_UI08)s[3]);
}

// resolve every reference of a parsed sidx against the concrete output bytes
struct Range { AP4_UI64 start, end; };

static std::vector<Range>
resolve_ranges(const ParsedSidx& s, AP4_UI64 sidx_end)
{
    std::vector<Range> ranges;
    AP4_UI64 cursor = sidx_end + s.first_offset;
    for (size_t i=0; i<s.sizes.size(); ++i) {
        ranges.push_back({cursor, cursor + s.sizes[i]});
        cursor += s.sizes[i];
    }
    return ranges;
}

} // namespace tc

TASK_CASE("relocation", "multi_fragment_refs_with_side_boxes_and_unindexed_fragment")
{
    using namespace tc;

    // four fragments: the first three are indexed, the fourth is not
    FragSpec f1{1, {3, 2}, 10};
    FragSpec f2{2, {4},    20};
    FragSpec f3{3, {2, 2, 1}, 30};
    FragSpec f4{4, {3},    40};
    Bytes b1 = make_fragment(f1);
    Bytes b2 = make_fragment(f2);
    Bytes b3 = make_fragment(f3);
    Bytes b4 = make_fragment(f4);

    Bytes free_gap  = box("free", {1,2,3,4,5,6});                 // inside leaf range
    Bytes free_root = box("free", {7,8,9});                       // between indexes
    Bytes styp      = box("styp", words({0x4D534E56, 0x10}));     // index -> media gap
    Bytes emsg      = box("emsg", {0x11,0x22,0x33});              // gap, not in a range
    Bytes pad       = box("free", Bytes(200, 0x5A));              // far separation

    // fixed v0 sidx sizes: 32 + 12*N
    const AP4_UI64 sidx_root_size = 32 + 12*2;
    const AP4_UI64 sidx_leaf_size = 32 + 12*1;

    // lay out the fragment stream in order
    std::vector<Item> items;
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)sidx_root_size, 0)}); // root placeholder
    items.push_back({AP4_ATOM_TYPE_FREE, free_root});
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)sidx_leaf_size, 0)}); // C1
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)sidx_leaf_size, 0)}); // C2
    items.push_back({AP4_ATOM_TYPE_STYP, styp});
    items.push_back({AP4_ATOM_TYPE_FREE, pad});
    items.push_back({AP4_ATOM_TYPE_MOOF, b1});
    items.push_back({AP4_ATOM_TYPE_FREE, free_gap});
    items.push_back({AP4_ATOM_TYPE_MOOF, b2});
    items.push_back({fourcc("emsg"), emsg});
    items.push_back({AP4_ATOM_TYPE_MOOF, b3});
    items.push_back({AP4_ATOM_TYPE_MOOF, b4});

    // indices into the ordered item list
    const size_t I_ROOT=0, I_C1=2, I_C2=3, I_F1=6,
                 I_F2=8, I_F3=10, I_F4=11;

    AP4_UI64 root_off = item_offset(items, I_ROOT);
    AP4_UI64 c1_off   = item_offset(items, I_C1);
    AP4_UI64 c2_off   = item_offset(items, I_C2);
    AP4_UI64 f1_off   = item_offset(items, I_F1);
    AP4_UI64 f3_off   = item_offset(items, I_F3);
    AP4_UI64 f4_off   = item_offset(items, I_F4);

    // root references both child index boxes (type 1)
    AP4_UI64 root_first = c1_off - (root_off + sidx_root_size);
    items[I_ROOT].bytes = make_sidx(0, 7, 1000, 0, root_first, {
        {true,  (AP4_UI32)sidx_leaf_size, 300},
        {true,  (AP4_UI32)sidx_leaf_size, 300},
    });

    // C1 covers f1 + free_gap + f2 (two fragments, one side box inside)
    AP4_UI64 c1_first = f1_off - (c1_off + sidx_leaf_size);
    AP4_UI64 c1_end   = item_offset(items, I_F2) + items[I_F2].bytes.size();
    items[I_C1].bytes = make_sidx(0, 1, 1000, 0, c1_first, {
        {false, (AP4_UI32)(c1_end - f1_off), 300},
    });

    // C2 covers f3 only; the emsg in front is a gap outside the range
    AP4_UI64 c2_first = f3_off - (c2_off + sidx_leaf_size);
    AP4_UI64 c3_end   = f3_off + items[I_F3].bytes.size();
    items[I_C2].bytes = make_sidx(0, 2, 1000, 300, c2_first, {
        {false, (AP4_UI32)(c3_end - f3_off), 300},
    });

    Bytes fragment_bytes = concat_items(items);
    TASK_CHECK(f4_off + items[I_F4].bytes.size() == fragment_bytes.size());

    std::vector<char> log;
    unsigned sample_counter = 0;
    // grow the moof of f1 and f3; every sample changes size unequally
    RecordingProcessor processor(true, log, sample_counter);
    RunResult rr = run_split(fragment_bytes, processor);
    TASK_CHECK(rr.result == AP4_SUCCESS);
    const Bytes& out = rr.output;

    // independent box-boundary parse of the output
    auto out_boxes = seed::scan(out);
    TASK_CHECK(seed::count(out, AP4_ATOM_TYPE_SIDX) == 3);
    TASK_CHECK(seed::count(out, AP4_ATOM_TYPE_MOOF) == 4);   // indexed + unindexed
    TASK_CHECK(seed::count(out, AP4_ATOM_TYPE_MDAT) == 4);
    TASK_CHECK(seed::count(out, AP4_ATOM_TYPE_FREE) >= 3);
    TASK_CHECK(seed::count(out, fourcc("styp")) == 1);
    TASK_CHECK(seed::count(out, fourcc("emsg")) == 1);

    // top-level ordering and side-box payloads are preserved (each moof
    // item contributes a moof/mdat pair to the output)
    std::vector<AP4_UI32> out_types;
    for (auto& x : out_boxes) out_types.push_back(x.type);
    std::vector<AP4_UI32> expected_types;
    for (auto& it : items) {
        expected_types.push_back(it.type);
        if (it.type == AP4_ATOM_TYPE_MOOF) expected_types.push_back(AP4_ATOM_TYPE_MDAT);
    }
    TASK_CHECK(out_types == expected_types);
    auto styp_in  = seed::scan(styp)[0];
    auto styp_out = boxes_of_type(out, fourcc("styp"))[0];
    TASK_CHECK(std::memcmp(out.data()+styp_out.offset+8,
                           styp.data()+styp_in.offset+8,
                           styp_in.size-8) == 0);
    auto emsg_in  = seed::scan(emsg)[0];
    auto emsg_out = boxes_of_type(out, fourcc("emsg"))[0];
    TASK_CHECK(std::memcmp(out.data()+emsg_out.offset+8,
                           emsg.data()+emsg_in.offset+8,
                           emsg_in.size-8) == 0);

    // parse all three indexes straight from the output bytes
    auto sidx_boxes = boxes_of_type(out, AP4_ATOM_TYPE_SIDX);
    ParsedSidx root = parse_sidx(out, sidx_boxes[0]);
    ParsedSidx c1   = parse_sidx(out, sidx_boxes[1]);
    ParsedSidx c2   = parse_sidx(out, sidx_boxes[2]);

    TASK_CHECK(root.ref_id == 7 && root.timescale == 1000 && root.version == 0);
    TASK_CHECK(root.types.size() == 2 && root.types[0] == 1 && root.types[1] == 1);
    TASK_CHECK(root.durations[0] == 300 && root.durations[1] == 300);
    TASK_CHECK(c1.types[0] == 0 && c1.durations[0] == 300 && c1.earliest == 0);
    TASK_CHECK(c2.types[0] == 0 && c2.durations[0] == 300 && c2.earliest == 300);
    for (auto& s : {&root, &c1, &c2})
        for (auto sap : s->saps) TASK_CHECK(sap == 0x90000000u);

    // type 1 references still point at exactly one whole child sidx box
    auto root_ranges = resolve_ranges(root, (AP4_UI64)sidx_boxes[0].offset + sidx_boxes[0].size);
    TASK_CHECK(root_ranges[0].start == (AP4_UI64)sidx_boxes[1].offset);
    TASK_CHECK(root_ranges[0].end   == (AP4_UI64)sidx_boxes[1].offset + sidx_boxes[1].size);
    TASK_CHECK(root_ranges[1].start == (AP4_UI64)sidx_boxes[2].offset);
    TASK_CHECK(root_ranges[1].end   == (AP4_UI64)sidx_boxes[2].offset + sidx_boxes[2].size);
    TASK_CHECK(root.sizes[0] == sidx_boxes[1].size);
    TASK_CHECK(root.sizes[1] == sidx_boxes[2].size);

    // leaf ranges: read back the media markers through the index
    auto c1_ranges = resolve_ranges(c1, (AP4_UI64)sidx_boxes[1].offset + sidx_boxes[1].size);
    auto c2_ranges = resolve_ranges(c2, (AP4_UI64)sidx_boxes[2].offset + sidx_boxes[2].size);
    auto m1 = markers_in_range(out, c1_ranges[0].start, c1_ranges[0].end);
    auto m2 = markers_in_range(out, c2_ranges[0].start, c2_ranges[0].end);
    TASK_CHECK((m1 == std::vector<AP4_UI08>{10, 20}));   // f1 then f2
    TASK_CHECK((m2 == std::vector<AP4_UI08>{30}));       // f3

    // referenced_size equals the real span of whole boxes (incl. internal free)
    AP4_UI64 real_c1 = 0, real_c2 = 0;
    for (auto& x : out_boxes) {
        AP4_UI64 xs = x.offset, xe = x.offset + x.size;
        if (xs >= c1_ranges[0].start && xe <= c1_ranges[0].end) real_c1 += x.size;
        if (xs >= c2_ranges[0].start && xe <= c2_ranges[0].end) real_c2 += x.size;
    }
    TASK_CHECK(real_c1 == c1.sizes[0]);
    TASK_CHECK(real_c2 == c2.sizes[0]);

    // the unindexed fragment still went through the handler (marker 40 present)
    bool found_unindexed = false;
    for (auto& x : out_boxes) {
        if (x.type == AP4_ATOM_TYPE_MDAT && out[x.offset+8] == 40) {
            found_unindexed = true;
        }
    }
    TASK_CHECK(found_unindexed);

    // grown moofs carry an extra 16-byte 'free' child (8-byte box header +
    // 8 payload bytes); the two untouched moofs keep their size
    auto moof_in  = boxes_of_type(fragment_bytes, AP4_ATOM_TYPE_MOOF);
    auto moof_out = boxes_of_type(out, AP4_ATOM_TYPE_MOOF);
    TASK_CHECK(moof_out.size() == 4 && moof_in.size() == 4);
    TASK_CHECK(moof_out[0].size == moof_in[0].size + 16);  // f1 grown
    TASK_CHECK(moof_out[1].size == moof_in[1].size);       // f2 untouched
    TASK_CHECK(moof_out[2].size == moof_in[2].size + 16);  // f3 grown
    TASK_CHECK(moof_out[3].size == moof_in[3].size);       // f4 untouched

    // handler call order/count: g,p,f once per fragment, s once per sample (8)
    std::string logstr(log.begin(), log.end());
    TASK_CHECK(std::count(log.begin(), log.end(), 'g') == 4);
    TASK_CHECK(std::count(log.begin(), log.end(), 'p') == 4);
    TASK_CHECK(std::count(log.begin(), log.end(), 'f') == 4);
    TASK_CHECK(std::count(log.begin(), log.end(), 's') == 2+1+3+1);
    for (size_t i=0; i+4 <= logstr.size(); i += 0) {
        // each fragment contributes the subsequence g p s... f
        break;
    }
    TASK_CHECK(logstr.front() == 'g' && logstr.back() == 'f');
}

/*======================================================================
|  hierarchy group
+=====================================================================*/
namespace tc {

// two independent roots, a two-level and a three-level chain, distinct
// timescales that are exactly equal in seconds
static Bytes
build_hierarchy_input()
{
    FragSpec fa1{1,{2},10}, fa2{2,{2},20}, fb1{3,{2},30}, fb2{4,{2},40};
    Bytes f1=make_fragment(fa1), f2=make_fragment(fa2),
          f3=make_fragment(fb1), f4=make_fragment(fb2);

    const AP4_UI64 S44 = 32+12, S56 = 32+24;
    std::vector<Item> items;
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)S44,0)}); // 0 RootA
    items.push_back({AP4_ATOM_TYPE_FREE, box("free",{1,2,3})});  // 1
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)S56,0)}); // 2 A1
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)S44,0)}); // 3 RootB
    items.push_back({AP4_ATOM_TYPE_FREE, box("free",{4,5,6})});  // 4
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)S44,0)}); // 5 B1
    items.push_back({AP4_ATOM_TYPE_SIDX, Bytes((size_t)S56,0)}); // 6 B2
    items.push_back({AP4_ATOM_TYPE_STYP, box("styp",words({1,2}))}); // 7
    items.push_back({AP4_ATOM_TYPE_MOOF, f1});                   // 8
    items.push_back({AP4_ATOM_TYPE_MOOF, f2});                   // 9
    items.push_back({AP4_ATOM_TYPE_MOOF, f3});                   // 10
    items.push_back({AP4_ATOM_TYPE_MOOF, f4});                   // 11

    AP4_UI64 pRootA=item_offset(items,0), pA1=item_offset(items,2),
             pRootB=item_offset(items,3), pB1=item_offset(items,5),
             pB2=item_offset(items,6),
             pF1=item_offset(items,8),
             pF3=item_offset(items,10);

    items[0].bytes = make_sidx(0, 11, 1000, 0, pA1-(pRootA+S44),
                               {{true,(AP4_UI32)S56,400}});
    items[2].bytes = make_sidx(0, 12, 2000, 0, pF1-(pA1+S56), {
        {false,(AP4_UI32)f1.size(),400},
        {false,(AP4_UI32)f2.size(),400},
    });
    items[3].bytes = make_sidx(0, 21, 1500, 0, pB1-(pRootB+S44),
                               {{true,(AP4_UI32)S44,600}});
    items[5].bytes = make_sidx(0, 22, 3000, 0, pB2-(pB1+S44),
                               {{true,(AP4_UI32)S56,1200}});
    items[6].bytes = make_sidx(0, 23, 6000, 0, pF3-(pB2+S56), {
        {false,(AP4_UI32)f3.size(),1200},
        {false,(AP4_UI32)f4.size(),1200},
    });
    return concat_items(items);
}

static bool
rational_equal(AP4_UI64 a, AP4_UI64 ta, AP4_UI64 b, AP4_UI64 tb)
{
    return a*tb == b*ta;
}

} // namespace tc

TASK_CASE("hierarchy", "nested_roots_distinct_timescales")
{
    using namespace tc;
    Bytes input = build_hierarchy_input();

    std::vector<char> log;
    unsigned counter = 0;
    RecordingProcessor processor(false, log, counter);
    RunResult rr = run_split(input, processor);
    TASK_CHECK(rr.result == AP4_SUCCESS);
    const Bytes& out = rr.output;

    auto sidx_boxes = boxes_of_type(out, AP4_ATOM_TYPE_SIDX);
    TASK_CHECK(sidx_boxes.size() == 5);
    ParsedSidx RootA=parse_sidx(out,sidx_boxes[0]);
    ParsedSidx A1   =parse_sidx(out,sidx_boxes[1]);
    ParsedSidx RootB=parse_sidx(out,sidx_boxes[2]);
    ParsedSidx B1   =parse_sidx(out,sidx_boxes[3]);
    ParsedSidx B2   =parse_sidx(out,sidx_boxes[4]);

    // identity and untouched fields
    TASK_CHECK(RootA.ref_id==11 && RootA.timescale==1000 && RootA.earliest==0);
    TASK_CHECK(A1.ref_id==12 && A1.timescale==2000 && A1.earliest==0);
    TASK_CHECK(RootB.ref_id==21 && RootB.timescale==1500 && RootB.earliest==0);
    TASK_CHECK(B1.ref_id==22 && B1.timescale==3000 && B1.earliest==0);
    TASK_CHECK(B2.ref_id==23 && B2.timescale==6000 && B2.earliest==0);
    TASK_CHECK(RootA.types==std::vector<AP4_UI32>({1}));
    TASK_CHECK(RootB.types==std::vector<AP4_UI32>({1}));
    TASK_CHECK(B1.types==std::vector<AP4_UI32>({1}));
    TASK_CHECK((A1.types==std::vector<AP4_UI32>({0,0})));
    TASK_CHECK((B2.types==std::vector<AP4_UI32>({0,0})));
    TASK_CHECK((RootA.durations==std::vector<AP4_UI32>({400})));
    TASK_CHECK((A1.durations==std::vector<AP4_UI32>({400,400})));
    TASK_CHECK((RootB.durations==std::vector<AP4_UI32>({600})));
    TASK_CHECK((B1.durations==std::vector<AP4_UI32>({1200})));
    TASK_CHECK((B2.durations==std::vector<AP4_UI32>({1200,1200})));
    for (auto& s : {&RootA,&A1,&RootB,&B1,&B2})
        for (auto sap : s->saps) TASK_CHECK(sap == 0x90000000u);

    // exact rational time equality across each parent/child boundary
    TASK_CHECK(rational_equal(400,RootA.timescale,800,A1.timescale));
    TASK_CHECK(rational_equal(600,RootB.timescale,1200,B1.timescale));
    TASK_CHECK(rational_equal(1200,B1.timescale,2400,B2.timescale));

    // indirect ranges land exactly on whole child sidx boxes
    auto rA = resolve_ranges(RootA, (AP4_UI64)sidx_boxes[0].offset+sidx_boxes[0].size);
    TASK_CHECK(rA[0].start==(AP4_UI64)sidx_boxes[1].offset);
    TASK_CHECK(rA[0].end==(AP4_UI64)sidx_boxes[1].offset+sidx_boxes[1].size);
    auto rB = resolve_ranges(RootB, (AP4_UI64)sidx_boxes[2].offset+sidx_boxes[2].size);
    TASK_CHECK(rB[0].start==(AP4_UI64)sidx_boxes[3].offset);
    TASK_CHECK(rB[0].end==(AP4_UI64)sidx_boxes[3].offset+sidx_boxes[3].size);
    auto rB1 = resolve_ranges(B1, (AP4_UI64)sidx_boxes[3].offset+sidx_boxes[3].size);
    TASK_CHECK(rB1[0].start==(AP4_UI64)sidx_boxes[4].offset);
    TASK_CHECK(rB1[0].end==(AP4_UI64)sidx_boxes[4].offset+sidx_boxes[4].size);

    // leaf ranges still identify the same media, read back via markers
    auto rA1 = resolve_ranges(A1, (AP4_UI64)sidx_boxes[1].offset+sidx_boxes[1].size);
    auto rB2 = resolve_ranges(B2, (AP4_UI64)sidx_boxes[4].offset+sidx_boxes[4].size);
    TASK_CHECK((markers_in_range(out,rA1[0].start,rA1[0].end)==std::vector<AP4_UI08>{10}));
    TASK_CHECK((markers_in_range(out,rA1[1].start,rA1[1].end)==std::vector<AP4_UI08>{20}));
    TASK_CHECK((markers_in_range(out,rB2[0].start,rB2[0].end)==std::vector<AP4_UI08>{30}));
    TASK_CHECK((markers_in_range(out,rB2[1].start,rB2[1].end)==std::vector<AP4_UI08>{40}));

    // reference count preserved
    TASK_CHECK(RootA.sizes.size()==1 && A1.sizes.size()==2 &&
               RootB.sizes.size()==1 && B1.sizes.size()==1 && B2.sizes.size()==2);
}

namespace tc {

struct Probe {
    AP4_Result result;
    unsigned   handler_creations;
    AP4_Size   output_bytes;
};

static Probe
probe_rejected(const Bytes& fragment_bytes)
{
    std::vector<char> log;
    unsigned counter = 0;
    RecordingProcessor processor(false, log, counter);
    Bytes init = box("moov");
    auto init_s = new AP4_MemoryByteStream(init.data(),(AP4_Size)init.size());
    auto frag_s = new AP4_MemoryByteStream(fragment_bytes.data(),
                                           (AP4_Size)fragment_bytes.size());
    auto out = new AP4_MemoryByteStream();
    AP4_Result r = processor.Process(*frag_s, *out, *init_s);
    Probe p;
    p.result = r;
    p.handler_creations = processor.HandlerCreations();
    p.output_bytes = out->GetDataSize();
    init_s->Release(); frag_s->Release(); out->Release();
    return p;
}

// one valid leaf sidx + one fragment, exposed for targeted byte surgery
struct LeafBase {
    Bytes stream;
    size_t sidx_offset;
    size_t sidx_size;
    size_t moof_offset;
};

static LeafBase
build_leaf_base(AP4_UI08 version, AP4_UI32 timescale,
                AP4_UI64 earliest, AP4_UI32 ref_duration,
                AP4_UI32 ref_size_override, bool override_size)
{
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    AP4_UI64 sidx_size = version==0 ? 44 : 52;
    AP4_UI64 first = 0;                   // sidx box directly precedes the moof
    AP4_UI32 ref_size = override_size ? ref_size_override : (AP4_UI32)frag.size();
    Bytes sidx = make_sidx(version, 1, timescale, earliest, first,
                           {{false, ref_size, ref_duration}});
    TASK_CHECK(sidx.size() == (size_t)sidx_size);
    LeafBase lb;
    lb.sidx_offset = 0;
    lb.sidx_size = sidx.size();
    lb.moof_offset = sidx.size();
    lb.stream = sidx;
    append(lb.stream, frag);
    return lb;
}

} // namespace tc

TASK_CASE("hierarchy", "truncated_box_rejected_before_handler")
{
    using namespace tc;
    LeafBase lb = build_leaf_base(0,1000,0,100,0,false);
    Bytes cut = lb.stream;
    cut.resize(cut.size()-5);               // truncate the trailing mdat
    Probe p = probe_rejected(cut);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0);
    TASK_CHECK(p.output_bytes == 0);
}

TASK_CASE("hierarchy", "trailing_partial_header_rejected_before_handler")
{
    using namespace tc;
    LeafBase lb = build_leaf_base(0,1000,0,100,0,false);
    Bytes cut = lb.stream;
    cut.push_back(0x00);                     // 1..7 bytes cannot form a box
    cut.push_back(0x00);
    cut.push_back(0x00);
    Probe p = probe_rejected(cut);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0);
    TASK_CHECK(p.output_bytes == 0);
}

TASK_CASE("hierarchy", "zero_timescale_rejected_before_handler")
{
    using namespace tc;
    // rebuild with timescale 0 (same box size)
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    Bytes sidx = make_sidx(0,1,0,0,0,{{false,(AP4_UI32)frag.size(),100}});
    Bytes stream = sidx; append(stream, frag);
    Probe p = probe_rejected(stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "zero_ref_size_rejected_before_handler")
{
    using namespace tc;
    LeafBase lb = build_leaf_base(0,1000,0,100,0,true);
    Probe p = probe_rejected(lb.stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "zero_duration_rejected_before_handler")
{
    using namespace tc;
    LeafBase lb = build_leaf_base(0,1000,0,0,false ? 1 : 0,false);
    Probe p = probe_rejected(lb.stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "range_past_end_rejected_before_handler")
{
    using namespace tc;
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    // first_offset pushes the range entirely past the stream end
    Bytes sidx = make_sidx(0,1,1000,0,(AP4_UI64)frag.size()+1000,
                           {{false,(AP4_UI32)frag.size(),100}});
    Bytes stream = sidx; append(stream, frag);
    Probe p = probe_rejected(stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "type1_target_not_sidx_rejected_before_handler")
{
    using namespace tc;
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    // indirect reference whose range is one whole moof box, not a sidx
    Bytes sidx = make_sidx(0,1,1000,0,0,
                           {{true,(AP4_UI32)seed::scan(frag)[0].size,100}});
    Bytes stream = sidx; append(stream, frag);
    Probe p = probe_rejected(stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "type0_ends_inside_box_rejected_before_handler")
{
    using namespace tc;
    LeafBase lb = build_leaf_base(0,1000,0,100,0,false);
    auto boxes = seed::scan(lb.stream);
    AP4_UI32 moof_size = boxes[1].size;
    // range runs 5 bytes into the mdat instead of ending on a box boundary
    Bytes sidx = make_sidx(0,1,1000,0,0,
                           {{false,moof_size+5,100}});
    Bytes stream = sidx;
    append(stream, Bytes(lb.stream.begin()+lb.sidx_size, lb.stream.end()));
    Probe p = probe_rejected(stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "type0_without_media_rejected_before_handler")
{
    using namespace tc;
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    AP4_UI32 moof_size = seed::scan(frag)[0].size;
    // range covers the moof only, ending before its mdat
    Bytes sidx = make_sidx(0,1,1000,0,0,
                           {{false,moof_size,100}});
    Bytes stream = sidx; append(stream, frag);
    Probe p = probe_rejected(stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "parent_child_time_mismatch_rejected_before_handler")
{
    using namespace tc;
    Bytes input = build_hierarchy_input();
    // corrupt A1's first duration (400 -> 401): identical field width,
    // spatial layout stays valid but the time equality breaks
    auto sidxs = boxes_of_type(input, AP4_ATOM_TYPE_SIDX);
    size_t a1_off = (size_t)sidxs[1].offset;   // RootA, free, A1 ...
    // first reference duration sits at sidx body: header12 + fields16 + count4
    // + first ref's size field (4), so at a1_off + 12 + 20 + 4
    size_t dur_at = a1_off + 12 + 20 + 4;
    TASK_CHECK(get(input, dur_at) == 400);
    Bytes corrupted = input;
    AP4_UI32 bad = 401;
    for (int i=3;i>=0;--i) corrupted[dur_at+i] = (AP4_UI08)(bad >> (8*(3-i)));
    Probe p = probe_rejected(corrupted);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

TASK_CASE("hierarchy", "time_addition_overflow_rejected_before_handler")
{
    using namespace tc;
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    const AP4_UI64 huge_time = 0xFFFFFFFFFFFFFF00ULL;
    // v1 earliest plus the 0x100 duration overflows 64 bits
    Bytes sidx = make_sidx(1,1,1000,huge_time,0,
                           {{false,(AP4_UI32)frag.size(),0x100}});
    Bytes stream = sidx; append(stream, frag);
    Probe p = probe_rejected(stream);
    TASK_CHECK(AP4_FAILED(p.result));
    TASK_CHECK(p.handler_creations == 0 && p.output_bytes == 0);
}

/*======================================================================
|  compat group
+=====================================================================*/
namespace tc {

static Bytes
single_frag_stream()
{
    FragSpec fs{1,{3,2},10};
    Bytes frag = make_fragment(fs);
    Bytes sidx = make_sidx(0, 1, 1000, 0, 0,
                           {{false,(AP4_UI32)frag.size(),200}});
    Bytes out = sidx; append(out, frag);
    return out;
}

} // namespace tc

TASK_CASE("compat", "both_process_entry_points_preserve_single_level_index")
{
    using namespace tc;
    Bytes frags = single_frag_stream();

    std::vector<char> log1; unsigned c1=0;
    RunResult split = run_split(frags, RecordingProcessor(false,log1,c1));
    TASK_CHECK(split.result == AP4_SUCCESS);
    TASK_CHECK(seed::count(split.output, AP4_ATOM_TYPE_SIDX) == 1);
    auto sb = boxes_of_type(split.output, AP4_ATOM_TYPE_SIDX);
    ParsedSidx s = parse_sidx(split.output, sb[0]);
    TASK_CHECK(s.sizes.size()==1 && s.types[0]==0 && s.durations[0]==200);
    auto ranges = resolve_ranges(s, (AP4_UI64)sb[0].offset+sb[0].size);
    TASK_CHECK((markers_in_range(split.output,ranges[0].start,ranges[0].end)
                == std::vector<AP4_UI08>{10}));

    // combined entry: moov precedes the media
    Bytes whole = box("moov"); append(whole, frags);
    std::vector<char> log2; unsigned c2=0;
    RunResult combined = run_combined(whole, RecordingProcessor(false,log2,c2));
    TASK_CHECK(combined.result == AP4_SUCCESS);
    TASK_CHECK(seed::count(combined.output, AP4_ATOM_TYPE_MOOV) == 1);
    TASK_CHECK(seed::count(combined.output, AP4_ATOM_TYPE_SIDX) == 1);
    auto cb = boxes_of_type(combined.output, AP4_ATOM_TYPE_SIDX);
    ParsedSidx cs = parse_sidx(combined.output, cb[0]);
    auto cranges = resolve_ranges(cs, (AP4_UI64)cb[0].offset+cb[0].size);
    TASK_CHECK((markers_in_range(combined.output,cranges[0].start,cranges[0].end)
                == std::vector<AP4_UI08>{10}));
}

TASK_CASE("compat", "nonzero_output_start_and_terminal_cursor")
{
    using namespace tc;
    Bytes frags = single_frag_stream();
    const AP4_Position START = 100;

    Bytes init = box("moov");
    auto init_s = new AP4_MemoryByteStream(init.data(),(AP4_Size)init.size());
    auto frag_s = new AP4_MemoryByteStream(frags.data(),(AP4_Size)frags.size());
    auto out = new AP4_MemoryByteStream();
    Bytes pad((size_t)START, 0xCD);
    out->Write(pad.data(), (AP4_Size)pad.size());
    out->Seek(START);

    std::vector<char> log; unsigned counter=0;
    RecordingProcessor processor(false, log, counter);
    AP4_Result r = processor.Process(*frag_s, *out, *init_s);
    TASK_CHECK(r == AP4_SUCCESS);

    AP4_Position end = 0; out->Tell(end);
    AP4_LargeSize total = 0; out->GetSize(total);
    TASK_CHECK(end == total && end > START);   // cursor at the output end

    Bytes result(out->GetData()+START, out->GetData()+out->GetDataSize());
    TASK_CHECK(seed::count(result, AP4_ATOM_TYPE_SIDX) == 1);
    auto sb = boxes_of_type(result, AP4_ATOM_TYPE_SIDX);
    ParsedSidx s = parse_sidx(result, sb[0]);
    auto ranges = resolve_ranges(s, (AP4_UI64)sb[0].offset+sb[0].size);
    TASK_CHECK((markers_in_range(result,ranges[0].start,ranges[0].end)
                == std::vector<AP4_UI08>{10}));
    // padding untouched
    TASK_CHECK(out->GetData()[0] == 0xCD && out->GetData()[START-1] == 0xCD);

    init_s->Release(); frag_s->Release(); out->Release();
}

TASK_CASE("compat", "index_at_input_zero_offset")
{
    using namespace tc;
    Bytes frags = single_frag_stream();
    TASK_CHECK(seed::scan(frags)[0].type == AP4_ATOM_TYPE_SIDX);
    TASK_CHECK(seed::scan(frags)[0].offset == 0);
    std::vector<char> log; unsigned c=0;
    RunResult rr = run_split(frags, RecordingProcessor(false,log,c));
    TASK_CHECK(rr.result == AP4_SUCCESS);
    TASK_CHECK(seed::scan(rr.output)[0].type == AP4_ATOM_TYPE_SIDX);
    TASK_CHECK(seed::scan(rr.output)[0].offset == 0);
}

TASK_CASE("compat", "v1_time_and_first_offset_preserved")
{
    using namespace tc;
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    Bytes gap = box("free", Bytes(40, 0x77));
    const AP4_UI64 EARLIEST = 0x100000005ULL;
    Bytes sidx = make_sidx(1, 5, 48000, EARLIEST, gap.size(),
                           {{false,(AP4_UI32)frag.size(),960}});
    Bytes stream = sidx; append(stream, gap); append(stream, frag);

    std::vector<char> log; unsigned c=0;
    RunResult rr = run_split(stream, RecordingProcessor(false,log,c));
    TASK_CHECK(rr.result == AP4_SUCCESS);
    auto sb = boxes_of_type(rr.output, AP4_ATOM_TYPE_SIDX);
    TASK_CHECK(sb.size() == 1);
    ParsedSidx s = parse_sidx(rr.output, sb[0]);
    TASK_CHECK(s.version == 1);
    TASK_CHECK(s.timescale == 48000);
    TASK_CHECK(s.earliest == EARLIEST);
    TASK_CHECK(s.first_offset == gap.size());       // unchanged relative layout
    TASK_CHECK(s.sizes[0] > frag.size());           // samples grew
    auto ranges = resolve_ranges(s, (AP4_UI64)sb[0].offset+sb[0].size);
    TASK_CHECK((markers_in_range(rr.output,ranges[0].start,ranges[0].end)
                == std::vector<AP4_UI08>{10}));
}

TASK_CASE("compat", "empty_sidx_kept_verbatim")
{
    using namespace tc;
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    Bytes empty = make_sidx(0, 9, 1000, 7, 0, {});
    Bytes stream = empty; append(stream, frag);

    std::vector<char> log; unsigned c=0;
    RunResult rr = run_split(stream, RecordingProcessor(false,log,c));
    TASK_CHECK(rr.result == AP4_SUCCESS);
    auto sb = boxes_of_type(rr.output, AP4_ATOM_TYPE_SIDX);
    TASK_CHECK(sb.size() == 1);
    ParsedSidx s = parse_sidx(rr.output, sb[0]);
    TASK_CHECK(s.ref_id == 9 && s.earliest == 7 && s.sizes.empty());
    // byte-for-byte identical to the input index
    TASK_CHECK(rr.output.size() >= empty.size());
    TASK_CHECK(std::memcmp(rr.output.data(), empty.data(), empty.size()) == 0);
    TASK_CHECK((markers_in_range(rr.output,0,(AP4_UI64)rr.output.size())
                == std::vector<AP4_UI08>{10}));
}

TASK_CASE("compat", "no_index_still_processes_fragments")
{
    using namespace tc;
    FragSpec a{1,{3},10}, b{2,{2},20};
    Bytes stream = make_fragment(a); append(stream, make_fragment(b));
    TASK_CHECK(seed::count(stream, AP4_ATOM_TYPE_SIDX) == 0);
    std::vector<char> log; unsigned c=0;
    RunResult rr = run_split(stream, RecordingProcessor(false,log,c));
    TASK_CHECK(rr.result == AP4_SUCCESS);
    TASK_CHECK(seed::count(rr.output, AP4_ATOM_TYPE_SIDX) == 0);
    TASK_CHECK(seed::count(rr.output, AP4_ATOM_TYPE_MOOF) == 2);
    auto markers = markers_in_range(rr.output,0,(AP4_UI64)rr.output.size());
    TASK_CHECK((markers == std::vector<AP4_UI08>{10,20}));
}

TASK_CASE("compat", "tfra_moof_offsets_updated_without_sidx")
{
    using namespace tc;
    FragSpec fs{1,{2},10};
    Bytes frag = make_fragment(fs);
    Bytes lead = box("free", Bytes(12, 0x33));

    // build a self-consistent mfra/tfra/mfro with the AP4 classes instead of
    // hand-laying bytes (mfro must declare the total mfra size)
    AP4_UI64 moov_size = box("moov").size();
    AP4_UI64 moof_in = moov_size + lead.size();
    auto mfra = new AP4_ContainerAtom(AP4_ATOM_TYPE_MFRA);
    auto tfra = new AP4_TfraAtom(1);
    tfra->AddEntry(500, moof_in);
    mfra->AddChild(tfra);
    // mfro is a 16-byte full atom and must declare the final mfra size
    const AP4_UI32 MFRO_BOX_SIZE = AP4_FULL_ATOM_HEADER_SIZE + 4;
    mfra->AddChild(new AP4_MfroAtom((AP4_UI32)mfra->GetSize() + MFRO_BOX_SIZE));
    Bytes mfra_bytes;
    {
        auto tmp = new AP4_MemoryByteStream();
        AP4_Result wr = mfra->Write(*tmp);
        TASK_CHECK(wr == AP4_SUCCESS);
        mfra_bytes.assign(tmp->GetData(), tmp->GetData()+tmp->GetDataSize());
        tmp->Release();
    }
    delete mfra;

    // combined entry: the moov precedes the media and the mfra is captured
    // and re-emitted at the end with updated tfra offsets
    Bytes whole = box("moov");
    append(whole, lead); append(whole, frag); append(whole, mfra_bytes);
    std::vector<char> log; unsigned c=0;
    RunResult rr = run_combined(whole, RecordingProcessor(false,log,c));
    TASK_CHECK(rr.result == AP4_SUCCESS);
    auto mfra_out = boxes_of_type(rr.output, AP4_ATOM_TYPE_MFRA);
    TASK_CHECK(mfra_out.size() == 1);
    // the tfra is the first child inside the mfra container
    AP4_UI32 inner_type = get(rr.output, mfra_out[0].offset + 8 + 4);
    TASK_CHECK(inner_type == AP4_ATOM_TYPE_TFRA);
    // entry layout inside tfra: header12 + track4 + fields4 + count4, then time4/off4
    AP4_UI32 moof_out = get(rr.output, mfra_out[0].offset + 8 + 12 + 12 + 4);
    TASK_CHECK(moof_out < rr.output.size());
    // the rewritten offset really lands on a moof box in the output
    bool lands_on_moof = false;
    for (auto& x : seed::scan(rr.output))
        if (x.offset == moof_out && x.type == AP4_ATOM_TYPE_MOOF) lands_on_moof = true;
    TASK_CHECK(lands_on_moof);
    // with an unchanged leading layout the relocated value is stable
    TASK_CHECK(moof_out == (AP4_UI32)moof_in);
}

TASK_CASE("compat", "read_failure_propagated_before_handler")
{
    using namespace tc;
    Bytes frags = single_frag_stream();
    Bytes init = box("moov");
    auto init_s = new AP4_MemoryByteStream(init.data(),(AP4_Size)init.size());
    auto frag_inner = new AP4_MemoryByteStream(frags.data(),(AP4_Size)frags.size());
    auto failing = new FailingStream(frag_inner, true, 2);
    auto out = new AP4_MemoryByteStream();
    std::vector<char> log; unsigned c=0;
    RecordingProcessor processor(false,log,c);
    AP4_Result r = processor.Process(*failing, *out, *init_s);
    TASK_CHECK(AP4_FAILED(r));
    TASK_CHECK(processor.HandlerCreations() == 0);
    init_s->Release(); failing->Release(); frag_inner->Release(); out->Release();
}

TASK_CASE("compat", "write_failure_propagated")
{
    using namespace tc;
    Bytes frags = single_frag_stream();
    Bytes init = box("moov");
    auto init_s = new AP4_MemoryByteStream(init.data(),(AP4_Size)init.size());
    auto frag_s = new AP4_MemoryByteStream(frags.data(),(AP4_Size)frags.size());
    auto out_inner = new AP4_MemoryByteStream();
    auto failing = new FailingStream(out_inner, false, 1);
    std::vector<char> log; unsigned c=0;
    RecordingProcessor processor(false,log,c);
    AP4_Result r = processor.Process(*frag_s, *failing, *init_s);
    TASK_CHECK(r == AP4_ERROR_WRITE_FAILED);
    init_s->Release(); frag_s->Release(); failing->Release();
    out_inner->Release();
}

TASK_CASE("compat", "handler_sample_and_finish_failures_propagated")
{
    using namespace tc;
    Bytes frags = single_frag_stream();

    std::vector<char> log1; unsigned c1=0;
    RunResult rs = run_split(frags,
        RecordingProcessor(false,log1,c1,AP4_ERROR_NOT_SUPPORTED,AP4_SUCCESS));
    TASK_CHECK(rs.result == AP4_ERROR_NOT_SUPPORTED);

    std::vector<char> log2; unsigned c2=0;
    RunResult rf = run_split(frags,
        RecordingProcessor(false,log2,c2,AP4_SUCCESS,AP4_ERROR_INVALID_STATE));
    TASK_CHECK(rf.result == AP4_ERROR_INVALID_STATE);
}

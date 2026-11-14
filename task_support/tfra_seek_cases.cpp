/*
|   TFRA seek test cases for AP4_LinearReader::SeekTo.
|
|   The fragmented MP4 files used here are synthesized in memory. The tfra
|   index of track 1 is written with AP4_TfraAtom (v0); the tfra index of
|   track 2 is written by hand as raw bytes (v1), as an independent
|   reference encoder.
*/
#include <vector>
#include <cstring>

#include "check.h"

#include "Ap4.h"
#include "Ap4LinearReader.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4SampleDescription.h"
#include "Ap4ContainerAtom.h"
#include "Ap4TfhdAtom.h"
#include "Ap4TfdtAtom.h"
#include "Ap4TrexAtom.h"
#include "Ap4TrunAtom.h"
#include "Ap4TfraAtom.h"
#include "Ap4MfroAtom.h"
#include "Ap4MfhdAtom.h"
#include "Ap4MovieFragment.h"

/*----------------------------------------------------------------------
|   constants of the synthetic files
+---------------------------------------------------------------------*/
#define T1_ID        1
#define T1_TIMESCALE 1000
#define T1_DURATION  40
#define T1_SIZE      100

#define T2_ID        2
#define T2_TIMESCALE 8000
#define T2_DURATION  640
#define T2_SIZE      160

#define NON_SYNC AP4_FRAG_FLAG_SAMPLE_IS_DIFFERENCE

/*----------------------------------------------------------------------
|   file description structures
+---------------------------------------------------------------------*/
struct SampleSpec {
    SampleSpec(AP4_UI32 d, AP4_UI32 s, AP4_UI32 f, AP4_UI32 c) :
        duration(d), size(s), flags(f), cts_delta(c) {}
    AP4_UI32 duration;
    AP4_UI32 size;
    AP4_UI32 flags;
    AP4_UI32 cts_delta;
};

struct TrunSpec {
    TrunSpec() : explicit_fields(false) {}
    bool                    explicit_fields; // per-sample fields, else tfhd defaults
    std::vector<SampleSpec> samples;
};

struct TrafSpec {
    TrafSpec(AP4_UI32 id) : track_id(id), has_tfdt(false), tfdt_version(0), tfdt(0), default_flags(0) {}
    AP4_UI32              track_id;
    bool                  has_tfdt;
    AP4_UI08              tfdt_version;
    AP4_UI64              tfdt;
    AP4_UI32              default_flags;
    std::vector<TrunSpec> truns;
};

struct FragSpec {
    std::vector<TrafSpec> trafs;
};

struct TfraEntrySpec {
    TfraEntrySpec(AP4_UI64 t, int f, AP4_UI32 traf, AP4_UI32 trun, AP4_UI32 sample) :
        time(t), frag(f), traf(traf), trun(trun), sample(sample) {}
    AP4_UI64 time;
    int      frag; // index into the fragment list; negative = deliberately out of range
    AP4_UI32 traf;
    AP4_UI32 trun;
    AP4_UI32 sample;
};

struct ExpectedSample {
    AP4_UI32 track_id;
    AP4_UI64 offset;
    AP4_UI64 dts;
    AP4_UI64 cts;
    AP4_UI32 duration;
    AP4_UI32 size;
    bool     sync;
    AP4_UI08 fill;
};

struct BuiltFile {
    BuiltFile() : stream(NULL) {}
    AP4_MemoryByteStream*                            stream;
    std::vector<ExpectedSample>                      storage;      // all samples, in file order
    std::vector<ExpectedSample>                      by_track[3];  // per track id (1 and 2)
    std::vector<std::pair<AP4_Position,AP4_Position> > mdat;       // media payload ranges
    std::vector<AP4_Position>                        moof_offsets;
};

/*----------------------------------------------------------------------
|   big-endian byte writers for the hand-built tfra
+---------------------------------------------------------------------*/
static void Put08(std::vector<AP4_UI08>& out, AP4_UI08 v) {
    out.push_back(v);
}
static void Put32(std::vector<AP4_UI08>& out, AP4_UI32 v) {
    out.push_back((AP4_UI08)(v>>24));
    out.push_back((AP4_UI08)(v>>16));
    out.push_back((AP4_UI08)(v>>8));
    out.push_back((AP4_UI08)(v));
}
static void Put64(std::vector<AP4_UI08>& out, AP4_UI64 v) {
    Put32(out, (AP4_UI32)(v>>32));
    Put32(out, (AP4_UI32)v);
}

/*----------------------------------------------------------------------
|   RecordingStream: delegates to another stream and records read ranges
+---------------------------------------------------------------------*/
class RecordingStream : public AP4_ByteStream {
public:
    RecordingStream(AP4_ByteStream* delegate) :
        m_Delegate(delegate),
        m_ReferenceCount(1) {
        m_Delegate->AddReference();
    }
    virtual ~RecordingStream() {
        m_Delegate->Release();
    }

    // AP4_ByteStream methods
    virtual AP4_Result ReadPartial(void*     buffer,
                                   AP4_Size  bytes_to_read,
                                   AP4_Size& bytes_read) {
        AP4_Position position = 0;
        m_Delegate->Tell(position);
        AP4_Result result = m_Delegate->ReadPartial(buffer, bytes_to_read, bytes_read);
        if (AP4_SUCCEEDED(result) && bytes_read) {
            reads.push_back(std::make_pair(position, position+bytes_read));
        }
        return result;
    }
    virtual AP4_Result WritePartial(const void* buffer,
                                    AP4_Size    bytes_to_write,
                                    AP4_Size&   bytes_written) {
        return m_Delegate->WritePartial(buffer, bytes_to_write, bytes_written);
    }
    virtual AP4_Result Seek(AP4_Position position)    { return m_Delegate->Seek(position); }
    virtual AP4_Result Tell(AP4_Position& position)   { return m_Delegate->Tell(position); }
    virtual AP4_Result GetSize(AP4_LargeSize& size)   { return m_Delegate->GetSize(size); }

    // AP4_Referenceable methods
    virtual void AddReference() { ++m_ReferenceCount; }
    virtual void Release()      { if (--m_ReferenceCount == 0) delete this; }

    std::vector<std::pair<AP4_Position,AP4_Position> > reads;

private:
    AP4_ByteStream* m_Delegate;
    unsigned int    m_ReferenceCount;
};

/*----------------------------------------------------------------------
|   CreateMovie: two tracks with different media time scales
+---------------------------------------------------------------------*/
static AP4_Movie* CreateMovie() {
    AP4_Movie* movie = new AP4_Movie(1000, 0);
    AP4_ContainerAtom* mvex = new AP4_ContainerAtom(AP4_ATOM_TYPE_MVEX);
    const AP4_UI32 ids[2]       = {T1_ID, T2_ID};
    const AP4_UI32 scales[2]    = {T1_TIMESCALE, T2_TIMESCALE};
    const AP4_UI32 durations[2] = {T1_DURATION, T2_DURATION};
    const AP4_UI32 sizes[2]     = {T1_SIZE, T2_SIZE};
    for (unsigned int i=0; i<2; i++) {
        AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
        table->AddSampleDescription(
            new AP4_GenericVideoSampleDescription(AP4_SAMPLE_FORMAT_AVC1, 320, 240, 0, "Bento4", NULL),
            true);
        AP4_Track* track = new AP4_Track(AP4_Track::TYPE_VIDEO,
                                         table,
                                         ids[i],
                                         1000,
                                         0,
                                         scales[i],
                                         0,
                                         "und",
                                         320<<16,
                                         240<<16);
        movie->AddTrack(track);
        mvex->AddChild(new AP4_TrexAtom(ids[i], 1, durations[i], sizes[i], 0));
    }
    movie->GetMoovAtom()->AddChild(mvex);
    return movie;
}

/*----------------------------------------------------------------------
|   MoofOffsetFor: resolve a fragment index to a file position
+---------------------------------------------------------------------*/
static AP4_UI64 MoofOffsetFor(const BuiltFile& built, int frag) {
    if (frag < 0 || (unsigned int)frag >= built.moof_offsets.size()) {
        // deliberately out of range
        return (AP4_UI64)0x7FFFFFFFFFFFULL;
    }
    return built.moof_offsets[frag];
}

/*----------------------------------------------------------------------
|   BuildFile: synthesize a fragmented MP4 stream
+---------------------------------------------------------------------*/
static BuiltFile* BuildFile(const std::vector<FragSpec>&      frags,
                            const std::vector<TfraEntrySpec>& tfra1,
                            const std::vector<TfraEntrySpec>& tfra2,
                            bool                              write_tfra2,
                            bool                              write_mfra) {
    BuiltFile* built = new BuiltFile();
    AP4_MemoryByteStream* stream = new AP4_MemoryByteStream();
    built->stream = stream;

    AP4_UI64 running_dts[3];
    running_dts[T1_ID] = 0;
    running_dts[T2_ID] = 0;
    AP4_UI08 fill = 0x20;

    // fragments
    for (unsigned int f=0; f<frags.size(); f++) {
        const FragSpec& frag = frags[f];
        AP4_Position moof_offset = 0;
        stream->Tell(moof_offset);
        built->moof_offsets.push_back(moof_offset);

        AP4_ContainerAtom moof(AP4_ATOM_TYPE_MOOF);
        moof.AddChild(new AP4_MfhdAtom(f+1));
        std::vector<AP4_TrunAtom*>    trun_atoms;
        std::vector<const TrunSpec*>  trun_specs;
        std::vector<const TrafSpec*>  trun_trafs;
        for (unsigned int a=0; a<frag.trafs.size(); a++) {
            const TrafSpec& traf_spec = frag.trafs[a];
            AP4_UI32 default_duration = traf_spec.track_id == T1_ID ? T1_DURATION : T2_DURATION;
            AP4_UI32 default_size     = traf_spec.track_id == T1_ID ? T1_SIZE     : T2_SIZE;
            AP4_ContainerAtom* traf = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
            traf->AddChild(new AP4_TfhdAtom(AP4_TFHD_FLAG_DEFAULT_BASE_IS_MOOF              |
                                            AP4_TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT   |
                                            AP4_TFHD_FLAG_DEFAULT_SAMPLE_SIZE_PRESENT       |
                                            AP4_TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT,
                                            traf_spec.track_id,
                                            0,
                                            0,
                                            default_duration,
                                            default_size,
                                            traf_spec.default_flags));
            if (traf_spec.has_tfdt) {
                traf->AddChild(new AP4_TfdtAtom(traf_spec.tfdt_version, traf_spec.tfdt));
            }
            for (unsigned int r=0; r<traf_spec.truns.size(); r++) {
                const TrunSpec& trun_spec = traf_spec.truns[r];
                AP4_UI32 flags = AP4_TRUN_FLAG_DATA_OFFSET_PRESENT;
                if (trun_spec.explicit_fields) {
                    flags |= AP4_TRUN_FLAG_SAMPLE_DURATION_PRESENT |
                             AP4_TRUN_FLAG_SAMPLE_SIZE_PRESENT     |
                             AP4_TRUN_FLAG_SAMPLE_FLAGS_PRESENT    |
                             AP4_TRUN_FLAG_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT;
                }
                AP4_TrunAtom* trun = new AP4_TrunAtom(flags, 0, 0);
                AP4_Array<AP4_TrunAtom::Entry> entries;
                for (unsigned int s=0; s<trun_spec.samples.size(); s++) {
                    AP4_TrunAtom::Entry entry;
                    entry.sample_duration                = trun_spec.samples[s].duration;
                    entry.sample_size                    = trun_spec.samples[s].size;
                    entry.sample_flags                   = trun_spec.samples[s].flags;
                    entry.sample_composition_time_offset = trun_spec.samples[s].cts_delta;
                    entries.Append(entry);
                }
                trun->SetEntries(entries);
                traf->AddChild(trun);
                trun_atoms.push_back(trun);
                trun_specs.push_back(&trun_spec);
                trun_trafs.push_back(&traf_spec);
            }
            moof.AddChild(traf);
        }

        // lay out the media payload in moof trun order and fix the data offsets
        AP4_UI32 payload_base = (AP4_UI32)moof.GetSize()+8;
        AP4_UI32 payload_size = 0;
        for (unsigned int r=0; r<trun_atoms.size(); r++) {
            trun_atoms[r]->SetDataOffset((AP4_SI32)(payload_base+payload_size));
            for (unsigned int e=0; e<trun_specs[r]->samples.size(); e++) {
                payload_size += trun_specs[r]->explicit_fields ?
                                trun_specs[r]->samples[e].size :
                                (trun_trafs[r]->track_id == T1_ID ? T1_SIZE : T2_SIZE);
            }
        }

        // write the moof
        moof.Write(*stream);

        // write the mdat
        AP4_Position mdat_payload_offset = 0;
        stream->Tell(mdat_payload_offset);
        mdat_payload_offset += 8;
        stream->WriteUI32(8+payload_size);
        stream->WriteUI32(AP4_ATOM_TYPE_MDAT);
        built->mdat.push_back(std::make_pair(mdat_payload_offset, mdat_payload_offset+payload_size));

        // write the payloads and record the expected samples
        AP4_UI64 sample_offset = mdat_payload_offset;
        for (unsigned int a=0; a<frag.trafs.size(); a++) {
            const TrafSpec& traf_spec = frag.trafs[a];
            AP4_UI64 dts = traf_spec.has_tfdt ? traf_spec.tfdt : running_dts[traf_spec.track_id];
            for (unsigned int r=0; r<traf_spec.truns.size(); r++) {
                const TrunSpec& trun_spec = traf_spec.truns[r];
                for (unsigned int s=0; s<trun_spec.samples.size(); s++) {
                    const SampleSpec& sample_spec = trun_spec.samples[s];
                    AP4_UI32 duration  = trun_spec.explicit_fields ? sample_spec.duration
                                         : (traf_spec.track_id == T1_ID ? T1_DURATION : T2_DURATION);
                    AP4_UI32 size      = trun_spec.explicit_fields ? sample_spec.size
                                         : (traf_spec.track_id == T1_ID ? T1_SIZE : T2_SIZE);
                    AP4_UI32 flags     = trun_spec.explicit_fields ? sample_spec.flags
                                         : traf_spec.default_flags;
                    AP4_UI32 cts_delta = trun_spec.explicit_fields ? sample_spec.cts_delta : 0;

                    ExpectedSample expected;
                    expected.track_id = traf_spec.track_id;
                    expected.offset   = sample_offset;
                    expected.dts      = dts;
                    expected.cts      = dts+cts_delta;
                    expected.duration = duration;
                    expected.size     = size;
                    expected.sync     = (flags & NON_SYNC) == 0;
                    expected.fill     = fill++;
                    built->storage.push_back(expected);
                    built->by_track[traf_spec.track_id].push_back(expected);

                    std::vector<AP4_UI08> payload(size, expected.fill);
                    stream->Write(&payload[0], size);

                    sample_offset += size;
                    dts           += duration;
                }
            }
            running_dts[traf_spec.track_id] = dts;
        }
    }

    // random access index
    if (write_mfra) {
        // track 1: written with the Bento4 writer (v0)
        AP4_TfraAtom tfra1_atom(T1_ID);
        for (unsigned int i=0; i<tfra1.size(); i++) {
            tfra1_atom.AddEntry(tfra1[i].time,
                                MoofOffsetFor(*built, tfra1[i].frag),
                                tfra1[i].traf,
                                tfra1[i].trun,
                                tfra1[i].sample);
        }

        // track 2: written by hand as raw bytes (v1), as an independent reference
        std::vector<AP4_UI08> tfra2_bytes;
        Put32(tfra2_bytes, 8+4+4+4+4+(AP4_UI32)tfra2.size()*19);
        Put32(tfra2_bytes, AP4_ATOM_TYPE_TFRA);
        Put08(tfra2_bytes, 1);     // version 1
        Put08(tfra2_bytes, 0);     // flags
        Put08(tfra2_bytes, 0);
        Put08(tfra2_bytes, 0);
        Put32(tfra2_bytes, T2_ID); // track_ID
        Put32(tfra2_bytes, 0);     // reserved + 1-byte traf/trun/sample numbers
        Put32(tfra2_bytes, (AP4_UI32)tfra2.size());
        for (unsigned int i=0; i<tfra2.size(); i++) {
            Put64(tfra2_bytes, tfra2[i].time);
            Put64(tfra2_bytes, MoofOffsetFor(*built, tfra2[i].frag));
            Put08(tfra2_bytes, (AP4_UI08)tfra2[i].traf);
            Put08(tfra2_bytes, (AP4_UI08)tfra2[i].trun);
            Put08(tfra2_bytes, (AP4_UI08)tfra2[i].sample);
        }

        AP4_UI32 mfra_size = 8 + (AP4_UI32)tfra1_atom.GetSize() + 16;
        if (write_tfra2) mfra_size += (AP4_UI32)tfra2_bytes.size();
        stream->WriteUI32(mfra_size);
        stream->WriteUI32(AP4_ATOM_TYPE_MFRA);
        tfra1_atom.Write(*stream);
        if (write_tfra2 && tfra2_bytes.size()) {
            stream->Write(&tfra2_bytes[0], (AP4_Size)tfra2_bytes.size());
        }
        stream->WriteUI32(16);
        stream->WriteUI32(AP4_ATOM_TYPE_MFRO);
        stream->WriteUI32(0);
        stream->WriteUI32(mfra_size);
    }

    stream->Seek(0);
    return built;
}

/*----------------------------------------------------------------------
|   standard file contents shared by most cases
+---------------------------------------------------------------------*/
static std::vector<FragSpec> StandardFrags() {
    std::vector<FragSpec> frags;

    // fragment A: both tracks, track 1 has two runs
    {
        FragSpec frag;
        TrafSpec t1(T1_ID);
        t1.has_tfdt     = true;
        t1.tfdt_version = 1;
        t1.tfdt         = 0;
        {
            TrunSpec run; // defaults from tfhd: 2 samples, dts 0, 40
            run.samples.push_back(SampleSpec(0,0,0,0));
            run.samples.push_back(SampleSpec(0,0,0,0));
            t1.truns.push_back(run);
        }
        {
            TrunSpec run; // explicit fields: 3 samples, dts 80, 120, 160
            run.explicit_fields = true;
            run.samples.push_back(SampleSpec(40, 100, 0,        4));
            run.samples.push_back(SampleSpec(40, 100, NON_SYNC, 2));
            run.samples.push_back(SampleSpec(40, 100, NON_SYNC, 8));
            t1.truns.push_back(run);
        }
        frag.trafs.push_back(t1);
        TrafSpec t2(T2_ID);
        t2.has_tfdt = true;
        t2.tfdt     = 0;
        {
            TrunSpec run; // 2 samples, dts 0, 640
            run.samples.push_back(SampleSpec(0,0,0,0));
            run.samples.push_back(SampleSpec(0,0,0,0));
            t2.truns.push_back(run);
        }
        frag.trafs.push_back(t2);
        frags.push_back(frag);
    }

    // fragment B: track 1 only (track 2 missing), track 1 carries a tfdt
    {
        FragSpec frag;
        TrafSpec t1(T1_ID);
        t1.has_tfdt = true;
        t1.tfdt     = 200;
        {
            TrunSpec run; // 2 samples, dts 200, 240
            run.samples.push_back(SampleSpec(0,0,0,0));
            run.samples.push_back(SampleSpec(0,0,0,0));
            t1.truns.push_back(run);
        }
        frag.trafs.push_back(t1);
        frags.push_back(frag);
    }

    // fragment C: both tracks again, both carry a tfdt
    {
        FragSpec frag;
        TrafSpec t1(T1_ID);
        t1.has_tfdt = true;
        t1.tfdt     = 280;
        {
            TrunSpec run; // 2 samples, dts 280, 320
            run.samples.push_back(SampleSpec(0,0,0,0));
            run.samples.push_back(SampleSpec(0,0,0,0));
            t1.truns.push_back(run);
        }
        frag.trafs.push_back(t1);
        TrafSpec t2(T2_ID);
        t2.has_tfdt = true;
        t2.tfdt     = 1280;
        {
            TrunSpec run; // explicit fields: 3 samples, dts 1280, 1920, 2560
            run.explicit_fields = true;
            run.samples.push_back(SampleSpec(640, 160, 0,        0));
            run.samples.push_back(SampleSpec(640, 160, NON_SYNC, 16));
            run.samples.push_back(SampleSpec(640, 160, 0,        32));
            t2.truns.push_back(run);
        }
        frag.trafs.push_back(t2);
        frags.push_back(frag);
    }

    // fragment D: no tfdt anywhere, the decode times continue from fragment C
    {
        FragSpec frag;
        TrafSpec t1(T1_ID);
        {
            TrunSpec run; // 2 samples, dts 360, 400
            run.samples.push_back(SampleSpec(0,0,0,0));
            run.samples.push_back(SampleSpec(0,0,0,0));
            t1.truns.push_back(run);
        }
        frag.trafs.push_back(t1);
        TrafSpec t2(T2_ID);
        {
            TrunSpec run; // 1 sample, dts 3200
            run.samples.push_back(SampleSpec(0,0,0,0));
            t2.truns.push_back(run);
        }
        frag.trafs.push_back(t2);
        frags.push_back(frag);
    }

    return frags;
}

static std::vector<TfraEntrySpec> StandardTfra1() {
    std::vector<TfraEntrySpec> entries;
    entries.push_back(TfraEntrySpec(0,   0, 1, 1, 1)); // frag A, traf 1, run 1, sample 1
    entries.push_back(TfraEntrySpec(120, 0, 1, 2, 2)); // frag A, traf 1, run 2, sample 2
    entries.push_back(TfraEntrySpec(200, 1, 1, 1, 1)); // frag B, traf 1, run 1, sample 1
    entries.push_back(TfraEntrySpec(240, 1, 1, 1, 2)); // frag B, traf 1, run 1, sample 2
    entries.push_back(TfraEntrySpec(280, 2, 1, 1, 1)); // frag C, traf 1, run 1, sample 1
    return entries;
}

static std::vector<TfraEntrySpec> StandardTfra2() {
    std::vector<TfraEntrySpec> entries;
    entries.push_back(TfraEntrySpec(0,    0, 2, 1, 1)); // frag A, traf 2, run 1, sample 1
    entries.push_back(TfraEntrySpec(1281, 2, 2, 1, 1)); // frag C, traf 2, run 1, sample 1
    return entries;
}

static BuiltFile* BuildStandardFile() {
    return BuildFile(StandardFrags(), StandardTfra1(), StandardTfra2(), true, true);
}

/*----------------------------------------------------------------------
|   verification helpers
+---------------------------------------------------------------------*/
static bool CheckSample(AP4_Sample&           sample,
                        const AP4_DataBuffer* data,
                        const ExpectedSample& expected) {
    if (sample.GetOffset()   != expected.offset)   return false;
    if (sample.GetDts()      != expected.dts)      return false;
    if (sample.GetCts()      != expected.cts)      return false;
    if (sample.GetDuration() != expected.duration) return false;
    if (sample.GetSize()     != expected.size)     return false;
    if (sample.IsSync()      != expected.sync)     return false;
    if (data) {
        if (data->GetDataSize() != expected.size) return false;
        const AP4_UI08* bytes = data->GetData();
        for (AP4_UI32 i=0; i<expected.size; i++) {
            if (bytes[i] != expected.fill) return false;
        }
    }
    return true;
}

// check that no read recorded since `from` touched a media payload range
static bool NoMediaReads(const RecordingStream& rec, const BuiltFile& file, size_t from) {
    for (size_t i=from; i<rec.reads.size(); i++) {
        for (size_t j=0; j<file.mdat.size(); j++) {
            if (rec.reads[i].first < file.mdat[j].second &&
                rec.reads[i].second > file.mdat[j].first) {
                return false;
            }
        }
    }
    return true;
}

/*----------------------------------------------------------------------
|   anchors: the seek lands on the indexed sample
+---------------------------------------------------------------------*/

// tfra points at the second sample of the second run of a fragment;
// the next read must deliver exactly that sample, and seeking must not
// read any media payload
static int Case_AnchorInSecondRun() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        size_t reads_before = stream->reads.size();
        AP4_UI32 actual = 0xFFFFFFFF;
        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(120, &actual)));
        // track 1 anchored at time 120, track 2 at time 0: earliest is 0
        TASK_CHECK(actual == 0);
        // seeking must only read the index, box headers and moofs
        TASK_CHECK(NoMediaReads(*stream, *file, reads_before));

        // the anchor of track 1 is frag A, traf 1, run 2, sample 2 (dts 120)
        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][3]));
        TASK_CHECK(sample.GetDts() == 120);
        TASK_CHECK(sample.GetCts() == 122);
        TASK_CHECK(!sample.IsSync());

        // the anchor of track 2 is frag A, traf 2, run 1, sample 1 (dts 0)
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T2_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T2_ID][0]));
        TASK_CHECK(sample.GetDts() == 0);
    }
    stream->Release();
    return 0;
}

// two tracks with different index lengths and time bases anchor in
// different fragments; the physical start is the earliest anchor in the
// file, the reported time is the earliest selected time in milliseconds
static int Case_PerTrackAnchors() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        AP4_UI32 actual = 0;
        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(200, &actual)));
        // track 1 selected time 200 -> 200 ms; track 2 selected time 1281
        // at 8000 Hz -> 160.125 ms, floored to 160
        TASK_CHECK(actual == 160);

        // track 1 starts in frag B (dts 200), track 2 in frag C (dts 1280)
        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][5]));
        TASK_CHECK(sample.GetDts() == 200);
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T2_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T2_ID][2]));
        TASK_CHECK(sample.GetDts() == 1280);
    }
    stream->Release();
    return 0;
}

// the track 2 index is the hand-written v1 tfra; a request just past the
// exact entry time must select it
static int Case_V1IndexBoundary() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        // 161 ms is 1288 units at 8000 Hz, just past the 1281 entry
        AP4_UI32 actual = 0;
        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(161, &actual)));
        // track 1 selected time 120 -> 120 ms; track 2 -> 160 ms
        TASK_CHECK(actual == 120);

        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T2_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T2_ID][2]));
        TASK_CHECK(sample.GetDts() == 1280);
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][3]));
        TASK_CHECK(sample.GetDts() == 120);
    }
    stream->Release();
    return 0;
}

// when several entries carry the same time, the last one in the table wins
static int Case_EqualTimeLastEntry() {
    std::vector<TfraEntrySpec> tfra1;
    tfra1.push_back(TfraEntrySpec(0,   0, 1, 1, 1));
    tfra1.push_back(TfraEntrySpec(120, 0, 1, 2, 2)); // dts 120
    tfra1.push_back(TfraEntrySpec(120, 0, 1, 2, 3)); // dts 160, same time, later in the table
    tfra1.push_back(TfraEntrySpec(200, 1, 1, 1, 1));

    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildFile(StandardFrags(), tfra1, StandardTfra2(), true, true);
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(120, NULL)));
        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][4]));
        TASK_CHECK(sample.GetDts() == 160);
    }
    stream->Release();
    return 0;
}

/*----------------------------------------------------------------------
|   gating: tracks must not deliver samples before their own anchor
+---------------------------------------------------------------------*/

// tracks anchor in different fragments; a track must not deliver samples
// from fragments that precede its anchor, then delivers continuously
static int Case_LateAnchorGating() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(200, NULL)));

        // expected delivery in storage order: track 1 from frag B on,
        // track 2 only from frag C on (its frag A samples are gated out)
        const AP4_UI32 expected_tracks[] = {1, 1, 1, 1, 2, 2, 2, 1, 1, 2};
        const AP4_UI64 expected_dts[]    = {200, 240, 280, 320, 1280, 1920, 2560, 360, 400, 3200};
        for (unsigned int i=0; i<10; i++) {
            AP4_Sample sample;
            AP4_DataBuffer data;
            AP4_UI32 track_id = 0;
            TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(sample, data, track_id)));
            TASK_CHECK(track_id == expected_tracks[i]);
            TASK_CHECK(sample.GetDts() == expected_dts[i]);
        }
        {
            AP4_Sample sample;
            AP4_DataBuffer data;
            AP4_UI32 track_id = 0;
            TASK_CHECK(reader.ReadNextSample(sample, data, track_id) == AP4_ERROR_EOS);
        }
    }
    stream->Release();
    return 0;
}

// from the anchor on, every sample is delivered (not only the indexed
// random access points), and the decode time of the fragment that follows
// the anchor continues from the anchor fragment even without a tfdt
static int Case_ContinuousAfterAnchor() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(120, NULL)));

        // track 1: dts 120, 160 from frag A, then 200, 240 (frag B) and
        // 280, 320 (frag C) from their tfdts, then 360, 400 from frag D,
        // which has no tfdt: the decode time continues, and the prefix of
        // frag A skipped by the seek still counts towards it
        const AP4_UI64 expected_dts[] = {120, 160, 200, 240, 280, 320, 360, 400};
        for (unsigned int i=0; i<8; i++) {
            AP4_Sample sample;
            AP4_DataBuffer data;
            TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
            TASK_CHECK(sample.GetDts() == expected_dts[i]);
            TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][3+i]));
        }
        {
            AP4_Sample sample;
            AP4_DataBuffer data;
            TASK_CHECK(reader.ReadNextSample(T1_ID, sample, data) == AP4_ERROR_EOS);
        }
    }
    stream->Release();
    return 0;
}

// an anchor in the middle of a fragment: the skipped prefix still belongs
// to the decode time of that fragment, so the clock of the following
// fragments (which omit the tfdt) must not start early
static int Case_SkippedPrefixClock() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        // anchor at frag B, run 1, sample 2: one sample (dts 200) is skipped
        AP4_UI32 actual = 0;
        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(240, &actual)));
        TASK_CHECK(actual == 160); // track 2 anchored at 1281/8000 Hz

        // dts 240 from frag B, 280, 320 from frag C (tfdt), then 360, 400
        // from frag D (no tfdt): only correct if the skipped sample of
        // frag B is accounted for
        const AP4_UI64 expected_dts[] = {240, 280, 320, 360, 400};
        for (unsigned int i=0; i<5; i++) {
            AP4_Sample sample;
            AP4_DataBuffer data;
            TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
            TASK_CHECK(sample.GetDts() == expected_dts[i]);
            TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][6+i]));
        }
    }
    stream->Release();
    return 0;
}

// after reading some samples, seeking again (backwards or forwards) must
// not let previously queued samples resurface
static int Case_ReseekFlushesQueues() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(120, NULL)));
        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(sample.GetDts() == 120);
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(sample.GetDts() == 160);
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T2_ID, sample, data)));
        TASK_CHECK(sample.GetDts() == 0);

        // seek backwards: the next samples must come from the new position,
        // not from the queues filled by the previous position
        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(0, NULL)));
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][0]));
        TASK_CHECK(sample.GetDts() == 0);
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T2_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T2_ID][0]));

        // seek forwards again
        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(280, NULL)));
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][7]));
        TASK_CHECK(sample.GetDts() == 280);
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T2_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T2_ID][2]));
        TASK_CHECK(sample.GetDts() == 1280);
    }
    stream->Release();
    return 0;
}

// the per-track ReadNextSample, the any-track ReadNextSample and
// GetNextSample all consume the same positioning state
static int Case_SharedPositionState() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(120, NULL)));

        AP4_Sample sample;
        AP4_DataBuffer data;
        AP4_UI32 track_id = 0;

        // storage order after the seek: (1,120) (1,160) (2,0) (2,640) (1,200) ...
        TASK_CHECK(AP4_SUCCEEDED(reader.GetNextSample(sample, track_id)));
        TASK_CHECK(track_id == T1_ID && sample.GetDts() == 120);

        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T2_ID, sample, data)));
        TASK_CHECK(sample.GetDts() == 0);

        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(sample, data, track_id)));
        TASK_CHECK(track_id == T1_ID && sample.GetDts() == 160);

        TASK_CHECK(AP4_SUCCEEDED(reader.GetNextSample(sample, track_id)));
        TASK_CHECK(track_id == T2_ID && sample.GetDts() == 640);

        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][5]));
        TASK_CHECK(sample.GetDts() == 200);
    }
    stream->Release();
    return 0;
}

/*----------------------------------------------------------------------
|   compat: pre-existing behavior and error handling
+---------------------------------------------------------------------*/

// sequential reading without any seek is unchanged
static int Case_SequentialReading() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildStandardFile();
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        const AP4_UI32 expected_tracks[] = {1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 2, 2, 1, 1, 2};
        const AP4_UI64 expected_dts[]    = {0, 40, 80, 120, 160, 0, 640, 200, 240, 280, 320, 1280, 1920, 2560, 360, 400, 3200};
        for (unsigned int i=0; i<17; i++) {
            AP4_Sample sample;
            AP4_DataBuffer data;
            AP4_UI32 track_id = 0;
            TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(sample, data, track_id)));
            TASK_CHECK(track_id == expected_tracks[i]);
            TASK_CHECK(sample.GetDts() == expected_dts[i]);
            TASK_CHECK(CheckSample(sample, &data, file->storage[i]));
            TASK_CHECK(sample.GetDescriptionIndex() == 0);
        }
        AP4_Sample sample;
        AP4_UI32 track_id = 0;
        TASK_CHECK(reader.GetNextSample(sample, track_id) == AP4_ERROR_EOS);
    }
    stream->Release();
    return 0;
}

// without an mfra, seeking reports that it is not supported and sequential
// reading keeps working
static int Case_NoMfra() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildFile(StandardFrags(), StandardTfra1(), StandardTfra2(), true, false);
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));

        AP4_UI32 actual = 12345;
        TASK_CHECK(reader.SeekTo(100, &actual) == AP4_ERROR_NOT_SUPPORTED);

        AP4_Sample sample;
        AP4_DataBuffer data;
        AP4_UI32 track_id = 0;
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(sample, data, track_id)));
        TASK_CHECK(track_id == T1_ID && sample.GetDts() == 0);
        TASK_CHECK(CheckSample(sample, &data, file->storage[0]));
    }
    stream->Release();
    return 0;
}

// entries with coordinates that are zero, out of range, or that point to
// another track's traf must be rejected
static int Case_BadCoordinates() {
    const struct {
        AP4_UI64 time;
        int      frag;
        AP4_UI32 traf;
        AP4_UI32 trun;
        AP4_UI32 sample;
    } bad_entries[] = {
        {120,  0, 0, 2, 2}, // traf number 0
        {120,  0, 1, 0, 2}, // trun number 0
        {120,  0, 1, 2, 0}, // sample number 0
        {120,  0, 3, 1, 1}, // traf number out of range
        {120,  0, 1, 5, 1}, // trun number out of range
        {120,  0, 1, 2, 9}, // sample number out of range
        {120,  0, 2, 1, 1}, // traf belongs to the other track
        {120, -1, 1, 1, 1}, // moof offset out of range
    };
    for (unsigned int i=0; i<sizeof(bad_entries)/sizeof(bad_entries[0]); i++) {
        std::vector<TfraEntrySpec> tfra1;
        tfra1.push_back(TfraEntrySpec(0, 0, 1, 1, 1));
        tfra1.push_back(TfraEntrySpec(bad_entries[i].time,
                                      bad_entries[i].frag,
                                      bad_entries[i].traf,
                                      bad_entries[i].trun,
                                      bad_entries[i].sample));
        AP4_Movie* movie = CreateMovie();
        BuiltFile* file = BuildFile(StandardFrags(), tfra1, StandardTfra2(), true, true);
        RecordingStream* stream = new RecordingStream(file->stream);
        {
            AP4_LinearReader reader(*movie, stream);
            TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
            TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));
            TASK_CHECK(AP4_FAILED(reader.SeekTo(120, NULL)));
        }
        stream->Release();
    }
    return 0;
}

// a request earlier than every index entry cannot be satisfied
static int Case_NoTimePredecessor() {
    std::vector<TfraEntrySpec> tfra1;
    tfra1.push_back(TfraEntrySpec(100, 0, 1, 2, 1));
    tfra1.push_back(TfraEntrySpec(200, 1, 1, 1, 1));

    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildFile(StandardFrags(), tfra1, StandardTfra2(), true, true);
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));
        TASK_CHECK(AP4_FAILED(reader.SeekTo(50, NULL)));
    }
    stream->Release();
    return 0;
}

// a track without an index only fails the seek if it is enabled
static int Case_MissingTrackIndex() {
    AP4_Movie* movie = CreateMovie();
    BuiltFile* file = BuildFile(StandardFrags(), StandardTfra1(), StandardTfra2(), false, true);
    RecordingStream* stream = new RecordingStream(file->stream);
    {
        // both tracks enabled, but track 2 has no tfra
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T2_ID)));
        TASK_CHECK(reader.SeekTo(100, NULL) == AP4_ERROR_NOT_SUPPORTED);
    }
    {
        // only the indexed track is enabled: the seek succeeds
        AP4_LinearReader reader(*movie, stream);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(T1_ID)));
        TASK_CHECK(AP4_SUCCEEDED(reader.SeekTo(100, NULL)));
        AP4_Sample sample;
        AP4_DataBuffer data;
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(T1_ID, sample, data)));
        TASK_CHECK(CheckSample(sample, &data, file->by_track[T1_ID][0]));
        TASK_CHECK(sample.GetDts() == 0);
    }
    stream->Release();
    return 0;
}

/*----------------------------------------------------------------------
|   case registry
+---------------------------------------------------------------------*/
const struct task_case task_cases[] = {
    {"anchors", "second_run_anchor",      Case_AnchorInSecondRun},
    {"anchors", "per_track_anchors",      Case_PerTrackAnchors},
    {"anchors", "v1_index_boundary",      Case_V1IndexBoundary},
    {"anchors", "equal_time_last_entry",  Case_EqualTimeLastEntry},
    {"gating",  "late_anchor_gating",     Case_LateAnchorGating},
    {"gating",  "continuous_after_anchor",Case_ContinuousAfterAnchor},
    {"gating",  "skipped_prefix_clock",   Case_SkippedPrefixClock},
    {"gating",  "reseek_flushes_queues",  Case_ReseekFlushesQueues},
    {"gating",  "shared_position_state",  Case_SharedPositionState},
    {"compat",  "sequential_reading",     Case_SequentialReading},
    {"compat",  "no_mfra",                Case_NoMfra},
    {"compat",  "bad_coordinates",        Case_BadCoordinates},
    {"compat",  "no_time_predecessor",    Case_NoTimePredecessor},
    {"compat",  "missing_track_index",    Case_MissingTrackIndex},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);

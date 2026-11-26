/*****************************************************************
|
|    Task tests: subtitle sample description runs
|
|    These tests exercise the real mp4fragment CLI and the AP4
|    library. They do not define main(): task_support/driver.c drives
|    the registered task cases.
|
****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4.h"
#include "check.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/*----------------------------------------------------------------------
|   small helpers
+---------------------------------------------------------------------*/
static std::string
TempPath(const char* name)
{
    const char* tmp = getenv("TMPDIR");
    std::string dir = tmp ? tmp : "/tmp";
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/ap4_desc_runs_%d_%s",
                  dir.c_str(), static_cast<int>(getpid()), name);
    return std::string(path);
}

static std::string
FragmenterPath()
{
    const char* env = getenv("MP4FRAGMENT_BIN");
    if (env && env[0]) return std::string(env);
#if defined(__APPLE__)
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string p(path);
        std::string::size_type slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash)+"/mp4fragment";
    }
#endif
    return "mp4fragment";
}

// run the real mp4fragment CLI, return its process exit status
static int
RunFragmenter(const std::vector<std::string>& args)
{
    std::string cmd = "'" + FragmenterPath() + "'";
    for (unsigned int i=0; i<args.size(); i++) {
        cmd += " '";
        for (std::string::const_iterator c = args[i].begin(); c != args[i].end(); ++c) {
            if (*c == '\'') cmd += "'\\''";
            else           cmd += *c;
        }
        cmd += "'";
    }
    cmd += " >/dev/null 2>&1";
    int status = std::system(cmd.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static AP4_Result
CopyStream(AP4_ByteStream& in, AP4_ByteStream& out)
{
    AP4_LargeSize size = 0;
    AP4_Result result = in.GetSize(size);
    if (AP4_FAILED(result)) return result;
    result = in.Seek(0);
    if (AP4_FAILED(result)) return result;
    char buffer[4096];
    AP4_LargeSize remaining = size;
    while (remaining) {
        AP4_Size chunk = remaining > sizeof(buffer) ? sizeof(buffer) : static_cast<AP4_Size>(remaining);
        AP4_Size got = 0;
        result = in.ReadPartial(buffer, chunk, got);
        if (AP4_FAILED(result)) return result;
        result = out.Write(buffer, got);
        if (AP4_FAILED(result)) return result;
        remaining -= got;
    }
    return AP4_SUCCESS;
}

static AP4_Result
WriteMemoryStreamToFile(AP4_MemoryByteStream& mem, const std::string& path)
{
    AP4_ByteStream* out = NULL;
    AP4_Result result = AP4_FileByteStream::Create(path.c_str(),
                                                   AP4_FileByteStream::STREAM_MODE_WRITE,
                                                   out);
    if (AP4_FAILED(result)) return result;
    result = CopyStream(mem, *out);
    out->Release();
    return result;
}

static AP4_Result
LoadTopLevelAtoms(const std::string& path, AP4_AtomParent& top_level)
{
    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(path.c_str(),
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) return result;
    AP4_DefaultAtomFactory factory;
    AP4_Atom* atom = NULL;
    while (factory.CreateAtomFromStream(*input, atom) == AP4_SUCCESS) {
        top_level.AddChild(atom);
    }
    input->Release();
    return AP4_SUCCESS;
}

static AP4_ContainerAtom*
FindTopLevel(AP4_AtomParent& top_level, AP4_Atom::Type type, AP4_Ordinal index = 0)
{
    AP4_Ordinal seen = 0;
    for (AP4_List<AP4_Atom>::Item* item = top_level.GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        if (item->GetData()->GetType() == type) {
            if (seen == index) {
                return AP4_DYNAMIC_CAST(AP4_ContainerAtom, item->GetData());
            }
            ++seen;
        }
    }
    return NULL;
}

static AP4_Atom*
FindAtom(AP4_AtomParent& parent, AP4_Atom::Type type, AP4_Ordinal index = 0)
{
    return parent.GetChild(type, index);
}

struct TrafSnapshot {
    AP4_UI32             m_TrackId;
    AP4_UI32             m_DescIndex;       // 1-based value stored in tfhd
    bool                 m_HasTfdt;
    AP4_UI64             m_Tfdt;
    bool                 m_HasBaseDataOffset;
    AP4_UI64             m_BaseDataOffset;
    AP4_UI32             m_TrunCount;
    AP4_Array<AP4_UI32>  m_TrunSampleCounts;
    AP4_Array<AP4_SI32>  m_TrunDataOffsets;
};

static void
SnapshotTraf(AP4_ContainerAtom* traf, TrafSnapshot& snap)
{
    AP4_TfhdAtom* tfhd = AP4_DYNAMIC_CAST(AP4_TfhdAtom, traf->GetChild(AP4_ATOM_TYPE_TFHD));
    snap.m_TrackId = tfhd->GetTrackId();
    snap.m_DescIndex = (tfhd->GetFlags() & AP4_TFHD_FLAG_SAMPLE_DESCRIPTION_INDEX_PRESENT) ?
                       tfhd->GetSampleDescriptionIndex() : 1;
    snap.m_HasBaseDataOffset = (tfhd->GetFlags() & AP4_TFHD_FLAG_BASE_DATA_OFFSET_PRESENT) != 0;
    snap.m_BaseDataOffset = tfhd->GetBaseDataOffset();

    AP4_TfdtAtom* tfdt = AP4_DYNAMIC_CAST(AP4_TfdtAtom, traf->GetChild(AP4_ATOM_TYPE_TFDT));
    snap.m_HasTfdt = tfdt != NULL;
    snap.m_Tfdt = tfdt ? tfdt->GetBaseMediaDecodeTime() : 0;

    snap.m_TrunCount = 0;
    for (AP4_List<AP4_Atom>::Item* item = traf->GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_TrunAtom* trun = AP4_DYNAMIC_CAST(AP4_TrunAtom, item->GetData());
        if (trun == NULL) continue;
        snap.m_TrunSampleCounts.Append(trun->GetEntries().ItemCount());
        snap.m_TrunDataOffsets.Append(trun->GetDataOffset());
        ++snap.m_TrunCount;
    }
}

static unsigned int
CountTopLevel(AP4_AtomParent& top_level, AP4_Atom::Type type)
{
    unsigned int count = 0;
    for (AP4_List<AP4_Atom>::Item* item = top_level.GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        if (item->GetData()->GetType() == type) ++count;
    }
    return count;
}

/*----------------------------------------------------------------------
|   raw box scanner: records the real on-disk box boundaries, instead
|   of relying only on the parsed atom tree
+---------------------------------------------------------------------*/
struct BoxRecord {
    AP4_Atom::Type m_Type;
    AP4_Position   m_HeaderOffset; // absolute offset of the size field
    AP4_UI64       m_Size;         // full box size including the header
    unsigned int   m_HeaderSize;
    unsigned int   m_Depth;
};

static bool
IsContainerBox(AP4_Atom::Type type)
{
    return type == AP4_ATOM_TYPE_MOOV || type == AP4_ATOM_TYPE_TRAK ||
           type == AP4_ATOM_TYPE_MDIA || type == AP4_ATOM_TYPE_MINF ||
           type == AP4_ATOM_TYPE_STBL || type == AP4_ATOM_TYPE_DINF ||
           type == AP4_ATOM_TYPE_EDTS || type == AP4_ATOM_TYPE_UDTA ||
           type == AP4_ATOM_TYPE_MVEX || type == AP4_ATOM_TYPE_MOOF ||
           type == AP4_ATOM_TYPE_TRAF || type == AP4_ATOM_TYPE_MFRA ||
           type == AP4_ATOM_TYPE_SCHI || type == AP4_ATOM_TYPE_SINF ||
           type == AP4_ATOM_TYPE_HNTI || type == AP4_ATOM_TYPE_TREF ||
           type == AP4_ATOM_TYPE_MDRI || type == AP4_ATOM_TYPE_WAVE ||
           type == AP4_ATOM_TYPE_ILST || type == AP4_ATOM_TYPE_STSD;
}

static AP4_Result
ScanBoxes(AP4_ByteStream& stream, AP4_Position start, AP4_Position end,
          unsigned int depth, std::vector<BoxRecord>& records)
{
    AP4_Position pos = start;
    while (pos + AP4_ATOM_HEADER_SIZE <= end) {
        AP4_Result result = stream.Seek(pos);
        if (AP4_FAILED(result)) return result;
        AP4_UI32 size32 = 0, type = 0;
        result = stream.ReadUI32(size32);
        if (AP4_FAILED(result)) return result;
        result = stream.ReadUI32(type);
        if (AP4_FAILED(result)) return result;

        unsigned int header_size = AP4_ATOM_HEADER_SIZE;
        AP4_UI64 size = size32;
        if (size32 == 1) {
            AP4_UI64 size64 = 0;
            result = stream.ReadUI64(size64);
            if (AP4_FAILED(result)) return result;
            size = size64;
            header_size = AP4_ATOM_HEADER_SIZE_64;
        } else if (size32 == 0) {
            size = end - pos; // box extends to end of stream
        }
        if (size < header_size || pos + size > end) return AP4_ERROR_INVALID_FORMAT;

        BoxRecord record;
        record.m_Type = type;
        record.m_HeaderOffset = pos;
        record.m_Size = size;
        record.m_HeaderSize = header_size;
        record.m_Depth = depth;
        records.push_back(record);

        if (IsContainerBox(type)) {
            result = ScanBoxes(stream, pos + header_size, pos + size, depth + 1, records);
            if (AP4_FAILED(result)) return result;
        }
        pos += size;
    }
    return AP4_SUCCESS;
}

static AP4_Result
ScanFileBoxes(const std::string& path, std::vector<BoxRecord>& records)
{
    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(path.c_str(),
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) return result;
    AP4_LargeSize file_size = 0;
    input->GetSize(file_size);
    result = ScanBoxes(*input, 0, file_size, 0, records);
    input->Release();
    return result;
}

static const BoxRecord*
FindBox(const std::vector<BoxRecord>& records, AP4_Atom::Type type, AP4_Ordinal ordinal = 0)
{
    AP4_Ordinal seen = 0;
    for (unsigned int i=0; i<records.size(); i++) {
        if (records[i].m_Type == type) {
            if (seen == ordinal) return &records[i];
            ++seen;
        }
    }
    return NULL;
}

/*----------------------------------------------------------------------
|   input builders
+---------------------------------------------------------------------*/
// build the canonical 4-sample, 2-stpp-description input
static AP4_Result
BuildFixtureInput(const std::string& path,
                  unsigned int       timescale = 1000,
                  unsigned int       illegal_desc = 0xFFFFFFFF,
                  bool               non_stpp = false)
{
    const std::string payloads[] = {"<tt>A</tt>", "<tt>BB</tt>", "<tt>CCC</tt>", "<tt>D</tt>"};
    std::string bytes;
    for (unsigned int i=0; i<4; i++) bytes += payloads[i];

    AP4_MemoryByteStream* stream =
        new AP4_MemoryByteStream(reinterpret_cast<const AP4_UI08*>(bytes.data()),
                                 static_cast<AP4_Size>(bytes.size()));
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    AP4_Result result = table->AddSampleDescription(
        new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP, "urn:ttml:A", "schema-A", ""), true);
    if (AP4_FAILED(result)) return result;
    if (non_stpp) {
        result = table->AddSampleDescription(
            new AP4_GenericAudioSampleDescription(AP4_SAMPLE_FORMAT_MP4A, 44100<<16, 16, 2, NULL), true);
    } else {
        result = table->AddSampleDescription(
            new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP, "urn:ttml:B", "schema-B", ""), true);
    }
    if (AP4_FAILED(result)) return result;

    const unsigned int descriptions[] = {0, 0, 1, 0};
    AP4_Position offset = 0;
    for (unsigned int i=0; i<4; i++) {
        unsigned int desc = descriptions[i];
        if (illegal_desc != 0xFFFFFFFF && i == 2) desc = illegal_desc;
        result = table->AddSample(*stream, offset, payloads[i].size(), 1000, desc, 1000*i, 0, true);
        if (AP4_FAILED(result)) return result;
        offset += payloads[i].size();
    }

    AP4_Movie* movie = new AP4_Movie(timescale, 4000, 0, 0);
    result = movie->AddTrack(new AP4_Track(AP4_Track::TYPE_SUBTITLES, table, 7,
                                           timescale, 4000, timescale, 4000, "eng", 0, 0));
    if (AP4_FAILED(result)) return result;
    AP4_File file(movie);
    AP4_UI32 brands[] = {AP4_FILE_BRAND_ISOM};
    result = file.SetFileType(AP4_FILE_BRAND_ISOM, 0, brands, 1);
    if (AP4_FAILED(result)) return result;

    AP4_ByteStream* output = NULL;
    result = AP4_FileByteStream::Create(path.c_str(), AP4_FileByteStream::STREAM_MODE_WRITE, output);
    if (AP4_FAILED(result)) return result;
    result = AP4_FileWriter::Write(file, *output);
    output->Release();
    stream->Release();
    return result;
}

// build an A/B/A input with `sample_count` samples alternating descriptions
static AP4_Result
BuildAbaInput(const std::string& path, unsigned int sample_count)
{
    AP4_MemoryByteStream* stream = new AP4_MemoryByteStream();
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(
        new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP, "urn:ttml:A", "schema-A", ""), true);
    table->AddSampleDescription(
        new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP, "urn:ttml:B", "schema-B", ""), true);

    AP4_Position offset = 0;
    for (unsigned int i=0; i<sample_count; i++) {
        char payload[32];
        unsigned int payload_size = 1 + (i % 5); // variable length, always positive
        std::memset(payload, 'a'+(i%26), payload_size);
        stream->Write(payload, payload_size);
        unsigned int desc = (i % 3 == 1) ? 1u : 0u; // 0,1,0,0,1,0,...
        AP4_Result result = table->AddSample(*stream, offset, payload_size, 1000, desc, 1000*i, 0, true);
        if (AP4_FAILED(result)) return result;
        offset += payload_size;
    }

    AP4_Movie* movie = new AP4_Movie(1000, sample_count*1000, 0, 0);
    AP4_Result result = movie->AddTrack(
        new AP4_Track(AP4_Track::TYPE_SUBTITLES, table, 7, 1000,
                      sample_count*1000, 1000, sample_count*1000, "eng", 0, 0));
    if (AP4_FAILED(result)) return result;
    AP4_File file(movie);
    AP4_UI32 brands[] = {AP4_FILE_BRAND_ISOM};
    file.SetFileType(AP4_FILE_BRAND_ISOM, 0, brands, 1);

    AP4_ByteStream* output = NULL;
    result = AP4_FileByteStream::Create(path.c_str(), AP4_FileByteStream::STREAM_MODE_WRITE, output);
    if (AP4_FAILED(result)) return result;
    result = AP4_FileWriter::Write(file, *output);
    output->Release();
    stream->Release();
    return result;
}

struct HandBuiltSample {
    unsigned int m_TrackId;
    unsigned int m_DescIndex;
    std::string  m_Payload;
    AP4_UI32     m_Duration;
};

// independently construct a fragmented file with, inside one moof:
// several same-track traf atoms, another track interleaved, multiple
// trun atoms, omitted tfdt in a later traf and in the next moof,
// variable-length payloads, an explicit base_data_offset and a
// non-zero stream offset (leading 'free' box).
static AP4_Result
BuildHandBuiltFile(const std::string& path)
{
    const AP4_UI32 SUB_ID = 1;
    const AP4_UI32 AUD_ID = 3;

    // all samples, in storage (payload) order
    std::vector<HandBuiltSample> samples;
    const char* sub_payloads[] = {"<s0>", "<s1-long>", "<s2>", "<s3-very-long>", "<s4>", "<s5>"};
    const unsigned int sub_descs[] = {0, 1, 1, 0, 0, 1};
    for (unsigned int i=0; i<6; i++) {
        HandBuiltSample s;
        s.m_TrackId = SUB_ID;
        s.m_DescIndex = sub_descs[i];
        s.m_Payload = sub_payloads[i];
        s.m_Duration = 1000;
        samples.push_back(s);
    }
    const char* aud_payloads[] = {"A0", "A1-longer"};
    for (unsigned int i=0; i<2; i++) {
        HandBuiltSample s;
        s.m_TrackId = AUD_ID;
        s.m_DescIndex = 0;
        s.m_Payload = aud_payloads[i];
        s.m_Duration = 512;
        samples.push_back(s);
    }
    // storage order inside moof 1: sub s0,s1 (traf A), sub s2,s3 (traf B),
    // sub s4 (traf A2), audio (traf C). moof 2: sub s5 (traf D).
    std::vector<HandBuiltSample> moof1_samples;
    for (unsigned int i=0; i<5; i++) moof1_samples.push_back(samples[i]);
    moof1_samples.push_back(samples[6]);
    moof1_samples.push_back(samples[7]);
    std::vector<HandBuiltSample> moof2_samples;
    moof2_samples.push_back(samples[5]);

    AP4_MemoryByteStream* file_bytes = new AP4_MemoryByteStream();

    // leading 'free' box so every absolute offset is non-zero
    char free_payload[8] = {0};
    file_bytes->WriteUI32(16);
    file_bytes->WriteUI32(AP4_ATOM_TYPE_FREE);
    file_bytes->Write(free_payload, 8);

    // ftyp
    AP4_UI32 compat[] = {AP4_FILE_BRAND_ISOM, AP4_FILE_BRAND_ISO5};
    AP4_FtypAtom ftyp(AP4_FILE_BRAND_ISOM, 0, compat, 2);
    ftyp.Write(*file_bytes);

    // moov with two empty (fragmented) tracks and an mvex
    AP4_Movie movie(1000, 0, 0, 0);
    AP4_SyntheticSampleTable* sub_table = new AP4_SyntheticSampleTable();
    sub_table->AddSampleDescription(
        new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP, "urn:ttml:A", "schema-A", ""), false);
    sub_table->AddSampleDescription(
        new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP, "urn:ttml:B", "schema-B", ""), false);
    movie.AddTrack(new AP4_Track(AP4_Track::TYPE_SUBTITLES, sub_table, SUB_ID,
                                 1000, 6000, 1000, 6000, "eng", 0, 0));
    AP4_SyntheticSampleTable* aud_table = new AP4_SyntheticSampleTable();
    aud_table->AddSampleDescription(
        new AP4_GenericAudioSampleDescription(AP4_SAMPLE_FORMAT_MP4A, 44100<<16, 16, 2, NULL), false);
    movie.AddTrack(new AP4_Track(AP4_Track::TYPE_AUDIO, aud_table, AUD_ID,
                                 1000, 0, 44100, 1024, "eng", 0, 0));
    AP4_ContainerAtom* mvex = new AP4_ContainerAtom(AP4_ATOM_TYPE_MVEX);
    mvex->AddChild(new AP4_MehdAtom(6000));
    mvex->AddChild(new AP4_TrexAtom(SUB_ID, 1, 0, 0, 0));
    mvex->AddChild(new AP4_TrexAtom(AUD_ID, 1, 0, 0, 0));
    movie.GetMoovAtom()->AddChild(mvex);
    movie.GetMoovAtom()->Write(*file_bytes);

    // positions are measured from the absolute start of the file
    AP4_Position moof1_start = 0;
    file_bytes->Tell(moof1_start);

    // ---- moof 1 ----
    AP4_ContainerAtom moof1(AP4_ATOM_TYPE_MOOF);
    moof1.AddChild(new AP4_MfhdAtom(1));

    AP4_UI32 sub_flags = AP4_TFHD_FLAG_DEFAULT_BASE_IS_MOOF |
                         AP4_TFHD_FLAG_SAMPLE_DESCRIPTION_INDEX_PRESENT |
                         AP4_TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT |
                         AP4_TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT;
    AP4_UI32 aud_flags = sub_flags;
    AP4_UI32 trun_flags = AP4_TRUN_FLAG_DATA_OFFSET_PRESENT |
                          AP4_TRUN_FLAG_SAMPLE_SIZE_PRESENT |
                          AP4_TRUN_FLAG_SAMPLE_DURATION_PRESENT;

    // traf A: sub sample 0 (desc 0), has tfdt, default-base-is-moof
    AP4_ContainerAtom* traf_a = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
    traf_a->AddChild(new AP4_TfhdAtom(sub_flags, SUB_ID, 0, 1, 1000, 0, 0));
    traf_a->AddChild(new AP4_TfdtAtom(1, 0));
    AP4_TrunAtom* trun_a = new AP4_TrunAtom(trun_flags, 0, 0);
    AP4_Array<AP4_TrunAtom::Entry> entries_a;
    entries_a.SetItemCount(1);
    entries_a[0].sample_duration = 1000; entries_a[0].sample_size = moof1_samples[0].m_Payload.size();
    trun_a->SetEntries(entries_a);
    traf_a->AddChild(trun_a);
    moof1.AddChild(traf_a);

    // traf B: sub samples 1-2 (desc 1), no tfdt, explicit base_data_offset
    AP4_ContainerAtom* traf_b = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
    AP4_UI32 b_flags = AP4_TFHD_FLAG_BASE_DATA_OFFSET_PRESENT |
                       AP4_TFHD_FLAG_SAMPLE_DESCRIPTION_INDEX_PRESENT |
                       AP4_TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT |
                       AP4_TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT;
    AP4_TfhdAtom* tfhd_b = new AP4_TfhdAtom(b_flags, SUB_ID, 0, 2, 1000, 0, 0);
    traf_b->AddChild(tfhd_b);
    AP4_TrunAtom* trun_b = new AP4_TrunAtom(trun_flags, 0, 0);
    AP4_Array<AP4_TrunAtom::Entry> entries_b;
    entries_b.SetItemCount(2);
    entries_b[0].sample_duration = 1000; entries_b[0].sample_size = moof1_samples[1].m_Payload.size();
    entries_b[1].sample_duration = 1000; entries_b[1].sample_size = moof1_samples[2].m_Payload.size();
    trun_b->SetEntries(entries_b);
    traf_b->AddChild(trun_b);
    moof1.AddChild(traf_b);

    // traf A2: sub samples 3-4 (desc 0), no tfdt, default-base-is-moof
    AP4_ContainerAtom* traf_a2 = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
    traf_a2->AddChild(new AP4_TfhdAtom(sub_flags, SUB_ID, 0, 1, 1000, 0, 0));
    AP4_TrunAtom* trun_a2 = new AP4_TrunAtom(trun_flags, 0, 0);
    AP4_Array<AP4_TrunAtom::Entry> entries_a2;
    entries_a2.SetItemCount(2);
    entries_a2[0].sample_duration = 1000; entries_a2[0].sample_size = moof1_samples[3].m_Payload.size();
    entries_a2[1].sample_duration = 1000; entries_a2[1].sample_size = moof1_samples[4].m_Payload.size();
    trun_a2->SetEntries(entries_a2);
    traf_a2->AddChild(trun_a2);
    moof1.AddChild(traf_a2);

    // traf C: audio, has tfdt, two trun atoms
    AP4_ContainerAtom* traf_c = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
    traf_c->AddChild(new AP4_TfhdAtom(aud_flags, AUD_ID, 0, 1, 512, 0, 0));
    traf_c->AddChild(new AP4_TfdtAtom(1, 0));
    AP4_TrunAtom* trun_c1 = new AP4_TrunAtom(trun_flags, 0, 0);
    AP4_Array<AP4_TrunAtom::Entry> entries_c1;
    entries_c1.SetItemCount(1);
    entries_c1[0].sample_duration = 512; entries_c1[0].sample_size = moof1_samples[5].m_Payload.size();
    trun_c1->SetEntries(entries_c1);
    traf_c->AddChild(trun_c1);
    AP4_TrunAtom* trun_c2 = new AP4_TrunAtom(trun_flags, 0, 0);
    AP4_Array<AP4_TrunAtom::Entry> entries_c2;
    entries_c2.SetItemCount(1);
    entries_c2[0].sample_duration = 512; entries_c2[0].sample_size = moof1_samples[6].m_Payload.size();
    trun_c2->SetEntries(entries_c2);
    traf_c->AddChild(trun_c2);
    moof1.AddChild(traf_c);

    // compute payload positions and data offsets once the moof size is known
    AP4_Position mdat1_payload = moof1_start + moof1.GetSize() + AP4_ATOM_HEADER_SIZE;
    AP4_Position cursor = mdat1_payload;
    AP4_Position b_payload = 0, a2_payload = 0, c_payload = 0;
    cursor += moof1_samples[0].m_Payload.size();                 // traf A
    b_payload = cursor;                                           // traf B
    for (unsigned int i=1; i<3; i++) cursor += moof1_samples[i].m_Payload.size();
    a2_payload = cursor;                                          // traf A2
    for (unsigned int i=3; i<5; i++) cursor += moof1_samples[i].m_Payload.size();
    c_payload = cursor;                                           // traf C

    trun_a->SetDataOffset(static_cast<AP4_SI32>(mdat1_payload - moof1_start));
    tfhd_b->SetBaseDataOffset(b_payload); // absolute, no trun-relative delta
    trun_b->SetDataOffset(0);
    trun_a2->SetDataOffset(static_cast<AP4_SI32>(a2_payload - moof1_start));
    trun_c1->SetDataOffset(static_cast<AP4_SI32>(c_payload - moof1_start));
    AP4_Position c2_payload = c_payload + moof1_samples[5].m_Payload.size();
    trun_c2->SetDataOffset(static_cast<AP4_SI32>(c2_payload - moof1_start));

    AP4_UI64 mdat1_size = AP4_ATOM_HEADER_SIZE;
    for (unsigned int i=0; i<moof1_samples.size(); i++) mdat1_size += moof1_samples[i].m_Payload.size();

    moof1.Write(*file_bytes);
    file_bytes->WriteUI32(static_cast<AP4_UI32>(mdat1_size));
    file_bytes->WriteUI32(AP4_ATOM_TYPE_MDAT);
    for (unsigned int i=0; i<moof1_samples.size(); i++) {
        file_bytes->Write(moof1_samples[i].m_Payload.data(),
                         static_cast<AP4_Size>(moof1_samples[i].m_Payload.size()));
    }

    // ---- moof 2: one traf, no tfdt (continues the subtitle clock) ----
    AP4_Position moof2_start = 0;
    file_bytes->Tell(moof2_start);
    AP4_ContainerAtom moof2(AP4_ATOM_TYPE_MOOF);
    moof2.AddChild(new AP4_MfhdAtom(2));
    AP4_ContainerAtom* traf_d = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
    traf_d->AddChild(new AP4_TfhdAtom(sub_flags, SUB_ID, 0, 2, 1000, 0, 0));
    AP4_TrunAtom* trun_d = new AP4_TrunAtom(trun_flags, 0, 0);
    AP4_Array<AP4_TrunAtom::Entry> entries_d;
    entries_d.SetItemCount(1);
    entries_d[0].sample_duration = 1000; entries_d[0].sample_size = moof2_samples[0].m_Payload.size();
    trun_d->SetEntries(entries_d);
    traf_d->AddChild(trun_d);
    moof2.AddChild(traf_d);
    AP4_Position mdat2_payload = moof2_start + moof2.GetSize() + AP4_ATOM_HEADER_SIZE;
    trun_d->SetDataOffset(static_cast<AP4_SI32>(moof2.GetSize() + AP4_ATOM_HEADER_SIZE));
    AP4_UI64 mdat2_size = AP4_ATOM_HEADER_SIZE + moof2_samples[0].m_Payload.size();
    moof2.Write(*file_bytes);
    file_bytes->WriteUI32(static_cast<AP4_UI32>(mdat2_size));
    file_bytes->WriteUI32(AP4_ATOM_TYPE_MDAT);
    file_bytes->Write(moof2_samples[0].m_Payload.data(),
                     static_cast<AP4_Size>(moof2_samples[0].m_Payload.size()));

    // mfra with one tfra per track
    AP4_ContainerAtom mfra(AP4_ATOM_TYPE_MFRA);
    AP4_TfraAtom* sub_tfra = new AP4_TfraAtom(SUB_ID);
    sub_tfra->AddEntry(0, moof1_start);
    sub_tfra->AddEntry(5000, moof2_start);
    mfra.AddChild(sub_tfra);
    AP4_TfraAtom* aud_tfra = new AP4_TfraAtom(AUD_ID);
    aud_tfra->AddEntry(0, moof1_start);
    mfra.AddChild(aud_tfra);
    AP4_MfroAtom* mfro = new AP4_MfroAtom(static_cast<AP4_UI32>(mfra.GetSize()+16));
    mfra.AddChild(mfro);
    mfra.Write(*file_bytes);

    AP4_Result write_result = WriteMemoryStreamToFile(*file_bytes, path);
    file_bytes->Release();
    return write_result;
}

/*----------------------------------------------------------------------
|   reading helpers
+---------------------------------------------------------------------*/
struct ReadSample {
    AP4_UI32    m_TrackId;
    AP4_UI64    m_Dts;
    AP4_UI32    m_CtsDelta;
    AP4_UI32    m_Duration;
    AP4_Ordinal m_DescIndex;
    AP4_Size    m_Size;
    AP4_Position m_Offset;
    std::string m_Data;
    bool        m_Sync;
};

static AP4_Result
ReadAllSamples(const std::string& path, std::vector<ReadSample>& out)
{
    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(path.c_str(),
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) return result;
    {
        AP4_File file(*input, true);
        if (file.GetMovie() == NULL) { input->Release(); return AP4_ERROR_INVALID_STATE; }
        AP4_LinearReader reader(*file.GetMovie(), input);
        AP4_Array<AP4_UI32> ids;
        // enable every track
        for (AP4_List<AP4_Track>::Item* item = file.GetMovie()->GetTracks().FirstItem();
             item;
             item = item->GetNext()) {
            result = reader.EnableTrack(item->GetData()->GetId());
            if (AP4_FAILED(result)) { input->Release(); return result; }
            ids.Append(item->GetData()->GetId());
        }
        AP4_Sample sample;
        AP4_DataBuffer data;
        AP4_UI32 track_id = 0;
        for (;;) {
            result = reader.GetNextSample(sample, track_id);
            if (result == AP4_ERROR_EOS) break;
            if (AP4_FAILED(result)) { input->Release(); return result; }
            AP4_Result rd = sample.ReadData(data);
            if (AP4_FAILED(rd)) { input->Release(); return rd; }
            ReadSample rs;
            rs.m_TrackId = track_id;
            rs.m_Dts = sample.GetDts();
            rs.m_CtsDelta = sample.GetCtsDelta();
            rs.m_Duration = sample.GetDuration();
            rs.m_DescIndex = sample.GetDescriptionIndex();
            rs.m_Size = sample.GetSize();
            rs.m_Offset = sample.GetOffset();
            rs.m_Data.assign(reinterpret_cast<const char*>(data.GetData()), data.GetDataSize());
            rs.m_Sync = sample.IsSync();
            out.push_back(rs);
        }
    }
    input->Release();
    return AP4_SUCCESS;
}

static AP4_Result
ReadTrackSamples(const std::string& path, AP4_UI32 track_id, std::vector<ReadSample>& out)
{
    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(path.c_str(),
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) return result;
    {
        AP4_File file(*input, true);
        if (file.GetMovie() == NULL) { input->Release(); return AP4_ERROR_INVALID_STATE; }
        AP4_LinearReader reader(*file.GetMovie(), input);
        result = reader.EnableTrack(track_id);
        if (AP4_FAILED(result)) { input->Release(); return result; }
        AP4_Sample sample;
        AP4_DataBuffer data;
        for (;;) {
            result = reader.ReadNextSample(track_id, sample, data);
            if (result == AP4_ERROR_EOS) break;
            if (AP4_FAILED(result)) { input->Release(); return result; }
            ReadSample rs;
            rs.m_TrackId = track_id;
            rs.m_Dts = sample.GetDts();
            rs.m_CtsDelta = sample.GetCtsDelta();
            rs.m_Duration = sample.GetDuration();
            rs.m_DescIndex = sample.GetDescriptionIndex();
            rs.m_Size = sample.GetSize();
            rs.m_Offset = sample.GetOffset();
            rs.m_Data.assign(reinterpret_cast<const char*>(data.GetData()), data.GetDataSize());
            rs.m_Sync = sample.IsSync();
            out.push_back(rs);
        }
    }
    input->Release();
    return AP4_SUCCESS;
}

static AP4_Result
ReadBytesAt(const std::string& path, AP4_Position offset, AP4_Size size, std::string& out)
{
    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(path.c_str(),
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) return result;
    out.resize(size);
    result = input->Seek(offset);
    if (AP4_FAILED(result)) { input->Release(); return result; }
    AP4_Size got = 0;
    result = input->ReadPartial(&out[0], size, got);
    input->Release();
    if (AP4_FAILED(result)) return result;
    out.resize(got);
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   layout group
+---------------------------------------------------------------------*/
static int
case_cli_fixture_layout()
{
    std::string in = TempPath("fixture_in.mp4");
    std::string out = TempPath("fixture_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in)));
    std::remove(out.c_str());
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--index");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) == 0);

    AP4_AtomParent top;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(out, top)));
    TASK_CHECK(CountTopLevel(top, AP4_ATOM_TYPE_MOOF) == 1);
    TASK_CHECK(CountTopLevel(top, AP4_ATOM_TYPE_MDAT) == 1);
    AP4_ContainerAtom* moof = FindTopLevel(top, AP4_ATOM_TYPE_MOOF, 0);
    TASK_CHECK(moof != NULL);

    AP4_MfhdAtom* mfhd = AP4_DYNAMIC_CAST(AP4_MfhdAtom, moof->GetChild(AP4_ATOM_TYPE_MFHD));
    TASK_CHECK(mfhd != NULL && mfhd->GetSequenceNumber() == 1);

    // three traf atoms, all track 7, tfhd description index 1/2/1
    AP4_Array<TrafSnapshot> snaps;
    for (AP4_List<AP4_Atom>::Item* item = moof->GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_ContainerAtom* traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, item->GetData());
        if (traf == NULL) continue;
        TrafSnapshot snap;
        SnapshotTraf(traf, snap);
        snaps.Append(snap);
    }
    TASK_CHECK(snaps.ItemCount() == 3);
    const AP4_UI32 expected_desc[] = {1, 2, 1};
    const AP4_UI32 expected_counts[] = {2, 1, 1};
    const AP4_UI64 expected_tfdt[] = {0, 2000, 3000};
    for (unsigned int i=0; i<3; i++) {
        TASK_CHECK(snaps[i].m_TrackId == 7);
        TASK_CHECK(snaps[i].m_DescIndex == expected_desc[i]);
        TASK_CHECK(snaps[i].m_HasTfdt);
        TASK_CHECK(snaps[i].m_Tfdt == expected_tfdt[i]);
        TASK_CHECK(snaps[i].m_TrunCount == 1);
        TASK_CHECK(snaps[i].m_TrunSampleCounts[0] == expected_counts[i]);
    }

    // every data offset is measured from the moof start; later runs reach
    // across the variable-length payloads of the earlier runs
    std::vector<BoxRecord> boxes;
    TASK_CHECK(AP4_SUCCEEDED(ScanFileBoxes(out, boxes)));
    const BoxRecord* raw_moof = FindBox(boxes, AP4_ATOM_TYPE_MOOF, 0);
    const BoxRecord* raw_mdat = FindBox(boxes, AP4_ATOM_TYPE_MDAT, 0);
    TASK_CHECK(raw_moof != NULL && raw_mdat != NULL);
    // the mdat immediately follows the moof (one moof/mdat pair)
    TASK_CHECK(raw_mdat->m_HeaderOffset == raw_moof->m_HeaderOffset + raw_moof->m_Size);
    // the first trun data offset must equal the full moof size + mdat header
    AP4_UI64 moof_size = moof->GetSize();
    TASK_CHECK(raw_moof->m_Size == moof_size);
    TASK_CHECK(snaps[0].m_TrunDataOffsets[0] == static_cast<AP4_SI32>(moof_size + AP4_ATOM_HEADER_SIZE));
    const std::string run1_payload = std::string("<tt>A</tt>") + "<tt>BB</tt>";
    const std::string run2_payload = "<tt>CCC</tt>";
    const std::string run3_payload = "<tt>D</tt>";
    AP4_SI32 expected_second = snaps[0].m_TrunDataOffsets[0] +
                               static_cast<AP4_SI32>(run1_payload.size());
    TASK_CHECK(snaps[1].m_TrunDataOffsets[0] == expected_second);
    AP4_SI32 expected_third = expected_second + static_cast<AP4_SI32>(run2_payload.size());
    TASK_CHECK(snaps[2].m_TrunDataOffsets[0] == expected_third);
    // the last run ends exactly at the mdat payload boundary
    TASK_CHECK(static_cast<AP4_UI64>(snaps[2].m_TrunDataOffsets[0]) + run3_payload.size() +
               raw_moof->m_HeaderOffset ==
               raw_mdat->m_HeaderOffset + raw_mdat->m_Size);

    // sidx covers the single real moof/mdat pair
    AP4_SidxAtom* sidx = AP4_DYNAMIC_CAST(AP4_SidxAtom, top.GetChild(AP4_ATOM_TYPE_SIDX));
    TASK_CHECK(sidx != NULL);
    TASK_CHECK(sidx->GetReferences().ItemCount() == 1);
    TASK_CHECK(sidx->GetReferences()[0].m_ReferencedSize ==
               static_cast<AP4_UI32>(moof->GetSize() + raw_mdat->m_Size));
    return 0;
}

static int
case_hand_built_layout()
{
    std::string path = TempPath("handbuilt.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildHandBuiltFile(path)));

    AP4_AtomParent top;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(path, top)));
    TASK_CHECK(CountTopLevel(top, AP4_ATOM_TYPE_MOOF) == 2);
    TASK_CHECK(CountTopLevel(top, AP4_ATOM_TYPE_MDAT) == 2);

    // a leading free box guarantees a non-zero absolute stream offset
    std::vector<BoxRecord> boxes;
    TASK_CHECK(AP4_SUCCEEDED(ScanFileBoxes(path, boxes)));
    const BoxRecord* raw_free = FindBox(boxes, AP4_ATOM_TYPE_FREE, 0);
    TASK_CHECK(raw_free != NULL);
    TASK_CHECK(raw_free->m_HeaderOffset == 0);

    const BoxRecord* raw_moof1 = FindBox(boxes, AP4_ATOM_TYPE_MOOF, 0);
    const BoxRecord* raw_moof2 = FindBox(boxes, AP4_ATOM_TYPE_MOOF, 1);
    TASK_CHECK(raw_moof1 != NULL && raw_moof2 != NULL);
    AP4_Position moof1_start = raw_moof1->m_HeaderOffset;
    AP4_Position moof2_start = raw_moof2->m_HeaderOffset;
    TASK_CHECK(moof1_start > 0);

    AP4_ContainerAtom* moof1 = FindTopLevel(top, AP4_ATOM_TYPE_MOOF, 0);
    AP4_ContainerAtom* moof2 = FindTopLevel(top, AP4_ATOM_TYPE_MOOF, 1);
    TASK_CHECK(moof1 != NULL && moof2 != NULL);

    AP4_Array<TrafSnapshot> snaps;
    for (AP4_List<AP4_Atom>::Item* item = moof1->GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_ContainerAtom* traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, item->GetData());
        if (traf == NULL) continue;
        TrafSnapshot snap;
        SnapshotTraf(traf, snap);
        snaps.Append(snap);
    }
    TASK_CHECK(snaps.ItemCount() == 4);
    const AP4_UI32 expected_tracks[] = {1, 1, 1, 3};
    const AP4_UI32 expected_trun_counts[] = {1, 1, 1, 2};
    const bool      expected_tfdt[] = {true, false, false, true};
    for (unsigned int i=0; i<4; i++) {
        TASK_CHECK(snaps[i].m_TrackId == expected_tracks[i]);
        TASK_CHECK(snaps[i].m_TrunCount == expected_trun_counts[i]);
        TASK_CHECK(snaps[i].m_HasTfdt == expected_tfdt[i]);
    }
    // traf B uses an explicit absolute base_data_offset
    TASK_CHECK(snaps[1].m_HasBaseDataOffset);
    TASK_CHECK(snaps[1].m_BaseDataOffset > moof1_start);
    TASK_CHECK(snaps[1].m_TrunDataOffsets[0] == 0);
    // the others use default-base-is-moof relative offsets
    TASK_CHECK(!snaps[0].m_HasBaseDataOffset);
    TASK_CHECK(!snaps[2].m_HasBaseDataOffset);
    TASK_CHECK(!snaps[3].m_HasBaseDataOffset);
    TASK_CHECK(snaps[0].m_TrunDataOffsets[0] ==
               static_cast<AP4_SI32>(raw_moof1->m_Size + AP4_ATOM_HEADER_SIZE));

    // moof 2 has a single traf without tfdt
    AP4_Array<TrafSnapshot> snaps2;
    for (AP4_List<AP4_Atom>::Item* item = moof2->GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_ContainerAtom* traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, item->GetData());
        if (traf == NULL) continue;
        TrafSnapshot snap;
        SnapshotTraf(traf, snap);
        snaps2.Append(snap);
    }
    TASK_CHECK(snaps2.ItemCount() == 1);
    TASK_CHECK(snaps2[0].m_TrackId == 1);
    TASK_CHECK(!snaps2[0].m_HasTfdt);
    TASK_CHECK(snaps2[0].m_TrunSampleCounts[0] == 1);

    // tfra entries point at moof boundaries, never at an inner traf
    AP4_ContainerAtom* mfra = FindTopLevel(top, AP4_ATOM_TYPE_MFRA, 0);
    TASK_CHECK(mfra != NULL);
    AP4_TfraAtom* sub_tfra = AP4_DYNAMIC_CAST(AP4_TfraAtom, mfra->GetChild(AP4_ATOM_TYPE_TFRA, 0));
    TASK_CHECK(sub_tfra != NULL);
    TASK_CHECK(sub_tfra->GetEntries().ItemCount() == 2);
    TASK_CHECK(sub_tfra->GetEntries()[0].m_MoofOffset == moof1_start);
    TASK_CHECK(sub_tfra->GetEntries()[1].m_MoofOffset == moof2_start);
    TASK_CHECK(sub_tfra->GetEntries()[0].m_Time == 0);
    TASK_CHECK(sub_tfra->GetEntries()[1].m_Time == 5000);
    return 0;
}

static int
case_aba_across_fragments()
{
    std::string in = TempPath("aba_in.mp4");
    std::string out = TempPath("aba_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildAbaInput(in, 9)));
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("2500");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) == 0);

    AP4_AtomParent top;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(out, top)));
    unsigned int moof_count = CountTopLevel(top, AP4_ATOM_TYPE_MOOF);
    TASK_CHECK(moof_count >= 2); // A/B/A must span more than one fragment

    std::vector<ReadSample> samples;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(out, 7, samples)));
    TASK_CHECK(samples.size() == 9);
    const unsigned int expected_desc[] = {0, 1, 0, 0, 1, 0, 0, 1, 0};
    for (unsigned int i=0; i<9; i++) {
        TASK_CHECK(samples[i].m_DescIndex == expected_desc[i]);
        TASK_CHECK(samples[i].m_Dts == i*1000);
        TASK_CHECK(samples[i].m_Duration == 1000);
        TASK_CHECK(samples[i].m_Sync);
    }
    return 0;
}

static int
case_timescale_conversion()
{
    std::string in = TempPath("ts_in.mp4");
    std::string out = TempPath("ts_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in, 1000)));
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--timescale");
    args.push_back("48000");
    args.push_back("--index");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) == 0);

    AP4_AtomParent top;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(out, top)));
    AP4_ContainerAtom* moof = FindTopLevel(top, AP4_ATOM_TYPE_MOOF, 0);
    TASK_CHECK(moof != NULL);
    AP4_Array<TrafSnapshot> snaps;
    for (AP4_List<AP4_Atom>::Item* item = moof->GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_ContainerAtom* traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, item->GetData());
        if (traf == NULL) continue;
        TrafSnapshot snap;
        SnapshotTraf(traf, snap);
        snaps.Append(snap);
    }
    TASK_CHECK(snaps.ItemCount() == 3);
    // 1000 -> 48000: tfdt 0/2000/3000 become 0/96000/144000
    TASK_CHECK(snaps[0].m_Tfdt == 0);
    TASK_CHECK(snaps[1].m_Tfdt == 96000);
    TASK_CHECK(snaps[2].m_Tfdt == 144000);

    AP4_SidxAtom* sidx = AP4_DYNAMIC_CAST(AP4_SidxAtom, FindAtom(top, AP4_ATOM_TYPE_SIDX));
    TASK_CHECK(sidx != NULL);
    TASK_CHECK(sidx->GetTimeScale() == 48000);
    TASK_CHECK(sidx->GetReferences()[0].m_SubsegmentDuration == 48000*4);

    std::vector<ReadSample> samples;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(out, 7, samples)));
    TASK_CHECK(samples.size() == 4);
    for (unsigned int i=0; i<4; i++) {
        TASK_CHECK(samples[i].m_Dts == i*48000);
        TASK_CHECK(samples[i].m_Duration == 48000);
    }
    return 0;
}

static int
case_index_ranges()
{
    std::string in = TempPath("idx_in.mp4");
    std::string out = TempPath("idx_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildAbaInput(in, 9)));
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("2500");
    args.push_back("--index");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) == 0);

    AP4_AtomParent top;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(out, top)));
    AP4_SidxAtom* sidx = AP4_DYNAMIC_CAST(AP4_SidxAtom, FindAtom(top, AP4_ATOM_TYPE_SIDX));
    TASK_CHECK(sidx != NULL);
    unsigned int moof_count = CountTopLevel(top, AP4_ATOM_TYPE_MOOF);
    TASK_CHECK(sidx->GetReferences().ItemCount() == moof_count);

    // each sidx reference covers the real byte range of one moof/mdat pair
    std::vector<BoxRecord> range_boxes;
    TASK_CHECK(AP4_SUCCEEDED(ScanFileBoxes(out, range_boxes)));
    AP4_UI64 total = 0;
    for (unsigned int i=0; i<moof_count; i++) {
        const BoxRecord* raw_moof = FindBox(range_boxes, AP4_ATOM_TYPE_MOOF, i);
        const BoxRecord* raw_mdat = FindBox(range_boxes, AP4_ATOM_TYPE_MDAT, i);
        TASK_CHECK(raw_moof != NULL && raw_mdat != NULL);
        AP4_UI32 expected = static_cast<AP4_UI32>(raw_moof->m_Size + raw_mdat->m_Size);
        TASK_CHECK(sidx->GetReferences()[i].m_ReferencedSize == expected);
        total += expected;
    }
    // the sum of sidx ranges equals all the fragment bytes (trafs are not
    // counted as standalone fragments)
    AP4_UI64 sidx_total = 0;
    for (unsigned int i=0; i<sidx->GetReferences().ItemCount(); i++) {
        sidx_total += sidx->GetReferences()[i].m_ReferencedSize;
    }
    TASK_CHECK(sidx_total == total);

    // tfra still reports one entry per moof
    AP4_ContainerAtom* mfra = FindTopLevel(top, AP4_ATOM_TYPE_MFRA, 0);
    TASK_CHECK(mfra != NULL);
    AP4_TfraAtom* tfra = AP4_DYNAMIC_CAST(AP4_TfraAtom, mfra->GetChild(AP4_ATOM_TYPE_TFRA, 0));
    TASK_CHECK(tfra != NULL);
    TASK_CHECK(tfra->GetEntries().ItemCount() == moof_count);
    return 0;
}

static int
case_single_description_legacy()
{
    // single description input: option enabled still yields one traf
    std::string in1 = TempPath("single_in.mp4");
    std::string out1 = TempPath("single_out.mp4");
    {
        AP4_MemoryByteStream* stream = new AP4_MemoryByteStream();
        AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
        table->AddSampleDescription(
            new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP, "urn:ttml:A", "schema-A", ""), true);
        AP4_Position offset = 0;
        for (unsigned int i=0; i<4; i++) {
            std::string p = "payload";
            p.push_back(char('0'+i));
            stream->Write(p.data(), p.size());
            table->AddSample(*stream, offset, p.size(), 1000, 0, 1000*i, 0, true);
            offset += p.size();
        }
        AP4_Movie* movie = new AP4_Movie(1000, 4000, 0, 0);
        movie->AddTrack(new AP4_Track(AP4_Track::TYPE_SUBTITLES, table, 7, 1000, 4000, 1000, 4000, "eng", 0, 0));
        AP4_File file(movie);
        AP4_UI32 brands[] = {AP4_FILE_BRAND_ISOM};
        file.SetFileType(AP4_FILE_BRAND_ISOM, 0, brands, 1);
        AP4_ByteStream* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(in1.c_str(), AP4_FileByteStream::STREAM_MODE_WRITE, out)));
        TASK_CHECK(AP4_SUCCEEDED(AP4_FileWriter::Write(file, *out)));
        out->Release();
        stream->Release();
    }
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--subtitle-description-runs");
    args.push_back(in1);
    args.push_back(out1);
    TASK_CHECK(RunFragmenter(args) == 0);
    AP4_AtomParent top;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(out1, top)));
    AP4_ContainerAtom* moof = FindTopLevel(top, AP4_ATOM_TYPE_MOOF, 0);
    TASK_CHECK(moof != NULL);
    unsigned int traf_count = 0;
    for (AP4_List<AP4_Atom>::Item* item = moof->GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        if (item->GetData()->GetType() == AP4_ATOM_TYPE_TRAF) ++traf_count;
    }
    TASK_CHECK(traf_count == 1); // no empty traf

    // option disabled: unchanged legacy single-traf behavior
    std::string in2 = TempPath("legacy_in.mp4");
    std::string out2 = TempPath("legacy_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in2)));
    std::vector<std::string> args2;
    args2.push_back("--quiet");
    args2.push_back("--fragment-duration");
    args2.push_back("4000");
    args2.push_back(in2);
    args2.push_back(out2);
    TASK_CHECK(RunFragmenter(args2) == 0);
    AP4_AtomParent top2;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(out2, top2)));
    AP4_ContainerAtom* moof2 = FindTopLevel(top2, AP4_ATOM_TYPE_MOOF, 0);
    TASK_CHECK(moof2 != NULL);
    TrafSnapshot snap;
    AP4_ContainerAtom* only_traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, moof2->GetChild(AP4_ATOM_TYPE_TRAF));
    TASK_CHECK(only_traf != NULL);
    SnapshotTraf(only_traf, snap);
    TASK_CHECK(snap.m_TrunSampleCounts[0] == 4);
    TASK_CHECK(snap.m_DescIndex == 1);
    return 0;
}

/*----------------------------------------------------------------------
|   reader group
+---------------------------------------------------------------------*/
static int
case_reader_fixture()
{
    std::string in = TempPath("rf_in.mp4");
    std::string out = TempPath("rf_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in)));
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) == 0);

    std::vector<ReadSample> samples;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(out, 7, samples)));
    TASK_CHECK(samples.size() == 4);
    const unsigned int expected_desc[] = {0, 0, 1, 0};
    const std::string  expected_data[] = {"<tt>A</tt>", "<tt>BB</tt>", "<tt>CCC</tt>", "<tt>D</tt>"};
    for (unsigned int i=0; i<4; i++) {
        TASK_CHECK(samples[i].m_DescIndex == expected_desc[i]);
        TASK_CHECK(samples[i].m_Dts == i*1000);
        TASK_CHECK(samples[i].m_CtsDelta == 0);
        TASK_CHECK(samples[i].m_Duration == 1000);
        TASK_CHECK(samples[i].m_Data == expected_data[i]);
        TASK_CHECK(samples[i].m_Size == expected_data[i].size());
        TASK_CHECK(samples[i].m_Sync);
        // the stored offset really points at this payload in the file
        std::string raw;
        TASK_CHECK(AP4_SUCCEEDED(ReadBytesAt(out, samples[i].m_Offset, samples[i].m_Size, raw)));
        TASK_CHECK(raw == expected_data[i]);
    }
    // offsets are contiguous and strictly increasing across the traf runs
    for (unsigned int i=1; i<4; i++) {
        TASK_CHECK(samples[i].m_Offset == samples[i-1].m_Offset + samples[i-1].m_Size);
    }

    // the per-track reader reports EOS after the last sample
    AP4_ByteStream* input = NULL;
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(out.c_str(), AP4_FileByteStream::STREAM_MODE_READ, input)));
    {
        AP4_File file(*input, true);
        AP4_LinearReader reader(*file.GetMovie(), input);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(7)));
        AP4_Sample sample;
        AP4_DataBuffer data;
        for (unsigned int i=0; i<4; i++) TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(7, sample, data)));
        TASK_CHECK(reader.ReadNextSample(7, sample, data) == AP4_ERROR_EOS);
    }
    input->Release();
    return 0;
}

static int
case_reader_hand_built()
{
    std::string path = TempPath("rb_hand.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildHandBuiltFile(path)));

    // per-track entry points
    std::vector<ReadSample> subs;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(path, 1, subs)));
    TASK_CHECK(subs.size() == 6);
    const unsigned int sub_desc[] = {0, 1, 1, 0, 0, 1};
    const char* sub_data[] = {"<s0>", "<s1-long>", "<s2>", "<s3-very-long>", "<s4>", "<s5>"};
    for (unsigned int i=0; i<6; i++) {
        TASK_CHECK(subs[i].m_DescIndex == sub_desc[i]);
        TASK_CHECK(subs[i].m_Dts == i*1000); // omitted tfdt traf/moof continue the clock
        TASK_CHECK(subs[i].m_Duration == 1000);
        TASK_CHECK(subs[i].m_Data == sub_data[i]);
        std::string raw;
        TASK_CHECK(AP4_SUCCEEDED(ReadBytesAt(path, subs[i].m_Offset, subs[i].m_Size, raw)));
        TASK_CHECK(raw == sub_data[i]);
    }

    std::vector<ReadSample> auds;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(path, 3, auds)));
    TASK_CHECK(auds.size() == 2); // two trun atoms in one traf
    TASK_CHECK(auds[0].m_Dts == 0 && auds[1].m_Dts == 512);
    TASK_CHECK(auds[0].m_Data == "A0" && auds[1].m_Data == "A1-longer");

    // the any-track entry point and GetNextSample share one storage cursor:
    // samples come back in strictly increasing payload offset order
    std::vector<ReadSample> all;
    TASK_CHECK(AP4_SUCCEEDED(ReadAllSamples(path, all)));
    TASK_CHECK(all.size() == 8);
    for (unsigned int i=1; i<all.size(); i++) {
        TASK_CHECK(all[i].m_Offset > all[i-1].m_Offset);
    }
    TASK_CHECK(all[0].m_TrackId == 1);
    // the audio payload is the last block of moof 1, before moof 2's sample
    bool saw_audio_before_last_sub = false;
    for (unsigned int i=0; i<all.size(); i++) {
        if (all[i].m_TrackId == 3) {
            for (unsigned int j=i+1; j<all.size(); j++) {
                if (all[j].m_TrackId == 1 && all[j].m_Data == "<s5>") saw_audio_before_last_sub = true;
            }
        }
    }
    TASK_CHECK(saw_audio_before_last_sub);

    // alternate all three entry points on one shared reader: they all
    // consume the same cursor, in storage (payload offset) order
    AP4_ByteStream* input = NULL;
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(path.c_str(), AP4_FileByteStream::STREAM_MODE_READ, input)));
    {
        AP4_File file(*input, true);
        AP4_LinearReader reader(*file.GetMovie(), input);
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(1)));
        TASK_CHECK(AP4_SUCCEEDED(reader.EnableTrack(3)));
        AP4_Sample sample;
        AP4_DataBuffer data;
        AP4_UI32 track_id = 0;

        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(1, sample, data)));
        TASK_CHECK(sample.GetDts() == 0);                                            // sub 0
        TASK_CHECK(AP4_SUCCEEDED(reader.GetNextSample(sample, track_id)));
        TASK_CHECK(track_id == 1 && sample.GetDts() == 1000);                        // sub 1
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(sample, data, track_id)));
        TASK_CHECK(track_id == 1 && sample.GetDts() == 2000);                        // sub 2
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(3, sample, data)));
        TASK_CHECK(sample.GetDts() == 0);                                            // audio 0
        TASK_CHECK(AP4_SUCCEEDED(reader.GetNextSample(sample, track_id)));
        TASK_CHECK(track_id == 1 && sample.GetDts() == 3000);                        // sub 3
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(1, sample, data)));
        TASK_CHECK(sample.GetDts() == 4000);                                         // sub 4
        TASK_CHECK(AP4_SUCCEEDED(reader.GetNextSample(sample, track_id)));
        TASK_CHECK(track_id == 3 && sample.GetDts() == 512);                         // audio 1
        TASK_CHECK(AP4_SUCCEEDED(reader.ReadNextSample(sample, data, track_id)));
        TASK_CHECK(track_id == 1 && sample.GetDts() == 5000);                        // sub 5 (moof 2, no tfdt)
        TASK_CHECK(reader.GetNextSample(sample, track_id) == AP4_ERROR_EOS);
    }
    input->Release();
    return 0;
}

static int
case_movie_fragment_api()
{
    std::string path = TempPath("api_hand.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildHandBuiltFile(path)));

    AP4_ByteStream* input = NULL;
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(path.c_str(), AP4_FileByteStream::STREAM_MODE_READ, input)));
    AP4_DefaultAtomFactory factory;
    AP4_Atom* atom = NULL;
    AP4_ContainerAtom* moof1 = NULL;
    while (factory.CreateAtomFromStream(*input, atom) == AP4_SUCCESS) {
        if (atom->GetType() == AP4_ATOM_TYPE_MOOF && moof1 == NULL) {
            moof1 = AP4_DYNAMIC_CAST(AP4_ContainerAtom, atom);
        } else {
            delete atom;
        }
    }
    TASK_CHECK(moof1 != NULL);

    AP4_MovieFragment fragment(moof1);
    AP4_Array<AP4_UI32> ids;
    TASK_CHECK(AP4_SUCCEEDED(fragment.GetTrackIds(ids)));
    TASK_CHECK(ids.ItemCount() == 2);
    TASK_CHECK(ids[0] == 1); // first occurrence order, de-duplicated
    TASK_CHECK(ids[1] == 3);

    // GetTrafAtom keeps returning the first matching traf
    AP4_ContainerAtom* traf = NULL;
    TASK_CHECK(AP4_SUCCEEDED(fragment.GetTrafAtom(1, traf)));
    TASK_CHECK(traf == moof1->GetChild(AP4_ATOM_TYPE_TRAF, 0));
    TASK_CHECK(fragment.GetTrafAtom(99, traf) == AP4_ERROR_NO_SUCH_ITEM);

    // reopen the movie so the sample tables can resolve descriptions
    input->Seek(0);
    AP4_File file(*input, true);
    TASK_CHECK(file.GetMovie() != NULL);
    std::vector<BoxRecord> api_boxes;
    TASK_CHECK(AP4_SUCCEEDED(ScanFileBoxes(path, api_boxes)));
    const BoxRecord* api_moof = FindBox(api_boxes, AP4_ATOM_TYPE_MOOF, 0);
    TASK_CHECK(api_moof != NULL);
    AP4_Position moof_offset = api_moof->m_HeaderOffset;
    TASK_CHECK(moof1->GetSize() == api_moof->m_Size);

    // both CreateSampleTable overloads aggregate all same-track trafs
    AP4_FragmentSampleTable* table_movie = NULL;
    TASK_CHECK(AP4_SUCCEEDED(fragment.CreateSampleTable(file.GetMovie(), 1, input,
                                                        moof_offset, moof_offset+moof1->GetSize()+8,
                                                        0, table_movie)));
    TASK_CHECK(table_movie != NULL);
    TASK_CHECK(table_movie->GetSampleCount() == 5);
    AP4_FragmentSampleTable* table_moov = NULL;
    TASK_CHECK(AP4_SUCCEEDED(fragment.CreateSampleTable(file.GetMovie()->GetMoovAtom(), 1, input,
                                                        moof_offset, moof_offset+moof1->GetSize()+8,
                                                        0, table_moov)));
    TASK_CHECK(table_moov != NULL);
    TASK_CHECK(table_moov->GetSampleCount() == 5);

    // every sample keeps its own description index, offset, dts and cts
    AP4_Sample s;
    const unsigned int desc[] = {0, 1, 1, 0, 0};
    AP4_Position previous_offset = 0;
    for (unsigned int i=0; i<5; i++) {
        TASK_CHECK(AP4_SUCCEEDED(table_movie->GetSample(i, s)));
        TASK_CHECK(s.GetDescriptionIndex() == desc[i]);
        TASK_CHECK(s.GetDts() == i*1000);
        TASK_CHECK(s.GetCtsDelta() == 0);
        if (i) TASK_CHECK(s.GetOffset() > previous_offset);
        previous_offset = s.GetOffset();
    }
    AP4_FragmentSampleTable* audio_table = NULL;
    TASK_CHECK(AP4_SUCCEEDED(fragment.CreateSampleTable(file.GetMovie(), 3, input,
                                                        moof_offset, moof_offset+moof1->GetSize()+8,
                                                        0, audio_table)));
    TASK_CHECK(audio_table->GetSampleCount() == 2);

    delete table_movie;
    delete table_moov;
    delete audio_table;
    input->Release();
    return 0;
}

static int
case_addressing_modes()
{
    std::string path = TempPath("addr_hand.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildHandBuiltFile(path)));

    std::vector<ReadSample> subs;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(path, 1, subs)));
    TASK_CHECK(subs.size() == 6);

    // resolve the explicit-base-data-offset traf (samples 2 and 3) and
    // confirm both addressing modes land on the exact payload bytes
    const char* expected[] = {"<s0>", "<s1-long>", "<s2>", "<s3-very-long>", "<s4>", "<s5>"};
    for (unsigned int i=0; i<6; i++) {
        std::string raw;
        TASK_CHECK(AP4_SUCCEEDED(ReadBytesAt(path, subs[i].m_Offset, subs[i].m_Size, raw)));
        TASK_CHECK(raw == expected[i]);
    }
    // samples 2/3 came from the explicit-base traf, 0/1/4 from default-base
    TASK_CHECK(subs[2].m_Offset > 0 && subs[3].m_Offset > subs[2].m_Offset);
    TASK_CHECK(subs[4].m_Offset > subs[3].m_Offset);

    // the audio traf, reached through default-base-is-moof with two truns
    std::vector<ReadSample> auds;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(path, 3, auds)));
    TASK_CHECK(auds.size() == 2);
    TASK_CHECK(auds[1].m_Offset == auds[0].m_Offset + auds[0].m_Size);
    return 0;
}

/*----------------------------------------------------------------------
|   compat group
+---------------------------------------------------------------------*/
// passthrough processor: no fragment handler, just repackages the atoms
class TestPassthroughProcessor : public AP4_Processor {
public:
    virtual AP4_Result Run(AP4_ByteStream& in, AP4_ByteStream& out) {
        return Process(in, out);
    }
};

static int
case_processor_roundtrip()
{
    std::string hand = TempPath("pc_hand.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildHandBuiltFile(hand)));
    std::string hand_copy = TempPath("pc_hand_copy.mp4");

    AP4_ByteStream* in = NULL;
    AP4_ByteStream* out = NULL;
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(hand.c_str(), AP4_FileByteStream::STREAM_MODE_READ, in)));
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(hand_copy.c_str(), AP4_FileByteStream::STREAM_MODE_WRITE, out)));
    TestPassthroughProcessor processor;
    TASK_CHECK(AP4_SUCCEEDED(processor.Run(*in, *out)));
    out->Release();
    in->Release();

    std::vector<ReadSample> before, after;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(hand, 1, before)));
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(hand_copy, 1, after)));
    TASK_CHECK(before.size() == after.size());
    for (unsigned int i=0; i<before.size(); i++) {
        TASK_CHECK(after[i].m_DescIndex == before[i].m_DescIndex);
        TASK_CHECK(after[i].m_Dts == before[i].m_Dts);
        TASK_CHECK(after[i].m_Data == before[i].m_Data);
    }

    // same round trip through the normal packaging entry for a CLI output
    std::string cli_in = TempPath("pc_cli_in.mp4");
    std::string cli_out = TempPath("pc_cli_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(cli_in)));
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--subtitle-description-runs");
    args.push_back(cli_in);
    args.push_back(cli_out);
    TASK_CHECK(RunFragmenter(args) == 0);
    std::string cli_copy = TempPath("pc_cli_copy.mp4");
    in = NULL; out = NULL;
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(cli_out.c_str(), AP4_FileByteStream::STREAM_MODE_READ, in)));
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(cli_copy.c_str(), AP4_FileByteStream::STREAM_MODE_WRITE, out)));
    TestPassthroughProcessor processor2;
    TASK_CHECK(AP4_SUCCEEDED(processor2.Run(*in, *out)));
    out->Release();
    in->Release();
    std::vector<ReadSample> cli_samples, copy_samples;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(cli_out, 7, cli_samples)));
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(cli_copy, 7, copy_samples)));
    TASK_CHECK(cli_samples.size() == 4 && copy_samples.size() == 4);
    for (unsigned int i=0; i<4; i++) {
        TASK_CHECK(copy_samples[i].m_DescIndex == cli_samples[i].m_DescIndex);
        TASK_CHECK(copy_samples[i].m_Data == cli_samples[i].m_Data);
    }
    return 0;
}

static int
case_repackage_and_refragment()
{
    std::string in = TempPath("rr_in.mp4");
    std::string out1 = TempPath("rr_out1.mp4");
    std::string out2 = TempPath("rr_out2.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in)));

    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out1);
    TASK_CHECK(RunFragmenter(args) == 0);

    // re-fragment the multi-traf file, with and without the option
    std::vector<std::string> args2 = args;
    args2.back() = out2;
    TASK_CHECK(RunFragmenter(args2) == 0);
    std::vector<ReadSample> s2;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(out2, 7, s2)));
    TASK_CHECK(s2.size() == 4);
    const unsigned int expected_desc[] = {0, 0, 1, 0};
    for (unsigned int i=0; i<4; i++) TASK_CHECK(s2[i].m_DescIndex == expected_desc[i]);

    std::string out3 = TempPath("rr_out3.mp4");
    std::vector<std::string> args3;
    args3.push_back("--quiet");
    args3.push_back("--fragment-duration");
    args3.push_back("4000");
    args3.push_back(out1);
    args3.push_back(out3);
    TASK_CHECK(RunFragmenter(args3) == 0);
    std::vector<ReadSample> s3;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(out3, 7, s3)));
    TASK_CHECK(s3.size() == 4);
    for (unsigned int i=0; i<4; i++) {
        // without the option the legacy single-traf output tags every sample
        // with the first sample's description index, but the payloads survive
        TASK_CHECK(s3[i].m_DescIndex == 0);
        TASK_CHECK(s3[i].m_Data == s2[i].m_Data);
    }
    return 0;
}

static int
case_invalid_desc_index_cli()
{
    std::string in = TempPath("bad_in.mp4");
    std::string out = TempPath("bad_out.mp4");
    std::remove(out.c_str());
    // sample 2 references description index 5, but only two entries exist
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in, 1000, 5)));
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) != 0);          // explicit non-zero exit
    TASK_CHECK(access(out.c_str(), F_OK) != 0);    // no partial output kept

    // without the option the same input is still processed (legacy behavior)
    std::string out_legacy = TempPath("bad_legacy.mp4");
    std::vector<std::string> args2;
    args2.push_back("--quiet");
    args2.push_back("--fragment-duration");
    args2.push_back("4000");
    args2.push_back(in);
    args2.push_back(out_legacy);
    TASK_CHECK(RunFragmenter(args2) == 0);
    return 0;
}

static int
case_unsupported_description_cli()
{
    std::string in = TempPath("unsup_in.mp4");
    std::string out = TempPath("unsup_out.mp4");
    std::remove(out.c_str());
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in, 1000, 0xFFFFFFFF, true))); // 2nd entry is mp4a
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) != 0);
    TASK_CHECK(access(out.c_str(), F_OK) != 0);
    return 0;
}

static int
case_stsd_and_payload_unchanged()
{
    std::string in = TempPath("stsd_in.mp4");
    std::string out = TempPath("stsd_out.mp4");
    TASK_CHECK(AP4_SUCCEEDED(BuildFixtureInput(in)));
    std::vector<std::string> args;
    args.push_back("--quiet");
    args.push_back("--fragment-duration");
    args.push_back("4000");
    args.push_back("--subtitle-description-runs");
    args.push_back(in);
    args.push_back(out);
    TASK_CHECK(RunFragmenter(args) == 0);

    AP4_AtomParent top_in, top_out;
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(in, top_in)));
    TASK_CHECK(AP4_SUCCEEDED(LoadTopLevelAtoms(out, top_out)));
    AP4_ContainerAtom* stsd_in = AP4_DYNAMIC_CAST(AP4_ContainerAtom,
                                                  top_in.FindChild("moov/trak/mdia/minf/stbl/stsd"));
    AP4_ContainerAtom* stsd_out = AP4_DYNAMIC_CAST(AP4_ContainerAtom,
                                                   top_out.FindChild("moov/trak/mdia/minf/stbl/stsd"));
    TASK_CHECK(stsd_in != NULL && stsd_out != NULL);
    TASK_CHECK(stsd_in->GetSize() == stsd_out->GetSize());

    // compare the serialized stsd bytes (both stpp entries preserved)
    AP4_MemoryByteStream* mem_in = new AP4_MemoryByteStream();
    AP4_MemoryByteStream* mem_out = new AP4_MemoryByteStream();
    TASK_CHECK(AP4_SUCCEEDED(stsd_in->Write(*mem_in)));
    TASK_CHECK(AP4_SUCCEEDED(stsd_out->Write(*mem_out)));
    TASK_CHECK(mem_in->GetDataSize() == mem_out->GetDataSize());
    TASK_CHECK(std::memcmp(mem_in->GetData(), mem_out->GetData(), mem_in->GetDataSize()) == 0);
    mem_in->Release();
    mem_out->Release();

    // all four payloads survive byte-for-byte, in order
    std::vector<ReadSample> samples;
    TASK_CHECK(AP4_SUCCEEDED(ReadTrackSamples(out, 7, samples)));
    const std::string expected[] = {"<tt>A</tt>", "<tt>BB</tt>", "<tt>CCC</tt>", "<tt>D</tt>"};
    for (unsigned int i=0; i<4; i++) TASK_CHECK(samples[i].m_Data == expected[i]);
    return 0;
}

/*----------------------------------------------------------------------
|   registration
+---------------------------------------------------------------------*/
const struct task_case task_cases[] = {
    {"layout", "cli_fixture_layout",        case_cli_fixture_layout},
    {"layout", "hand_built_layout",         case_hand_built_layout},
    {"layout", "aba_across_fragments",      case_aba_across_fragments},
    {"layout", "timescale_conversion",      case_timescale_conversion},
    {"layout", "index_ranges",              case_index_ranges},
    {"layout", "single_description_legacy", case_single_description_legacy},
    {"reader", "reader_fixture",            case_reader_fixture},
    {"reader", "reader_hand_built",         case_reader_hand_built},
    {"reader", "movie_fragment_api",        case_movie_fragment_api},
    {"reader", "addressing_modes",          case_addressing_modes},
    {"compat", "processor_roundtrip",       case_processor_roundtrip},
    {"compat", "repackage_and_refragment",  case_repackage_and_refragment},
    {"compat", "invalid_desc_index_cli",    case_invalid_desc_index_cli},
    {"compat", "unsupported_description_cli", case_unsupported_description_cli},
    {"compat", "stsd_and_payload_unchanged", case_stsd_and_payload_unchanged},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);

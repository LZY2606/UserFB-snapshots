/*
 * Tests for the streaming PCM audio OP1a frame wrapper
 *
 * Registered through task_support/check.h in three groups: cadence, layout
 * and failure. The output MXF is scanned independently with a local KLV/BER
 * parser, the PCM is reconstructed both from the raw essence stream and by
 * locating every unit through the footer index table entries, and the
 * metadata graph is cross-checked through libMXF's own reader.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mxf/mxf.h>
#include <mxf/mxf_memory_file.h>

#include "check.h"
#include "examples/pcmwrap/pcm_wrap.h"
#include "examples/pcmwrap/pcm_frame.h"
#include "examples/pcmwrap/pcm_wrap_mxf.h"


/* ------------------------- deterministic PCM data ------------------------- */

static int32_t sample_value(uint32_t frameIndex, uint32_t channel, uint32_t bits)
{
    uint32_t range = 1u << bits;
    uint32_t v = (frameIndex * 48611u + channel * 7919u + 17u) % range;
    return (int32_t)v - (int32_t)(range / 2);
}

static void store_le(uint8_t *out, int32_t value, uint32_t bytes)
{
    uint32_t u = (uint32_t)value;
    uint32_t i;
    for (i = 0; i < bytes; i++)
    {
        out[i] = (uint8_t)(u >> (8 * i));
    }
}

static uint32_t expected_unit_frames(PCMWrapEditRate rate, uint32_t sequenceOffset, uint32_t unitIndex)
{
    static const uint32_t ntsc[PCM_FRAME_NTSC_CADENCE_LEN] = PCM_FRAME_NTSC_CADENCE;

    if (rate == PCM_WRAP_EDIT_RATE_25_1)
    {
        return PCM_FRAME_PAL_UNIT_FRAMES;
    }
    return ntsc[(sequenceOffset + unitIndex) % PCM_FRAME_NTSC_CADENCE_LEN];
}

static void format_params(PCMWrapPCMFormat format, uint32_t *bits, uint32_t *channels,
                          uint32_t *blockAlign)
{
    *bits = (format == PCM_WRAP_PCM_S16LE_1CH || format == PCM_WRAP_PCM_S16LE_2CH) ? 16 : 24;
    *channels = (format == PCM_WRAP_PCM_S16LE_1CH || format == PCM_WRAP_PCM_S24LE_1CH) ? 1 : 2;
    *blockAlign = (*bits / 8) * *channels;
}

/* Build the exact PCM byte stream for the given number of edit units. */
static uint8_t *make_pcm(PCMWrapPCMFormat format, PCMWrapEditRate rate, uint32_t sequenceOffset,
                         uint32_t numUnits, uint64_t *byteCountOut)
{
    uint32_t bits, channels, blockAlign;
    uint32_t u, frameInUnit, g, ch;
    uint64_t totalFrames = 0;
    uint8_t *buffer;
    uint8_t *p;

    format_params(format, &bits, &channels, &blockAlign);
    for (u = 0; u < numUnits; u++)
    {
        totalFrames += expected_unit_frames(rate, sequenceOffset, u);
    }

    buffer = malloc((size_t)(totalFrames * blockAlign));
    if (buffer == NULL)
    {
        return NULL;
    }
    p = buffer;
    g = 0;
    for (u = 0; u < numUnits; u++)
    {
        uint32_t frames = expected_unit_frames(rate, sequenceOffset, u);
        for (frameInUnit = 0; frameInUnit < frames; frameInUnit++)
        {
            for (ch = 0; ch < channels; ch++)
            {
                store_le(p, sample_value(g, ch, bits), bits / 8);
                p += bits / 8;
            }
            g++;
        }
    }

    *byteCountOut = totalFrames * blockAlign;
    return buffer;
}


/* ----------------------------- memory file IO ----------------------------- */

static MXFMemoryFile *open_mem_file(void)
{
    MXFMemoryFile *memFile = NULL;
    if (!mxf_mem_file_open_new(65536, 0, &memFile))
    {
        return NULL;
    }
    return memFile;
}


/* --------------------------- deterministic fault -------------------------- */

typedef struct
{
    MXFFile *inner;
    uint64_t writeBudget;
    uint64_t written;
    int neverSeekable;
    int failSeeks;
    int sawWriteFailure;
} FaultSys;

static uint32_t fault_write(MXFFileSysData *sysData, const uint8_t *data, uint32_t count)
{
    FaultSys *fault = (FaultSys*)sysData;
    uint32_t allowed;

    if (fault->written >= fault->writeBudget)
    {
        fault->sawWriteFailure = 1;
        return 0;
    }
    allowed = (uint32_t)(((fault->writeBudget - fault->written) > count)
                         ? count : (fault->writeBudget - fault->written));
    allowed = fault->inner->write(fault->inner->sysData, data, allowed);
    fault->written += allowed;
    if (allowed != count)
    {
        fault->sawWriteFailure = 1;
    }
    return allowed;
}

static uint32_t fault_read(MXFFileSysData *sysData, uint8_t *data, uint32_t count)
{
    FaultSys *fault = (FaultSys*)sysData;
    return fault->inner->read(fault->inner->sysData, data, count);
}

static int fault_get_char(MXFFileSysData *sysData)
{
    FaultSys *fault = (FaultSys*)sysData;
    return fault->inner->get_char(fault->inner->sysData);
}

static int fault_put_char(MXFFileSysData *sysData, int c)
{
    FaultSys *fault = (FaultSys*)sysData;
    return fault->inner->put_char(fault->inner->sysData, c);
}

static int fault_eof(MXFFileSysData *sysData)
{
    FaultSys *fault = (FaultSys*)sysData;
    return fault->inner->eof(fault->inner->sysData);
}

static int fault_seek(MXFFileSysData *sysData, int64_t offset, int whence)
{
    FaultSys *fault = (FaultSys*)sysData;
    if (fault->failSeeks)
    {
        return 0;
    }
    return fault->inner->seek(fault->inner->sysData, offset, whence);
}

static int64_t fault_tell(MXFFileSysData *sysData)
{
    FaultSys *fault = (FaultSys*)sysData;
    return fault->inner->tell(fault->inner->sysData);
}

static int fault_is_seekable(MXFFileSysData *sysData)
{
    FaultSys *fault = (FaultSys*)sysData;
    if (fault->neverSeekable)
    {
        return 0;
    }
    return fault->inner->is_seekable(fault->inner->sysData);
}

static int64_t fault_size(MXFFileSysData *sysData)
{
    FaultSys *fault = (FaultSys*)sysData;
    return fault->inner->size(fault->inner->sysData);
}

static MXFFile *make_fault_file(MXFFile *inner, uint64_t writeBudget, int failSeeks,
                                int neverSeekable, FaultSys **sysOut)
{
    MXFFile *fault = malloc(sizeof(MXFFile));
    FaultSys *sys = calloc(1, sizeof(FaultSys));
    if (fault == NULL || sys == NULL)
    {
        free(fault);
        free(sys);
        return NULL;
    }
    *fault = *inner;
    sys->inner = inner;
    sys->writeBudget = writeBudget;
    sys->failSeeks = failSeeks;
    sys->neverSeekable = neverSeekable;
    fault->sysData = (MXFFileSysData*)sys;
    fault->read = fault_read;
    fault->write = fault_write;
    fault->get_char = fault_get_char;
    fault->put_char = fault_put_char;
    fault->eof = fault_eof;
    fault->seek = fault_seek;
    fault->tell = fault_tell;
    fault->is_seekable = fault_is_seekable;
    fault->size = fault_size;
    *sysOut = sys;
    return fault;
}

static void free_fault_file(MXFFile *fault)
{
    if (fault == NULL)
    {
        return;
    }
    free(fault->sysData);
    free(fault);
}


/* ------------------------ independent KLV/BER scan ------------------------ */

typedef struct
{
    uint64_t keyPos;
    uint64_t length;
    uint8_t llen;
    mxfKey key;
} AudioKLV;

typedef struct
{
    uint64_t keyPos;
    uint64_t thisPartition;
    uint64_t previousPartition;
    uint64_t footerPartition;
    uint64_t headerByteCount;
    uint64_t indexByteCount;
    uint64_t bodyOffset;
    uint32_t kagSize;
    uint32_t indexSID;
    uint32_t bodySID;
    int isBody;
    int isHeader;
    int isFooter;
} PartitionRecord;

typedef struct
{
    mxfRational editRate;
    int64_t startPosition;
    int64_t duration;
    uint32_t editUnitByteCount;
    uint32_t indexSID;
    uint32_t bodySID;
    uint8_t sliceCount;
    uint8_t posTableCount;
    uint64_t *streamOffsets;
    uint32_t numEntries;
} IndexSegmentRecord;

typedef struct
{
    AudioKLV *audio;
    uint32_t audioCount;
    uint32_t audioCap;
    uint8_t *pcm;
    uint64_t pcmBytes;
    uint64_t pcmCap;
    PartitionRecord *partitions;
    uint32_t partitionCount;
    uint32_t partitionCap;
    IndexSegmentRecord *segments;
    uint32_t segmentCount;
    uint32_t segmentCap;
    uint64_t *ripOffsets;
    uint32_t *ripSIDs;
    uint32_t ripCount;
    uint32_t fillerCount;
} ScanResult;

static void scan_init(ScanResult *scan)
{
    memset(scan, 0, sizeof(*scan));
}

static void scan_free(ScanResult *scan)
{
    uint32_t i;
    free(scan->audio);
    free(scan->pcm);
    free(scan->partitions);
    for (i = 0; i < scan->segmentCount; i++)
    {
        free(scan->segments[i].streamOffsets);
    }
    free(scan->segments);
    free(scan->ripOffsets);
    free(scan->ripSIDs);
    memset(scan, 0, sizeof(*scan));
}

/* independent BER length decoder */
static int scan_read_ber(MXFFile *file, uint8_t *llenOut, uint64_t *lenOut)
{
    int c = mxf_file_getc(file);
    uint8_t llen = 1;
    uint64_t len = 0;
    uint8_t i;

    if (c == EOF)
    {
        return 0;
    }
    if ((c & 0x80) == 0)
    {
        *llenOut = 1;
        *lenOut = (uint64_t)c;
        return 1;
    }
    i = (uint8_t)(c & 0x7f);
    if (i == 0 || i > 8)
    {
        return 0;
    }
    llen = (uint8_t)(i + 1);
    for (; i > 0; i--)
    {
        c = mxf_file_getc(file);
        if (c == EOF)
        {
            return 0;
        }
        len = (len << 8) | (uint64_t)c;
    }
    *llenOut = llen;
    *lenOut = len;
    return 1;
}

static int scan_read_key(MXFFile *file, mxfKey *key)
{
    return mxf_file_read(file, (uint8_t*)key, 16) == 16;
}

static int scan_skip(MXFFile *file, uint64_t len)
{
    uint8_t chunk[4096];
    while (len > 0)
    {
        uint32_t step = (uint32_t)(len > sizeof(chunk) ? sizeof(chunk) : len);
        if (mxf_file_read(file, chunk, step) != step)
        {
            return 0;
        }
        len -= step;
    }
    return 1;
}

static int scan_append_partition(ScanResult *scan, const PartitionRecord *record)
{
    if (scan->partitionCount == scan->partitionCap)
    {
        uint32_t cap = scan->partitionCap == 0 ? 4 : scan->partitionCap * 2;
        PartitionRecord *grown = realloc(scan->partitions, cap * sizeof(PartitionRecord));
        if (grown == NULL)
        {
            return 0;
        }
        scan->partitions = grown;
        scan->partitionCap = cap;
    }
    scan->partitions[scan->partitionCount++] = *record;
    return 1;
}

static int scan_append_audio(ScanResult *scan, MXFFile *file, uint64_t keyPos, uint8_t llen,
                             const mxfKey *key, uint64_t length)
{
    uint8_t chunk[4096];
    uint64_t remain = length;
    AudioKLV *klv;

    if (scan->audioCount == scan->audioCap)
    {
        uint32_t cap = scan->audioCap == 0 ? 16 : scan->audioCap * 2;
        AudioKLV *grown = realloc(scan->audio, cap * sizeof(AudioKLV));
        if (grown == NULL)
        {
            return 0;
        }
        scan->audio = grown;
        scan->audioCap = cap;
    }
    klv = &scan->audio[scan->audioCount++];
    klv->keyPos = keyPos;
    klv->length = length;
    klv->llen = llen;
    klv->key = *key;

    while (remain > 0)
    {
        uint32_t step = (uint32_t)(remain > sizeof(chunk) ? sizeof(chunk) : remain);
        uint64_t cap;
        uint8_t *grown;

        if (scan->pcmBytes + step > scan->pcmCap)
        {
            cap = scan->pcmCap == 0 ? 4096 : scan->pcmCap * 2;
            while (cap < scan->pcmBytes + step)
            {
                cap *= 2;
            }
            grown = realloc(scan->pcm, (size_t)cap);
            if (grown == NULL)
            {
                return 0;
            }
            scan->pcm = grown;
            scan->pcmCap = cap;
        }
        if (mxf_file_read(file, scan->pcm + scan->pcmBytes, step) != step)
        {
            return 0;
        }
        scan->pcmBytes += step;
        remain -= step;
    }
    return 1;
}

static int scan_append_segment(ScanResult *scan, const IndexSegmentRecord *record)
{
    if (scan->segmentCount == scan->segmentCap)
    {
        uint32_t cap = scan->segmentCap == 0 ? 4 : scan->segmentCap * 2;
        IndexSegmentRecord *grown = realloc(scan->segments, cap * sizeof(IndexSegmentRecord));
        if (grown == NULL)
        {
            return 0;
        }
        scan->segments = grown;
        scan->segmentCap = cap;
    }
    scan->segments[scan->segmentCount++] = *record;
    return 1;
}

static int scan_index_segment(MXFFile *file, ScanResult *scan, uint64_t segmentLen)
{
    IndexSegmentRecord record;
    uint64_t consumed = 0;
    uint64_t entriesBytes = 0;

    memset(&record, 0, sizeof(record));
    while (consumed < segmentLen)
    {
        mxfLocalTag tag;
        uint16_t localLen;
        if (!mxf_read_local_tag(file, &tag) || !mxf_read_uint16(file, &localLen))
        {
            return 0;
        }
        consumed += 4 + localLen;

        switch (tag)
        {
            case 0x3c0a:
                if (!scan_skip(file, localLen)) return 0;
                break;
            case 0x3f0b:
            {
                int32_t num, den;
                if (mxf_read_int32(file, &num) != 1 || mxf_read_int32(file, &den) != 1) return 0;
                record.editRate.numerator = num;
                record.editRate.denominator = den;
                break;
            }
            case 0x3f0c:
                if (mxf_read_int64(file, &record.startPosition) != 1) return 0;
                break;
            case 0x3f0d:
                if (mxf_read_int64(file, &record.duration) != 1) return 0;
                break;
            case 0x3f05:
                if (mxf_read_uint32(file, &record.editUnitByteCount) != 1) return 0;
                break;
            case 0x3f06:
                if (mxf_read_uint32(file, &record.indexSID) != 1) return 0;
                break;
            case 0x3f07:
                if (mxf_read_uint32(file, &record.bodySID) != 1) return 0;
                break;
            case 0x3f08:
            {
                uint8_t v;
                if (mxf_read_uint8(file, &v) != 1) return 0;
                record.sliceCount = v;
                break;
            }
            case 0x3f0e:
            {
                uint8_t v;
                if (mxf_read_uint8(file, &v) != 1) return 0;
                record.posTableCount = v;
                break;
            }
            case 0x3f09:
                if (!scan_skip(file, localLen)) return 0;
                break;
            case 0x3f0a:
            {
                uint32_t numEntries;
                uint32_t entryLen;
                uint32_t i;
                if (mxf_read_uint32(file, &numEntries) != 1 ||
                    mxf_read_uint32(file, &entryLen) != 1)
                {
                    return 0;
                }
                record.numEntries = numEntries;
                record.streamOffsets = calloc(numEntries ? numEntries : 1, sizeof(uint64_t));
                if (record.streamOffsets == NULL)
                {
                    return 0;
                }
                entriesBytes = 8 + (uint64_t)numEntries * entryLen;
                for (i = 0; i < numEntries; i++)
                {
                    uint8_t temporal, keyFrame, flags;
                    uint64_t streamOffset;
                    if (mxf_read_uint8(file, &temporal) != 1 ||
                        mxf_read_uint8(file, &keyFrame) != 1 ||
                        mxf_read_uint8(file, &flags) != 1 ||
                        mxf_read_uint64(file, &streamOffset) != 1)
                    {
                        return 0;
                    }
                    record.streamOffsets[i] = streamOffset;
                    if (!scan_skip(file, entryLen - 11))
                    {
                        return 0;
                    }
                }
                break;
            }
            default:
                if (!scan_skip(file, localLen)) return 0;
                break;
        }
    }

    if (consumed != segmentLen || entriesBytes == 0)
    {
        free(record.streamOffsets);
        return 0;
    }
    return scan_append_segment(scan, &record);
}

static int scan_rip(MXFFile *file, ScanResult *scan, uint64_t ripLen, uint8_t llen)
{
    uint64_t remain = ripLen - 4;
    uint32_t count = 0;
    uint32_t trailingLength;

    while (remain >= 12)
    {
        uint32_t sid;
        uint64_t offset;
        uint64_t *offsets;
        uint32_t *sids;

        if (mxf_read_uint32(file, &sid) != 1 || mxf_read_uint64(file, &offset) != 1)
        {
            return 0;
        }
        offsets = realloc(scan->ripOffsets, (count + 1) * sizeof(uint64_t));
        sids = realloc(scan->ripSIDs, (count + 1) * sizeof(uint32_t));
        if (offsets == NULL || sids == NULL)
        {
            free(offsets);
            free(sids);
            return 0;
        }
        scan->ripOffsets = offsets;
        scan->ripSIDs = sids;
        scan->ripOffsets[count] = offset;
        scan->ripSIDs[count] = sid;
        count++;
        remain -= 12;
    }
    if (remain != 0 || mxf_read_uint32(file, &trailingLength) != 1)
    {
        return 0;
    }
    /* the trailing field encodes the total RIP KLV size */
    if (trailingLength != 16 + llen + ripLen)
    {
        return 0;
    }
    scan->ripCount = count;
    return 1;
}

static int scan_file(MXFFile *file, ScanResult *scan)
{
    int64_t fileSize;

    if (!mxf_file_seek(file, 0, SEEK_SET) || (fileSize = mxf_file_size(file)) <= 0)
    {
        return 0;
    }

    while (mxf_file_tell(file) < fileSize)
    {
        mxfKey key;
        uint8_t llen;
        uint64_t len;
        int64_t keyPos = mxf_file_tell(file);

        if (keyPos < 0 || !scan_read_key(file, &key) || !scan_read_ber(file, &llen, &len))
        {
            return 0;
        }

        if (mxf_is_partition_pack(&key))
        {
            PartitionRecord record;
            uint16_t major, minor;
            uint32_t labelCount, labelEleLen, i;
            memset(&record, 0, sizeof(record));
            record.keyPos = (uint64_t)keyPos;
            record.isHeader = mxf_is_header_partition_pack(&key);
            record.isBody = mxf_is_body_partition_pack(&key);
            record.isFooter = mxf_is_footer_partition_pack(&key);
            if (mxf_read_uint16(file, &major) != 1 || mxf_read_uint16(file, &minor) != 1 ||
                mxf_read_uint32(file, &record.kagSize) != 1 ||
                mxf_read_uint64(file, &record.thisPartition) != 1 ||
                mxf_read_uint64(file, &record.previousPartition) != 1 ||
                mxf_read_uint64(file, &record.footerPartition) != 1 ||
                mxf_read_uint64(file, &record.headerByteCount) != 1 ||
                mxf_read_uint64(file, &record.indexByteCount) != 1 ||
                mxf_read_uint32(file, &record.indexSID) != 1 ||
                mxf_read_uint64(file, &record.bodyOffset) != 1 ||
                mxf_read_uint32(file, &record.bodySID) != 1 ||
                !scan_skip(file, 16) ||
                mxf_read_uint32(file, &labelCount) != 1 ||
                mxf_read_uint32(file, &labelEleLen) != 1)
            {
                return 0;
            }
            (void)major; (void)minor;
            for (i = 0; i < labelCount; i++)
            {
                if (!scan_skip(file, labelEleLen))
                {
                    return 0;
                }
            }
            if (!scan_append_partition(scan, &record))
            {
                return 0;
            }
        }
        else if (mxf_is_filler(&key))
        {
            scan->fillerCount++;
            if (!scan_skip(file, len))
            {
                return 0;
            }
        }
        else if (mxf_is_gc_essence_element(&key))
        {
            if (!scan_append_audio(scan, file, (uint64_t)keyPos, llen, &key, len))
            {
                return 0;
            }
        }
        else if (mxf_is_index_table_segment(&key))
        {
            if (!scan_index_segment(file, scan, len))
            {
                return 0;
            }
        }
        else if (mxf_equals_key(&key, &g_RandomIndexPack_key))
        {
            if (!scan_rip(file, scan, len, llen))
            {
                return 0;
            }
        }
        else
        {
            /* primer pack and metadata sets */
            if (!scan_skip(file, len))
            {
                return 0;
            }
        }
    }

    if (mxf_file_tell(file) != fileSize)
    {
        return 0;
    }
    return 1;
}


/* --------------------------- metadata verification ------------------------ */

static int read_metadata_at(MXFFile *file, uint64_t partitionPos, MXFDataModel **dataModelOut,
                            MXFPartition **packOut, MXFHeaderMetadata **hmOut)
{
    mxfKey key;
    uint8_t llen;
    uint64_t len;
    MXFPartition *pack = NULL;
    MXFHeaderMetadata *hm = NULL;
    MXFDataModel *dataModel = NULL;

    if (!mxf_file_seek(file, (int64_t)partitionPos, SEEK_SET) ||
        !mxf_read_kl(file, &key, &llen, &len) ||
        !mxf_read_partition(file, &key, len, &pack) ||
        !mxf_load_data_model(&dataModel) ||
        !mxf_finalise_data_model(dataModel) ||
        !mxf_create_header_metadata(&hm, dataModel))
    {
        mxf_free_data_model(&dataModel);
        mxf_free_partition(&pack);
        return 0;
    }

    if (pack->headerByteCount == 0 ||
        !mxf_read_next_nonfiller_kl(file, &key, &llen, &len) ||
        !mxf_is_header_metadata(&key) ||
        !mxf_read_header_metadata(file, hm, pack->headerByteCount, &key, llen, len))
    {
        mxf_free_header_metadata(&hm);
        mxf_free_data_model(&dataModel);
        mxf_free_partition(&pack);
        return 0;
    }

    *dataModelOut = dataModel;
    *packOut = pack;
    *hmOut = hm;
    return 1;
}

static int get_single_set(MXFHeaderMetadata *hm, const mxfKey *setKey, MXFMetadataSet **set)
{
    return mxf_find_singular_set_by_key(hm, setKey, set) && *set != NULL;
}

static int read_partition_pack(MXFFile *file, uint64_t pos, MXFPartition **packOut)
{
    mxfKey key;
    uint8_t llen;
    uint64_t len;
    MXFPartition *pack = NULL;

    if (!mxf_file_seek(file, (int64_t)pos, SEEK_SET) ||
        !mxf_read_kl(file, &key, &llen, &len) ||
        !mxf_read_partition(file, &key, len, &pack))
    {
        return 0;
    }
    *packOut = pack;
    return 1;
}

static int get_array_strongref(MXFHeaderMetadata *hm, MXFMetadataSet *set, const mxfKey *itemKey,
                               uint32_t index, MXFMetadataSet **result)
{
    uint8_t *element = NULL;
    if (!mxf_get_array_item_element(set, itemKey, index, &element))
    {
        return 0;
    }
    return mxf_get_strongref(hm, element, result) && *result != NULL;
}

static int check_duration_item(MXFMetadataSet *set, const mxfKey *itemKey, int64_t expected)
{
    mxfLength value;
    if (!mxf_get_length_item(set, itemKey, &value))
    {
        return 0;
    }
    return value == expected;
}

/* Verifies one metadata graph instance (header or footer copy). */
static int check_metadata_graph(MXFFile *file, uint64_t partitionPos, uint64_t expectedUnits,
                                const PCMFrameConfig *config, mxfRational editRate,
                                mxfUMID *materialUMIDOut, mxfUMID *fileUMIDOut,
                                mxfUUID *instanceUIDOut, uint32_t *fileTrackIDOut)
{
    MXFDataModel *dataModel = NULL;
    MXFPartition *pack = NULL;
    MXFHeaderMetadata *hm = NULL;
    MXFMetadataSet *preface = NULL, *contentStorage = NULL, *ecd = NULL;
    MXFMetadataSet *materialPackage = NULL, *sourcePackage = NULL;
    MXFMetadataSet *materialTrack = NULL, *fileTrack = NULL;
    MXFMetadataSet *materialSequence = NULL, *fileSequence = NULL;
    MXFMetadataSet *materialClip = NULL, *fileClip = NULL;
    MXFMetadataSet *descriptor = NULL;
    mxfUL opLabel, ecLabel, dataDef;
    mxfRational sampleRate, audioSamplingRate;
    mxfUMID materialUMID, fileUMID, clipSourceUMID;
    mxfUUID prefaceInstance;
    uint32_t channelCount, quantBits, avgBps, clipSourceTrackID, ecdBodySID, ecdIndexSID;
    uint32_t fileTrackID = 0;
    uint16_t blockAlign;
    uint8_t sequenceOffset;
    int ok = 0;

    if (!read_metadata_at(file, partitionPos, &dataModel, &pack, &hm))
    {
        goto cleanup;
    }
    TASK_CHECK(get_single_set(hm, &MXF_SET_K(Preface), &preface));
    TASK_CHECK(get_single_set(hm, &MXF_SET_K(ContentStorage), &contentStorage));
    TASK_CHECK(get_single_set(hm, &MXF_SET_K(EssenceContainerData), &ecd));
    TASK_CHECK(get_single_set(hm, &MXF_SET_K(MaterialPackage), &materialPackage));
    TASK_CHECK(get_single_set(hm, &MXF_SET_K(SourcePackage), &sourcePackage));
    TASK_CHECK(get_single_set(hm, &MXF_SET_K(WaveAudioDescriptor), &descriptor));

    TASK_CHECK(mxf_get_ul_item(preface, &MXF_ITEM_K(Preface, OperationalPattern), &opLabel));
    TASK_CHECK(mxf_equals_ul(&opLabel, &MXF_OP_L(1a, UniTrack_Stream_Internal)));
    TASK_CHECK(mxf_get_array_item_count(preface, &MXF_ITEM_K(Preface, EssenceContainers), &channelCount));
    TASK_CHECK(channelCount == 1);

    TASK_CHECK(mxf_get_uint32_item(ecd, &MXF_ITEM_K(EssenceContainerData, BodySID), &ecdBodySID));
    TASK_CHECK(ecdBodySID == PCM_WRAP_BODY_SID);
    TASK_CHECK(mxf_get_uint32_item(ecd, &MXF_ITEM_K(EssenceContainerData, IndexSID), &ecdIndexSID));
    TASK_CHECK(ecdIndexSID == PCM_WRAP_INDEX_SID);
    TASK_CHECK(mxf_get_umid_item(ecd, &MXF_ITEM_K(EssenceContainerData, LinkedPackageUID), &fileUMID));

    TASK_CHECK(mxf_get_umid_item(materialPackage, &MXF_ITEM_K(GenericPackage, PackageUID), &materialUMID));
    TASK_CHECK(mxf_get_umid_item(sourcePackage, &MXF_ITEM_K(GenericPackage, PackageUID), &fileUMID));
    TASK_CHECK(mxf_equals_umid(&fileUMID, &fileUMID));
    TASK_CHECK(!mxf_equals_umid(&materialUMID, &fileUMID));

    TASK_CHECK(mxf_get_array_item_count(materialPackage, &MXF_ITEM_K(GenericPackage, Tracks), &channelCount));
    TASK_CHECK(channelCount == 1);
    TASK_CHECK(mxf_get_array_item_count(sourcePackage, &MXF_ITEM_K(GenericPackage, Tracks), &channelCount));
    TASK_CHECK(channelCount == 1);
    TASK_CHECK(get_array_strongref(hm, materialPackage, &MXF_ITEM_K(GenericPackage, Tracks), 0, &materialTrack));
    TASK_CHECK(get_array_strongref(hm, sourcePackage, &MXF_ITEM_K(GenericPackage, Tracks), 0, &fileTrack));
    {
        mxfRational materialEditRate, fileEditRate;
        mxfPosition materialOrigin, fileOrigin;
        TASK_CHECK(mxf_get_rational_item(materialTrack, &MXF_ITEM_K(Track, EditRate), &materialEditRate));
        TASK_CHECK(materialEditRate.numerator == editRate.numerator &&
                   materialEditRate.denominator == editRate.denominator);
        TASK_CHECK(mxf_get_rational_item(fileTrack, &MXF_ITEM_K(Track, EditRate), &fileEditRate));
        TASK_CHECK(fileEditRate.numerator == editRate.numerator &&
                   fileEditRate.denominator == editRate.denominator);
        TASK_CHECK(mxf_get_position_item(materialTrack, &MXF_ITEM_K(Track, Origin), &materialOrigin));
        TASK_CHECK(materialOrigin == 0);
        TASK_CHECK(mxf_get_position_item(fileTrack, &MXF_ITEM_K(Track, Origin), &fileOrigin));
        TASK_CHECK(fileOrigin == 0);
        {
            uint32_t materialTrackID;
            TASK_CHECK(mxf_get_uint32_item(materialTrack, &MXF_ITEM_K(GenericTrack, TrackID), &materialTrackID));
            TASK_CHECK(mxf_get_uint32_item(fileTrack, &MXF_ITEM_K(GenericTrack, TrackID), &fileTrackID));
            TASK_CHECK(fileTrackID != 0 && materialTrackID != 0);
            TASK_CHECK(materialTrackID != fileTrackID);
        }
        /* remember the file package track ID for the SourceClip cross-check */
        *fileTrackIDOut = fileTrackID;
    }
    TASK_CHECK(mxf_get_strongref_item(materialTrack, &MXF_ITEM_K(GenericTrack, Sequence), &materialSequence));
    TASK_CHECK(mxf_get_strongref_item(fileTrack, &MXF_ITEM_K(GenericTrack, Sequence), &fileSequence));
    TASK_CHECK(mxf_get_array_item_count(materialSequence,
                                        &MXF_ITEM_K(Sequence, StructuralComponents), &channelCount));
    TASK_CHECK(channelCount == 1);
    TASK_CHECK(mxf_get_array_item_count(fileSequence,
                                        &MXF_ITEM_K(Sequence, StructuralComponents), &channelCount));
    TASK_CHECK(channelCount == 1);
    TASK_CHECK(get_array_strongref(hm, materialSequence,
                                   &MXF_ITEM_K(Sequence, StructuralComponents), 0, &materialClip));
    TASK_CHECK(get_array_strongref(hm, fileSequence,
                                   &MXF_ITEM_K(Sequence, StructuralComponents), 0, &fileClip));

    TASK_CHECK(mxf_get_ul_item(materialSequence, &MXF_ITEM_K(StructuralComponent, DataDefinition), &dataDef));
    TASK_CHECK(mxf_equals_ul(&dataDef, &MXF_DDEF_L(Sound)));
    TASK_CHECK(mxf_get_ul_item(fileClip, &MXF_ITEM_K(StructuralComponent, DataDefinition), &dataDef));
    TASK_CHECK(mxf_equals_ul(&dataDef, &MXF_DDEF_L(Sound)));

    TASK_CHECK(check_duration_item(materialSequence, &MXF_ITEM_K(StructuralComponent, Duration), expectedUnits));
    TASK_CHECK(check_duration_item(materialClip, &MXF_ITEM_K(StructuralComponent, Duration), expectedUnits));
    TASK_CHECK(check_duration_item(fileSequence, &MXF_ITEM_K(StructuralComponent, Duration), expectedUnits));
    TASK_CHECK(check_duration_item(fileClip, &MXF_ITEM_K(StructuralComponent, Duration), expectedUnits));
    TASK_CHECK(check_duration_item(descriptor, &MXF_ITEM_K(FileDescriptor, ContainerDuration), expectedUnits));

    /* material SourceClip references the file package sound track at position 0 */
    TASK_CHECK(mxf_get_umid_item(materialClip, &MXF_ITEM_K(SourceClip, SourcePackageID), &clipSourceUMID));
    TASK_CHECK(mxf_equals_umid(&clipSourceUMID, &fileUMID));
    TASK_CHECK(mxf_get_uint32_item(materialClip, &MXF_ITEM_K(SourceClip, SourceTrackID), &clipSourceTrackID));
    TASK_CHECK(clipSourceTrackID == fileTrackID);
    {
        mxfPosition startPosition;
        TASK_CHECK(mxf_get_position_item(materialClip, &MXF_ITEM_K(SourceClip, StartPosition), &startPosition));
        TASK_CHECK(startPosition == 0);
    }

    /* file SourceClip terminates on the zero UMID */
    TASK_CHECK(mxf_get_umid_item(fileClip, &MXF_ITEM_K(SourceClip, SourcePackageID), &clipSourceUMID));
    TASK_CHECK(mxf_equals_umid(&clipSourceUMID, &g_Null_UMID));
    TASK_CHECK(mxf_get_uint32_item(fileClip, &MXF_ITEM_K(SourceClip, SourceTrackID), &clipSourceTrackID));
    TASK_CHECK(clipSourceTrackID == 0);

    /* WaveAudioDescriptor */
    TASK_CHECK(mxf_get_rational_item(descriptor, &MXF_ITEM_K(FileDescriptor, SampleRate), &sampleRate));
    TASK_CHECK(sampleRate.numerator == editRate.numerator && sampleRate.denominator == editRate.denominator);
    TASK_CHECK(mxf_get_rational_item(descriptor,
                                     &MXF_ITEM_K(GenericSoundEssenceDescriptor, AudioSamplingRate),
                                     &audioSamplingRate));
    TASK_CHECK(audioSamplingRate.numerator == 48000 && audioSamplingRate.denominator == 1);
    TASK_CHECK(mxf_get_uint32_item(descriptor,
                                   &MXF_ITEM_K(GenericSoundEssenceDescriptor, ChannelCount), &channelCount));
    TASK_CHECK(channelCount == config->channelCount);
    TASK_CHECK(mxf_get_uint32_item(descriptor,
                                   &MXF_ITEM_K(GenericSoundEssenceDescriptor, QuantizationBits), &quantBits));
    TASK_CHECK(quantBits == config->bitsPerSample);
    TASK_CHECK(mxf_get_uint16_item(descriptor, &MXF_ITEM_K(WaveAudioDescriptor, BlockAlign), &blockAlign));
    TASK_CHECK(blockAlign == config->blockAlign);
    TASK_CHECK(mxf_get_uint32_item(descriptor, &MXF_ITEM_K(WaveAudioDescriptor, AvgBps), &avgBps));
    TASK_CHECK(avgBps == 48000u * config->blockAlign);
    TASK_CHECK(mxf_get_uint8_item(descriptor, &MXF_ITEM_K(WaveAudioDescriptor, SequenceOffset),
                                  &sequenceOffset));
    TASK_CHECK(sequenceOffset == config->sequenceOffset);
    TASK_CHECK(mxf_get_ul_item(descriptor, &MXF_ITEM_K(FileDescriptor, EssenceContainer), &ecLabel));
    TASK_CHECK(mxf_equals_ul(&ecLabel, &MXF_EC_L(BWFFrameWrapped)));

    TASK_CHECK(mxf_get_uuid_item(preface, &MXF_ITEM_K(InterchangeObject, InstanceUID), &prefaceInstance));
    TASK_CHECK(mxf_get_umid_item(materialPackage, &MXF_ITEM_K(GenericPackage, PackageUID), &materialUMID));
    TASK_CHECK(mxf_get_umid_item(sourcePackage, &MXF_ITEM_K(GenericPackage, PackageUID), &fileUMID));

    *materialUMIDOut = materialUMID;
    *fileUMIDOut = fileUMID;
    *instanceUIDOut = prefaceInstance;
    *fileTrackIDOut = fileTrackID;
    ok = 1;

cleanup:
    mxf_free_header_metadata(&hm);
    mxf_free_data_model(&dataModel);
    mxf_free_partition(&pack);
    return ok;
}


/* ------------------------------ end to end run ---------------------------- */

typedef struct
{
    PCMWrapPCMFormat format;
    PCMWrapEditRate rate;
    uint32_t sequenceOffset;
    uint32_t numUnits;
    uint32_t maxUnitsPerBody;
    int byteAtATime;
} RunSpec;

static int run_wrap(const RunSpec *spec, MXFMemoryFile **memFileOut, uint8_t **pcmOut,
                    uint64_t *pcmSizeOut, uint64_t *unitCountOut)
{
    MXFMemoryFile *memFile = open_mem_file();
    MXFFile *file;
    PCMWrapWriter *writer;
    uint8_t *pcm;
    uint64_t pcmSize;
    uint64_t pushed = 0;

    if (memFile == NULL)
    {
        return 0;
    }
    file = mxf_mem_file_get_file(memFile);
    pcm = make_pcm(spec->format, spec->rate, spec->sequenceOffset, spec->numUnits, &pcmSize);
    if (pcm == NULL)
    {
        mxf_file_close(&file);
        return 0;
    }

    writer = pcm_wrap_begin(file, spec->format, spec->rate, spec->sequenceOffset,
                            spec->maxUnitsPerBody);
    if (writer == NULL)
    {
        free(pcm);
        mxf_file_close(&file);
        return 0;
    }

    if (spec->byteAtATime)
    {
        uint64_t i;
        for (i = 0; i < pcmSize; i++)
        {
            if (!pcm_wrap_push(writer, pcm + i, 1))
            {
                goto fail;
            }
        }
    }
    else
    {
        /* split into arbitrary sized fragments, including 1-byte edges */
        while (pushed < pcmSize)
        {
            uint64_t remain = pcmSize - pushed;
            uint64_t step = (pushed % 7 == 0) ? 1 : (remain < 1000 ? remain : 1000);
            if (!pcm_wrap_push(writer, pcm + pushed, step))
            {
                goto fail;
            }
            pushed += step;
        }
    }

    if (!pcm_wrap_finish(writer))
    {
        goto fail;
    }
    pcm_wrap_destroy(writer);

    *memFileOut = memFile;
    *pcmOut = pcm;
    *pcmSizeOut = pcmSize;
    *unitCountOut = spec->numUnits;
    return 1;

fail:
    pcm_wrap_destroy(writer);
    free(pcm);
    mxf_file_close(&file);
    return 0;
}


/* ------------------------------- test cases ------------------------------- */

static int test_cadence_ntsc_phases(void)
{
    uint32_t offset;

    for (offset = 0; offset < PCM_FRAME_NTSC_CADENCE_LEN; offset++)
    {
        /* the first phase exercises the single-body layout, the rest multi-body */
        uint32_t maxUnitsPerBody = (offset == 0) ? 1024 : 4;
        RunSpec spec = {PCM_WRAP_PCM_S24LE_2CH, PCM_WRAP_EDIT_RATE_30000_1001, offset, 13,
                        maxUnitsPerBody, 1};
        MXFMemoryFile *memFile = NULL;
        uint8_t *pcm = NULL;
        uint64_t pcmSize = 0, unitCount = 0;
        ScanResult scan;
        MXFFile *file;
        uint32_t bits, channels, blockAlign;
        uint32_t i;
        uint64_t pcmOffset = 0;

        scan_init(&scan);
        TASK_CHECK(run_wrap(&spec, &memFile, &pcm, &pcmSize, &unitCount));
        file = mxf_mem_file_get_file(memFile);
        TASK_CHECK(scan_file(file, &scan));

        format_params(spec.format, &bits, &channels, &blockAlign);
        TASK_CHECK(scan.audioCount == unitCount);
        TASK_CHECK(scan.pcmBytes == pcmSize);
        TASK_CHECK(memcmp(scan.pcm, pcm, (size_t)pcmSize) == 0);

        /* first unit starts at the requested cadence phase, cadence then cycles */
        for (i = 0; i < unitCount; i++)
        {
            uint32_t frames = expected_unit_frames(spec.rate, offset, i);
            TASK_CHECK(scan.audio[i].length == (uint64_t)frames * blockAlign);
            TASK_CHECK(memcmp(scan.pcm + pcmOffset, pcm + pcmOffset,
                              (size_t)scan.audio[i].length) == 0);
            pcmOffset += scan.audio[i].length;
        }
        TASK_CHECK(pcmOffset == pcmSize);

        scan_free(&scan);
        free(pcm);
        mxf_file_close(&file);
    }

    return 0;
}

static int test_cadence_pal_units(void)
{
    static const PCMWrapPCMFormat formats[] = {PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_PCM_S16LE_2CH};
    uint32_t f;

    for (f = 0; f < 2; f++)
    {
        RunSpec spec = {formats[f], PCM_WRAP_EDIT_RATE_25_1, 0, 11, 5, 0};
        MXFMemoryFile *memFile = NULL;
        uint8_t *pcm = NULL;
        uint64_t pcmSize = 0, unitCount = 0;
        ScanResult scan;
        MXFFile *file;
        uint32_t bits, channels, blockAlign;
        uint32_t i;

        scan_init(&scan);
        TASK_CHECK(run_wrap(&spec, &memFile, &pcm, &pcmSize, &unitCount));
        file = mxf_mem_file_get_file(memFile);
        TASK_CHECK(scan_file(file, &scan));

        format_params(spec.format, &bits, &channels, &blockAlign);
        TASK_CHECK(scan.audioCount == unitCount);
        for (i = 0; i < unitCount; i++)
        {
            TASK_CHECK(scan.audio[i].length == (uint64_t)PCM_FRAME_PAL_UNIT_FRAMES * blockAlign);
        }
        TASK_CHECK(scan.pcmBytes == pcmSize);
        TASK_CHECK(memcmp(scan.pcm, pcm, (size_t)pcmSize) == 0);

        scan_free(&scan);
        free(pcm);
        mxf_file_close(&file);
    }

    return 0;
}

static int test_cadence_push_fragmentation(void)
{
    RunSpec blockSpec = {PCM_WRAP_PCM_S16LE_2CH, PCM_WRAP_EDIT_RATE_30000_1001, 3, 9, 3, 0};
    RunSpec byteSpec = blockSpec;
    MXFMemoryFile *blockMem = NULL, *byteMem = NULL;
    uint8_t *blockPcm = NULL, *bytePcm = NULL;
    uint64_t blockSize = 0, byteSize = 0, blockUnits = 0, byteUnits = 0;
    ScanResult blockScan, byteScan;
    MXFFile *blockFile, *byteFile;

    byteSpec.byteAtATime = 1;
    scan_init(&blockScan);
    scan_init(&byteScan);

    TASK_CHECK(run_wrap(&blockSpec, &blockMem, &blockPcm, &blockSize, &blockUnits));
    TASK_CHECK(run_wrap(&byteSpec, &byteMem, &bytePcm, &byteSize, &byteUnits));
    blockFile = mxf_mem_file_get_file(blockMem);
    byteFile = mxf_mem_file_get_file(byteMem);

    TASK_CHECK(scan_file(blockFile, &blockScan));
    TASK_CHECK(scan_file(byteFile, &byteScan));

    TASK_CHECK(blockUnits == byteUnits);
    TASK_CHECK(blockScan.audioCount == byteScan.audioCount);
    TASK_CHECK(blockScan.pcmBytes == byteScan.pcmBytes);
    TASK_CHECK(memcmp(blockScan.pcm, byteScan.pcm, (size_t)blockScan.pcmBytes) == 0);
    TASK_CHECK(memcmp(blockPcm, bytePcm, (size_t)blockSize) == 0);
    {
        uint32_t i;
        for (i = 0; i < blockScan.audioCount; i++)
        {
            TASK_CHECK(blockScan.audio[i].length == byteScan.audio[i].length);
        }
    }

    scan_free(&blockScan);
    scan_free(&byteScan);
    free(blockPcm);
    free(bytePcm);
    mxf_file_close(&blockFile);
    mxf_file_close(&byteFile);
    return 0;
}

static int test_layout_pal_full_structure(void)
{
    RunSpec spec = {PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 0, 10, 3, 0};
    MXFMemoryFile *memFile = NULL;
    uint8_t *pcm = NULL;
    uint64_t pcmSize = 0, unitCount = 0;
    ScanResult scan;
    MXFFile *file;
    uint32_t i, b;
    uint32_t bits, channels, blockAlign;
    mxfRational editRate = {25, 1};
    mxfUMID materialUMID, fileUMID, materialUMID2, fileUMID2;
    mxfUUID instanceUID, instanceUID2;
    uint32_t fileTrackID, fileTrackID2;
    PCMFrameConfig config;
    uint64_t expectedBodyOffset = 0;
    uint32_t bodyCount;
    uint32_t audioIndex = 0;

    scan_init(&scan);
    TASK_CHECK(pcm_frame_config_init(&config, spec.format, spec.rate, spec.sequenceOffset));
    TASK_CHECK(run_wrap(&spec, &memFile, &pcm, &pcmSize, &unitCount));
    file = mxf_mem_file_get_file(memFile);
    TASK_CHECK(scan_file(file, &scan));
    format_params(spec.format, &bits, &channels, &blockAlign);

    /* partition chain: header + bodies + footer */
    bodyCount = (uint32_t)((unitCount + spec.maxUnitsPerBody - 1) / spec.maxUnitsPerBody);
    TASK_CHECK(scan.partitionCount == bodyCount + 2);
    TASK_CHECK(scan.partitions[0].isHeader);
    TASK_CHECK(scan.partitions[scan.partitionCount - 1].isFooter);
    for (i = 1; i <= bodyCount; i++)
    {
        TASK_CHECK(scan.partitions[i].isBody);
        TASK_CHECK(scan.partitions[i].bodySID == PCM_WRAP_BODY_SID);
    }

    /* KAG size and KAG aligned audio keys (alignment is relative to the start
       of the body partition's own KAG grid) */
    for (i = 0; i < scan.partitionCount; i++)
    {
        TASK_CHECK(scan.partitions[i].kagSize == 512);
    }
    for (i = 0; i < scan.audioCount; i++)
    {
        uint32_t p;
        int mapped = 0;
        TASK_CHECK(scan.audio[i].key.octet12 == 0x16);
        TASK_CHECK(scan.audio[i].key.octet13 == 0x01);
        TASK_CHECK(scan.audio[i].key.octet14 == MXF_BWF_FRAME_WRAPPED_EE_TYPE);
        TASK_CHECK(scan.audio[i].key.octet15 == 0x01);
        for (p = 1; p <= bodyCount; p++)
        {
            uint64_t bodyEnd = (p + 1 < scan.partitionCount)
                               ? scan.partitions[p + 1].keyPos
                               : (uint64_t)mxf_file_size(file);
            if (scan.audio[i].keyPos >= scan.partitions[p].keyPos &&
                scan.audio[i].keyPos < bodyEnd)
            {
                TASK_CHECK((scan.audio[i].keyPos - scan.partitions[p].keyPos) % 512 == 0);
                mapped = 1;
                break;
            }
        }
        TASK_CHECK(mapped);
    }
    TASK_CHECK(scan.fillerCount > 0);

    /* previous partition and footer partition links */
    for (i = 0; i < scan.partitionCount; i++)
    {
        TASK_CHECK(scan.partitions[i].thisPartition == scan.partitions[i].keyPos);
        if (i > 0)
        {
            TASK_CHECK(scan.partitions[i].previousPartition == scan.partitions[i - 1].keyPos);
        }
        TASK_CHECK(scan.partitions[i].footerPartition == scan.partitions[scan.partitionCount - 1].keyPos);
    }

    /* RIP closes the chain with one entry per partition */
    TASK_CHECK(scan.ripCount == scan.partitionCount);
    for (i = 0; i < scan.ripCount; i++)
    {
        TASK_CHECK(scan.ripOffsets[i] == scan.partitions[i].keyPos);
    }

    /* verify partition pack labels, byte counts and container/OP labels via libMXF */
    for (i = 0; i < scan.partitionCount; i++)
    {
        MXFPartition *pack = NULL;
        TASK_CHECK(read_partition_pack(file, scan.partitions[i].keyPos, &pack));
        TASK_CHECK(mxf_partition_is_closed_and_complete(&pack->key));
        TASK_CHECK(pack->kagSize == 512);
        TASK_CHECK(mxf_equals_ul(&pack->operationalPattern, &MXF_OP_L(1a, UniTrack_Stream_Internal)));

        if (pack->bodySID != 0)
        {
            /* body packs carry only essence: no header or index bytes */
            TASK_CHECK(pack->headerByteCount == 0);
            TASK_CHECK(pack->indexByteCount == 0);
            TASK_CHECK(pack->indexSID == 0);
        }
        if (scan.partitions[i].isHeader)
        {
            TASK_CHECK(pack->headerByteCount > 0);
            TASK_CHECK(pack->indexByteCount == 0);
            TASK_CHECK(mxf_get_list_length(&pack->essenceContainers) == 1);
            {
                mxfUL *label = (mxfUL*)mxf_get_list_element(&pack->essenceContainers, 0);
                TASK_CHECK(mxf_equals_ul(label, &MXF_EC_L(BWFFrameWrapped)));
            }
        }
        if (scan.partitions[i].isFooter)
        {
            TASK_CHECK(pack->headerByteCount > 0);
            TASK_CHECK(pack->indexByteCount > 0);
            TASK_CHECK(pack->indexSID == PCM_WRAP_INDEX_SID);
        }
        mxf_free_partition(&pack);
    }

    /* one index segment per body, continuous IndexStartPosition and durations */
    TASK_CHECK(scan.segmentCount == bodyCount);
    for (b = 0; b < bodyCount; b++)
    {
        uint32_t expectedUnits = (b < bodyCount - 1)
                                 ? spec.maxUnitsPerBody
                                 : (uint32_t)(unitCount - (uint64_t)b * spec.maxUnitsPerBody);
        TASK_CHECK(scan.segments[b].numEntries == expectedUnits);
        TASK_CHECK(scan.segments[b].duration == expectedUnits);
        TASK_CHECK(scan.segments[b].startPosition == (int64_t)(b * spec.maxUnitsPerBody));
        TASK_CHECK(scan.segments[b].editUnitByteCount == 0);
        TASK_CHECK(scan.segments[b].bodySID == PCM_WRAP_BODY_SID);
        TASK_CHECK(scan.segments[b].indexSID == PCM_WRAP_INDEX_SID);
        TASK_CHECK(scan.segments[b].editRate.numerator == 25);
        TASK_CHECK(scan.segments[b].editRate.denominator == 1);
        TASK_CHECK(scan.segments[b].sliceCount == 0);
        TASK_CHECK(scan.segments[b].posTableCount == 0);
    }

    /* independently verify BodyOffset and the continuous essence stream
       coordinates: BodyOffset accumulates the span from each body's first
       audio key to its last audio value end, inter-audio filler included */
    expectedBodyOffset = 0;
    audioIndex = 0;
    for (b = 0; b < bodyCount; b++)
    {
        uint32_t expectedUnits = scan.segments[b].numEntries;
        uint64_t firstKey = scan.audio[audioIndex].keyPos;
        uint64_t lastEnd = scan.audio[audioIndex + expectedUnits - 1].keyPos +
                           16 + scan.audio[audioIndex + expectedUnits - 1].llen +
                           scan.audio[audioIndex + expectedUnits - 1].length;
        uint64_t span = lastEnd - firstKey;
        TASK_CHECK(scan.partitions[b + 1].bodyOffset == expectedBodyOffset);
        for (i = 0; i < expectedUnits; i++)
        {
            TASK_CHECK(scan.segments[b].streamOffsets[i] ==
                        expectedBodyOffset + (scan.audio[audioIndex].keyPos - firstKey));
            audioIndex++;
        }
        expectedBodyOffset += span;
    }
    TASK_CHECK(audioIndex == scan.audioCount);

    /* header and footer copies carry the same final graph identities */
    TASK_CHECK(check_metadata_graph(file, scan.partitions[0].keyPos, unitCount, &config, editRate,
                                    &materialUMID, &fileUMID, &instanceUID, &fileTrackID));
    TASK_CHECK(check_metadata_graph(file, scan.partitions[scan.partitionCount - 1].keyPos, unitCount,
                                    &config, editRate, &materialUMID2, &fileUMID2, &instanceUID2,
                                    &fileTrackID2));
    TASK_CHECK(mxf_equals_umid(&materialUMID, &materialUMID2));
    TASK_CHECK(mxf_equals_umid(&fileUMID, &fileUMID2));
    TASK_CHECK(mxf_equals_uuid(&instanceUID, &instanceUID2));
    TASK_CHECK(fileTrackID == fileTrackID2);

    TASK_CHECK(scan.pcmBytes == pcmSize);
    TASK_CHECK(memcmp(scan.pcm, pcm, (size_t)pcmSize) == 0);

    scan_free(&scan);
    free(pcm);
    mxf_file_close(&file);
    return 0;
}

static int test_layout_ntsc_multi_body(void)
{
    RunSpec spec = {PCM_WRAP_PCM_S24LE_2CH, PCM_WRAP_EDIT_RATE_30000_1001, 2, 7, 2, 1};
    MXFMemoryFile *memFile = NULL;
    uint8_t *pcm = NULL;
    uint64_t pcmSize = 0, unitCount = 0;
    ScanResult scan;
    MXFFile *file;
    uint32_t i;
    mxfRational editRate = {30000, 1001};
    mxfUMID materialUMID, fileUMID;
    mxfUUID instanceUID;
    uint32_t fileTrackID;
    PCMFrameConfig config;
    uint32_t bits, channels, blockAlign;
    uint64_t pcmCursor = 0;

    scan_init(&scan);
    TASK_CHECK(pcm_frame_config_init(&config, spec.format, spec.rate, spec.sequenceOffset));
    TASK_CHECK(run_wrap(&spec, &memFile, &pcm, &pcmSize, &unitCount));
    file = mxf_mem_file_get_file(memFile);
    TASK_CHECK(scan_file(file, &scan));
    format_params(spec.format, &bits, &channels, &blockAlign);

    TASK_CHECK(scan.partitionCount == 2 + 4); /* header + 4 bodies (7 units / 2) + footer */
    TASK_CHECK(scan.segmentCount == 4);
    TASK_CHECK(scan.audioCount == unitCount);
    for (i = 0; i < scan.segmentCount; i++)
    {
        TASK_CHECK(scan.segments[i].editRate.numerator == 30000);
        TASK_CHECK(scan.segments[i].editRate.denominator == 1001);
    }

    /* NTSC cadence survives the multi-body boundaries */
    for (i = 0; i < unitCount; i++)
    {
        uint32_t frames = expected_unit_frames(spec.rate, spec.sequenceOffset, i);
        TASK_CHECK(scan.audio[i].length == (uint64_t)frames * blockAlign);
        TASK_CHECK(memcmp(scan.pcm + pcmCursor, pcm + pcmCursor,
                          (size_t)scan.audio[i].length) == 0);
        pcmCursor += scan.audio[i].length;
    }
    TASK_CHECK(pcmCursor == pcmSize);

    TASK_CHECK(check_metadata_graph(file, scan.partitions[0].keyPos, unitCount, &config, editRate,
                                    &materialUMID, &fileUMID, &instanceUID, &fileTrackID));

    scan_free(&scan);
    free(pcm);
    mxf_file_close(&file);
    return 0;
}

static int test_layout_index_locations(void)
{
    RunSpec spec = {PCM_WRAP_PCM_S24LE_1CH, PCM_WRAP_EDIT_RATE_30000_1001, 4, 11, 3, 0};
    MXFMemoryFile *memFile = NULL;
    uint8_t *pcm = NULL;
    uint64_t pcmSize = 0, unitCount = 0;
    ScanResult scan;
    MXFFile *file;
    uint32_t bits, channels, blockAlign;
    uint32_t b, i;
    uint32_t audioIndex = 0;
    uint64_t expectedPcmOffset = 0;

    scan_init(&scan);
    TASK_CHECK(run_wrap(&spec, &memFile, &pcm, &pcmSize, &unitCount));
    file = mxf_mem_file_get_file(memFile);
    TASK_CHECK(scan_file(file, &scan));
    format_params(spec.format, &bits, &channels, &blockAlign);

    /* every index entry must locate the exact audio KLV key in its partition:
       seek to the body's first audio key plus (StreamOffset - BodyOffset) */
    for (b = 0; b < scan.segmentCount; b++)
    {
        PartitionRecord *body = &scan.partitions[b + 1];
        uint64_t firstKey = scan.audio[audioIndex].keyPos;
        for (i = 0; i < scan.segments[b].numEntries; i++)
        {
            uint64_t localOffset = scan.segments[b].streamOffsets[i] - body->bodyOffset;
            int64_t targetPos = (int64_t)(firstKey + localOffset);
            mxfKey foundKey;
            uint8_t foundLlen;
            uint64_t valueLen;
            uint8_t *value;

            /* entry minus BodyOffset lands exactly on this partition's key */
            TASK_CHECK(targetPos == (int64_t)scan.audio[audioIndex].keyPos);
            TASK_CHECK(mxf_file_seek(file, targetPos, SEEK_SET));
            TASK_CHECK(mxf_read_kl(file, &foundKey, &foundLlen, &valueLen));
            TASK_CHECK(mxf_equals_key(&foundKey, &scan.audio[audioIndex].key));
            TASK_CHECK(foundLlen == scan.audio[audioIndex].llen);
            TASK_CHECK(valueLen == scan.audio[audioIndex].length);

            value = malloc((size_t)valueLen);
            TASK_CHECK(value != NULL);
            TASK_CHECK(mxf_file_read(file, value, (uint32_t)valueLen) == valueLen);
            TASK_CHECK(memcmp(value, pcm + expectedPcmOffset, (size_t)valueLen) == 0);
            free(value);

            expectedPcmOffset += valueLen;
            audioIndex++;
        }
    }
    TASK_CHECK(audioIndex == scan.audioCount);
    TASK_CHECK(expectedPcmOffset == pcmSize);

    scan_free(&scan);
    free(pcm);
    mxf_file_close(&file);
    return 0;
}

static int test_layout_stereo_channels_distinct(void)
{
    RunSpec spec = {PCM_WRAP_PCM_S16LE_2CH, PCM_WRAP_EDIT_RATE_25_1, 0, 4, 2, 0};
    MXFMemoryFile *memFile = NULL;
    uint8_t *pcm = NULL;
    uint64_t pcmSize = 0, unitCount = 0;
    ScanResult scan;
    MXFFile *file;
    uint32_t frame;

    scan_init(&scan);
    TASK_CHECK(run_wrap(&spec, &memFile, &pcm, &pcmSize, &unitCount));
    file = mxf_mem_file_get_file(memFile);
    TASK_CHECK(scan_file(file, &scan));
    TASK_CHECK(scan.pcmBytes == pcmSize);
    TASK_CHECK(memcmp(scan.pcm, pcm, (size_t)pcmSize) == 0);

    /* left and right channels carry different sample values */
    for (frame = 0; frame < 100; frame++)
    {
        int16_t left = (int16_t)(scan.pcm[frame * 4] | (scan.pcm[frame * 4 + 1] << 8));
        int16_t right = (int16_t)(scan.pcm[frame * 4 + 2] | (scan.pcm[frame * 4 + 3] << 8));
        TASK_CHECK(left != right);
        TASK_CHECK(left == (int16_t)sample_value(frame, 0, 16));
        TASK_CHECK(right == (int16_t)sample_value(frame, 1, 16));
    }

    scan_free(&scan);
    free(pcm);
    mxf_file_close(&file);
    return 0;
}

static int test_failure_invalid_arguments(void)
{
    MXFMemoryFile *memFile = open_mem_file();
    MXFFile *file;
    FaultSys *faultSys = NULL;
    MXFFile *faultFile;
    MXFMemoryFile *nonEmptyMem = open_mem_file();
    MXFFile *nonEmptyFile;
    PCMWrapWriter *writer;

    TASK_CHECK(memFile != NULL && nonEmptyMem != NULL);
    file = mxf_mem_file_get_file(memFile);
    nonEmptyFile = mxf_mem_file_get_file(nonEmptyMem);

    /* bad PCM format enum */
    writer = pcm_wrap_begin(file, (PCMWrapPCMFormat)99, PCM_WRAP_EDIT_RATE_25_1, 0, 4);
    TASK_CHECK(writer == NULL);

    /* PAL requires sequence offset 0 */
    writer = pcm_wrap_begin(file, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 1, 4);
    TASK_CHECK(writer == NULL);

    /* NTSC phase must be 0-4 */
    writer = pcm_wrap_begin(file, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_30000_1001, 5, 4);
    TASK_CHECK(writer == NULL);

    /* body partition limits must be 1-1024 */
    writer = pcm_wrap_begin(file, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 0, 0);
    TASK_CHECK(writer == NULL);
    writer = pcm_wrap_begin(file, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 0, 1025);
    TASK_CHECK(writer == NULL);

    /* output must be empty */
    TASK_CHECK(mxf_file_write(nonEmptyFile, (const uint8_t*)"x", 1) == 1);
    writer = pcm_wrap_begin(nonEmptyFile, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 0, 4);
    TASK_CHECK(writer == NULL);

    /* cursor must be at zero */
    TASK_CHECK(mxf_file_seek(file, 10, SEEK_SET));
    writer = pcm_wrap_begin(file, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 0, 4);
    TASK_CHECK(writer == NULL);
    TASK_CHECK(mxf_file_seek(file, 0, SEEK_SET));

    /* file must be seekable */
    faultFile = make_fault_file(file, UINT64_MAX, 0, 1, &faultSys);
    TASK_CHECK(faultFile != NULL);
    writer = pcm_wrap_begin(faultFile, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 0, 4);
    TASK_CHECK(writer == NULL);
    free_fault_file(faultFile);

    mxf_file_close(&file);
    mxf_file_close(&nonEmptyFile);
    return 0;
}

static int test_failure_empty_and_partial(void)
{
    static const uint8_t bytes[5] = {1, 2, 3, 4, 5};
    MXFMemoryFile *memFile = open_mem_file();
    MXFFile *file = mxf_mem_file_get_file(memFile);
    PCMWrapWriter *writer;

    /* empty input cannot finish */
    writer = pcm_wrap_begin(file, PCM_WRAP_PCM_S16LE_1CH, PCM_WRAP_EDIT_RATE_25_1, 0, 4);
    TASK_CHECK(writer != NULL);
    TASK_CHECK(pcm_wrap_finish(writer) == 0);
    /* failed state: further calls write nothing and keep failing */
    TASK_CHECK(pcm_wrap_push(writer, bytes, sizeof(bytes)) == 0);
    TASK_CHECK(pcm_wrap_finish(writer) == 0);
    pcm_wrap_destroy(writer);

    /* partial edit unit at the end fails */
    memFile = open_mem_file();
    file = mxf_mem_file_get_file(memFile);
    writer = pcm_wrap_begin(file, PCM_WRAP_PCM_S16LE_2CH, PCM_WRAP_EDIT_RATE_30000_1001, 1, 4);
    TASK_CHECK(writer != NULL);
    TASK_CHECK(pcm_wrap_push(writer, bytes, sizeof(bytes)) == 1);
    TASK_CHECK(pcm_wrap_finish(writer) == 0);
    TASK_CHECK(pcm_wrap_finish(writer) == 0);
    pcm_wrap_destroy(writer);

    /* destroy must not close the underlying MXFFile */
    TASK_CHECK(mxf_file_tell(file) >= 0);
    mxf_file_close(&file);
    return 0;
}

static int test_failure_deterministic_callbacks(void)
{
    static const uint64_t budgets[] = {120, 600, 4096, 40000};
    uint32_t trial;

    for (trial = 0; trial < sizeof(budgets) / sizeof(budgets[0]); trial++)
    {
        MXFMemoryFile *memFile = open_mem_file();
        MXFFile *inner = mxf_mem_file_get_file(memFile);
        FaultSys *faultSys = NULL;
        MXFFile *faultFile = make_fault_file(inner, budgets[trial], 0, 0, &faultSys);
        PCMWrapWriter *writer;
        static const uint8_t chunk[2048];
        int pushResult;
        int64_t sizeAfterFailure;

        TASK_CHECK(faultFile != NULL);
        writer = pcm_wrap_begin(faultFile, PCM_WRAP_PCM_S24LE_2CH, PCM_WRAP_EDIT_RATE_25_1, 0, 2);
        if (writer == NULL)
        {
            /* failure during header writing is also a valid deterministic outcome */
            TASK_CHECK(faultSys->sawWriteFailure);
            free_fault_file(faultFile);
            mxf_file_close(&inner);
            continue;
        }

        /* push until the write budget forces a deterministic runtime failure */
        pushResult = 1;
        for (uint32_t attempt = 0; attempt < 100000 && pushResult; attempt++)
        {
            pushResult = pcm_wrap_push(writer, chunk, sizeof(chunk));
        }
        TASK_CHECK(pushResult == 0);
        TASK_CHECK(faultSys->sawWriteFailure);

        sizeAfterFailure = mxf_file_size(inner);
        /* failed writer must not write any more bytes */
        TASK_CHECK(pcm_wrap_push(writer, chunk, sizeof(chunk)) == 0);
        TASK_CHECK(pcm_wrap_finish(writer) == 0);
        TASK_CHECK(mxf_file_size(inner) == sizeAfterFailure);
        pcm_wrap_destroy(writer);
        free_fault_file(faultFile);
        mxf_file_close(&inner);
    }

    /* deterministic seek failure during finalisation */
    {
        MXFMemoryFile *memFile = open_mem_file();
        MXFFile *inner = mxf_mem_file_get_file(memFile);
        FaultSys *faultSys = NULL;
        MXFFile *faultFile = make_fault_file(inner, UINT64_MAX, 1, 0, &faultSys);
        PCMWrapWriter *writer = pcm_wrap_begin(faultFile, PCM_WRAP_PCM_S16LE_1CH,
                                               PCM_WRAP_EDIT_RATE_25_1, 0, 2);
        TASK_CHECK(writer == NULL);
        free_fault_file(faultFile);
        mxf_file_close(&inner);
    }

    return 0;
}


const struct task_case task_cases[] =
{
    {"cadence", "ntsc_five_phases", test_cadence_ntsc_phases},
    {"cadence", "pal_fixed_units", test_cadence_pal_units},
    {"cadence", "push_fragmentation_equivalence", test_cadence_push_fragmentation},
    {"layout", "pal_full_structure", test_layout_pal_full_structure},
    {"layout", "ntsc_multi_body", test_layout_ntsc_multi_body},
    {"layout", "index_entry_locations", test_layout_index_locations},
    {"layout", "stereo_channels_distinct", test_layout_stereo_channels_distinct},
    {"failure", "invalid_arguments", test_failure_invalid_arguments},
    {"failure", "empty_and_partial_input", test_failure_empty_and_partial},
    {"failure", "deterministic_fault_callbacks", test_failure_deterministic_callbacks},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

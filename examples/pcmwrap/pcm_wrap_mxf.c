/*
 * Audio-only OP1a MXF partition, metadata and index writing
 *
 * Copyright (C) 2026
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the British Broadcasting Corporation nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>

#include <mxf/mxf.h>
#include <mxf/mxf_macros.h>

#include "pcm_wrap_mxf.h"


#define MATERIAL_TRACK_ID       1
#define SOURCE_TRACK_ID         2


static const mxfKey BWF_FRAME_AUDIO_ELEMENT_KEY =
    MXF_AES3BWF_EE_K(1, MXF_BWF_FRAME_WRAPPED_EE_TYPE, 1);


typedef struct
{
    MXFPartition *partition;
    uint32_t startUnitIndex;        /* global index of the first unit in this body */
    uint32_t unitCount;
    uint64_t firstAudioKeyFilePos;  /* file position of the first audio KLV key */
    uint64_t lastValueEndFilePos;   /* file position after the last audio value */
} BodyRecord;


struct PCMMxfMuxer
{
    MXFFile *file;
    PCMFrameConfig config;
    uint32_t maxUnitsPerBody;

    MXFDataModel *dataModel;
    MXFHeaderMetadata *headerMetadata;
    MXFFilePartitions *partitions;
    MXFPartition *headerPartition;
    MXFPartition *currentBody;

    MXFMetadataSet *materialSequence;
    MXFMetadataSet *materialClip;
    MXFMetadataSet *fileSequence;
    MXFMetadataSet *fileClip;
    MXFMetadataSet *descriptor;

    mxfUMID materialUMID;
    mxfUMID fileUMID;
    mxfKey audioElementKey;

    int64_t headerMetadataStart;
    int64_t headerMetadataEnd;

    uint64_t *keyFilePositions;     /* audio key file position per completed unit */
    uint32_t keyPositionCount;
    uint32_t keyPositionCapacity;

    BodyRecord *bodies;
    uint32_t numBodies;
    uint32_t bodyCapacity;
    uint32_t unitsInCurrentBody;
};


static int grow_key_positions(PCMMxfMuxer *muxer)
{
    uint32_t newCapacity;
    uint64_t *newArray;

    if (muxer->keyPositionCount < muxer->keyPositionCapacity)
    {
        return 1;
    }

    newCapacity = muxer->keyPositionCapacity == 0 ? 64 : muxer->keyPositionCapacity * 2;
    newArray = realloc(muxer->keyFilePositions, newCapacity * sizeof(uint64_t));
    if (newArray == NULL)
    {
        return 0;
    }
    muxer->keyFilePositions = newArray;
    muxer->keyPositionCapacity = newCapacity;
    return 1;
}

static int grow_bodies(PCMMxfMuxer *muxer)
{
    uint32_t newCapacity;
    BodyRecord *newArray;

    if (muxer->numBodies < muxer->bodyCapacity)
    {
        return 1;
    }

    newCapacity = muxer->bodyCapacity == 0 ? 4 : muxer->bodyCapacity * 2;
    newArray = realloc(muxer->bodies, newCapacity * sizeof(BodyRecord));
    if (newArray == NULL)
    {
        return 0;
    }
    muxer->bodies = newArray;
    muxer->bodyCapacity = newCapacity;
    return 1;
}

static int create_metadata(PCMMxfMuxer *muxer)
{
    MXFHeaderMetadata *hm = NULL;
    MXFMetadataSet *preface = NULL;
    MXFMetadataSet *identification = NULL;
    MXFMetadataSet *contentStorage = NULL;
    MXFMetadataSet *essenceContainerData = NULL;
    MXFMetadataSet *materialPackage = NULL;
    MXFMetadataSet *sourcePackage = NULL;
    MXFMetadataSet *materialTrack = NULL;
    MXFMetadataSet *fileTrack = NULL;
    mxfTimestamp now;
    mxfUUID generationUID;
    mxfUUID productUID = {0x05, 0x7c, 0xd8, 0x49, 0x17, 0x8a, 0x4b, 0x88,
                          0xb4, 0xc7, 0x82, 0x5a, 0xf8, 0x76, 0x1b, 0x23};
    static const mxfUTF16Char companyName[] = L"libMXF";
    static const mxfUTF16Char productName[] = L"PCM OP1a frame wrapper";
    static const mxfUTF16Char versionString[] = L"1.0";
    mxfRational audioSamplingRate = {PCM_FRAME_SAMPLE_RATE, 1};
    mxfRational editRate;
    uint16_t blockAlign;
    uint32_t avgBps;

    CHK_ORET(mxf_load_data_model(&muxer->dataModel));
    CHK_OFAIL(mxf_finalise_data_model(muxer->dataModel));
    CHK_OFAIL(mxf_create_header_metadata(&hm, muxer->dataModel));
    muxer->headerMetadata = hm;

    mxf_get_timestamp_now(&now);
    mxf_generate_uuid(&generationUID);
    mxf_generate_umid(&muxer->materialUMID);
    mxf_generate_umid(&muxer->fileUMID);

    if (muxer->config.editRate == PCM_WRAP_EDIT_RATE_25_1)
    {
        editRate.numerator = 25;
        editRate.denominator = 1;
    }
    else
    {
        editRate.numerator = 30000;
        editRate.denominator = 1001;
    }
    blockAlign = (uint16_t)muxer->config.blockAlign;
    avgBps = PCM_FRAME_SAMPLE_RATE * blockAlign;

    /* Identification */
    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(Identification), &identification));
    CHK_OFAIL(mxf_set_uuid_item(identification, &MXF_ITEM_K(Identification, ThisGenerationUID), &generationUID));
    CHK_OFAIL(mxf_set_utf16string_item(identification, &MXF_ITEM_K(Identification, CompanyName), companyName));
    CHK_OFAIL(mxf_set_utf16string_item(identification, &MXF_ITEM_K(Identification, ProductName), productName));
    CHK_OFAIL(mxf_set_utf16string_item(identification, &MXF_ITEM_K(Identification, VersionString), versionString));
    CHK_OFAIL(mxf_set_uuid_item(identification, &MXF_ITEM_K(Identification, ProductUID), &productUID));
    CHK_OFAIL(mxf_set_timestamp_item(identification, &MXF_ITEM_K(Identification, ModificationDate), &now));

    /* Preface */
    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(Preface), &preface));
    CHK_OFAIL(mxf_set_timestamp_item(preface, &MXF_ITEM_K(Preface, LastModifiedDate), &now));
    CHK_OFAIL(mxf_set_version_type_item(preface, &MXF_ITEM_K(Preface, Version), MXF_PREFACE_VER(1, 1)));
    CHK_OFAIL(mxf_set_uint32_item(preface, &MXF_ITEM_K(Preface, ObjectModelVersion), 1));
    CHK_OFAIL(mxf_set_ul_item(preface, &MXF_ITEM_K(Preface, OperationalPattern),
                              &MXF_OP_L(1a, UniTrack_Stream_Internal)));
    {
        uint8_t *essenceContainerElement = NULL;
        CHK_OFAIL(mxf_alloc_array_item_elements(preface, &MXF_ITEM_K(Preface, EssenceContainers),
                                                mxfUL_extlen, 1, &essenceContainerElement));
        mxf_set_ul(&MXF_EC_L(BWFFrameWrapped), essenceContainerElement);
    }
    CHK_OFAIL(mxf_add_array_item_strongref(preface, &MXF_ITEM_K(Preface, Identifications), identification));

    /* ContentStorage */
    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(ContentStorage), &contentStorage));
    CHK_OFAIL(mxf_set_strongref_item(preface, &MXF_ITEM_K(Preface, ContentStorage), contentStorage));

    /* EssenceContainerData: BodySID=1, IndexSID=1, linked to the file package */
    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(EssenceContainerData), &essenceContainerData));
    CHK_OFAIL(mxf_set_umid_item(essenceContainerData, &MXF_ITEM_K(EssenceContainerData, LinkedPackageUID),
                                &muxer->fileUMID));
    CHK_OFAIL(mxf_set_uint32_item(essenceContainerData, &MXF_ITEM_K(EssenceContainerData, IndexSID),
                                  PCM_WRAP_INDEX_SID));
    CHK_OFAIL(mxf_set_uint32_item(essenceContainerData, &MXF_ITEM_K(EssenceContainerData, BodySID),
                                  PCM_WRAP_BODY_SID));
    CHK_OFAIL(mxf_add_array_item_strongref(contentStorage, &MXF_ITEM_K(ContentStorage, EssenceContainerData),
                                           essenceContainerData));

    /* MaterialPackage */
    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(MaterialPackage), &materialPackage));
    CHK_OFAIL(mxf_add_array_item_strongref(contentStorage, &MXF_ITEM_K(ContentStorage, Packages), materialPackage));
    CHK_OFAIL(mxf_set_umid_item(materialPackage, &MXF_ITEM_K(GenericPackage, PackageUID), &muxer->materialUMID));
    CHK_OFAIL(mxf_set_timestamp_item(materialPackage, &MXF_ITEM_K(GenericPackage, PackageCreationDate), &now));
    CHK_OFAIL(mxf_set_timestamp_item(materialPackage, &MXF_ITEM_K(GenericPackage, PackageModifiedDate), &now));

    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(Track), &materialTrack));
    CHK_OFAIL(mxf_add_array_item_strongref(materialPackage, &MXF_ITEM_K(GenericPackage, Tracks), materialTrack));
    CHK_OFAIL(mxf_set_uint32_item(materialTrack, &MXF_ITEM_K(GenericTrack, TrackID), MATERIAL_TRACK_ID));
    CHK_OFAIL(mxf_set_uint32_item(materialTrack, &MXF_ITEM_K(GenericTrack, TrackNumber), MATERIAL_TRACK_ID));
    CHK_OFAIL(mxf_set_rational_item(materialTrack, &MXF_ITEM_K(Track, EditRate), &editRate));
    CHK_OFAIL(mxf_set_position_item(materialTrack, &MXF_ITEM_K(Track, Origin), 0));

    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(Sequence), &muxer->materialSequence));
    CHK_OFAIL(mxf_set_strongref_item(materialTrack, &MXF_ITEM_K(GenericTrack, Sequence), muxer->materialSequence));
    CHK_OFAIL(mxf_set_ul_item(muxer->materialSequence, &MXF_ITEM_K(StructuralComponent, DataDefinition),
                              &MXF_DDEF_L(Sound)));
    CHK_OFAIL(mxf_set_length_item(muxer->materialSequence, &MXF_ITEM_K(StructuralComponent, Duration), 0));

    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(SourceClip), &muxer->materialClip));
    CHK_OFAIL(mxf_add_array_item_strongref(muxer->materialSequence,
                                           &MXF_ITEM_K(Sequence, StructuralComponents), muxer->materialClip));
    CHK_OFAIL(mxf_set_ul_item(muxer->materialClip, &MXF_ITEM_K(StructuralComponent, DataDefinition),
                              &MXF_DDEF_L(Sound)));
    CHK_OFAIL(mxf_set_length_item(muxer->materialClip, &MXF_ITEM_K(StructuralComponent, Duration), 0));
    CHK_OFAIL(mxf_set_position_item(muxer->materialClip, &MXF_ITEM_K(SourceClip, StartPosition), 0));
    CHK_OFAIL(mxf_set_umid_item(muxer->materialClip, &MXF_ITEM_K(SourceClip, SourcePackageID), &muxer->fileUMID));
    CHK_OFAIL(mxf_set_uint32_item(muxer->materialClip, &MXF_ITEM_K(SourceClip, SourceTrackID), SOURCE_TRACK_ID));

    /* File SourcePackage */
    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(SourcePackage), &sourcePackage));
    CHK_OFAIL(mxf_add_array_item_strongref(contentStorage, &MXF_ITEM_K(ContentStorage, Packages), sourcePackage));
    CHK_OFAIL(mxf_set_umid_item(sourcePackage, &MXF_ITEM_K(GenericPackage, PackageUID), &muxer->fileUMID));
    CHK_OFAIL(mxf_set_timestamp_item(sourcePackage, &MXF_ITEM_K(GenericPackage, PackageCreationDate), &now));
    CHK_OFAIL(mxf_set_timestamp_item(sourcePackage, &MXF_ITEM_K(GenericPackage, PackageModifiedDate), &now));

    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(Track), &fileTrack));
    CHK_OFAIL(mxf_add_array_item_strongref(sourcePackage, &MXF_ITEM_K(GenericPackage, Tracks), fileTrack));
    CHK_OFAIL(mxf_set_uint32_item(fileTrack, &MXF_ITEM_K(GenericTrack, TrackID), SOURCE_TRACK_ID));
    CHK_OFAIL(mxf_set_uint32_item(fileTrack, &MXF_ITEM_K(GenericTrack, TrackNumber), SOURCE_TRACK_ID));
    CHK_OFAIL(mxf_set_rational_item(fileTrack, &MXF_ITEM_K(Track, EditRate), &editRate));
    CHK_OFAIL(mxf_set_position_item(fileTrack, &MXF_ITEM_K(Track, Origin), 0));

    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(Sequence), &muxer->fileSequence));
    CHK_OFAIL(mxf_set_strongref_item(fileTrack, &MXF_ITEM_K(GenericTrack, Sequence), muxer->fileSequence));
    CHK_OFAIL(mxf_set_ul_item(muxer->fileSequence, &MXF_ITEM_K(StructuralComponent, DataDefinition),
                              &MXF_DDEF_L(Sound)));
    CHK_OFAIL(mxf_set_length_item(muxer->fileSequence, &MXF_ITEM_K(StructuralComponent, Duration), 0));

    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(SourceClip), &muxer->fileClip));
    CHK_OFAIL(mxf_add_array_item_strongref(muxer->fileSequence,
                                           &MXF_ITEM_K(Sequence, StructuralComponents), muxer->fileClip));
    CHK_OFAIL(mxf_set_ul_item(muxer->fileClip, &MXF_ITEM_K(StructuralComponent, DataDefinition),
                              &MXF_DDEF_L(Sound)));
    CHK_OFAIL(mxf_set_length_item(muxer->fileClip, &MXF_ITEM_K(StructuralComponent, Duration), 0));
    CHK_OFAIL(mxf_set_position_item(muxer->fileClip, &MXF_ITEM_K(SourceClip, StartPosition), 0));
    CHK_OFAIL(mxf_set_umid_item(muxer->fileClip, &MXF_ITEM_K(SourceClip, SourcePackageID), &g_Null_UMID));
    CHK_OFAIL(mxf_set_uint32_item(muxer->fileClip, &MXF_ITEM_K(SourceClip, SourceTrackID), 0));

    /* WaveAudioDescriptor */
    CHK_OFAIL(mxf_create_set(hm, &MXF_SET_K(WaveAudioDescriptor), &muxer->descriptor));
    CHK_OFAIL(mxf_set_strongref_item(sourcePackage, &MXF_ITEM_K(SourcePackage, Descriptor), muxer->descriptor));
    CHK_OFAIL(mxf_set_rational_item(muxer->descriptor, &MXF_ITEM_K(FileDescriptor, SampleRate), &editRate));
    CHK_OFAIL(mxf_set_length_item(muxer->descriptor, &MXF_ITEM_K(FileDescriptor, ContainerDuration), 0));
    CHK_OFAIL(mxf_set_ul_item(muxer->descriptor, &MXF_ITEM_K(FileDescriptor, EssenceContainer),
                              &MXF_EC_L(BWFFrameWrapped)));
    CHK_OFAIL(mxf_set_rational_item(muxer->descriptor,
                                    &MXF_ITEM_K(GenericSoundEssenceDescriptor, AudioSamplingRate),
                                    &audioSamplingRate));
    CHK_OFAIL(mxf_set_uint32_item(muxer->descriptor,
                                  &MXF_ITEM_K(GenericSoundEssenceDescriptor, ChannelCount),
                                  muxer->config.channelCount));
    CHK_OFAIL(mxf_set_uint32_item(muxer->descriptor,
                                  &MXF_ITEM_K(GenericSoundEssenceDescriptor, QuantizationBits),
                                  muxer->config.bitsPerSample));
    CHK_OFAIL(mxf_set_uint16_item(muxer->descriptor, &MXF_ITEM_K(WaveAudioDescriptor, BlockAlign), blockAlign));
    CHK_OFAIL(mxf_set_uint8_item(muxer->descriptor, &MXF_ITEM_K(WaveAudioDescriptor, SequenceOffset),
                                 (uint8_t)muxer->config.sequenceOffset));
    CHK_OFAIL(mxf_set_uint32_item(muxer->descriptor, &MXF_ITEM_K(WaveAudioDescriptor, AvgBps), avgBps));

    return 1;

fail:
    return 0;
}

static int start_body_partition(PCMMxfMuxer *muxer)
{
    MXFPartition *body = NULL;
    int64_t filePos;

    CHK_ORET(grow_bodies(muxer));
    CHK_ORET(mxf_append_new_from_partition(muxer->partitions, muxer->headerPartition, &body));
    body->key = MXF_PP_K(OpenIncomplete, Body);
    body->bodySID = PCM_WRAP_BODY_SID;
    body->indexSID = 0;

    CHK_OFAIL((filePos = mxf_file_tell(muxer->file)) >= 0);
    CHK_OFAIL(mxf_write_partition(muxer->file, body));
    /* align the first audio KLV key to the KAG grid */
    CHK_OFAIL(mxf_fill_to_kag(muxer->file, body));

    muxer->currentBody = body;
    muxer->bodies[muxer->numBodies].partition = body;
    muxer->bodies[muxer->numBodies].startUnitIndex = muxer->keyPositionCount;
    muxer->bodies[muxer->numBodies].unitCount = 0;
    CHK_OFAIL((muxer->bodies[muxer->numBodies].firstAudioKeyFilePos = mxf_file_tell(muxer->file)) >= 0);
    muxer->bodies[muxer->numBodies].lastValueEndFilePos =
        muxer->bodies[muxer->numBodies].firstAudioKeyFilePos;
    muxer->numBodies++;
    muxer->unitsInCurrentBody = 0;
    return 1;

fail:
    return 0;
}

PCMMxfMuxer *pcm_mxf_create(MXFFile *mxfFile, const PCMFrameConfig *config,
                            uint32_t maxEditUnitsPerBodyPartition)
{
    PCMMxfMuxer *muxer = NULL;

    if (mxfFile == NULL || config == NULL ||
        maxEditUnitsPerBodyPartition < 1 || maxEditUnitsPerBodyPartition > PCM_WRAP_MAX_UNITS_PER_BODY)
    {
        return NULL;
    }

    muxer = calloc(1, sizeof(PCMMxfMuxer));
    if (muxer == NULL)
    {
        return NULL;
    }
    muxer->file = mxfFile;
    muxer->config = *config;
    muxer->maxUnitsPerBody = maxEditUnitsPerBodyPartition;
    muxer->audioElementKey = BWF_FRAME_AUDIO_ELEMENT_KEY;

    if (!mxf_create_file_partitions(&muxer->partitions))
    {
        goto fail;
    }

    if (!create_metadata(muxer))
    {
        goto fail;
    }

    /* header partition pack */
    if (!mxf_append_new_partition(muxer->partitions, &muxer->headerPartition) ||
        !mxf_append_partition_esscont_label(muxer->headerPartition, &MXF_EC_L(BWFFrameWrapped)))
    {
        goto fail;
    }
    muxer->headerPartition->key = MXF_PP_K(ClosedIncomplete, Header);
    muxer->headerPartition->kagSize = PCM_WRAP_KAG_SIZE;
    muxer->headerPartition->operationalPattern = MXF_OP_L(1a, UniTrack_Stream_Internal);

    if (!mxf_write_partition(muxer->file, muxer->headerPartition) ||
        !mxf_fill_to_kag(muxer->file, muxer->headerPartition))
    {
        goto fail;
    }

    /* placeholder header metadata (zero durations, same size as the final graph) */
    if ((muxer->headerMetadataStart = mxf_file_tell(muxer->file)) < 0 ||
        !mxf_mark_header_start(muxer->file, muxer->headerPartition) ||
        !mxf_write_header_metadata(muxer->file, muxer->headerMetadata) ||
        !mxf_mark_header_end(muxer->file, muxer->headerPartition))
    {
        goto fail;
    }
    if ((muxer->headerMetadataEnd = mxf_file_tell(muxer->file)) < 0)
    {
        goto fail;
    }

    return muxer;

fail:
    pcm_mxf_destroy(muxer);
    return NULL;
}

int pcm_mxf_write_unit(PCMMxfMuxer *muxer, const uint8_t *unit, uint32_t byteLength)
{
    int64_t keyPos;

    if (muxer == NULL || unit == NULL || byteLength == 0)
    {
        return 0;
    }

    if (muxer->numBodies == 0 || muxer->unitsInCurrentBody >= muxer->maxUnitsPerBody)
    {
        CHK_ORET(start_body_partition(muxer));
    }
    else
    {
        /* keep every audio KLV key on the KAG grid; inter-audio filler is
           part of the BodySID=1 essence span */
        CHK_ORET(mxf_fill_to_kag(muxer->file, muxer->currentBody));
    }

    CHK_ORET(grow_key_positions(muxer));
    CHK_ORET((keyPos = mxf_file_tell(muxer->file)) >= 0);
    muxer->keyFilePositions[muxer->keyPositionCount] = (uint64_t)keyPos;
    muxer->keyPositionCount++;

    CHK_ORET(mxf_write_kl(muxer->file, &muxer->audioElementKey, byteLength));
    CHK_ORET(mxf_file_write(muxer->file, unit, byteLength) == byteLength);

    CHK_ORET((muxer->bodies[muxer->numBodies - 1].lastValueEndFilePos = mxf_file_tell(muxer->file)) >= 0);
    muxer->bodies[muxer->numBodies - 1].unitCount++;
    muxer->unitsInCurrentBody++;

    return 1;
}

static int write_index_segments(PCMMxfMuxer *muxer, const uint64_t *streamOffsets)
{
    uint32_t b;
    int result = 0;
    MXFIndexTableSegment *segment = NULL;
    MXFIndexEntry indexEntry;

    for (b = 0; b < muxer->numBodies; b++)
    {
        uint32_t i;

        if (!mxf_create_index_table_segment(&segment))
        {
            goto fail;
        }
        mxf_generate_uuid(&segment->instanceUID);

        if (muxer->config.editRate == PCM_WRAP_EDIT_RATE_25_1)
        {
            segment->indexEditRate.numerator = 25;
            segment->indexEditRate.denominator = 1;
        }
        else
        {
            segment->indexEditRate.numerator = 30000;
            segment->indexEditRate.denominator = 1001;
        }
        segment->indexStartPosition = (mxfPosition)muxer->bodies[b].startUnitIndex;
        segment->indexDuration = (mxfLength)muxer->bodies[b].unitCount;
        segment->editUnitByteCount = 0;
        segment->indexSID = PCM_WRAP_INDEX_SID;
        segment->bodySID = PCM_WRAP_BODY_SID;
        segment->sliceCount = 0;
        segment->posTableCount = 0;

        memset(&indexEntry, 0, sizeof(indexEntry));
        for (i = 0; i < muxer->bodies[b].unitCount; i++)
        {
            uint32_t unitIndex = muxer->bodies[b].startUnitIndex + i;
            indexEntry.streamOffset = streamOffsets[unitIndex];
            if (!mxf_default_add_index_entry(NULL, 0, segment, 0, 0, 0,
                                             indexEntry.streamOffset, NULL, NULL))
            {
                goto fail;
            }
        }

        if (!mxf_write_index_table_segment(muxer->file, segment))
        {
            goto fail;
        }
        mxf_free_index_table_segment(&segment);
    }

    result = 1;

fail:
    mxf_free_index_table_segment(&segment);
    return result;
}

int pcm_mxf_finish(PCMMxfMuxer *muxer, uint64_t completedUnits)
{
    uint64_t *streamOffsets = NULL;
    uint64_t *bodyStreamBase = NULL;
    uint64_t streamBase = 0;
    uint32_t b;
    MXFPartition *footerPartition = NULL;

    if (muxer == NULL || muxer->numBodies == 0 ||
        completedUnits != muxer->keyPositionCount || completedUnits == 0)
    {
        return 0;
    }

    streamOffsets = malloc(muxer->keyPositionCount * sizeof(uint64_t));
    bodyStreamBase = malloc(muxer->numBodies * sizeof(uint64_t));
    if (streamOffsets == NULL || bodyStreamBase == NULL)
    {
        free(streamOffsets);
        free(bodyStreamBase);
        return 0;
    }

    /* Build the continuous BodySID=1 essence stream coordinates: each body
       starts at the accumulated essence span of the previous bodies. The
       alignment filler between a partition pack and its first audio key is
       not part of the essence stream; filler between audio KLVs is. */
    for (b = 0; b < muxer->numBodies; b++)
    {
        BodyRecord *body = &muxer->bodies[b];
        uint32_t i;

        bodyStreamBase[b] = streamBase;
        for (i = 0; i < body->unitCount; i++)
        {
            uint32_t unitIndex = body->startUnitIndex + i;
            streamOffsets[unitIndex] =
                streamBase + (muxer->keyFilePositions[unitIndex] - body->firstAudioKeyFilePos);
        }
        streamBase += body->lastValueEndFilePos - body->firstAudioKeyFilePos;
    }

    /* final durations in the metadata graph */
    CHK_OFAIL(mxf_set_length_item(muxer->materialSequence, &MXF_ITEM_K(StructuralComponent, Duration),
                                  (mxfLength)completedUnits));
    CHK_OFAIL(mxf_set_length_item(muxer->materialClip, &MXF_ITEM_K(StructuralComponent, Duration),
                                  (mxfLength)completedUnits));
    CHK_OFAIL(mxf_set_length_item(muxer->fileSequence, &MXF_ITEM_K(StructuralComponent, Duration),
                                  (mxfLength)completedUnits));
    CHK_OFAIL(mxf_set_length_item(muxer->fileClip, &MXF_ITEM_K(StructuralComponent, Duration),
                                  (mxfLength)completedUnits));
    CHK_OFAIL(mxf_set_length_item(muxer->descriptor, &MXF_ITEM_K(FileDescriptor, ContainerDuration),
                                  (mxfLength)completedUnits));

    /* rewrite the header metadata graph in place (same item set and sizes) */
    CHK_OFAIL(mxf_file_seek(muxer->file, muxer->headerMetadataStart, SEEK_SET));
    CHK_OFAIL(mxf_mark_header_start(muxer->file, muxer->headerPartition));
    CHK_OFAIL(mxf_write_header_metadata(muxer->file, muxer->headerMetadata));
    CHK_OFAIL(mxf_fill_to_position(muxer->file, (uint64_t)muxer->headerMetadataEnd));
    CHK_OFAIL(mxf_mark_header_end(muxer->file, muxer->headerPartition));

    /* return to the end of the body content before appending the footer */
    CHK_OFAIL(mxf_file_seek(muxer->file, 0, SEEK_END));

    /* finalise partition pack states and body offsets */
    muxer->headerPartition->key = MXF_PP_K(ClosedComplete, Header);
    for (b = 0; b < muxer->numBodies; b++)
    {
        muxer->bodies[b].partition->key = MXF_PP_K(ClosedComplete, Body);
        muxer->bodies[b].partition->bodyOffset = bodyStreamBase[b];
    }

    /* footer partition pack */
    CHK_OFAIL(mxf_append_new_from_partition(muxer->partitions, muxer->headerPartition, &footerPartition));
    footerPartition->key = MXF_PP_K(ClosedComplete, Footer);
    footerPartition->bodySID = 0;
    footerPartition->indexSID = PCM_WRAP_INDEX_SID;
    CHK_OFAIL(mxf_write_partition(muxer->file, footerPartition));
    CHK_OFAIL(mxf_fill_to_kag(muxer->file, footerPartition));

    /* footer copy of the final metadata graph */
    CHK_OFAIL(mxf_mark_header_start(muxer->file, footerPartition));
    CHK_OFAIL(mxf_write_header_metadata(muxer->file, muxer->headerMetadata));
    CHK_OFAIL(mxf_mark_header_end(muxer->file, footerPartition));

    /* one VBE index table segment per body partition */
    CHK_OFAIL(mxf_mark_index_start(muxer->file, footerPartition));
    CHK_OFAIL(write_index_segments(muxer, streamOffsets));
    CHK_OFAIL(mxf_fill_to_kag(muxer->file, footerPartition));
    CHK_OFAIL(mxf_mark_index_end(muxer->file, footerPartition));

    /* random index pack, then close the partition chain on disk */
    CHK_OFAIL(mxf_write_rip(muxer->file, muxer->partitions));
    CHK_OFAIL(mxf_update_partitions(muxer->file, muxer->partitions));

    free(streamOffsets);
    free(bodyStreamBase);
    return 1;

fail:
    free(streamOffsets);
    free(bodyStreamBase);
    return 0;
}

uint32_t pcm_mxf_num_bodies(const PCMMxfMuxer *muxer)
{
    return muxer->numBodies;
}

void pcm_mxf_destroy(PCMMxfMuxer *muxer)
{
    if (muxer == NULL)
    {
        return;
    }
    free(muxer->keyFilePositions);
    free(muxer->bodies);
    if (muxer->partitions != NULL)
    {
        mxf_free_file_partitions(&muxer->partitions);
    }
    if (muxer->headerMetadata != NULL)
    {
        mxf_free_header_metadata(&muxer->headerMetadata);
    }
    if (muxer->dataModel != NULL)
    {
        mxf_free_data_model(&muxer->dataModel);
    }
    free(muxer);
}

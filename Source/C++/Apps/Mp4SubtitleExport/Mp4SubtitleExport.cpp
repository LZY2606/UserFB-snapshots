/*****************************************************************
|
|    AP4 - Subtitle (TTML/stpp) Export Example
|
|    Copyright 2002-2024 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2 of the License, or
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
|    02111-1307, USA.
|
****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "Ap4.h"

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
#define BANNER "MP4 Subtitle Export Example - Version 1.0\n"\
               "(Bento4 Version " AP4_VERSION_STRING ")\n"\
               "(c) 2002-2024 Axiomatic Systems, LLC"

/*----------------------------------------------------------------------
|   PrintUsageAndExit
+---------------------------------------------------------------------*/
static void
PrintUsageAndExit()
{
    fprintf(stderr,
            BANNER
            "\n\nusage: mp4subtitleexport [options] <input> <output-prefix>\n"
            "  --track <id>   only export the given track ID (default: every subtitles track)\n"
            "  --list         list the tracks and their sample descriptions, then exit\n"
            "\n"
            "Each exported track is written to <output-prefix>.track<id>.ttml,\n"
            "one TTML document per line, in decode order. The sample description\n"
            "index carried by every sample is reported, so a single track whose\n"
            "fragment is split into several 'traf' atoms (for example when the TTML\n"
            "configuration changes mid-fragment) is exported through the normal\n"
            "AP4_LinearReader entry point, without any special handling.\n");
    exit(1);
}

/*----------------------------------------------------------------------
|   ListTracks
+---------------------------------------------------------------------*/
static void
ListTracks(AP4_Movie& movie)
{
    for (AP4_List<AP4_Track>::Item* item = movie.GetTracks().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_Track* track = item->GetData();
        const char* type_name = "unknown";
        switch (track->GetType()) {
            case AP4_Track::TYPE_VIDEO:     type_name = "video"; break;
            case AP4_Track::TYPE_AUDIO:     type_name = "audio"; break;
            case AP4_Track::TYPE_SUBTITLES: type_name = "subtitles"; break;
            default: break;
        }
        printf("track id=%u type=%s descriptions=%u timescale=%u\n",
               track->GetId(),
               type_name,
               track->GetSampleDescriptionCount(),
               track->GetMediaTimeScale());
        for (unsigned int i=0; i<track->GetSampleDescriptionCount(); i++) {
            AP4_SampleDescription* desc = track->GetSampleDescription(i);
            char format_name[5] = {0};
            if (desc) {
                AP4_FormatFourChars(format_name, desc->GetFormat());
            }
            printf("  description[%u] format=%s\n", i, desc ? format_name : "<none>");
        }
    }
}

/*----------------------------------------------------------------------
|   ExportTrack
+---------------------------------------------------------------------*/
static AP4_Result
ExportTrack(AP4_LinearReader& reader,
            AP4_UI32          track_id,
            const std::string& output_path)
{
    AP4_ByteStream* output = NULL;
    AP4_Result result = AP4_FileByteStream::Create(output_path.c_str(),
                                                   AP4_FileByteStream::STREAM_MODE_WRITE,
                                                   output);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot create output '%s' (%d)\n", output_path.c_str(), result);
        return result;
    }

    AP4_Sample     sample;
    AP4_DataBuffer sample_data;
    unsigned int   sample_index = 0;
    for (;;) {
        result = reader.ReadNextSample(track_id, sample, sample_data);
        if (result == AP4_ERROR_EOS) {
            result = AP4_SUCCESS;
            break;
        }
        if (AP4_FAILED(result)) {
            fprintf(stderr, "ERROR: failed to read sample %u of track %u (%d)\n",
                    sample_index, track_id, result);
            break;
        }

        // report the description index so mid-fragment configuration
        // switches (one 'traf' per description run) remain visible
        printf("track %u sample %u dts=%llu description_index=%u size=%u\n",
               track_id,
               sample_index,
               (unsigned long long)sample.GetDts(),
               sample.GetDescriptionIndex(),
               sample.GetSize());

        // write the TTML payload verbatim, one document per line: the bytes
        // are never parsed or re-encoded
        result = output->Write(sample_data.GetData(), sample_data.GetDataSize());
        if (AP4_FAILED(result)) break;
        result = output->Write("\n", 1);
        if (AP4_FAILED(result)) break;
        ++sample_index;
    }

    output->Release();
    if (AP4_SUCCEEDED(result)) {
        printf("track %u: exported %u sample(s) to %s\n",
               track_id, sample_index, output_path.c_str());
    }
    return result;
}

/*----------------------------------------------------------------------
|   main
+---------------------------------------------------------------------*/
int
main(int argc, char** argv)
{
    if (argc < 2) PrintUsageAndExit();

    const char* input_filename  = NULL;
    const char* output_prefix   = NULL;
    int         selected_track  = 0;
    bool        list_only       = false;

    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            list_only = true;
        } else if (!strcmp(argv[i], "--track")) {
            if (++i >= argc) PrintUsageAndExit();
            selected_track = atoi(argv[i]);
        } else if (input_filename == NULL) {
            input_filename = argv[i];
        } else if (output_prefix == NULL) {
            output_prefix = argv[i];
        } else {
            fprintf(stderr, "ERROR: unexpected argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (input_filename == NULL || (output_prefix == NULL && !list_only)) {
        PrintUsageAndExit();
    }

    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(input_filename,
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open input '%s' (%d)\n", input_filename, result);
        return 1;
    }

    AP4_File file(*input, true);
    if (file.GetMovie() == NULL) {
        fprintf(stderr, "ERROR: no movie found in '%s'\n", input_filename);
        input->Release();
        return 1;
    }

    if (list_only) {
        ListTracks(*file.GetMovie());
        input->Release();
        return 0;
    }

    // one linear reader for every track; the same reader is used whether a
    // 'moof' contains a single 'traf' for a track or several of them
    AP4_LinearReader reader(*file.GetMovie(), input);
    AP4_Array<AP4_UI32> tracks_to_export;
    for (AP4_List<AP4_Track>::Item* item = file.GetMovie()->GetTracks().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_Track* track = item->GetData();
        if (selected_track) {
            if (static_cast<int>(track->GetId()) != selected_track) continue;
        } else if (track->GetType() != AP4_Track::TYPE_SUBTITLES) {
            continue;
        }
        result = reader.EnableTrack(track->GetId());
        if (AP4_FAILED(result)) {
            fprintf(stderr, "ERROR: cannot enable track %u (%d)\n", track->GetId(), result);
            input->Release();
            return 1;
        }
        tracks_to_export.Append(track->GetId());
    }
    if (tracks_to_export.ItemCount() == 0) {
        fprintf(stderr, "ERROR: no subtitles track to export\n");
        input->Release();
        return 1;
    }

    for (unsigned int i=0; i<tracks_to_export.ItemCount(); i++) {
        AP4_UI32 track_id = tracks_to_export[i];
        char output_path[1024];
        snprintf(output_path, sizeof(output_path), "%s.track%u.ttml", output_prefix, track_id);
        result = ExportTrack(reader, track_id, output_path);
        if (AP4_FAILED(result)) {
            input->Release();
            return 1;
        }
    }

    input->Release();
    return 0;
}

/**********************************************************************************************/
/* The MIT License                                                                            */
/*                                                                                            */
/* Copyright 2016-2017 Twitch Interactive, Inc. or its affiliates. All Rights Reserved.       */
/*                                                                                            */
/* Permission is hereby granted, free of charge, to any person obtaining a copy               */
/* of this software and associated documentation files (the "Software"), to deal              */
/* in the Software without restriction, including without limitation the rights               */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell                  */
/* copies of the Software, and to permit persons to whom the Software is                      */
/* furnished to do so, subject to the following conditions:                                   */
/*                                                                                            */
/* The above copyright notice and this permission notice shall be included in                 */
/* all copies or substantial portions of the Software.                                        */
/*                                                                                            */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR                 */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,                   */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE                */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER                     */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,              */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN                  */
/* THE SOFTWARE.                                                                              */
/**********************************************************************************************/

// Karaoke preview example: parse a cue with an inline time tag, inspect the
// token sequence, preview PAST/FUTURE phases, edit text/timing, then dump.

#include "vtt.h"
#include "caption.h"
#include <stdio.h>
#include <string.h>

static const char* phase_name(caption_frame_phase_t phase)
{
    switch (phase) {
    case caption_phase_past:
        return "PAST";
    case caption_phase_future:
        return "FUTURE";
    default:
        return "NONE";
    }
}

static void preview(vtt_block_t* cue, double at)
{
    caption_frame_t frame;
    vtt_cue_to_caption_frame_at(cue, at, &frame);
    printf("preview at %6.3fs:\n", at);

    for (int r = 0; r < SCREEN_ROWS; ++r) {
        int any = 0;

        for (int c = 0; c < SCREEN_COLS; ++c) {
            if (frame.front.cell[r][c].data[0]) {
                any = 1;
            }
        }

        if (!any) {
            continue;
        }

        printf("  row %02d: ", r);

        for (int c = 0; c < SCREEN_COLS; ++c) {
            const char* ch = frame.front.cell[r][c].data;

            if (!ch[0]) {
                break;
            }

            printf("%s[%s]", ch, phase_name(caption_frame_read_phase(&frame, r, c)));
        }

        printf("\n");
    }
}

int main(void)
{
    static const char input[]
        = "WEBVTT\r\n\r\n"
          "00:00:10.000 --> 00:00:14.000\r\n"
          "<i>A&amp;B</i><00:00:12.000><u>CD</u>\r\n\r\n";

    vtt_t* vtt = vtt_parse(input, strlen(input));

    if (!vtt || !vtt->cue_head) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }

    vtt_block_t* cue = vtt->cue_head;

    // 1. walk the structured token sequence
    printf("tokens: %zu\n", vtt_cue_token_count(cue));

    for (size_t i = 0; i < vtt_cue_token_count(cue); ++i) {
        const vtt_text_token_t* tok = vtt_cue_token(cue, i);

        switch (tok->type) {
        case VTT_TOKEN_TEXT:
            printf("  text   \"%s\" (b=%d i=%d u=%d)\n", tok->text, tok->bold, tok->italic, tok->underline);
            break;
        case VTT_TOKEN_NEWLINE:
            printf("  newline\n");
            break;
        case VTT_TOKEN_TIME:
            printf("  time   %lldms\n", (long long)tok->time_ms);
            break;
        default:
            printf("  raw    \"%s\"\n", tok->text);
            break;
        }
    }

    // 2. preview before, at and after the inline marker
    preview(cue, 11.0);
    preview(cue, 12.0);
    preview(cue, 13.0);
    preview(cue, 14.0); // outside the half-open cue: empty screen

    // 3. edit the first text token; "<X&" is stored literally and escaped
    if (LIBCAPTION_OK != vtt_cue_token_set_text(cue, 0, "<X&")) {
        fprintf(stderr, "text edit failed\n");
        return 2;
    }

    // 4. retime the whole cue; the inner marker moves with it
    if (LIBCAPTION_OK != vtt_cue_retime(cue, 20.0, 8.0)) {
        fprintf(stderr, "retime failed\n");
        return 3;
    }

    // 5. serialize the edited payload through the dedicated interface;
    //    vtt_block_data() still holds the original unedited bytes
    utf8_char_t out[1024];
    size_t n = vtt_cue_serialize(cue, out, sizeof(out));

    if ((size_t)-1 == n) {
        fprintf(stderr, "serialize failed\n");
        return 4;
    }

    printf("edited payload: %.*s\n", (int)n, out);
    printf("original payload still available: %s\n", vtt_block_data(cue));
    printf("full edited document:\n");
    vtt_dump(vtt);

    // the marker originally at 12s now activates at 24s
    caption_frame_t frame;
    vtt_cue_to_caption_frame_at(cue, 23.999, &frame);
    printf("at 23.999s final cell phase: %s\n",
        phase_name(caption_frame_read_phase(&frame, 0, 3)));
    vtt_cue_to_caption_frame_at(cue, 24.0, &frame);
    printf("at 24.000s final cell phase: %s\n",
        phase_name(caption_frame_read_phase(&frame, 0, 3)));

    vtt_free(vtt);
    return 0;
}

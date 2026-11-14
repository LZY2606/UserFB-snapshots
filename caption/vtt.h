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
#ifndef LIBCAPTION_VTT_H
#define LIBCAPTION_VTT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "caption.h"
#include "eia608.h"

enum VTT_BLOCK_TYPE {
    VTT_REGION = 0,
    VTT_STYLE = 1,
    VTT_NOTE = 2,
    VTT_CUE = 3
};

// Structured, editable representation of a cue's payload.
// Text runs, hard line breaks and inline time tags are kept as separate
// tokens instead of forcing every caller to split the raw markup string.
enum VTT_TEXT_TOKEN_TYPE {
    VTT_TOKEN_TEXT = 0, //< decoded literal text run, carries b/i/u state
    VTT_TOKEN_NEWLINE = 1, //< hard line break
    VTT_TOKEN_TIME = 2, //< inline time tag, time stored as whole milliseconds
    VTT_TOKEN_RAW = 3 //< unsupported markup preserved verbatim (voice spans, ...)
};

typedef struct vtt_text_token_t {
    enum VTT_TEXT_TOKEN_TYPE type;
    utf8_char_t* text; //< decoded text (VTT_TOKEN_TEXT only), NULL otherwise
    unsigned char bold; //< <b>, kept in the structure only
    unsigned char italic; //< <i>, maps to eia608_style_italics
    unsigned char underline; //< <u>, maps to the cell underline bit
    int64_t time_ms; //< absolute marker time in ms (VTT_TOKEN_TIME only), -1 otherwise
} vtt_text_token_t;

typedef struct vtt_cue_text_t {
    vtt_text_token_t* tokens;
    size_t count;
    size_t capacity;
    int valid; //< 0 when the raw payload could not be parsed
} vtt_cue_text_t;

// CUE represents a block of caption text
typedef struct _vtt_block_t {
    struct _vtt_block_t* next;
    enum VTT_BLOCK_TYPE type;
    // CUE-Only
    double timestamp;
    double duration; // -1.0 for no duration
    char* cue_settings;
    char* cue_id;
    // Structured cue payload (CUE blocks only, NULL for other block types)
    vtt_cue_text_t* cue_text;
    // Standard block data
    size_t text_size;
    char* block_text;
} vtt_block_t;

// VTT files are a collection of REGION, STYLE and CUE blocks.
// XXX: Comments (NOTE blocks) are ignored
typedef struct _vtt_t {
    vtt_block_t* region_head;
    vtt_block_t* region_tail;
    vtt_block_t* style_head;
    vtt_block_t* style_tail;
    vtt_block_t* cue_head;
    vtt_block_t* cue_tail;
} vtt_t;

/*! \brief
    \param
*/
vtt_t* vtt_new();
/*! \brief
    \param
*/
void vtt_free(vtt_t* vtt);

/*! \brief
    \param
*/
vtt_block_t* vtt_block_new(vtt_t* vtt, const utf8_char_t* data, size_t size, enum VTT_BLOCK_TYPE type);

/*! \brief
    \param
*/
void vtt_cue_free_head(vtt_t* vtt);

/*! \brief
    \param
*/
void vtt_style_free_head(vtt_t* vtt);

/*! \brief
    \param
*/
void vtt_region_free_head(vtt_t* vtt);

// returns a vtt_t, containing linked lists of blocks. must be freed when done
/*! \brief
    \param
*/
vtt_t* vtt_parse(const utf8_char_t* data, size_t size);

/*! \brief
    \param
*/
vtt_t* _vtt_parse(const utf8_char_t* data, size_t size, int srt_mode);

/*! \brief
    \param
*/
static inline vtt_block_t* vtt_cue_next(vtt_block_t* block) { return block->next; }

/*! \brief
    \param
*/
static inline utf8_char_t* vtt_block_data(vtt_block_t* block) { return (utf8_char_t*)(block) + sizeof(vtt_block_t); }

/*! \brief
    \param
*/
static inline void vtt_crack_time(double tt, int* hh, int* mm, int* ss, int* ms)
{
    (*ms) = (int)((int64_t)(tt * 1000) % 1000);
    (*ss) = (int)((int64_t)(tt) % 60);
    (*mm) = (int)((int64_t)(tt / (60)) % 60);
    (*hh) = (int)((int64_t)(tt / (60 * 60)));
}

// This only converts the current CUE, it does not walk the list
/*! \brief
    \param
*/
int vtt_cue_to_caption_frame(vtt_block_t* cue, caption_frame_t* frame);

/*! \brief Preview of a cue at an absolute time (seconds)
    Cells already active at absolute_time carry phase caption_phase_past,
    cells whose first time tag is still ahead carry caption_phase_future.
    All text keeps its final position; time tags occupy no cell.
    Outside the cue interval the frame is cleared and every cell reads NONE.
    \param cue A cue with a parseable structured payload
    \param absolute_time Preview time in seconds, measured on the same clock as the cue
    \param frame A pointer to an allocated caption_frame_t object
    \return LIBCAPTION_OK on success, LIBCAPTION_ERROR on a NULL/unparseable cue
*/
libcaption_stauts_t vtt_cue_to_caption_frame_at(vtt_block_t* cue, double absolute_time, caption_frame_t* frame);

/*! \brief Number of tokens in the structured cue payload, 0 for non-cue blocks
*/
size_t vtt_cue_token_count(vtt_block_t* cue);

/*! \brief Access a structured cue payload token by index
    \return Pointer owned by the cue, NULL when the index is out of range
*/
const vtt_text_token_t* vtt_cue_token(vtt_block_t* cue, size_t index);

/*! \brief Replace the literal text of a VTT_TOKEN_TEXT token
    The new text is stored verbatim and escaped on serialization, so values
    such as "<X&" can never be reactivated as markup. Format state is kept.
    \return LIBCAPTION_OK on success, LIBCAPTION_ERROR on a bad index or token type
*/
libcaption_stauts_t vtt_cue_token_set_text(vtt_block_t* cue, size_t index, const utf8_char_t* text);

/*! \brief Move an inline time tag to a new absolute time (seconds)
    Accepts hh:mm:ss.mmm or mm:ss.mmm semantics through a finite double,
    rounded to whole milliseconds. The new value must stay strictly inside
    the cue and stay strictly ordered against neighboring time tags.
    \return LIBCAPTION_OK on success, LIBCAPTION_ERROR when the edit is rejected
*/
libcaption_stauts_t vtt_cue_token_set_time(vtt_block_t* cue, size_t index, double absolute_time);

/*! \brief Move the cue and all of its inline time tags with one affine transform
    new_start/new_duration are seconds; only finite values and a positive
    duration are accepted. Marker times are rounded to whole milliseconds,
    must stay strictly inside the moved cue and may not collide on one
    millisecond. On rejection the cue is left untouched.
    \return LIBCAPTION_OK on success, LIBCAPTION_ERROR when the retime is rejected
*/
libcaption_stauts_t vtt_cue_retime(vtt_block_t* cue, double new_start, double new_duration);

/*! \brief Serialize the edited structured payload back to WebVTT cue text
    Writes a NUL terminated string including any trailing line break the
    payload carries. Returns the number of bytes written, excluding the NUL,
    or (size_t)-1 when buf_size is too small / the cue has no payload.
    vtt_block_data() keeps returning the original unedited payload; this is
    the interface for reading edited text.
*/
size_t vtt_cue_serialize(vtt_block_t* cue, utf8_char_t* buf, size_t buf_size);

// returns the new cue
/*! \brief
    \param
*/
vtt_block_t* vtt_cue_from_caption_frame(caption_frame_t* frame, vtt_t* vtt);
/*! \brief
    \param
*/
void vtt_dump(vtt_t* vtt);

#ifdef __cplusplus
}
#endif
#endif

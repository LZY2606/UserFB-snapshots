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
#ifndef LIBCAPTION_H
#define LIBCAPTION_H
#ifdef __cplusplus
extern "C" {
#endif

#include "eia608.h"
#include "utf8.h"
#include "xds.h"

// ssize_t is POSIX and does not exist on Windows
#if defined(_MSC_VER)
#if defined(_WIN64)
typedef signed long ssize_t;
#else
typedef signed int ssize_t;
#endif
#endif

typedef enum {
    LIBCAPTION_ERROR = 0,
    LIBCAPTION_OK = 1,
    LIBCAPTION_READY = 2
} libcaption_stauts_t;

static inline libcaption_stauts_t libcaption_status_update(libcaption_stauts_t old_stat, libcaption_stauts_t new_stat)
{
    return (LIBCAPTION_ERROR == old_stat || LIBCAPTION_ERROR == new_stat) ? LIBCAPTION_ERROR : (LIBCAPTION_READY == old_stat) ? LIBCAPTION_READY : new_stat;
}

#define SCREEN_ROWS 15
#define SCREEN_COLS 32

// Karaoke phase of a single cell.
// NONE is used for plain text/608 decoded frames, PAST/FUTURE only by
// vtt_cue_to_caption_frame_at() preview frames.
typedef enum {
    caption_phase_none = 0,
    caption_phase_past = 1,
    caption_phase_future = 2
} caption_frame_phase_t;

typedef struct {
    unsigned int uln : 1; //< underline
    unsigned int sty : 3; //< style
    utf8_char_t data[5]; //< 4 byte utf8 values plus null term
} caption_frame_cell_t;

typedef struct {
    caption_frame_cell_t cell[SCREEN_ROWS][SCREEN_COLS];
    // parallel array of karaoke phases; zero (caption_phase_none) for
    // frames produced by caption_frame_from_text() or 608 decoding
    unsigned char phase[SCREEN_ROWS][SCREEN_COLS];
} caption_frame_buffer_t;

typedef struct {
    unsigned int uln : 1; //< underline
    unsigned int sty : 3; //< style
    unsigned int rup : 2; //< roll-up line count minus 1
    int8_t row, col;
    uint16_t cc_data;
} caption_frame_state_t;

// timestamp and duration are in seconds
typedef struct {
    double timestamp;
    xds_t xds;
    caption_frame_state_t state;
    caption_frame_buffer_t front;
    caption_frame_buffer_t back;
    caption_frame_buffer_t* write;
    libcaption_stauts_t status;
} caption_frame_t;

/*!
    \brief Initializes an allocated caption_frame_t instance
    \param frame Pointer to prealocated caption_frame_t object
*/
void caption_frame_init(caption_frame_t* frame);
/*! \brief Promotes the back buffer to the front buffer and clears the back buffer
*/
libcaption_stauts_t caption_frame_end(caption_frame_t* frame);
/*! \brief
    \param
*/
static inline int caption_frame_popon(caption_frame_t* frame) { return (frame->write == &frame->back) ? 1 : 0; }
/*! \brief
    \param
*/
static inline int caption_frame_painton(caption_frame_t* frame) { return (frame->write == &frame->front) ? 1 : 0; }
/*! \brief
    \param
*/
const static int _caption_frame_rollup[] = { 0, 2, 3, 4 };
static inline int caption_frame_rollup(caption_frame_t* frame) { return _caption_frame_rollup[frame->state.rup]; }
/*! \brief
    \param
*/
static inline double caption_frame_timestamp(caption_frame_t* frame) { return frame->timestamp; }
/*! \brief Writes a single charcter to a caption_frame_t object
    \param frame A pointer to an allocted and initialized caption_frame_t object
    \param row Row position to write charcter, must be between 0 and SCREEN_ROWS-1
    \param col Column position to write charcter, must be between 0 and SCREEN_ROWS-1
    \param style Style to apply to charcter
    \param underline Set underline attribute, 0 = off any other value = on
    \param c pointer to a single valid utf8 charcter. Bytes are automatically determined, and a NULL terminator is not required
*/
int caption_frame_write_char(caption_frame_t* frame, int row, int col, eia608_style_t style, int underline, const utf8_char_t* c);
/*! \brief
    \param
*/
const utf8_char_t* caption_frame_read_char(caption_frame_t* frame, int row, int col, eia608_style_t* style, int* underline);
/*! \brief Reads the karaoke phase of the cell at row/col of the front buffer
    \param frame A pointer to an allocated and initialized caption_frame_t object
    \param row Row position, must be between 0 and SCREEN_ROWS-1
    \param col Column position, must be between 0 and SCREEN_COLS-1
    \return caption_phase_none for plain/608 frames and empty cells,
            caption_phase_past/caption_phase_future for VTT preview frames
*/
static inline caption_frame_phase_t caption_frame_read_phase(caption_frame_t* frame, int row, int col)
{
    if (!frame || 0 > row || SCREEN_ROWS <= row || 0 > col || SCREEN_COLS <= col) {
        return caption_phase_none;
    }

    return (caption_frame_phase_t)frame->front.phase[row][col];
}
/*! \brief
    \param
*/
libcaption_stauts_t caption_frame_decode(caption_frame_t* frame, uint16_t cc_data, double timestamp);
/*! \brief
    \param
*/
int caption_frame_from_text(caption_frame_t* frame, const utf8_char_t* data);
/*! \brief
    \param
*/
#define CAPTION_FRAME_TEXT_BYTES (4 * ((SCREEN_COLS + 2) * SCREEN_ROWS) + 1)
size_t caption_frame_to_text(caption_frame_t* frame, utf8_char_t* data);
/*! \brief
    \param
*/
#define CAPTION_FRAME_DUMP_BUF_SIZE 8192
size_t caption_frame_dump_buffer(caption_frame_t* frame, utf8_char_t* buf);
void caption_frame_dump(caption_frame_t* frame);

#ifdef __cplusplus
}
#endif
#endif

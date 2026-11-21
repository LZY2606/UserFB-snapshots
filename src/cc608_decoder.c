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
#include "cc608_decoder.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CC608_FIELDS 2
#define CC608_XDS_CONTENT_SIZE 32

/* Opaque decoder. Every mutable value the decoder needs is stored in this
   structure; the serialized form is defined independently below. */
struct cc608_decoder {
    caption_frame_t frame[CC608_SERVICE_COUNT];
    int selected[CC608_FIELDS]; /* channel index 0/1 per field */
};

////////////////////////////////////////////////////////////////////////////////
// Helpers

static int cc608_field_for_type(cea708_cc_type_t type, int* field)
{
    if (cc_type_ntsc_cc_field_1 == type) {
        (*field) = 0;
        return 1;
    }

    if (cc_type_ntsc_cc_field_2 == type) {
        (*field) = 1;
        return 1;
    }

    return 0;
}

/* A frame's write pointer must always name one of its own buffers. Frames are
   stored by value in several places (the decoder, and snapshots handed to
   callers), so copying a caption_frame_t naively leaves a dangling pointer. */
static void cc608_rebind_write(caption_frame_t* frame)
{
    if (0 == frame->write) {
        return;
    }

    frame->write = (frame->write == &frame->back) ? &frame->back : &frame->front;
}

/* Copy a frame into a destination at a new address. The source write pointer
   is re-expressed against the destination's own buffers, never dereferenced,
   so a struct-by-value copy can never carry a pointer to temporary storage. */
static void cc608_frame_copy_into(caption_frame_t* destination, const caption_frame_t* source)
{
    int write_state = (0 == source->write) ? -1 : (source->write == &source->back ? 1 : 0);
    (*destination) = (*source);

    if (0 == write_state) {
        destination->write = &destination->front;
    } else if (1 == write_state) {
        destination->write = &destination->back;
    } else {
        destination->write = 0;
    }
}

static int cc608_is_finite(double value) { return isfinite(value) ? 1 : 0; }

static int cc608_frame_state_valid(const caption_frame_t* frame)
{
    if (!cc608_is_finite(frame->timestamp)) {
        return 0;
    }

    /* A preamble may select the "unused" row code, which the existing row map
       turns into -1, so allow it here. Columns only grow through tabs. */
    if (-1 > frame->state.row || SCREEN_ROWS - 1 < frame->state.row) {
        return 0;
    }

    if (0 > frame->state.col || 0x7F < frame->state.col) {
        return 0;
    }

    if (0 > frame->state.rup || 3 < frame->state.rup) {
        return 0;
    }

    if (0 > frame->state.sty || 7 < frame->state.sty) {
        return 0;
    }

    if (0 > frame->state.uln || 1 < frame->state.uln) {
        return 0;
    }

    if (0 > frame->status || LIBCAPTION_READY < frame->status) {
        return 0;
    }

    if (1 < frame->xds.state || 15 < frame->xds.class_code || 15 < frame->xds.type || CC608_XDS_CONTENT_SIZE < frame->xds.size) {
        return 0;
    }

    if (frame->write != 0 && frame->write != &frame->front && frame->write != &frame->back) {
        return 0;
    }

    return 1;
}

/* Routing needs the pre-update selection: a channel-bearing word selects the
   service that will consume it, then the word is decoded into that service. */
static void cc608_select_service(cc608_decoder_t* decoder, int field, int channel)
{
    if (0 > channel || 1 < channel) {
        channel = 0;
    }

    decoder->selected[field] = channel;
}

static caption_frame_t* cc608_current_frame(cc608_decoder_t* decoder, int field)
{
    return &decoder->frame[field * 2 + decoder->selected[field]];
}

////////////////////////////////////////////////////////////////////////////////
// Lifecycle

cc608_decoder_t* cc608_decoder_create(void)
{
    cc608_decoder_t* decoder = (cc608_decoder_t*)malloc(sizeof(cc608_decoder_t));

    if (!decoder) {
        return 0;
    }

    cc608_decoder_reset(decoder);
    return decoder;
}

void cc608_decoder_free(cc608_decoder_t* decoder) { free(decoder); }

void cc608_decoder_reset(cc608_decoder_t* decoder)
{
    if (!decoder) {
        return;
    }

    memset(decoder, 0, sizeof(cc608_decoder_t));

    for (int i = 0; i < CC608_SERVICE_COUNT; ++i) {
        caption_frame_init(&decoder->frame[i]);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Frame access

int cc608_decoder_get_frame(const cc608_decoder_t* decoder, cc608_service_t service, caption_frame_t* frame)
{
    if (!decoder || !frame || 0 > (int)service || CC608_SERVICE_COUNT <= (int)service) {
        return 0;
    }

    cc608_frame_copy_into(frame, &decoder->frame[(int)service]);
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Decoding

/* Feed one 608 word to a single service frame. The word was already routed,
   so channel bearing words must not re-route. The existing single-frame
   decoder implements the character, preamble, style, mode and repeat rules;
   this wrapper only rebinds write and isolates the frame from XDS. */
static libcaption_stauts_t cc608_decode_word(caption_frame_t* frame, uint16_t cc_data, double timestamp)
{
    libcaption_stauts_t status = caption_frame_decode(frame, cc_data, timestamp);
    cc608_rebind_write(frame);
    return status;
}

/* An XDS packet is only meaningful in field 2 and is not a caption word. It
   is consumed by the private xds_t carried by the CC3 frame, mirroring how the
   single-frame decoder keeps XDS state, and is never displayed or routed. */
static void cc608_decode_xds(caption_frame_t* frame, uint16_t cc_data) { xds_decode(&frame->xds, cc_data); }

libcaption_stauts_t cc608_decoder_decode(cc608_decoder_t* decoder, const cea708_t* cea708)
{
    if (!decoder || !cea708) {
        return LIBCAPTION_ERROR;
    }

    if (GA94 != cea708->user_identifier || !cea708->user_data.process_cc_data_flag) {
        return LIBCAPTION_OK;
    }

    libcaption_stauts_t status = LIBCAPTION_OK;
    int count = cea708_cc_count((user_data_t*)&cea708->user_data);

    for (int i = 0; i < count; ++i) {
        int valid;
        cea708_cc_type_t type;
        uint16_t cc_data = cea708_cc_data((user_data_t*)&cea708->user_data, i, &valid, &type);
        int field;

        if (!valid || !cc608_field_for_type(type, &field)) {
            continue;
        }

        /* Odd parity is mandatory for 608 words. A bad word is rejected
           before it can affect selection or frame state. */
        if (!eia608_parity_varify(cc_data)) {
            status = LIBCAPTION_ERROR;
            continue;
        }

        if (eia608_is_padding(cc_data)) {
            continue;
        }

        if (1 == field) {
            caption_frame_t* xds_frame = &decoder->frame[cc608_cc3];

            /* A packet in progress consumes every following field 2 word until
               its end word, so payload bytes can never leak into captions. */
            if (xds_frame->xds.state || eia608_is_xds(cc_data)) {
                cc608_decode_xds(xds_frame, cc_data);
                continue;
            }
        }

        if (eia608_is_preamble(cc_data)) {
            int row, col, chan, underline;
            eia608_style_t style;
            eia608_parse_preamble(cc_data, &row, &col, &style, &chan, &underline);
            cc608_select_service(decoder, field, chan);
            status = libcaption_status_update(status, cc608_decode_word(cc608_current_frame(decoder, field), cc_data, cea708->timestamp));
            continue;
        }

        if (eia608_is_midrowchange(cc_data)) {
            int chan, underline;
            eia608_style_t style;
            eia608_parse_midrowchange(cc_data, &chan, &style, &underline);
            cc608_select_service(decoder, field, chan);
            status = libcaption_status_update(status, cc608_decode_word(cc608_current_frame(decoder, field), cc_data, cea708->timestamp));
            continue;
        }

        if (eia608_is_control(cc_data)) {
            cc608_select_service(decoder, field, eia608_test_second_channel_bit(cc_data) ? 1 : 0);
            status = libcaption_status_update(status, cc608_decode_word(cc608_current_frame(decoder, field), cc_data, cea708->timestamp));
            continue;
        }

        if (eia608_is_specialna(cc_data) || eia608_is_westeu(cc_data)) {
            /* Special and extended characters carry a channel bit. */
            cc608_select_service(decoder, field, eia608_test_second_channel_bit(cc_data) ? 1 : 0);
            status = libcaption_status_update(status, cc608_decode_word(cc608_current_frame(decoder, field), cc_data, cea708->timestamp));
            continue;
        }

        if (eia608_is_basicna(cc_data)) {
            /* Basic characters have no channel bit: use the recent choice. */
            status = libcaption_status_update(status, cc608_decode_word(cc608_current_frame(decoder, field), cc_data, cea708->timestamp));
            continue;
        }

        /* Unclassified words are left alone; they must not disturb routing. */
    }

    return status;
}

////////////////////////////////////////////////////////////////////////////////
// Checkpoint format
//
// All multi-byte integers are unsigned little endian. The fixed header is:
//
//   offset  size  value
//   0       4     magic "C608"
//   4       1     version (1)
//   5       1     byte order marker (0x01 means little endian)
//   6       2     header size in bytes (16)
//   8       2     service record size in bytes (7739)
//   10      2     service record count (4)
//   12      4     total byte length, including header and checksum
//
// followed by four fixed-size service records, a 2-byte routing tail and a
// 4-byte FNV-1a checksum of every preceding byte.
// A service record is:
//
//   size  field
//   1     service index
//   1     flags: bit 0 write active, bit 1 write target (0 front, 1 back)
//   1     decoder status
//   1     row
//   1     col
//   1     style
//   1     underline
//   1     roll-up
//   1     XDS state
//   1     XDS class code
//   1     XDS type
//   2     XDS payload size (0..32, even)
//   4     XDS checksum
//   8     timestamp as IEEE-754 double, little endian bits
//   2     recent control word (cc_data)
//   32    XDS payload, unused bytes zero
//   3840  front buffer cells
//   3840  back buffer cells
//
// Each cell is: 1 byte flags (bit 0 underline, bits 1..3 style), 3 reserved
// zero bytes, and 4 UTF-8 bytes. Records are identified by their explicit
// index, so a wrong or duplicated index is rejected.

#define CC608_CHECKPOINT_MAGIC "C608"
#define CC608_CHECKPOINT_VERSION 1
#define CC608_CHECKPOINT_ORDER 1
#define CC608_CHECKPOINT_HEADER_SIZE 16
#define CC608_CELL_SIZE 8
#define CC608_BUFFER_SIZE (SCREEN_ROWS * SCREEN_COLS * CC608_CELL_SIZE)

#define CC608_R_IDX 0
#define CC608_R_FLAGS 1
#define CC608_R_STATUS 2
#define CC608_R_ROW 3
#define CC608_R_COL 4
#define CC608_R_STY 5
#define CC608_R_ULN 6
#define CC608_R_RUP 7
#define CC608_R_XDS_STATE 8
#define CC608_R_XDS_CLASS 9
#define CC608_R_XDS_TYPE 10
#define CC608_R_XDS_SIZE 11
#define CC608_R_XDS_CHECKSUM 13
#define CC608_R_TIMESTAMP 17
#define CC608_R_CC_DATA 25
#define CC608_R_XDS_CONTENT 27
#define CC608_R_FRONT (CC608_R_XDS_CONTENT + CC608_XDS_CONTENT_SIZE)
#define CC608_R_BACK (CC608_R_FRONT + CC608_BUFFER_SIZE)
#define CC608_SERVICE_RECORD_SIZE (CC608_R_BACK + CC608_BUFFER_SIZE)
#define CC608_ROUTING_TAIL_SIZE 2
#define CC608_CHECKSUM_SIZE 4
#define CC608_CHECKPOINT_SIZE (CC608_CHECKPOINT_HEADER_SIZE + CC608_SERVICE_COUNT * CC608_SERVICE_RECORD_SIZE + CC608_ROUTING_TAIL_SIZE + CC608_CHECKSUM_SIZE)

static void cc608_put_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void cc608_put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t cc608_get_u16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t cc608_get_u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t cc608_checksum(const uint8_t* data, size_t size)
{
    uint32_t hash = 2166136261u;

    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }

    return hash;
}

static void cc608_put_double(uint8_t* p, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, 8);

    for (int i = 0; i < 8; ++i) {
        p[i] = (uint8_t)((bits >> (8 * i)) & 0xFF);
    }
}

static double cc608_get_double(const uint8_t* p)
{
    uint64_t bits = 0;

    for (int i = 0; i < 8; ++i) {
        bits |= ((uint64_t)p[i]) << (8 * i);
    }

    double value;
    memcpy(&value, &bits, 8);
    return value;
}

static void cc608_put_cell(uint8_t* p, const caption_frame_cell_t* cell)
{
    p[0] = (uint8_t)((cell->uln ? 0x01 : 0x00) | ((cell->sty & 0x07) << 1));
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    memcpy(&p[4], cell->data, 4);
}

static void cc608_get_cell(caption_frame_cell_t* cell, const uint8_t* p)
{
    memset(cell, 0, sizeof(caption_frame_cell_t));
    cell->uln = (p[0] & 0x01) ? 1 : 0;
    cell->sty = (p[0] >> 1) & 0x07;
    memcpy(cell->data, &p[4], 4);
    cell->data[4] = 0;
}

static void cc608_put_buffer(uint8_t* p, const caption_frame_buffer_t* buffer)
{
    for (int row = 0; row < SCREEN_ROWS; ++row) {
        for (int col = 0; col < SCREEN_COLS; ++col) {
            cc608_put_cell(p, &buffer->cell[row][col]);
            p += CC608_CELL_SIZE;
        }
    }
}

static void cc608_get_buffer(caption_frame_buffer_t* buffer, const uint8_t* p)
{
    memset(buffer, 0, sizeof(caption_frame_buffer_t));

    for (int row = 0; row < SCREEN_ROWS; ++row) {
        for (int col = 0; col < SCREEN_COLS; ++col) {
            cc608_get_cell(&buffer->cell[row][col], p);
            p += CC608_CELL_SIZE;
        }
    }
}

size_t cc608_decoder_checkpoint_size(const cc608_decoder_t* decoder) { return decoder ? CC608_CHECKPOINT_SIZE : 0; }

static void cc608_put_service(uint8_t* p, int service, const caption_frame_t* frame)
{
    uint8_t flags = 0;

    if (frame->write == &frame->back) {
        flags |= 0x02;
    }

    if (frame->write) {
        flags |= 0x01;
    }

    memset(p, 0, CC608_SERVICE_RECORD_SIZE);
    p[CC608_R_IDX] = (uint8_t)service;
    p[CC608_R_FLAGS] = flags;
    p[CC608_R_STATUS] = (uint8_t)frame->status;
    p[CC608_R_ROW] = (uint8_t)frame->state.row;
    p[CC608_R_COL] = (uint8_t)frame->state.col;
    p[CC608_R_STY] = (uint8_t)frame->state.sty;
    p[CC608_R_ULN] = (uint8_t)frame->state.uln;
    p[CC608_R_RUP] = (uint8_t)frame->state.rup;
    p[CC608_R_XDS_STATE] = (uint8_t)frame->xds.state;
    p[CC608_R_XDS_CLASS] = frame->xds.class_code;
    p[CC608_R_XDS_TYPE] = frame->xds.type;
    cc608_put_u16(&p[CC608_R_XDS_SIZE], (uint16_t)(frame->xds.size > CC608_XDS_CONTENT_SIZE ? CC608_XDS_CONTENT_SIZE : frame->xds.size));
    cc608_put_u32(&p[CC608_R_XDS_CHECKSUM], frame->xds.checksum);
    cc608_put_double(&p[CC608_R_TIMESTAMP], frame->timestamp);
    cc608_put_u16(&p[CC608_R_CC_DATA], frame->state.cc_data);
    memcpy(&p[CC608_R_XDS_CONTENT], frame->xds.content, CC608_XDS_CONTENT_SIZE);
    cc608_put_buffer(&p[CC608_R_FRONT], &frame->front);
    cc608_put_buffer(&p[CC608_R_BACK], &frame->back);
}

int cc608_decoder_checkpoint_export(const cc608_decoder_t* decoder, uint8_t* buffer, size_t capacity, size_t* size)
{
    if (!decoder || !buffer || !size || capacity < CC608_CHECKPOINT_SIZE) {
        return 0;
    }

    memset(buffer, 0, CC608_CHECKPOINT_SIZE);
    memcpy(buffer, CC608_CHECKPOINT_MAGIC, 4);
    buffer[4] = CC608_CHECKPOINT_VERSION;
    buffer[5] = CC608_CHECKPOINT_ORDER;
    cc608_put_u16(&buffer[6], CC608_CHECKPOINT_HEADER_SIZE);
    cc608_put_u16(&buffer[8], (uint16_t)CC608_SERVICE_RECORD_SIZE);
    cc608_put_u16(&buffer[10], CC608_SERVICE_COUNT);
    cc608_put_u32(&buffer[12], (uint32_t)CC608_CHECKPOINT_SIZE);

    uint8_t* record = &buffer[CC608_CHECKPOINT_HEADER_SIZE];

    for (int i = 0; i < CC608_SERVICE_COUNT; ++i) {
        cc608_put_service(record, i, &decoder->frame[i]);
        record += CC608_SERVICE_RECORD_SIZE;
    }

    record[0] = (uint8_t)decoder->selected[0];
    record[1] = (uint8_t)decoder->selected[1];
    record += CC608_ROUTING_TAIL_SIZE;

    cc608_put_u32(record, cc608_checksum(buffer, CC608_CHECKPOINT_SIZE - CC608_CHECKSUM_SIZE));
    (*size) = CC608_CHECKPOINT_SIZE;
    return 1;
}

static int cc608_get_service(caption_frame_t* frame, int service, const uint8_t* p, int* write_target)
{
    uint8_t flags = p[CC608_R_FLAGS];
    int write_active = (flags & 0x01) ? 1 : 0;
    int write_back = (flags & 0x02) ? 1 : 0;
    uint16_t payload_size = cc608_get_u16(&p[CC608_R_XDS_SIZE]);
    uint32_t checksum = cc608_get_u32(&p[CC608_R_XDS_CHECKSUM]);

    if ((int)p[CC608_R_IDX] != service) {
        return 0;
    }

    if (0 != (flags & 0xFC)) {
        return 0;
    }

    if (write_back && !write_active) {
        return 0;
    }

    if (CC608_XDS_CONTENT_SIZE < payload_size || 0 != (payload_size & 1)) {
        return 0;
    }

    if (0 != (checksum & 0xFFFFFF00u)) {
        return 0;
    }

    /* Validate the raw bytes before assigning them. Several state members are
       bit-fields, so an out of range byte would silently truncate instead of
       being detected after the copy. */
    if (LIBCAPTION_ERROR > p[CC608_R_STATUS] || LIBCAPTION_READY < p[CC608_R_STATUS]) {
        return 0;
    }

    if (0xFF != p[CC608_R_ROW] && SCREEN_ROWS - 1 < p[CC608_R_ROW]) {
        return 0;
    }

    if (0x7F < p[CC608_R_COL]) {
        return 0;
    }

    if (7 < p[CC608_R_STY] || 1 < p[CC608_R_ULN] || 3 < p[CC608_R_RUP]) {
        return 0;
    }

    if (1 < p[CC608_R_XDS_STATE] || 15 < p[CC608_R_XDS_CLASS] || 15 < p[CC608_R_XDS_TYPE]) {
        return 0;
    }

    for (int i = 0; i < CC608_XDS_CONTENT_SIZE; ++i) {
        if ((int)payload_size <= i && 0 != p[CC608_R_XDS_CONTENT + i]) {
            return 0;
        }
    }

    /* The cell encoding leaves bits that must be zero. */
    for (int i = 0; i < SCREEN_ROWS * SCREEN_COLS; ++i) {
        const uint8_t* cell = &p[CC608_R_FRONT + i * CC608_CELL_SIZE];
        if (0 != cell[1] || 0 != cell[2] || 0 != cell[3]) {
            return 0;
        }

        cell = &p[CC608_R_BACK + i * CC608_CELL_SIZE];
        if (0 != cell[1] || 0 != cell[2] || 0 != cell[3]) {
            return 0;
        }
    }

    memset(frame, 0, sizeof(caption_frame_t));
    caption_frame_init(frame);
    frame->state.row = (int8_t)p[CC608_R_ROW];
    frame->state.col = (int8_t)p[CC608_R_COL];
    frame->state.sty = p[CC608_R_STY];
    frame->state.uln = p[CC608_R_ULN];
    frame->state.rup = p[CC608_R_RUP];
    frame->xds.state = p[CC608_R_XDS_STATE];
    frame->xds.class_code = p[CC608_R_XDS_CLASS];
    frame->xds.type = p[CC608_R_XDS_TYPE];
    frame->xds.size = payload_size;
    frame->xds.checksum = (uint8_t)(checksum & 0xFF);
    frame->timestamp = cc608_get_double(&p[CC608_R_TIMESTAMP]);
    frame->state.cc_data = cc608_get_u16(&p[CC608_R_CC_DATA]);
    frame->write = 0;
    memcpy(frame->xds.content, &p[CC608_R_XDS_CONTENT], payload_size);
    cc608_get_buffer(&frame->front, &p[CC608_R_FRONT]);
    cc608_get_buffer(&frame->back, &p[CC608_R_BACK]);
    frame->status = (libcaption_stauts_t)p[CC608_R_STATUS];
    (*write_target) = write_active ? (write_back ? 1 : 0) : -1;

    if (!cc608_frame_state_valid(frame)) {
        return 0;
    }

    return 1;
}

int cc608_decoder_checkpoint_restore(cc608_decoder_t* decoder, const uint8_t* buffer, size_t size)
{
    if (!decoder || !buffer) {
        return 0;
    }

    if (size != CC608_CHECKPOINT_SIZE) {
        return 0;
    }

    if (0 != memcmp(buffer, CC608_CHECKPOINT_MAGIC, 4)) {
        return 0;
    }

    if (CC608_CHECKPOINT_VERSION != buffer[4]) {
        return 0;
    }

    if (CC608_CHECKPOINT_ORDER != buffer[5]) {
        return 0;
    }

    if (CC608_CHECKPOINT_HEADER_SIZE != cc608_get_u16(&buffer[6])) {
        return 0;
    }

    if (CC608_SERVICE_RECORD_SIZE != cc608_get_u16(&buffer[8]) || CC608_SERVICE_COUNT != cc608_get_u16(&buffer[10])) {
        return 0;
    }

    if ((uint32_t)CC608_CHECKPOINT_SIZE != cc608_get_u32(&buffer[12])) {
        return 0;
    }

    if (cc608_get_u32(&buffer[CC608_CHECKPOINT_SIZE - CC608_CHECKSUM_SIZE]) != cc608_checksum(buffer, CC608_CHECKPOINT_SIZE - CC608_CHECKSUM_SIZE)) {
        return 0;
    }

    cc608_decoder_t restored;
    memset(&restored, 0, sizeof(restored));
    const uint8_t* record = &buffer[CC608_CHECKPOINT_HEADER_SIZE];
    int write_target[CC608_SERVICE_COUNT];

    for (int i = 0; i < CC608_SERVICE_COUNT; ++i) {
        if (!cc608_get_service(&restored.frame[i], i, record, &write_target[i])) {
            return 0;
        }

        record += CC608_SERVICE_RECORD_SIZE;
    }

    restored.selected[0] = record[0];
    restored.selected[1] = record[1];

    if (1 < restored.selected[0] || 1 < restored.selected[1]) {
        return 0;
    }

    /* Assigning by value copies write pointers that name the temporary's own
       buffers, so rebind them against the destination after the copy. */
    (*decoder) = restored;

    for (int i = 0; i < CC608_SERVICE_COUNT; ++i) {
        if (0 == write_target[i]) {
            decoder->frame[i].write = &decoder->frame[i].front;
        } else if (1 == write_target[i]) {
            decoder->frame[i].write = &decoder->frame[i].back;
        } else {
            decoder->frame[i].write = 0;
        }
    }

    return 1;
}

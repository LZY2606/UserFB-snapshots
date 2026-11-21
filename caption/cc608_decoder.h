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
#ifndef LIBCAPTION_CC608_DECODER_H
#define LIBCAPTION_CC608_DECODER_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "caption.h"
#include "cea708.h"

/*! \brief Independent CEA-608 CC1..CC4 decoder.
 *
 *  A decoder instance owns four independent caption frames. CEA-708 GA94
 *  objects are split by their native cc_type: field 1 feeds CC1/CC2 and
 *  field 2 feeds CC3/CC4. Each field starts on its first service (CC1 for
 *  field 1, CC3 for field 2). Channel bearing words update the selection
 *  for that field, while basic characters inherit the current selection.
 *
 *  XDS packets in field 2 are consumed as metadata and never become
 *  visible caption text or change a caption service selection.
 *
 *  The object is opaque; its layout is private to the implementation.
 */
typedef struct cc608_decoder cc608_decoder_t;

/*! \brief Caption service identifiers. The numeric values are also the
    indices accepted by cc608_decoder_get_frame(). */
typedef enum {
    cc608_cc1 = 0,
    cc608_cc2 = 1,
    cc608_cc3 = 2,
    cc608_cc4 = 3
} cc608_service_t;

#define CC608_SERVICE_COUNT 4

/*! \brief Allocate and initialize a decoder.
    \return A new decoder, or NULL if allocation fails.
    The caller owns the returned object and releases it with
    cc608_decoder_free(). */
cc608_decoder_t* cc608_decoder_create(void);

/*! \brief Release a decoder previously returned by
    cc608_decoder_create(). Passing NULL is a no-op. */
void cc608_decoder_free(cc608_decoder_t* decoder);

/*! \brief Clear every service frame, both field selections and any XDS
    state. Passing NULL is a no-op. */
void cc608_decoder_reset(cc608_decoder_t* decoder);

/*! \brief Decode one GA94 object.
    \param decoder Decoder to update.
    \param cea708 Object to read. It is not modified and is not retained.
    \return LIBCAPTION_ERROR if a processed, cc_valid field 1/2 608 word
    fails odd parity (that word changes no state; earlier words are kept),
    LIBCAPTION_READY if any service reached its ready state, otherwise
    LIBCAPTION_OK. Invalid entries, padding, native type 2/3 entries,
    non-GA94 objects and objects with process_cc_data_flag clear are
    ignored without changing service routing. Passing NULL returns
    LIBCAPTION_ERROR. */
libcaption_stauts_t cc608_decoder_decode(cc608_decoder_t* decoder, const cea708_t* cea708);

/*! \brief Copy a snapshot of one service's caption frame.
    \param decoder Decoder to read.
    \param service Service to copy, cc608_cc1..cc608_cc4.
    \param frame Destination owned by the caller. It receives a complete
    value copy of the service frame. If the frame's write pointer is
    non-NULL it is rebound to point at the destination's own front or
    back buffer, never at decoder storage. Later decoding, reset or
    freeing of the decoder does not alter the snapshot.
    \return 1 on success, 0 for NULL arguments or an unknown service. */
int cc608_decoder_get_frame(const cc608_decoder_t* decoder, cc608_service_t service, caption_frame_t* frame);

/*! \brief Number of bytes cc608_decoder_checkpoint_export() writes.
    \return The exact checkpoint size, or 0 if decoder is NULL. */
size_t cc608_decoder_checkpoint_size(const cc608_decoder_t* decoder);

/*! \brief Serialize the complete decoder state into caller-owned bytes.
    \param decoder Decoder to export.
    \param buffer Caller-owned destination. Not retained by the decoder.
    \param capacity Bytes available at buffer.
    \param size Receives the number of bytes written on success. May be
    NULL only when the call cannot succeed.
    \return 1 on success, 0 if an argument is NULL or capacity is too
    small. The bytes are self-describing: they record a format version,
    fixed record lengths and an explicit byte-order marker.
    The output is a deterministic little-endian encoding that does not
    contain host structures, pointers, bit-fields or structure padding.
    Timestamps are stored as finite IEEE-754 bit patterns and therefore
    round trip exactly. */
int cc608_decoder_checkpoint_export(const cc608_decoder_t* decoder, uint8_t* buffer, size_t capacity, size_t* size);

/*! \brief Restore a decoder in place from checkpoint bytes.
    \param decoder Destination decoder, unchanged if the call fails.
    \param buffer Checkpoint bytes to read. Not retained.
    \param size Exact byte count reported by checkpoint export.
    \return 1 on success. Returns 0, leaving decoder exactly as it was,
    for unknown versions, truncated input, trailing bytes, malformed
    lengths, illegal state fields, non-finite timestamps, or corruption
    detected by the trailing checksum. */
int cc608_decoder_checkpoint_restore(cc608_decoder_t* decoder, const uint8_t* buffer, size_t size);

#ifdef __cplusplus
}
#endif
#endif

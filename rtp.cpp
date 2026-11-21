/******************************************************************************
*   Copyright (c) 2013-2015 thundernet development group, inc.
*   http://thundernet.com
*
*   Permission is hereby granted, free of charge, to any person obtaining a
*   copy of this software and associated documentation files (the "Software"),
*   to deal in the Software without restriction, including without limitation
*   the rights to use, copy, modify, merge, publish, distribute, sublicense,
*   and/or sell copies of the Software, and to permit persons to whom the
*   Software is furnished to do so, subject to the following conditions:
*
*   1. The above copyright notice and this permission notice shall be included
*      in all copies or substantial portions of the Software.
*
*   2. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
*      OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
*      MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
*      IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
*      CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
*      TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
*      SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
*   -----
*
*   RTP header parsing and RFC 4588 retransmission (RTX) restoration helpers.
*
******************************************************************************/

#include "rtp.h"


/******************************************************************************
*   Validates the RTP layout of a raw packet: version 2, fixed header and
*   CSRC list present, extension (if flagged) fully inside the buffer, and a
*   legal padding count.  On success fills info with host-order fields and
*   the payload boundaries; failures leave everything untouched.
******************************************************************************/
bool RTPPacket::parse_header(const uint8 *data, uint16 len, RtpHeaderInfo& info)
{
    if (data == NULL || len < RTP_HEADER_LENGTH) {
        return false;
    }

    uint8 flags = data[0];
    if ((flags & 0xC0) != (RTP_VERSION << 6)) {
        return false;
    }

    uint8 byte1 = data[1];
    uint16 header_len = RTP_HEADER_LENGTH + static_cast<uint16>(flags & 0x0F) * 4u;
    if (header_len > len) {
        return false;
    }

    if (flags & (RTP_FLAGS_EXTENSION >> 8)) {
        // extension needs its 4-byte profile/length header
        if (static_cast<uint32>(header_len) + 4u > len) {
            return false;
        }
        uint16 ext_words = static_cast<uint16>((data[header_len + 2] << 8)
                                             | data[header_len + 3]);
        uint32 ext_total = 4u + static_cast<uint32>(ext_words) * 4u;
        if (static_cast<uint32>(header_len) + ext_total > len) {
            return false;
        }
        header_len = static_cast<uint16>(header_len + ext_total);
    }

    uint16 payload_len = static_cast<uint16>(len - header_len);
    uint8 padding_len = 0;
    if (flags & (RTP_FLAGS_PADDING >> 8)) {
        if (payload_len < 1) {
            return false;
        }
        padding_len = data[len - 1];
        if (padding_len == 0 || padding_len > payload_len) {
            return false;
        }
        payload_len = static_cast<uint16>(payload_len - padding_len);
    }

    info.cc = flags & 0x0F;
    info.padding = (flags & (RTP_FLAGS_PADDING >> 8)) != 0;
    info.extension = (flags & (RTP_FLAGS_EXTENSION >> 8)) != 0;
    info.marker = (byte1 & 0x80) != 0;
    info.payload_type = byte1 & RTP_FLAGS_PAYLOAD_TYPE;
    info.sequence = static_cast<uint16>((data[2] << 8) | data[3]);
    info.timestamp = static_cast<uint32>((data[4] << 24) | (data[5] << 16)
                                       | (data[6] << 8) | data[7]);
    info.ssrc = static_cast<uint32>((data[8] << 24) | (data[9] << 16)
                                  | (data[10] << 8) | data[11]);
    info.header_len = header_len;
    info.payload_len = payload_len;
    info.padding_len = padding_len;
    return true;
}



/******************************************************************************
*   Restores an RFC 4588 RTX packet into its original primary form.  The
*   two-byte big-endian OSN prefixing the payload is removed and the header
*   sequence/SSRC/PT are rewritten to the primary values.  Everything else
*   (marker, timestamp, CSRC, extension, media bytes and legal padding) is
*   copied byte for byte; the caller's input buffer is never modified.
******************************************************************************/
rawrtp_ptr RTPPacket::restore_rtx(const uint8 *data, uint16 len,
                                  const RtpHeaderInfo& info, uint16 osn,
                                  uint32 primary_ssrc, uint8 primary_pt,
                                  uint16 packet_ms)
{
    // the payload, with any padding already accounted for, must hold the OSN
    if (data == NULL || info.payload_len < 2 || len < info.header_len + 2) {
        return rawrtp_ptr();
    }

    uint16 new_len = static_cast<uint16>(len - 2);
    // the existing constructor only reads its input; keep its signature
    rawrtp_ptr restored(new RTPPacket(const_cast<uint8 *>(data), new_len));
    if (restored->pData == NULL) {
        return rawrtp_ptr();
    }

    // RTPPacket copied the first new_len bytes; drop the two OSN bytes by
    //  moving the media payload (and any trailing padding) up behind the header
    uint16 header_len = info.header_len;
    memmove(restored->pData + header_len,
            data + header_len + 2,
            len - header_len - 2);

    // rewrite the fixed header fields while keeping V/P/X/CC and marker bits
    restored->pData[0] = data[0];
    restored->pData[1] = static_cast<uint8>((data[1] & 0x80) | (primary_pt & 0x7F));
    restored->pData[2] = static_cast<uint8>(osn >> 8);
    restored->pData[3] = static_cast<uint8>(osn & 0xFF);
    restored->pData[8] = static_cast<uint8>(primary_ssrc >> 24);
    restored->pData[9] = static_cast<uint8>(primary_ssrc >> 16);
    restored->pData[10] = static_cast<uint8>(primary_ssrc >> 8);
    restored->pData[11] = static_cast<uint8>(primary_ssrc & 0xFF);

    restored->payload_type = primary_pt;
    restored->payload_ms = packet_ms;
    restored->payload_bytes = static_cast<uint16>(info.payload_len - 2);
    restored->use_redundant_payload = false;
    return restored;
}

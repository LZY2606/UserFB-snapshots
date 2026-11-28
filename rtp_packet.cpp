/******************************************************************************
*   RTP packet envelope helpers shared by the jitter buffer and the repair
*   path: header/CSRC/extension/padding parsing and RTX restoration.
*
******************************************************************************/

#include "rtp.h"
#include <arpa/inet.h>


/******************************************************************************
*   Validates the fixed header, CSRC list, header extension and padding of an
*   RTP packet and reports the byte layout.  Every arithmetic step is bounded
*   by nLen, so a truncated or inconsistent packet is simply rejected.
*
*   Returns true when the packet is structurally valid
******************************************************************************/
bool RTPPacket::parse_layout(Layout& out) const
{
    int64  header;
    int64  padding = 0;

    if ((pData == NULL) || (nLen < RTP_HEADER_LENGTH)) {
        return false;
    }

    uint8 byte0 = pData[0];
    uint8 byte1 = pData[1];

    if ((byte0 >> 6) != RTP_VERSION) {
        return false;
    }

    header = RTP_HEADER_LENGTH + 4 * (byte0 & 0x0F);
    if (header > nLen) {
        return false;
    }

    // header extension: 4 byte header followed by 32 bit words
    if (byte0 & 0x10) {
        if (header + 4 > nLen) {
            return false;
        }
        uint16 words_raw;
        memcpy(&words_raw, pData + header + 2, sizeof(words_raw));
        int64 words = ntohs(words_raw);
        header += 4 + 4 * words;
        if (header > nLen) {
            return false;
        }
    }

    // padding: the last byte counts the padding octets, including itself
    if (byte0 & 0x20) {
        if (header >= nLen) {
            return false;
        }
        padding = pData[nLen - 1];
        if ((padding < 1) || (padding > (nLen - header))) {
            return false;
        }
    }

    out.header_len     = (uint16)header;
    out.payload_offset = (uint16)header;
    out.payload_len    = (uint16)(nLen - header);
    out.padding_len    = (uint16)padding;
    out.media_len      = (uint16)(nLen - header - padding);
    (void)byte1;
    return true;
}



/******************************************************************************
*   Restores an RTX packet to primary media form.  The RFC 4588 two byte OSN
*   prefix is stripped, the sequence number is replaced by the original one,
*   and SSRC/payload type are rewritten to the primary values.  Marker,
*   timestamp, CSRC list, header extension, media bytes and valid padding are
*   copied verbatim.  The source packet is not touched.
*
*   Returns the restored packet, or null if the input is malformed
******************************************************************************/
rawrtp_ptr RTPPacket::restore_rtx(const RTPPacket& rtx,
                                  uint16          osn,
                                  uint32          ssrc,
                                  uint8           payload_type,
                                  uint16          payload_ms)
{
    Layout  layout;
    int64   body_len;
    uint8  *wire;
    rawrtp_ptr restored;

    if (!rtx.parse_layout(layout)) {
        return rawrtp_ptr();
    }
    if (layout.media_len < 2) {
        return rawrtp_ptr();
    }

    // the packet's constructor takes a signed short length
    body_len = (int64)rtx.nLen - 2;
    if ((body_len < RTP_HEADER_LENGTH) || (body_len > 0x7FFF)) {
        return rawrtp_ptr();
    }

    wire = new uint8[body_len];
    if (wire == NULL) {
        return rawrtp_ptr();
    }

    // everything before the payload, then the payload past the OSN prefix
    memcpy(wire, rtx.pData, layout.payload_offset);
    memcpy(wire + layout.payload_offset,
           rtx.pData + layout.payload_offset + 2,
           rtx.nLen - layout.payload_offset - 2);

    // rewrite the header for the primary stream, keeping the marker bit
    uint16 seq = htons(osn);
    uint32 src = htonl(ssrc);
    wire[1] = (uint8)((wire[1] & RTP_FLAGS_MARKER_BIT) | payload_type);
    memcpy(wire + 2, &seq, 2);
    memcpy(wire + 8, &src, 4);

    restored.reset(new RTPPacket(wire, (short)body_len));
    delete[] wire;
    if (!restored) {
        return rawrtp_ptr();
    }

    restored->payload_ms            = payload_ms;
    restored->payload_type          = payload_type;
    restored->payload_bytes         = (uint16)(layout.media_len - 2);
    restored->use_redundant_payload = false;

    return restored;
}

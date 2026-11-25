// RFC6184 RTP Payload Format for H.264 Video
//
// Shared RTP payload structure reader. The default (non-interleaved) decoder
// and the interleaved-mode de-packetizer both drive this reader, so the RTP
// packet / STAP / MTAP / FU parsing exists in exactly one place.
//
// The reader reports complete NAL units in transmission order together with
// the decoding order number (DON) when the payload carries one. It never
// reorders anything itself.

#include "rtp-h264-internal.h"
#include "rtp-payload.h"
#include "rtp-param.h"
#include "rtp-util.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void rtp_h264_reader_init(struct rtp_h264_reader_t* reader)
{
	if (!reader)
		return;

	memset(reader, 0, sizeof(*reader));
	reader->flags = -1; // first packet(no pending loss flag)
}

void rtp_h264_reader_free(struct rtp_h264_reader_t* reader)
{
	if (!reader)
		return;

	free(reader->fu);
	reader->fu = NULL;
	reader->fu_size = 0;
	reader->fu_capacity = 0;
	reader->fu_active = 0;
}

static void rtp_h264_reader_discard_fu(struct rtp_h264_reader_t* reader)
{
	reader->fu_size = 0;
	reader->fu_active = 0;
}

static int rtp_h264_is_nalu_type(int type)
{
	return type > 0 && type < 24; // 1-23 are the NAL unit types allowed in packets
}

/// Emit one complete NAL unit. `data` is only valid during the handler call.
static int rtp_h264_reader_emit(struct rtp_h264_reader_t* reader, rtp_h264_nalu_handler handler, void* param,
	const uint8_t* data, int bytes, uint32_t timestamp, uint16_t don, int has_don)
{
	struct rtp_h264_nalu_t nalu;

	nalu.data = data;
	nalu.bytes = bytes;
	nalu.timestamp = timestamp;
	nalu.don = don;
	nalu.has_don = has_don;
	nalu.flags = (reader->flags < 0) ? 0 : reader->flags;

	reader->flags = 0;
	reader->fu_size = 0;
	reader->fu_active = 0;

	return handler(param, &nalu);
}

static int rtp_h264_reader_fu_append(struct rtp_h264_reader_t* reader, const uint8_t* data, int bytes)
{
	int capacity;
	uint8_t* p;

	if (bytes <= 0)
		return 0;

	if (reader->fu_size > RTP_PAYLOAD_MAX_SIZE - bytes)
		return -EINVAL;

	if (reader->fu_size + bytes > reader->fu_capacity)
	{
		capacity = reader->fu_size + bytes;
		capacity += capacity / 4 > 128000 ? capacity / 4 : 128000;
		p = (uint8_t*)realloc(reader->fu, (size_t)capacity);
		if (!p)
			return -ENOMEM;

		reader->fu = p;
		reader->fu_capacity = capacity;
	}

	memcpy(reader->fu + reader->fu_size, data, (size_t)bytes);
	reader->fu_size += bytes;
	return 0;
}

// 5.7.1. Single-Time Aggregation Packet (STAP) (p23)
/*
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |STAP-B NAL HDR |            DON                |  NALU 1 Size  |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |  NALU 1 Size  | NALU 1 HDR    |         NALU 1 Data           |
*/
// 5.7.2. Multi-Time Aggregation Packets (MTAPs) (p27)
/*
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |MTAP16 NAL HDR |   decoding order number base  |  NALU 1 Size  |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |  NALU 1 Size  |  NALU 1 DOND  |       NALU 1 TS offset        |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |  NALU 1 HDR   |  NALU 1 DATA                                  |
*/
//
// mode: 0-STAP-A, 1-STAP-B, 2-MTAP16, 3-MTAP24
//
// The NALU size convention differs between the two decoders. The historical
// default decoder reads the MTAP size as covering DOND + TS offset + NAL unit;
// the interleaved entry follows RFC6184 5.7.2 where the size counts only the
// NAL unit itself and the DOND/TS offset fields sit between the size field and
// the NAL unit. reader->mtap_size_nal_only selects the convention.

// One aggregation member, decoded from a size field onward.
struct rtp_h264_member_t
{
	const uint8_t* nal;		// NAL unit bytes
	int nalbytes;			// NAL unit size in bytes
	int total;				// bytes consumed by size field + fields + NAL
	uint16_t don;			// DON of this member
	uint32_t timestamp;		// timestamp of this member
};

/// Decode the member at `ptr` with `rem` bytes remaining.
/// @param[in] strict 1-additionally reject invalid NAL unit types,
///                   0-only enforce the sizes (historical release behaviour)
/// @return 0-ok, <0-error/incomplete
static int rtp_h264_member_parse(const uint8_t* ptr, int rem, int mode, int nal_only, int skip,
	uint16_t don, uint32_t timestamp, int strict, struct rtp_h264_member_t* m)
{
	int len;

	if (rem < 2)
		return -EINVAL; // no room for the NAL unit size field

	len = nbo_r16(ptr);
	if (mode >= 2)
	{
		// size(2) + DOND + TS offset + NAL. The NAL always follows the
		// DOND/TS offset; only the size convention differs.
		//   nal_only:  NAL = len, advance = 2 + DOND + TS + len
		//   otherwise: NAL = len - DOND - TS, advance = 2 + len
		uint32_t ts;

		m->nalbytes = nal_only ? len : len - skip;
		m->nal = ptr + 2 + skip;
		m->total = 2 + skip + m->nalbytes;
		if (m->nalbytes < 1 || m->total > rem)
			return -EINVAL;
		if (strict && (0 == RTP_H264_NAL_TYPE(m->nal[0]) || RTP_H264_NAL_TYPE(m->nal[0]) >= 24))
			return -EINVAL;

		ts = nbo_r16(ptr + 3);
		if (3 == mode)
			ts = (ts << 8) | ptr[5]; // MTAP24
		m->timestamp = timestamp + ts;
		m->don = (uint16_t)(don + ptr[2]); // DONB + DOND
	}
	else
	{
		// size(2) + NAL(len)
		m->nal = ptr + 2;
		m->nalbytes = len;
		m->total = 2 + len;
		if (len < 2 || m->total > rem)
			return -EINVAL;
		if (strict && (0 == RTP_H264_NAL_TYPE(m->nal[0]) || RTP_H264_NAL_TYPE(m->nal[0]) >= 24))
			return -EINVAL;

		m->timestamp = timestamp;
		m->don = don;
	}

	return 0;
}

static int rtp_h264_reader_aggregation(struct rtp_h264_reader_t* reader, rtp_h264_nalu_handler handler, void* param,
	const uint8_t* payload, int bytes, uint32_t timestamp, int mode)
{
	const uint8_t* ptr;
	struct rtp_h264_member_t m;
	int header, n, rem, r, limit;
	int nal_only, skip; // MTAP: 1-size counts NAL only, bytes before the NAL
	uint16_t don;

	header = (0 == mode) ? 1 : 3; // NAL header(+ DON / DONB)
	n = (mode >= 2) ? mode : 0; // TS offset size: MTAP16=2, MTAP24=3
	nal_only = (mode >= 2) ? reader->mtap_size_nal_only : 1;
	skip = 1 + n; // DOND + TS offset, empty for STAP

	// A trailing run that cannot even hold a size field (2 bytes for STAP,
	// n+3 bytes for MTAP) is ignored by the historical decoder. The strict
	// entry treats it as an incomplete member instead.
	limit = (mode >= 2) ? n + 3 : 2;
	if (reader->strict)
		limit = 0;

	if (bytes < header)
		return -EINVAL;

	don = (0 == mode) ? 0 : nbo_r16(payload + 1); // STAP-B DON / MTAP DONB

	// The strict entry validates the whole aggregation packet before
	// publishing anything, so an incomplete packet never leaks partial
	// members downstream. The historical default entry keeps its original
	// behaviour: members are published as they are parsed.
	if (reader->strict)
	{
		ptr = payload + header;
		rem = bytes - header;
		while (rem > limit)
		{
			if (rtp_h264_member_parse(ptr, rem, mode, nal_only, skip, don, timestamp, 1, &m) < 0)
				goto invalid;
			ptr += m.total;
			rem -= m.total;
			if (1 == mode)
				don = (uint16_t)(don + 1); // STAP-B increments per member
		}
		don = (0 == mode) ? 0 : nbo_r16(payload + 1); // reset for the publish pass
	}

	// publish the members in packet order
	ptr = payload + header;
	rem = bytes - header;
	r = 0;

	while (rem > limit && 0 == r)
	{
		if (rtp_h264_member_parse(ptr, rem, mode, nal_only, skip, don, timestamp, reader->strict, &m) < 0)
			goto invalid;

		r = rtp_h264_reader_emit(reader, handler, param, m.nal, m.nalbytes,
			m.timestamp, m.don, 1 == mode || mode >= 2);
		ptr += m.total;
		rem -= m.total;
		if (1 == mode)
			don = (uint16_t)(don + 1); // STAP-B increments per member
	}

	return r ? r : 1; // packet handled

invalid:
	reader->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
	rtp_h264_reader_discard_fu(reader);
	return -EINVAL;
}

// 5.8. Fragmentation Units (FUs) (p29)
/*
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |  FU indicator |   FU header   |              DON              |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                          FU payload                           |
*/
static int rtp_h264_reader_fu(struct rtp_h264_reader_t* reader, rtp_h264_nalu_handler handler, void* param,
	const uint8_t* payload, int bytes, uint32_t timestamp, int fu_b, int interleaved)
{
	int n, r;
	uint8_t fuheader, hdr, nal_type;
	const uint8_t* data;
	int datalen;

	if (bytes < 2)
		return -EINVAL;

	fuheader = payload[1];
	if (RTP_H264_FU_START(fuheader))
	{
		if (interleaved && !fu_b)
		{
			// interleaved mode only allows FU-B as the start fragment: an
			// FU-A start fragment carries no DON
			rtp_h264_reader_discard_fu(reader);
			return 0; // packet discard
		}

		n = fu_b ? 4 : 2; // FU indicator + FU header + DON(FU-B)
		if (bytes < n)
			return -EINVAL;

		hdr = (uint8_t)((payload[0] & 0xE0) | (fuheader & 0x1F));
		nal_type = RTP_H264_NAL_TYPE(hdr);
		if (reader->strict && !rtp_h264_is_nalu_type(nal_type))
			return -EINVAL; // the historical decoder only asserted this

		// a new start fragment discards any unfinished fragment
		reader->fu_size = 0;
		reader->fu_active = 1;
		reader->fu_timestamp = timestamp;
		reader->fu_nal_type = nal_type;
		reader->fu_has_don = fu_b ? 1 : 0;
		reader->fu_don = fu_b ? nbo_r16(payload + 2) : 0;

		r = rtp_h264_reader_fu_append(reader, &hdr, 1);
		if (r)
		{
			reader->fu_active = 0;
			return r;
		}

		data = payload + n;
		datalen = bytes - n;
	}
	else
	{
		// A continuation fragment. In interleaved mode the start fragment is
		// FU-B(DON included) and every following fragment is FU-A(no DON).
		// In the default modes a FU-B continuation keeps its DON field.
		if (fu_b && interleaved)
		{
			rtp_h264_reader_discard_fu(reader);
			return 0; // packet discard
		}

		n = fu_b ? 4 : 2;
		if (bytes < n)
			return -EINVAL;

		if (!reader->fu_active || reader->fu_size <= 0)
		{
			reader->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			return 0; // packet discard
		}

		// Interleaved mode additionally requires the timestamp and the
		// reconstructed NAL unit type to match before fragments are joined.
		if (interleaved && (timestamp != reader->fu_timestamp || RTP_H264_NAL_TYPE(fuheader) != reader->fu_nal_type))
		{
			rtp_h264_reader_discard_fu(reader);
			reader->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			return 0; // packet discard
		}

		data = payload + n;
		datalen = bytes - n;
	}

	r = rtp_h264_reader_fu_append(reader, data, datalen);
	if (r)
	{
		reader->fu_active = 0;
		return r;
	}

	if (RTP_H264_FU_END(fuheader))
	{
		if (reader->fu_size <= 0)
		{
			rtp_h264_reader_discard_fu(reader);
			return 0;
		}

		// the DON carried by a FU-B start fragment is kept until the FU ends
		r = rtp_h264_reader_emit(reader, handler, param, reader->fu, reader->fu_size,
			timestamp, reader->fu_don, reader->fu_has_don);
		return r ? r : 1; // packet handled
	}

	return 1; // packet handled, wait for more fragments
}

int rtp_h264_reader_input(struct rtp_h264_reader_t* reader, const void* packet, int bytes,
	rtp_h264_nalu_handler handler, void* param, int interleaved)
{
	struct rtp_packet_t pkt;
	const uint8_t* payload;
	int nalt, r;

	if (!reader || !handler || 0 != rtp_packet_deserialize(&pkt, packet, bytes) || pkt.payloadlen < 1)
		return -EINVAL;

	payload = (const uint8_t*)pkt.payload;

	if (reader->flags < 0)
	{
		reader->flags = 0;
		reader->seq = (uint16_t)(pkt.rtp.seq - 1); // disable packet lost on first packet
	}

	if ((uint16_t)pkt.rtp.seq != (uint16_t)(reader->seq + 1))
	{
		// a missing packet means an unfinished FU can no longer be completed
		reader->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
		rtp_h264_reader_discard_fu(reader);
	}
	reader->seq = (uint16_t)pkt.rtp.seq;

	nalt = RTP_H264_NAL_TYPE(payload[0]);
	switch (nalt)
	{
	case 0: // reserved
	case 31: // reserved
		return 0; // packet discard

	case 24: // STAP-A
		if (interleaved)
			return 0; // not allowed in interleaved mode
		return rtp_h264_reader_aggregation(reader, handler, param, payload, pkt.payloadlen, pkt.rtp.timestamp, 0);

	case 25: // STAP-B
		return rtp_h264_reader_aggregation(reader, handler, param, payload, pkt.payloadlen, pkt.rtp.timestamp, 1);

	case 26: // MTAP16
		return rtp_h264_reader_aggregation(reader, handler, param, payload, pkt.payloadlen, pkt.rtp.timestamp, 2);

	case 27: // MTAP24
		return rtp_h264_reader_aggregation(reader, handler, param, payload, pkt.payloadlen, pkt.rtp.timestamp, 3);

	case 28: // FU-A
		return rtp_h264_reader_fu(reader, handler, param, payload, pkt.payloadlen, pkt.rtp.timestamp, 0, interleaved);

	case 29: // FU-B
		return rtp_h264_reader_fu(reader, handler, param, payload, pkt.payloadlen, pkt.rtp.timestamp, 1, interleaved);

	default: // 1-23 single NAL unit
		if (interleaved)
			return 0; // not allowed in interleaved mode
		rtp_h264_reader_discard_fu(reader);
		r = rtp_h264_reader_emit(reader, handler, param, payload, pkt.payloadlen, pkt.rtp.timestamp, 0, 0);
		return r ? r : 1; // packet handled
	}
}

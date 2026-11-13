// RFC6184 RTP Payload Format for H.264 Video
// Structural readers shared by rtp-h264-unpack.c (default, transmission order)
// and rtp-h264-interleaved.c (DON based reordering). The readers only walk the
// caller supplied packet buffer, they never allocate or copy bytes. NAL unit
// type policy is left to the callers; these routines only enforce packet
// structure so external data never depends on assert() being enabled.

#include "rtp-h264-reader.h"
#include "rtp-util.h"
#include <errno.h>

#define H264_NAL(v) ((v) & 0x1F)

int rtp_h264_stap_reader_init(struct rtp_h264_stap_reader_t* reader, const uint8_t* payload, int bytes, int stap_b)
{
	int n;

	if (!reader || !payload || bytes < 1 || H264_NAL(payload[0]) != (stap_b ? 25 : 24))
		return -EINVAL;

	n = stap_b ? 3 : 1; // NAL header, STAP-B additionally carries DON
	if (bytes < n)
		return -EINVAL;

	reader->stap_b = stap_b;
	reader->don = stap_b ? nbo_r16(payload + 1) : 0;
	reader->ptr = payload + n;
	reader->remain = bytes - n;
	return 0;
}

int rtp_h264_stap_reader_next(struct rtp_h264_stap_reader_t* reader, uint32_t timestamp, struct rtp_h264_agg_unit_t* unit)
{
	uint16_t len;

	// a member needs its 2-byte size field; 0..2 trailing bytes mark the end,
	// callers that demand exact packet consumption check reader->remain == 0
	if (reader->remain <= 2)
		return 0; // no more complete member

	len = nbo_r16(reader->ptr);
	if (len + 2 > reader->remain || len < 2 /*NAL header plus at least one payload byte*/)
		return -EINVAL;

	unit->nalu = reader->ptr + 2;
	unit->bytes = len;
	unit->don = reader->don;
	unit->timestamp = timestamp;

	reader->ptr += len + 2;
	reader->remain -= len + 2;
	reader->don = (uint16_t)(reader->don + 1); // STAP members use consecutive DON
	return 1;
}

int rtp_h264_mtap_reader_init(struct rtp_h264_mtap_reader_t* reader, const uint8_t* payload, int bytes, int offset_bytes, uint32_t timestamp)
{
	if (!reader || !payload || bytes < 3)
		return -EINVAL;
	if (offset_bytes != 2 && offset_bytes != 3)
		return -EINVAL;
	if (H264_NAL(payload[0]) != (offset_bytes == 2 ? 26 : 27))
		return -EINVAL;

	reader->offset_bytes = offset_bytes;
	reader->donb = nbo_r16(payload + 1);
	reader->timestamp = timestamp;
	reader->ptr = payload + 3; // NAL header + DONB
	reader->remain = bytes - 3;
	return 0;
}

int rtp_h264_mtap_reader_next(struct rtp_h264_mtap_reader_t* reader, struct rtp_h264_agg_unit_t* unit)
{
	uint16_t len;
	uint8_t dond;
	uint32_t offset;
	int n;

	n = reader->offset_bytes;
	// smallest possible member is size(2) + DOND(1) + offset(n) + NAL header(1)
	if (reader->remain < n + 4)
		return 0; // no more complete member

	len = nbo_r16(reader->ptr);
	// member layout: NALU size(2) + DOND(1) + TS offset(n) + NAL unit
	if (len + 2 > reader->remain || len < 1 + n + 1 /*DOND + offset + NAL header*/)
		return -EINVAL;

	dond = reader->ptr[2];
	offset = nbo_r16(reader->ptr + 3);
	if (3 == n)
		offset = (offset << 8) | reader->ptr[5]; // MTAP24 24-bit offset

	unit->don = (uint16_t)(reader->donb + dond); // 16-bit wrap
	unit->timestamp = reader->timestamp + offset; // 32-bit wrap
	unit->nalu = reader->ptr + 2 + 1 + n; // skip size, DOND and TS offset
	unit->bytes = len - 1 - n; // NAL unit only, DOND/offset bytes excluded

	reader->ptr += len + 2;
	reader->remain -= len + 2;
	return 1;
}

int rtp_h264_read_fu(const uint8_t* payload, int bytes, int fu_b, struct rtp_h264_fu_t* fu)
{
	int n;

	if (!payload || !fu)
		return -EINVAL;

	n = fu_b ? 4 : 2; // FU-B adds the 16-bit DON field
	if (bytes < n || H264_NAL(payload[0]) != (fu_b ? 29 : 28))
		return -EINVAL;

	fu->fu_header = payload[1];
	fu->nalu_header = (uint8_t)((payload[0] & 0xE0) | (fu->fu_header & 0x1F));
	fu->start = (fu->fu_header & 0x80) != 0;
	fu->end = (fu->fu_header & 0x40) != 0;
	fu->has_don = fu_b;
	fu->don = fu_b ? nbo_r16(payload + 2) : 0;
	fu->data = payload + n;
	fu->bytes = bytes - n;
	return 0;
}

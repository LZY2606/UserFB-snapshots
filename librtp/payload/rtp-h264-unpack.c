// RFC6184 RTP Payload Format for H.264 Video

#include "rtp-packet.h"
#include "rtp-payload-internal.h"
#include "rtp-h264-reader.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define H264_NAL(v)	((v) & 0x1F)

struct rtp_decode_h264_t
{
	struct rtp_payload_t handler;
	void* cbparam;

	uint16_t seq; // rtp seq
	uint32_t timestamp;

	uint8_t* ptr;
	int size, capacity;

	int flags;
};

static void* rtp_h264_unpack_create(struct rtp_payload_t *handler, void* param)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)calloc(1, sizeof(*unpacker));
	if(!unpacker)
		return NULL;

	memcpy(&unpacker->handler, handler, sizeof(unpacker->handler));
	unpacker->cbparam = param;
	unpacker->flags = -1;
	return unpacker;
}

static void rtp_h264_unpack_destroy(void* p)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)p;

	if(unpacker->ptr)
		free(unpacker->ptr);
#if defined(_DEBUG) || defined(DEBUG)
	memset(unpacker, 0xCC, sizeof(*unpacker));
#endif
	free(unpacker);
}

// 5.7.1. Single-Time Aggregation Packet (STAP) (p23)
// Packet structure is parsed by the shared STAP reader; this wrapper only
// keeps the historical transmission-order delivery behavior.
static int rtp_h264_unpack_stap(struct rtp_decode_h264_t *unpacker, const uint8_t* ptr, int bytes, uint32_t timestamp, int stap_b)
{
	int r, rr;
	struct rtp_h264_stap_reader_t reader;
	struct rtp_h264_agg_unit_t unit;

	r = 0;
	if (0 != rtp_h264_stap_reader_init(&reader, ptr, bytes, stap_b))
	{
		assert(0);
		return -EINVAL; // error
	}

	while (0 == r)
	{
		rr = rtp_h264_stap_reader_next(&reader, timestamp, &unit);
		if (rr < 0)
		{
			assert(0);
			unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			unpacker->size = 0;
			return -EINVAL; // error
		}
		if (0 == rr)
			break; // no more member

		assert(H264_NAL(unit.nalu[0]) > 0 && H264_NAL(unit.nalu[0]) < 24);
		r = unpacker->handler.packet(unpacker->cbparam, unit.nalu, unit.bytes, unit.timestamp, unpacker->flags);
		unpacker->flags = 0;
		unpacker->size = 0;
	}

	return 0 == r ? 1 : r; // packet handled
}

// 5.7.2. Multi-Time Aggregation Packets (MTAPs) (p27)
static int rtp_h264_unpack_mtap(struct rtp_decode_h264_t *unpacker, const uint8_t* ptr, int bytes, uint32_t timestamp, int n)
{
	int r, rr;
	struct rtp_h264_mtap_reader_t reader;
	struct rtp_h264_agg_unit_t unit;

	r = 0;
	if (0 != rtp_h264_mtap_reader_init(&reader, ptr, bytes, n, timestamp))
	{
		assert(0);
		return -EINVAL; // error
	}

	while (0 == r)
	{
		rr = rtp_h264_mtap_reader_next(&reader, &unit);
		if (rr < 0)
		{
			assert(0);
			unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			unpacker->size = 0;
			return -EINVAL; // error
		}
		if (0 == rr)
			break; // no more member

		assert(H264_NAL(unit.nalu[0]) > 0 && H264_NAL(unit.nalu[0]) < 24);
		r = unpacker->handler.packet(unpacker->cbparam, unit.nalu, unit.bytes, unit.timestamp, unpacker->flags);
		unpacker->flags = 0;
		unpacker->size = 0;
	}

	return 0 == r ? 1 : r; // packet handled
}

// 5.8. Fragmentation Units (FUs) (p29)
/*
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  FU indicator |   FU header   |              DON              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-|
|                                                               |
|                          FU payload                           |
|                                                               |
|                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               :   ...OPTIONAL RTP padding     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
static int rtp_h264_unpack_fu(struct rtp_decode_h264_t *unpacker, const uint8_t* ptr, int bytes, uint32_t timestamp, int fu_b)
{
	int r;
	struct rtp_h264_fu_t fu;

	r = 0;
	if (0 != rtp_h264_read_fu(ptr, bytes, fu_b, &fu) || unpacker->size + fu.bytes > RTP_PAYLOAD_MAX_SIZE)
	{
		assert(0);
		return -EINVAL; // error
	}

	if (unpacker->size + fu.bytes + 1 /*NALU*/ > unpacker->capacity)
	{
		void* p = NULL;
		int size = unpacker->size + fu.bytes + 1;
		size += size / 4 > 128000 ? size / 4 : 128000;
		p = realloc(unpacker->ptr, size);
		if (!p)
		{
			// set packet lost flag
			unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			unpacker->size = 0;
			return -ENOMEM; // error
		}
		unpacker->ptr = (uint8_t*)p;
		unpacker->capacity = size;
	}

	if (fu.start)
	{
#if 0
		if (unpacker->size > 0)
		{
			unpacker->flags |= RTP_PAYLOAD_FLAG_PACKET_CORRUPT;
			unpacker->handler.packet(unpacker->cbparam, unpacker->ptr, unpacker->size, unpacker->timestamp, unpacker->flags);
			unpacker->flags = 0;
			unpacker->size = 0; // reset
		}
#endif

		unpacker->size = 1; // NAL unit type byte
		unpacker->ptr[0] = fu.nalu_header;
		assert(H264_NAL(unpacker->ptr[0]) > 0 && H264_NAL(unpacker->ptr[0]) < 24);
	}
	else
	{
		if (0 == unpacker->size)
		{
			unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			return 0; // packet discard
		}
		assert(unpacker->size > 0);
	}

	unpacker->timestamp = timestamp;
	if (fu.bytes > 0)
	{
		assert(unpacker->capacity >= unpacker->size + fu.bytes);
		memmove(unpacker->ptr + unpacker->size, fu.data, fu.bytes);
		unpacker->size += fu.bytes;
	}

	if(fu.end)
	{
        if(unpacker->size > 0)
            r = unpacker->handler.packet(unpacker->cbparam, unpacker->ptr, unpacker->size, timestamp, unpacker->flags);
		unpacker->flags = 0;
		unpacker->size = 0; // reset
	}

	return 0 == r ? 1 : r; // packet handled
}

static int rtp_h264_unpack_input(void* p, const void* packet, int bytes)
{
	int r;
	uint8_t nalt;
	struct rtp_packet_t pkt;
	struct rtp_decode_h264_t *unpacker;

	unpacker = (struct rtp_decode_h264_t *)p;
	if(!unpacker || 0 != rtp_packet_deserialize(&pkt, packet, bytes) || pkt.payloadlen < 1)
		return -EINVAL;

	if (-1 == unpacker->flags)
	{
		unpacker->flags = 0;
		unpacker->seq = (uint16_t)(pkt.rtp.seq - 1); // disable packet lost
	}

	if ((uint16_t)pkt.rtp.seq != (uint16_t)(unpacker->seq + 1))
	{
		unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
		unpacker->size = 0; // discard previous packets
	}
	unpacker->seq = (uint16_t)pkt.rtp.seq;

	nalt = ((unsigned char *)pkt.payload)[0];
	switch(nalt & 0x1F)
	{
	case 0: // reserved
	case 31: // reserved
		assert(0);
		return 0; // packet discard

	case 24: // STAP-A
		return rtp_h264_unpack_stap(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 0);
	case 25: // STAP-B
		return rtp_h264_unpack_stap(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 1);
	case 26: // MTAP16
		return rtp_h264_unpack_mtap(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 2);
	case 27: // MTAP24
		return rtp_h264_unpack_mtap(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 3);
	case 28: // FU-A
		return rtp_h264_unpack_fu(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 0);
	case 29: // FU-B
		return rtp_h264_unpack_fu(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 1);

	default: // 1-23 NAL unit
		r = unpacker->handler.packet(unpacker->cbparam, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, unpacker->flags);
		unpacker->flags = 0;
		unpacker->size = 0;
		return 0 == r ? 1 : r; // packet handled
	}
}

struct rtp_payload_decode_t *rtp_h264_decode()
{
	static struct rtp_payload_decode_t unpacker = {
		rtp_h264_unpack_create,
		rtp_h264_unpack_destroy,
		rtp_h264_unpack_input,
	};

	return &unpacker;
}

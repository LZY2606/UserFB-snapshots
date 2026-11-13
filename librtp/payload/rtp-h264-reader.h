#ifndef _rtp_h264_reader_h_
#define _rtp_h264_reader_h_

// RFC6184 RTP Payload Format for H.264 Video
// Shared structural readers for the default H.264 unpacker and the
// interleaved (DON aware) unpacker. None of these routines allocate memory;
// every returned pointer references the caller supplied packet buffer and is
// only valid while that buffer stays alive.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One completely parsed NAL unit embedded in an aggregation packet.
struct rtp_h264_agg_unit_t
{
	const uint8_t* nalu; // NAL unit bytes (NAL header + payload), no size/DOND/offset fields
	int bytes;           // NAL unit length in bytes
	uint16_t don;        // decoding order number
	uint32_t timestamp;  // NAL unit RTP timestamp (32-bit wrap already applied)
};

// 5.7.1 Single-Time Aggregation Packet (STAP-A type 24 / STAP-B type 25)
struct rtp_h264_stap_reader_t
{
	const uint8_t* ptr;
	int remain;
	uint16_t don; // next member DON (STAP-A keeps a running counter too)
	int stap_b;
};

/// @param[in] stap_b 0-STAP-A, 1-STAP-B
/// @return 0-ok, EINVAL-malformed header
int rtp_h264_stap_reader_init(struct rtp_h264_stap_reader_t* reader, const uint8_t* payload, int bytes, int stap_b);

/// @return 1-one unit filled, 0-no more unit, <0-malformed packet (no unit published)
int rtp_h264_stap_reader_next(struct rtp_h264_stap_reader_t* reader, uint32_t timestamp, struct rtp_h264_agg_unit_t* unit);

// 5.7.2 Multi-Time Aggregation Packet (MTAP16 type 26 / MTAP24 type 27)
struct rtp_h264_mtap_reader_t
{
	const uint8_t* ptr;
	int remain;
	uint16_t donb;
	int offset_bytes; // 2-MTAP16, 3-MTAP24
	uint32_t timestamp;
};

/// @param[in] offset_bytes 2-MTAP16, 3-MTAP24
/// @return 0-ok, EINVAL-malformed header
int rtp_h264_mtap_reader_init(struct rtp_h264_mtap_reader_t* reader, const uint8_t* payload, int bytes, int offset_bytes, uint32_t timestamp);

/// @return 1-one unit filled, 0-no more unit, <0-malformed packet (no unit published)
int rtp_h264_mtap_reader_next(struct rtp_h264_mtap_reader_t* reader, struct rtp_h264_agg_unit_t* unit);

// 5.8 Fragmentation Unit (FU-A type 28 / FU-B type 29)
struct rtp_h264_fu_t
{
	uint8_t nalu_header; // reconstructed NAL unit header byte
	uint8_t fu_header;   // raw FU header (S/E/type bits)
	const uint8_t* data; // fragment payload, NULL when empty
	int bytes;           // fragment payload length
	int start;           // S bit
	int end;             // E bit
	int has_don;         // 1-FU-B, DON field present
	uint16_t don;        // valid only when has_don is 1
};

/// @param[in] fu_b 0-FU-A, 1-FU-B
/// @return 0-ok, EINVAL-malformed fragment
int rtp_h264_read_fu(const uint8_t* payload, int bytes, int fu_b, struct rtp_h264_fu_t* fu);

#ifdef __cplusplus
}
#endif

#endif /* !_rtp_h264_reader_h_ */

/*

  This file is a part of JRTPLIB
  Copyright (c) 1999-2004 Jori Liesenborgs

  Contact: jori@lumumba.luc.ac.be

  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#ifndef RTPPCMUMIXER_H

#define RTPPCMUMIXER_H

#include "rtpconfig.h"
#include "rtptypes.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

class RTPPacket;
class RTPPacketBuilder;

struct RTPPCMUMixInput
{
	u_int32_t ssrc;
	u_int32_t timestamp;
};

// Internal mixer state. One instance is owned by RTPSources; the default
// constructed instance is disabled until Configure() succeeds.
class RTPPCMUMixer
{
public:
	static const size_t PayloadSize = 160;		// samples per 20 ms PCMU packet
	static const size_t MaxInputs = 15;		// RTP_MAXCSRCS
	static const size_t MaxWindowSlots = 256;
	static const uint64_t SlotDurationUs = 20000ULL;

	RTPPCMUMixer();
	~RTPPCMUMixer();

	int Configure(const std::vector<RTPPCMUMixInput>& inputs,uint64_t start_us,size_t window_slots);
	void Reset();
	bool IsConfigured() const							{ return configured; }

	bool IsMember(u_int32_t ssrc) const;
	void ProcessPacket(RTPPacket *pack);
	void RemoveMember(u_int32_t ssrc);

	// Builds exactly one due slot. Returns 0 on success, 1 when the head
	// slot is not due yet, or a negative JRTPLIB error code. On builder
	// failure nothing is advanced: head, stored samples and packet are
	// left untouched so the same slot can be retried.
	int BuildPacket(uint64_t now_us,RTPPacketBuilder& builder,std::vector<u_int8_t>& packet);

	// ITU-T G.711 mu-law conversions, shared by the mixer and its tests.
	static int16_t PCMUDecode(u_int8_t value);
	static u_int8_t PCMUEncode(int16_t value);
private:
	struct Member
	{
		u_int32_t ssrc;
		u_int32_t base_timestamp;
		bool active;
	};

	bool configured;
	uint64_t start_us;
	size_t window_slots;
	size_t head;						// next slot to output
	std::vector<Member> members;			// kept sorted by ssrc
	std::vector<u_int8_t> samples;		// ring buffer: window*members*160 bytes
	std::vector<u_int8_t> filled;		// ring buffer: window*members presence flags
};

#endif // RTPPCMUMIXER_H


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

#include "rtppcmumixer.h"
#include "rtppacket.h"
#include "rtppacketbuilder.h"
#include "rtpdefines.h"
#include "rtperrors.h"
#include <algorithm>
#include <cstring>

// ITU-T G.711 mu-law constants
static const int PCMU_BIAS = 0x84;			// 132
static const int PCMU_CLIP = 32635;
static const int PCMU_SEGEND[8] = { 0xFF,0x1FF,0x3FF,0x7FF,0xFFF,0x1FFF,0x3FFF,0x7FFF };

RTPPCMUMixer::RTPPCMUMixer()
{
	configured = false;
	start_us = 0;
	window_slots = 0;
	head = 0;
}

RTPPCMUMixer::~RTPPCMUMixer()
{
}

int RTPPCMUMixer::Configure(const std::vector<RTPPCMUMixInput>& inputs,uint64_t start,size_t windowslots)
{
	if (configured)
		return ERR_RTP_PACKBUILD_ALREADYINIT;
	if (inputs.empty() || inputs.size() > MaxInputs)
		return ERR_RTP_PACKET_ILLEGALBUFFERSIZE;
	if (windowslots < 1 || windowslots > MaxWindowSlots)
		return ERR_RTP_PACKET_ILLEGALBUFFERSIZE;

	std::vector<RTPPCMUMixInput> sorted = inputs;
	std::sort(sorted.begin(),sorted.end(),[](const RTPPCMUMixInput& a,const RTPPCMUMixInput& b) {
		return a.ssrc < b.ssrc;
	});
	for (size_t i = 1 ; i < sorted.size() ; i++)
	{
		if (sorted[i].ssrc == sorted[i-1].ssrc)
			return ERR_RTP_PACKBUILD_CSRCALREADYINLIST;
	}

	members.clear();
	for (size_t i = 0 ; i < sorted.size() ; i++)
	{
		Member m;
		m.ssrc = sorted[i].ssrc;
		m.base_timestamp = sorted[i].timestamp;
		m.active = true;
		members.push_back(m);
	}

	start_us = start;
	window_slots = windowslots;
	head = 0;
	samples.assign(window_slots * members.size() * PayloadSize,0);
	filled.assign(window_slots * members.size(),0);
	configured = true;
	return 0;
}

void RTPPCMUMixer::Reset()
{
	configured = false;
	start_us = 0;
	window_slots = 0;
	head = 0;
	members.clear();
	samples.clear();
	filled.clear();
}

bool RTPPCMUMixer::IsMember(u_int32_t ssrc) const
{
	if (!configured)
		return false;
	for (size_t i = 0 ; i < members.size() ; i++)
	{
		if (members[i].ssrc == ssrc)
			return members[i].active;
	}
	return false;
}

void RTPPCMUMixer::RemoveMember(u_int32_t ssrc)
{
	if (!configured)
		return;
	for (size_t m = 0 ; m < members.size() ; m++)
	{
		if (members[m].ssrc == ssrc && members[m].active)
		{
			// Revoke membership together with every contribution that has
			// not been output yet. The entry itself is kept (inactive) so
			// the ring buffer layout stays valid.
			members[m].active = false;
			for (size_t slot = head ; slot < head + window_slots ; slot++)
			{
				size_t ring = slot % window_slots;
				filled[ring * members.size() + m] = 0;
			}
			return;
		}
	}
}

void RTPPCMUMixer::ProcessPacket(RTPPacket *pack)
{
	if (!configured)
		return;

	// Input contract: no CSRC, payload type 0 (PCMU), exactly one 20 ms frame.
	if (pack->GetCSRCCount() != 0)
		return;
	if (pack->GetPayloadType() != 0)
		return;
	if (pack->GetPayloadLength() != PayloadSize)
		return;

	size_t m = 0;
	bool found = false;
	for ( ; m < members.size() ; m++)
	{
		if (members[m].ssrc == pack->GetSSRC())
		{
			found = true;
			break;
		}
	}
	if (!found || !members[m].active)
		return;

	// Signed modular difference against the configured slot-0 timestamp.
	int32_t d = (int32_t)(pack->GetTimestamp() - members[m].base_timestamp);
	if (d < 0)
		return;						// older than the configured origin
	if ((d % (int32_t)PayloadSize) != 0)
		return;						// does not land on a slot boundary
	size_t slot = (size_t)(d / (int32_t)PayloadSize);
	if (slot < head)
		return;						// late packet for a consumed slot
	if (slot >= head + window_slots)
		return;						// outside the reception window

	size_t ring = slot % window_slots;
	size_t fillidx = ring * members.size() + m;
	if (filled[fillidx])
		return;						// first valid packet for the slot wins

	std::memcpy(&samples[ring * members.size() * PayloadSize + m * PayloadSize],
	            pack->GetPayloadData(),PayloadSize);
	filled[fillidx] = 1;
}

int RTPPCMUMixer::BuildPacket(uint64_t now_us,RTPPacketBuilder& builder,std::vector<u_int8_t>& packet)
{
	if (!configured)
		return ERR_RTP_PACKBUILD_NOTINIT;

	uint64_t due_us = start_us + (uint64_t)head * SlotDurationUs;
	if (now_us < due_us)
		return 1;					// head slot is not due yet; nothing changes

	size_t ring = head % window_slots;
	size_t nmembers = members.size();
	u_int8_t mixed[PayloadSize];
	std::vector<u_int32_t> csrcs;

	for (size_t i = 0 ; i < PayloadSize ; i++)
	{
		int32_t sum = 0;			// wide accumulator: saturate only the total
		for (size_t m = 0 ; m < nmembers ; m++)
		{
			if (members[m].active && filled[ring * nmembers + m])
				sum += (int32_t)PCMUDecode(samples[ring * nmembers * PayloadSize + m * PayloadSize + i]);
		}
		if (sum > 32767)
			sum = 32767;
		if (sum < -32768)
			sum = -32768;
		mixed[i] = PCMUEncode((int16_t)sum);
	}

	for (size_t m = 0 ; m < nmembers ; m++)
	{
		if (members[m].active && filled[ring * nmembers + m])
			csrcs.push_back(members[m].ssrc);	// members are sorted by ssrc
	}

	bool mark = (head == 0);
	int status = builder.BuildPacketWithCSRC(mixed,PayloadSize,0,mark,PayloadSize,csrcs);
	if (status < 0)
		return status;				// builder state, head, samples and packet kept

	packet.assign(builder.GetPacket(),builder.GetPacket() + builder.GetPacketLength());

	// Release the slot: later late packets must not be able to revive it.
	for (size_t m = 0 ; m < nmembers ; m++)
		filled[ring * nmembers + m] = 0;
	head++;
	return 0;
}

int16_t RTPPCMUMixer::PCMUDecode(u_int8_t value)
{
	u_int8_t v = (u_int8_t)(~value);
	int segment = (v & 0x70) >> 4;
	int t = ((v & 0x0F) << 3) + PCMU_BIAS;
	t <<= segment;
	int linear = t - PCMU_BIAS;
	// Sign bit set (after complement) marks the positive half of the scale.
	return (v & 0x80) ? (int16_t)(-linear) : (int16_t)linear;
}

u_int8_t RTPPCMUMixer::PCMUEncode(int16_t value)
{
	int pcm = (int)value;
	int mask;

	if (pcm < 0)
	{
		pcm = PCMU_BIAS - pcm;
		mask = 0x7F;
	}
	else
	{
		pcm += PCMU_BIAS;
		mask = 0xFF;
	}
	if (pcm > PCMU_CLIP)
		pcm = PCMU_CLIP;

	int segment = 0;
	while (segment < 8 && pcm > PCMU_SEGEND[segment])
		segment++;
	if (segment >= 8)
		return (u_int8_t)(0x7F ^ mask);

	u_int8_t aval = (u_int8_t)((segment << 4) | ((pcm >> (segment + 3)) & 0x0F));
	return (u_int8_t)(aval ^ mask);	// zero naturally encodes to 0xff
}


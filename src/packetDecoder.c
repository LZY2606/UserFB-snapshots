/*
 * Copyright (c) 2011-2019 Belledonne Communications SARL.
 *
 * This file is part of bcg729.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "typedef.h"

#include "bcg729/decoder.h"
#include "bcg729/packetDecoder.h"

/* A packet decoder wraps one independent decoder channel plus the activity  */
/* state the packet layer is responsible for tracking. The underlying decoder */
/* keeps its own previousFrameIsActiveFlag, this flag drives the packet level */
/* checks (DTX legality, PLC labelling) and is committed only once a whole     */
/* packet has been decoded.                                                   */
struct bcg729PacketDecoderContextStruct_struct {
	bcg729DecoderChannelContextStruct *decoderChannelContext;
	uint8_t inComfortNoise; /* 0: active speech, 1: comfort noise after a SID */
};

/*****************************************************************************/
/* initBcg729PacketDecoder : create and initialise a packet decoder context  */
/*    return value :                                                         */
/*      - the packet decoder context, NULL on allocation failure             */
/*                                                                           */
/*****************************************************************************/
bcg729PacketDecoderContextStruct *initBcg729PacketDecoder(void)
{
	bcg729PacketDecoderContextStruct *context = malloc(sizeof(bcg729PacketDecoderContextStruct));
	if (context == NULL) {
		return NULL;
	}

	context->decoderChannelContext = initBcg729DecoderChannel();
	if (context->decoderChannelContext == NULL) {
		free(context);
		return NULL;
	}

	/* a fresh instance starts in active speech */
	context->inComfortNoise = 0;

	return context;
}

/*****************************************************************************/
/* closeBcg729PacketDecoder : free memory of the context structure           */
/*    parameters:                                                            */
/*      -(i) context : the packet decoder context data                       */
/*                                                                           */
/*****************************************************************************/
void closeBcg729PacketDecoder(bcg729PacketDecoderContextStruct *context)
{
	if (context != NULL) {
		closeBcg729DecoderChannel(context->decoderChannelContext);
		free(context);
	}
	return;
}

/*****************************************************************************/
/* bcg729DecodePacket : decode one RTP-free G.729 packet, or one missing     */
/* frame, into 80 samples PCM frames.                                        */
/*                                                                           */
/*****************************************************************************/
int bcg729DecodePacket(bcg729PacketDecoderContextStruct *context, bcg729PacketEvent_t event, const uint8_t *payload, size_t payloadLength, int16_t *pcm, size_t pcmCapacity, bcg729FrameType_t *frameInfo, size_t infoCapacity, size_t *frameCount)
{
	size_t frameNumber = 0;      /* number of frames this call would deliver */
	size_t speechFrameNumber = 0; /* number of 10 bytes speech frames in a PAYLOAD */
	uint8_t hasSID = 0;          /* a PAYLOAD carries a trailing 2 bytes SID */
	uint8_t nextComfortNoise;    /* state at the end of the call, committed on success only */

	/* every successful call delivers at least one frame, so all output pointers are mandatory */
	if (context == NULL || pcm == NULL || frameInfo == NULL || frameCount == NULL) {
		return 0;
	}
	if (context->decoderChannelContext == NULL) {
		return 0;
	}

	nextComfortNoise = context->inComfortNoise;

	switch (event) {
		case BCG729_PACKET_EVENT_PAYLOAD: {
			size_t remainder;
			/* a PAYLOAD must be a non empty raw G.729 Annex A/B payload */
			if (payload == NULL || payloadLength == 0) {
				return 0;
			}
			remainder = payloadLength % BCG729_PACKET_SPEECH_FRAME_LENGTH;
			/* only 10*n or 10*n+2 lengths are legal, the 2 bytes can only be a SID */
			if (remainder != 0 && remainder != BCG729_PACKET_SID_FRAME_LENGTH) {
				return 0;
			}
			speechFrameNumber = payloadLength / BCG729_PACKET_SPEECH_FRAME_LENGTH;
			hasSID = (remainder == BCG729_PACKET_SID_FRAME_LENGTH);
			frameNumber = speechFrameNumber + (hasSID ? 1 : 0);
			break;
		}
		case BCG729_PACKET_EVENT_DTX:
			/* DTX carries no payload and is legal only once a SID entered comfort noise */
			if (payload != NULL || payloadLength != 0) {
				return 0;
			}
			if (!nextComfortNoise) {
				return 0;
			}
			frameNumber = 1;
			break;
		case BCG729_PACKET_EVENT_ERASURE:
			/* an erasure carries no payload, the decoder channel runs PLC or CNG itself */
			if (payload != NULL || payloadLength != 0) {
				return 0;
			}
			frameNumber = 1;
			break;
		default:
			/* unknown event */
			return 0;
	}

	/* capacity and overflow checks, all done before any decoding happens:    */
	/* division based comparison avoids any size_t overflow on frameNumber*80 */
	if (frameNumber > pcmCapacity / BCG729_PACKET_SAMPLES_PER_FRAME) {
		return 0;
	}
	if (frameNumber > infoCapacity) {
		return 0;
	}

	if (event == BCG729_PACKET_EVENT_PAYLOAD) {
		size_t frameIndex;
		/* decode the 10 bytes speech frames in order, each one re-enters active speech */
		for (frameIndex = 0; frameIndex < speechFrameNumber; frameIndex++) {
			bcg729Decoder(context->decoderChannelContext,
				payload + frameIndex * BCG729_PACKET_SPEECH_FRAME_LENGTH,
				BCG729_PACKET_SPEECH_FRAME_LENGTH, 0, 0, 0,
				pcm + frameIndex * BCG729_PACKET_SAMPLES_PER_FRAME);
			frameInfo[frameIndex] = BCG729_FRAME_SPEECH;
			nextComfortNoise = 0;
		}
		/* the trailing 2 bytes are a SID frame and switch to comfort noise */
		if (hasSID) {
			bcg729Decoder(context->decoderChannelContext,
				payload + speechFrameNumber * BCG729_PACKET_SPEECH_FRAME_LENGTH,
				BCG729_PACKET_SID_FRAME_LENGTH, 0, 1, 0,
				pcm + speechFrameNumber * BCG729_PACKET_SAMPLES_PER_FRAME);
			frameInfo[speechFrameNumber] = BCG729_FRAME_SID;
			nextComfortNoise = 1;
		}
	} else if (event == BCG729_PACKET_EVENT_DTX) {
		/* untransmitted frame in comfort noise: the decoder reuses the last SID parameters */
		bcg729Decoder(context->decoderChannelContext, NULL, 0, 1, 0, 0, pcm);
		frameInfo[0] = BCG729_FRAME_DTX;
	} else {
		/* erasure: the decoder channel selects speech PLC or CNG from its own activity state */
		bcg729Decoder(context->decoderChannelContext, NULL, 0, 1, 0, 0, pcm);
		frameInfo[0] = nextComfortNoise ? BCG729_FRAME_PLC_COMFORT : BCG729_FRAME_PLC_SPEECH;
	}

	/* commit the new activity state and deliver the frame count only on success */
	context->inComfortNoise = nextComfortNoise;
	*frameCount = frameNumber;

	return 1;
}

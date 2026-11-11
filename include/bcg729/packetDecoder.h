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
#ifndef PACKET_DECODER_H
#define PACKET_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
	#ifdef BCG729_STATIC
		#define BCG729_PACKET_VISIBILITY
	#else
		#ifdef BCG729_EXPORTS
			#define BCG729_PACKET_VISIBILITY __declspec(dllexport)
		#else
			#define BCG729_PACKET_VISIBILITY __declspec(dllimport)
		#endif
	#endif
#else
	#define BCG729_PACKET_VISIBILITY __attribute__ ((visibility ("default")))
#endif

/* Number of 16 bits PCM samples delivered per G.729 frame */
#define BCG729_PACKET_SAMPLES_PER_FRAME 80

/* Length in bytes of an encoded speech frame and of a SID frame */
#define BCG729_PACKET_SPEECH_FRAME_LENGTH 10
#define BCG729_PACKET_SID_FRAME_LENGTH 2

/* Event that drove the production of a decoded frame */
typedef enum bcg729PacketEvent {
	BCG729_PACKET_EVENT_PAYLOAD = 0,  /* a G.729 Annex A/B payload was received */
	BCG729_PACKET_EVENT_DTX = 1,      /* no frame transmitted, comfort noise shall be generated */
	BCG729_PACKET_EVENT_ERASURE = 2   /* a frame was lost, packet loss concealment shall run */
} bcg729PacketEvent_t;

/* Interpretation of each delivered frame */
typedef enum bcg729FrameType {
	BCG729_FRAME_SPEECH = 0,      /* frame decoded from a 10 bytes speech payload */
	BCG729_FRAME_SID = 1,         /* frame decoded from the 2 bytes SID at the end of a payload */
	BCG729_FRAME_DTX = 2,         /* comfort noise generated after a SID, no new payload */
	BCG729_FRAME_PLC_SPEECH = 3,  /* concealed frame while the session is in active speech */
	BCG729_FRAME_PLC_COMFORT = 4  /* concealed frame while the session is in comfort noise */
} bcg729FrameType_t;

typedef struct bcg729PacketDecoderContextStruct_struct bcg729PacketDecoderContextStruct;

/*****************************************************************************/
/* initBcg729PacketDecoder : create and initialise a packet decoder context  */
/*    return value :                                                         */
/*      - the packet decoder context, NULL on allocation failure             */
/*                                                                           */
/* Each context owns an independent decoding state (PLC history, CNG random  */
/* seeds, ...). Several contexts can be used concurrently in an interleaved  */
/* way, they never share any mutable state.                                  */
/* A freshly created context is in active speech state.                      */
/*****************************************************************************/
BCG729_PACKET_VISIBILITY bcg729PacketDecoderContextStruct *initBcg729PacketDecoder(void);

/*****************************************************************************/
/* closeBcg729PacketDecoder : free memory of the context structure           */
/*    parameters:                                                            */
/*      -(i) context : the packet decoder context data, NULL is tolerated    */
/*                                                                           */
/*****************************************************************************/
BCG729_PACKET_VISIBILITY void closeBcg729PacketDecoder(bcg729PacketDecoderContextStruct *context);

/*****************************************************************************/
/* bcg729DecodePacket : decode one RTP-free G.729 packet, or one missing     */
/* frame, into 80 samples PCM frames.                                        */
/*                                                                           */
/*    parameters:                                                            */
/*      -(i) context : the packet decoder context data                       */
/*      -(i) event : PAYLOAD, DTX or ERASURE                                 */
/*      -(i) payload : raw G.729 Annex A/B payload (RTP header removed),    */
/*           must be NULL for DTX and ERASURE                                */
/*      -(i) payloadLength : payload length in bytes, must be 0 for DTX and  */
/*           ERASURE                                                         */
/*      -(o) pcm : buffer receiving the decoded 16 bits mono samples        */
/*      -(i) pcmCapacity : usable size of the pcm buffer, in int16_t        */
/*           samples                                                         */
/*      -(o) frameInfo : buffer receiving one bcg729FrameType_t per         */
/*           delivered frame                                                 */
/*      -(i) infoCapacity : usable size of the frameInfo buffer, in         */
/*           bcg729FrameType_t entries                                       */
/*      -(o) frameCount : number of delivered 80 samples frames              */
/*                                                                           */
/* PAYLOAD accepts only lengths 10*n or 10*n+2 (n >= 0): the last 2 bytes   */
/* of a 10*n+2 payload are a SID frame and can never be parsed as speech.   */
/* DTX is legal only once a SID frame put the context in comfort noise       */
/* state; ERASURE always delivers one frame, concealed as speech or comfort  */
/* noise according to the current state.                                     */
/*                                                                           */
/* All arguments and lengths are validated before any decoding happens: on   */
/* failure the function returns 0 and the pcm, frameInfo and frameCount      */
/* buffers as well as the decoding history are kept untouched. On success it */
/* returns 1 and *frameCount holds the number of delivered frames (each one  */
/* being 80 samples).                                                        */
/*****************************************************************************/
BCG729_PACKET_VISIBILITY int bcg729DecodePacket(bcg729PacketDecoderContextStruct *context, bcg729PacketEvent_t event, const uint8_t *payload, size_t payloadLength, int16_t *pcm, size_t pcmCapacity, bcg729FrameType_t *frameInfo, size_t infoCapacity, size_t *frameCount);

#endif /* ifndef PACKET_DECODER_H */

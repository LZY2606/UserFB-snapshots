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
/*****************************************************************************/
/* Conformance test for the packet based decoder entry point.               */
/*                                                                           */
/* Speech frames are produced by the library encoder from integer formula    */
/* generated square wave/silence inputs, SID frames are built from explicit  */
/* locally defined bytes. Every packet decoder output is compared sample by  */
/* sample with a plain decoder channel driven frame by frame with the        */
/* matching speech/SID/erasure flags. No audio file, sound card or network   */
/* is used.                                                                  */
/*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "bcg729/encoder.h"
#include "bcg729/decoder.h"
#include "bcg729/packetDecoder.h"

#define TEST_FRAME_SIZE 80
#define MAX_SEGMENT_FRAMES 64
#define MAX_CALL_FRAMES 16

/* SID frames built from explicit bytes: bit layout is L0(1) L1(5) L2(6) Gain(5) */
static const uint8_t sidPatternA[2] = {0x6c, 0x92};
static const uint8_t sidPatternB[2] = {0x12, 0x48};

static FILE *traceFile = NULL;

static const char *eventName(bcg729PacketEvent_t event)
{
	switch (event) {
		case BCG729_PACKET_EVENT_PAYLOAD:
			return "PAYLOAD";
		case BCG729_PACKET_EVENT_DTX:
			return "DTX";
		case BCG729_PACKET_EVENT_ERASURE:
			return "ERASURE";
		default:
			return "UNKNOWN";
	}
}

static const char *frameTypeName(bcg729FrameType_t type)
{
	switch (type) {
		case BCG729_FRAME_SPEECH:
			return "SPEECH";
		case BCG729_FRAME_SID:
			return "SID";
		case BCG729_FRAME_DTX:
			return "DTX";
		case BCG729_FRAME_PLC_SPEECH:
			return "PLC_SPEECH";
		case BCG729_FRAME_PLC_COMFORT:
			return "PLC_COMFORT";
		default:
			return "INVALID";
	}
}

static void traceCall(const char *segment, bcg729PacketEvent_t event, const uint8_t *payload, size_t payloadLength, int accepted, const bcg729FrameType_t *types, size_t frameCount)
{
	size_t i;
	fprintf(traceFile, "%s: event=%s len=%zu payload=", segment, eventName(event), payloadLength);
	if (payload == NULL) {
		fprintf(traceFile, "null");
	} else {
		for (i = 0; i < payloadLength; i++) {
			fprintf(traceFile, "%02x%s", payload[i], (i + 1 == payloadLength) ? "" : " ");
		}
	}
	fprintf(traceFile, " -> %s", accepted ? "OK" : "REJECTED");
	if (accepted) {
		fprintf(traceFile, " frames=%zu types=", frameCount);
		for (i = 0; i < frameCount; i++) {
			fprintf(traceFile, "%s%s", frameTypeName(types[i]), (i + 1 == frameCount) ? "" : ",");
		}
	}
	fprintf(traceFile, "\n");
	fflush(traceFile);
}

/* A session pairs one packet decoder under test with one reference encoder   */
/* (speech frame production) and one reference plain decoder channel.        */
typedef struct {
	const char *name;
	bcg729PacketDecoderContextStruct *packet;
	bcg729EncoderChannelContextStruct *encoder;
	bcg729DecoderChannelContextStruct *reference;
	int16_t pcm[MAX_SEGMENT_FRAMES * TEST_FRAME_SIZE];
	bcg729FrameType_t types[MAX_SEGMENT_FRAMES];
	size_t frames;
	int generationIndex;
	int firstDifference; /* global sample index of the first PCM mismatch, -1 if none */
} Session;

static Session *createSession(const char *name)
{
	Session *session = calloc(1, sizeof(Session));
	if (session == NULL) {
		return NULL;
	}
	session->name = name;
	session->packet = initBcg729PacketDecoder();
	session->encoder = initBcg729EncoderChannel(0); /* VAD off: every encoded frame is 10 bytes speech */
	session->reference = initBcg729DecoderChannel();
	if (session->packet == NULL || session->encoder == NULL || session->reference == NULL) {
		printf("FATAL: context allocation failed for %s\n", name);
		exit(2);
	}
	session->frames = 0;
	session->generationIndex = 0;
	session->firstDifference = -1;
	return session;
}

static void destroySession(Session *session)
{
	if (session == NULL) {
		return;
	}
	closeBcg729PacketDecoder(session->packet);
	closeBcg729EncoderChannel(session->encoder);
	closeBcg729DecoderChannel(session->reference);
	free(session);
}

/* Integer formula only: short square wave on the first frames, silence after */
static void generateInputFrame(Session *session, int16_t inputFrame[TEST_FRAME_SIZE])
{
	int i;
	int index = session->generationIndex++;
	for (i = 0; i < TEST_FRAME_SIZE; i++) {
		if (index < 4) {
			/* 400 Hz square wave (20 samples period), bounded amplitude */
			inputFrame[i] = (((i / 10) % 2) == 0) ? 5000 : -5000;
		} else {
			inputFrame[i] = 0;
		}
	}
}

static void encodeSpeechFrame(Session *session, uint8_t bitStream[10])
{
	int16_t inputFrame[TEST_FRAME_SIZE];
	uint8_t bitStreamLength = 0;
	generateInputFrame(session, inputFrame);
	bcg729Encoder(session->encoder, inputFrame, bitStream, &bitStreamLength);
	if (bitStreamLength != 10) {
		printf("FATAL: encoder produced a %u bytes frame, expected 10\n", bitStreamLength);
		exit(2);
	}
}

/* Drive the reference decoder one frame with the flag matching the packet    */
/* layer interpretation, then keep the PCM for the sample by sample compare.  */
static void referenceDecodeFrame(Session *session, bcg729FrameType_t type, const uint8_t *frame)
{
	int16_t output[TEST_FRAME_SIZE];
	switch (type) {
		case BCG729_FRAME_SPEECH:
			bcg729Decoder(session->reference, frame, 10, 0, 0, 0, output);
			break;
		case BCG729_FRAME_SID:
			bcg729Decoder(session->reference, frame, 2, 0, 1, 0, output);
			break;
		case BCG729_FRAME_DTX:
		case BCG729_FRAME_PLC_SPEECH:
		case BCG729_FRAME_PLC_COMFORT:
			/* the plain decoder selects speech PLC or CNG from its own activity state */
			bcg729Decoder(session->reference, NULL, 0, 1, 0, 0, output);
			break;
		default:
			printf("FATAL: invalid reference frame type %d\n", (int)type);
			exit(2);
	}
	memcpy(session->pcm + session->frames * TEST_FRAME_SIZE, output, sizeof(output));
	session->types[session->frames] = type;
	session->frames++;
}

/* Compare the packet decoder output of one successful call with the freshly   */
/* produced reference output, returns the index of the first mismatch or -1.  */
static int compareCallOutput(Session *session, const int16_t *pcm, const bcg729FrameType_t *types, size_t frameCount, size_t baseFrame)
{
	size_t frame;
	for (frame = 0; frame < frameCount; frame++) {
		size_t sample;
		if (types[frame] != session->types[baseFrame + frame]) {
			printf("  %s: frame %zu type mismatch: got %s expected %s\n", session->name, baseFrame + frame, frameTypeName(types[frame]), frameTypeName(session->types[baseFrame + frame]));
			if (session->firstDifference < 0) {
				session->firstDifference = (int)(baseFrame + frame) * TEST_FRAME_SIZE;
			}
			return (int)(baseFrame + frame);
		}
		for (sample = 0; sample < TEST_FRAME_SIZE; sample++) {
			size_t index = (baseFrame + frame) * TEST_FRAME_SIZE + sample;
			if (pcm[frame * TEST_FRAME_SIZE + sample] != session->pcm[index]) {
				printf("  %s: sample %zu mismatch: got %d expected %d\n", session->name, index, (int)pcm[frame * TEST_FRAME_SIZE + sample], (int)session->pcm[index]);
				if (session->firstDifference < 0) {
					session->firstDifference = (int)index;
				}
				return (int)index;
			}
		}
	}
	return -1;
}

/* Successful PAYLOAD call with caller supplied capacities: expectedTypes lists */
/* the frame interpretations. The reference decoder is fed the same frames so  */
/* its history stays aligned with the packet decoder under test.               */
static int expectPacketCapacity(Session *session, const char *segment, const uint8_t *payload, size_t payloadLength, const bcg729FrameType_t *expectedTypes, size_t expectedCount, size_t pcmCapacity, size_t infoCapacity)
{
	int16_t pcm[MAX_CALL_FRAMES * TEST_FRAME_SIZE];
	bcg729FrameType_t types[MAX_CALL_FRAMES];
	size_t frameCount = 0;
	size_t frame;
	int firstDifference;
	int rc = bcg729DecodePacket(session->packet, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, pcm, pcmCapacity, types, infoCapacity, &frameCount);
	if (rc != 1) {
		printf("  %s: valid %zu bytes payload rejected\n", session->name, payloadLength);
		traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, 0, NULL, 0);
		return 1;
	}
	if (frameCount != expectedCount) {
		printf("  %s: frame count %zu, expected %zu\n", session->name, frameCount, expectedCount);
		traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, 1, types, frameCount);
		return 1;
	}
	for (frame = 0; frame < expectedCount; frame++) {
		const uint8_t *framePayload;
		if (expectedTypes[frame] == BCG729_FRAME_SID) {
			framePayload = payload + payloadLength - 2;
		} else {
			framePayload = payload + frame * 10;
		}
		referenceDecodeFrame(session, expectedTypes[frame], framePayload);
	}
	firstDifference = compareCallOutput(session, pcm, types, frameCount, session->frames - frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, 1, types, frameCount);
	if (firstDifference >= 0) {
		return 1;
	}
	return 0;
}

/* Successful PAYLOAD call: expectedTypes lists the frame interpretations.     */
static int expectPacket(Session *session, const char *segment, const uint8_t *payload, size_t payloadLength, const bcg729FrameType_t *expectedTypes, size_t expectedCount)
{
	return expectPacketCapacity(session, segment, payload, payloadLength, expectedTypes, expectedCount, MAX_CALL_FRAMES * TEST_FRAME_SIZE, MAX_CALL_FRAMES);
}

/* Successful DTX/ERASURE call: exactly one frame of the expected type.        */
static int expectEvent(Session *session, const char *segment, bcg729PacketEvent_t event, bcg729FrameType_t expectedType)
{
	int16_t pcm[TEST_FRAME_SIZE];
	bcg729FrameType_t types[1];
	size_t frameCount = 0;
	int firstDifference;
	int rc = bcg729DecodePacket(session->packet, event, NULL, 0, pcm, TEST_FRAME_SIZE, types, 1, &frameCount);
	if (rc != 1 || frameCount != 1) {
		printf("  %s: %s event failed (rc=%d count=%zu)\n", session->name, eventName(event), rc, frameCount);
		traceCall(segment, event, NULL, 0, 0, NULL, 0);
		return 1;
	}
	referenceDecodeFrame(session, expectedType, NULL);
	firstDifference = compareCallOutput(session, pcm, types, 1, session->frames - 1);
	traceCall(segment, event, NULL, 0, 1, types, frameCount);
	if (firstDifference >= 0) {
		return 1;
	}
	return 0;
}

static int64_t segmentEnergy(const Session *session)
{
	int64_t energy = 0;
	size_t i;
	for (i = 0; i < session->frames * TEST_FRAME_SIZE; i++) {
		energy += (int64_t)session->pcm[i] * (int64_t)session->pcm[i];
	}
	return energy;
}

static void reportSegment(const char *title, const Session *session, int failures)
{
	printf("[%s/%s] frames=%zu samples=%zu energy=%lld first_diff=%d %s\n",
		title, session->name, session->frames, session->frames * TEST_FRAME_SIZE,
		(long long)segmentEnergy(session),
		session->firstDifference,
		failures ? "FAIL" : "PASS");
}

/* Build a payload of speech frames optionally followed by a 2 bytes SID.      */
static void buildPayload(Session *session, uint8_t *payload, size_t speechFrameCount, const uint8_t *sid, size_t *payloadLength)
{
	size_t i;
	size_t length = 0;
	for (i = 0; i < speechFrameCount; i++) {
		encodeSpeechFrame(session, payload + length);
		length += 10;
	}
	if (sid != NULL) {
		payload[length++] = sid[0];
		payload[length++] = sid[1];
	}
	*payloadLength = length;
}

/* Segment 1: erasure while in active speech gives PLC_SPEECH.                 */
static int segmentActiveErasure(const char *segment)
{
	Session *session = createSession("active-erasure");
	int failures = 0;
	uint8_t payload[128];
	size_t payloadLength;
	size_t i;
	static const bcg729FrameType_t fourSpeech[4] = {BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH};
	static const bcg729FrameType_t twoSpeech[2] = {BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH};

	buildPayload(session, payload, 4, NULL, &payloadLength);
	failures += expectPacket(session, segment, payload, payloadLength, fourSpeech, 4);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_SPEECH);
	buildPayload(session, payload, 2, NULL, &payloadLength);
	failures += expectPacket(session, segment, payload, payloadLength, twoSpeech, 2);
	for (i = 0; i < session->frames && i < MAX_SEGMENT_FRAMES; i++) {
		if (session->types[i] != ((i == 4) ? BCG729_FRAME_PLC_SPEECH : BCG729_FRAME_SPEECH)) {
			printf("  active-erasure: unexpected type at frame %zu\n", i);
			failures++;
		}
	}
	reportSegment(segment, session, failures);
	{
		int result = failures;
		destroySession(session);
		return result;
	}
}

/* Segment 2: SID switches to comfort noise, then DTX and erasures stay there. */
static int segmentSidDtxErasure(const char *segment)
{
	Session *session = createSession("sid-dtx-erasure");
	int failures = 0;
	uint8_t payload[128];
	size_t payloadLength;
	size_t i;
	static const bcg729FrameType_t expected[9] = {
		BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SID,
		BCG729_FRAME_DTX, BCG729_FRAME_DTX, BCG729_FRAME_DTX,
		BCG729_FRAME_PLC_COMFORT, BCG729_FRAME_PLC_COMFORT};

	buildPayload(session, payload, 3, sidPatternA, &payloadLength);
	failures += expectPacket(session, segment, payload, payloadLength, expected, 4);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_COMFORT);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_COMFORT);
	for (i = 0; i < session->frames; i++) {
		if (session->types[i] != expected[i]) {
			printf("  sid-dtx-erasure: unexpected type at frame %zu\n", i);
			failures++;
		}
	}
	{
		int result = failures;
		reportSegment(segment, session, failures);
		destroySession(session);
		return result;
	}
}

/* Segment 3: speech recovers active state, DTX is then illegal.               */
static int segmentRecovery(const char *segment)
{
	Session *session = createSession("recovery");
	int failures = 0;
	uint8_t payload[128];
	size_t payloadLength;
	size_t frameCount = 0;
	int16_t pcm[TEST_FRAME_SIZE];
	bcg729FrameType_t types[1];
	int rc;
	static const bcg729FrameType_t sidOnly[1] = {BCG729_FRAME_SID};
	static const bcg729FrameType_t oneSpeech[1] = {BCG729_FRAME_SPEECH};

	buildPayload(session, payload, 0, sidPatternB, &payloadLength);
	failures += expectPacket(session, segment, payload, payloadLength, sidOnly, 1);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	buildPayload(session, payload, 1, NULL, &payloadLength);
	failures += expectPacket(session, segment, payload, payloadLength, oneSpeech, 1);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_SPEECH);
	/* DTX must now be rejected and must not alter any output or history */
	rc = bcg729DecodePacket(session->packet, BCG729_PACKET_EVENT_DTX, NULL, 0, pcm, TEST_FRAME_SIZE, types, 1, &frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_DTX, NULL, 0, 0, NULL, 0);
	if (rc != 0) {
		printf("  recovery: DTX in active state was accepted\n");
		failures++;
	}
	{
		int result = failures;
		reportSegment(segment, session, failures);
		destroySession(session);
		return result;
	}
}

/* Segment 4: a payload made of a single SID frame (10*0 + 2 bytes).           */
static int segmentPureSid(const char *segment)
{
	Session *session = createSession("pure-sid");
	int failures = 0;
	uint8_t payload[2];
	size_t payloadLength;
	static const bcg729FrameType_t sidOnly[1] = {BCG729_FRAME_SID};

	buildPayload(session, payload, 0, sidPatternA, &payloadLength);
	if (payloadLength != 2) {
		printf("  pure-sid: payload length %zu, expected 2\n", payloadLength);
		failures++;
	}
	failures += expectPacket(session, segment, payload, payloadLength, sidOnly, 1);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	{
		int result = failures;
		reportSegment(segment, session, failures);
		destroySession(session);
		return result;
	}
}

/* Segment 5: the specification example: 3 speech + SID, erasure, speech,      */
/* erasure, then a rejected DTX.                                              */
static int segmentSpecExample(const char *segment)
{
	Session *session = createSession("spec-example");
	int failures = 0;
	uint8_t payload[128];
	size_t payloadLength;
	int rc;
	size_t frameCount = 0;
	int16_t pcm[TEST_FRAME_SIZE];
	bcg729FrameType_t types[1];
	static const bcg729FrameType_t expected[4] = {BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SID};
	static const bcg729FrameType_t oneSpeech[1] = {BCG729_FRAME_SPEECH};

	buildPayload(session, payload, 3, sidPatternA, &payloadLength);
	if (payloadLength != 32) {
		printf("  spec-example: payload length %zu, expected 32\n", payloadLength);
		failures++;
	}
	failures += expectPacket(session, segment, payload, payloadLength, expected, 4);
	if (session->frames != 4 || session->frames * TEST_FRAME_SIZE != 320) {
		printf("  spec-example: expected 4 frames/320 samples, got %zu frames\n", session->frames);
		failures++;
	}
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_COMFORT);
	buildPayload(session, payload, 1, NULL, &payloadLength);
	failures += expectPacket(session, segment, payload, payloadLength, oneSpeech, 1);
	failures += expectEvent(session, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_SPEECH);
	rc = bcg729DecodePacket(session->packet, BCG729_PACKET_EVENT_DTX, NULL, 0, pcm, TEST_FRAME_SIZE, types, 1, &frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_DTX, NULL, 0, 0, NULL, 0);
	if (rc != 0) {
		printf("  spec-example: DTX in active state was accepted\n");
		failures++;
	}
	{
		int result = failures;
		reportSegment(segment, session, failures);
		destroySession(session);
		return result;
	}
}

/* Segment 6: two instances driven in an interleaved way, each compared to its */
/* own reference; comfort noise generation on one instance while the other one */
/* stays active would diverge if random seeds or history were shared.          */
static int segmentDualInstance(const char *segment)
{
	Session *first = createSession("dual-1");
	Session *second = createSession("dual-2");
	int failures = 0;
	uint8_t payload[128];
	size_t payloadLength;
	static const bcg729FrameType_t oneSpeech[1] = {BCG729_FRAME_SPEECH};
	static const bcg729FrameType_t twoSpeech[2] = {BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH};
	static const bcg729FrameType_t sidOnly[1] = {BCG729_FRAME_SID};

	buildPayload(first, payload, 2, NULL, &payloadLength);
	failures += expectPacket(first, segment, payload, payloadLength, twoSpeech, 2);
	buildPayload(second, payload, 1, NULL, &payloadLength);
	failures += expectPacket(second, segment, payload, payloadLength, oneSpeech, 1);
	/* first instance enters comfort noise with a pure SID while second is active */
	buildPayload(first, payload, 0, sidPatternB, &payloadLength);
	failures += expectPacket(first, segment, payload, payloadLength, sidOnly, 1);
	buildPayload(second, payload, 1, NULL, &payloadLength);
	failures += expectPacket(second, segment, payload, payloadLength, oneSpeech, 1);
	failures += expectEvent(first, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	failures += expectEvent(second, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_SPEECH);
	failures += expectEvent(first, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_COMFORT);
	buildPayload(second, payload, 1, NULL, &payloadLength);
	failures += expectPacket(second, segment, payload, payloadLength, oneSpeech, 1);
	failures += expectEvent(first, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	failures += expectEvent(second, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_SPEECH);
	{
		int result = failures;
		reportSegment(segment, first, failures);
		printf("[%s/dual-2] frames=%zu samples=%zu energy=%lld first_diff=%d %s\n",
			segment, second->frames, second->frames * TEST_FRAME_SIZE,
			(long long)segmentEnergy(second), second->firstDifference,
			failures ? "FAIL" : "PASS");
		destroySession(first);
		destroySession(second);
		return result;
	}
}

/* Check that a rejected call leaves every output buffer untouched.            */
static int checkUntouched(const char *tag, const int16_t *pcm, size_t pcmCapacity, const bcg729FrameType_t *info, size_t infoCapacity, size_t count)
{
	size_t i;
	int failures = 0;
	for (i = 0; i < pcmCapacity; i++) {
		if (pcm[i] != (int16_t)0xaaaa) {
			printf("  %s: pcm sample %zu altered by rejected call\n", tag, i);
			failures++;
			break;
		}
	}
	for (i = 0; i < infoCapacity; i++) {
		if ((uint8_t)info[i] != 0x77) {
			printf("  %s: frame_info entry %zu altered by rejected call\n", tag, i);
			failures++;
			break;
		}
	}
	if (count != (size_t)0xdeadbeef) {
		printf("  %s: frame_count altered by rejected call\n", tag);
		failures++;
	}
	return failures;
}

/* Segment 7: every rejection path, untouched outputs, and retry against a      */
/* control instance that never saw the failed calls.                           */
static int segmentRejections(const char *segment)
{
	Session *tested = createSession("reject-tested");
	Session *control = createSession("reject-control");
	int failures = 0;
	uint8_t payload[128];
	size_t payloadLength;
	size_t i;
	int16_t pcm[MAX_CALL_FRAMES * TEST_FRAME_SIZE];
	bcg729FrameType_t info[MAX_CALL_FRAMES];
	size_t frameCount;
	int rc;
	static const bcg729FrameType_t prefixTypes[4] = {BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH, BCG729_FRAME_SID};
	static const bcg729FrameType_t twoSpeech[2] = {BCG729_FRAME_SPEECH, BCG729_FRAME_SPEECH};

	/* identical valid prefix on both instances */
	buildPayload(tested, payload, 3, sidPatternA, &payloadLength);
	failures += expectPacket(tested, segment, payload, payloadLength, prefixTypes, 4);
	buildPayload(control, payload, 3, sidPatternA, &payloadLength);
	failures += expectPacket(control, segment, payload, payloadLength, prefixTypes, 4);
	failures += expectEvent(tested, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);
	failures += expectEvent(control, segment, BCG729_PACKET_EVENT_DTX, BCG729_FRAME_DTX);

	memset(pcm, 0xaa, sizeof(pcm));
	memset(info, 0x77, sizeof(info));
	frameCount = (size_t)0xdeadbeef;

	/* illegal payload lengths: every remainder other than 0 or 2 is rejected before decoding */
	{
		size_t badLengths[] = {1, 3, 5, 7, 9, 11, 21, 33};
		for (i = 0; i < sizeof(badLengths) / sizeof(badLengths[0]); i++) {
			memset(payload, 0x3b, sizeof(payload));
			rc = bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_PAYLOAD, payload, badLengths[i], pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, &frameCount);
			traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, payload, badLengths[i], 0, NULL, 0);
			if (rc != 0) {
				printf("  rejections: %zu bytes payload accepted\n", badLengths[i]);
				failures++;
			}
			failures += checkUntouched("rejections", pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, frameCount);
		}
	}

	/* PAYLOAD needs a non empty buffer */
	rc = bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_PAYLOAD, NULL, 0, pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, &frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, NULL, 0, 0, NULL, 0);
	if (rc != 0) {
		printf("  rejections: empty PAYLOAD accepted\n");
		failures++;
	}
	failures += checkUntouched("rejections", pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, frameCount);

	rc = bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_PAYLOAD, NULL, 10, pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, &frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, NULL, 10, 0, NULL, 0);
	if (rc != 0) {
		printf("  rejections: NULL PAYLOAD with length accepted\n");
		failures++;
	}

	/* DTX/ERASURE require NULL payload and zero length */
	{
		uint8_t dummy[2] = {0x00, 0x00};
		bcg729PacketEvent_t events[2] = {BCG729_PACKET_EVENT_DTX, BCG729_PACKET_EVENT_ERASURE};
		for (i = 0; i < 2; i++) {
			rc = bcg729DecodePacket(tested->packet, events[i], dummy, 0, pcm, TEST_FRAME_SIZE, info, 1, &frameCount);
			traceCall(segment, events[i], dummy, 0, 0, NULL, 0);
			if (rc != 0) {
				printf("  rejections: %s with non NULL payload accepted\n", eventName(events[i]));
				failures++;
			}
			rc = bcg729DecodePacket(tested->packet, events[i], NULL, 2, pcm, TEST_FRAME_SIZE, info, 1, &frameCount);
			traceCall(segment, events[i], NULL, 2, 0, NULL, 0);
			if (rc != 0) {
				printf("  rejections: %s with non zero length accepted\n", eventName(events[i]));
				failures++;
			}
		}
	}

	/* DTX on a fresh active instance is illegal */
	{
		Session *fresh = createSession("reject-fresh");
		rc = bcg729DecodePacket(fresh->packet, BCG729_PACKET_EVENT_DTX, NULL, 0, pcm, TEST_FRAME_SIZE, info, 1, &frameCount);
		traceCall(segment, BCG729_PACKET_EVENT_DTX, NULL, 0, 0, NULL, 0);
		if (rc != 0) {
			printf("  rejections: DTX on active instance accepted\n");
			failures++;
		}
		destroySession(fresh);
	}

	/* unknown event */
	rc = bcg729DecodePacket(tested->packet, (bcg729PacketEvent_t)99, NULL, 0, pcm, TEST_FRAME_SIZE, info, 1, &frameCount);
	traceCall(segment, (bcg729PacketEvent_t)99, NULL, 0, 0, NULL, 0);
	if (rc != 0) {
		printf("  rejections: unknown event accepted\n");
		failures++;
	}

	/* NULL pointers */
	if (bcg729DecodePacket(NULL, BCG729_PACKET_EVENT_ERASURE, NULL, 0, pcm, TEST_FRAME_SIZE, info, 1, &frameCount) != 0) {
		printf("  rejections: NULL context accepted\n");
		failures++;
	}
	if (bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_ERASURE, NULL, 0, NULL, TEST_FRAME_SIZE, info, 1, &frameCount) != 0) {
		printf("  rejections: NULL pcm accepted\n");
		failures++;
	}
	if (bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_ERASURE, NULL, 0, pcm, TEST_FRAME_SIZE, NULL, 1, &frameCount) != 0) {
		printf("  rejections: NULL frame_info accepted\n");
		failures++;
	}
	if (bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_ERASURE, NULL, 0, pcm, TEST_FRAME_SIZE, info, 1, NULL) != 0) {
		printf("  rejections: NULL frame_count accepted\n");
		failures++;
	}

	/* capacity shortfalls on a 4 frames (3 speech + SID) packet: no partial decode */
	buildPayload(tested, payload, 3, sidPatternA, &payloadLength);
	/* the same legal payload with one extra trailing byte must be rejected wholesale */
	rc = bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength + 1, pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, &frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength + 1, 0, NULL, 0);
	if (rc != 0) {
		printf("  rejections: legal payload plus one trailing byte accepted\n");
		failures++;
	}
	failures += checkUntouched("rejections", pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, frameCount);
	rc = bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, pcm, 3 * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, &frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, 0, NULL, 0);
	if (rc != 0) {
		printf("  rejections: short pcm capacity accepted\n");
		failures++;
	}
	failures += checkUntouched("rejections", pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, frameCount);
	rc = bcg729DecodePacket(tested->packet, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, 3, &frameCount);
	traceCall(segment, BCG729_PACKET_EVENT_PAYLOAD, payload, payloadLength, 0, NULL, 0);
	if (rc != 0) {
		printf("  rejections: short info capacity accepted\n");
		failures++;
	}
	failures += checkUntouched("rejections", pcm, MAX_CALL_FRAMES * TEST_FRAME_SIZE, info, MAX_CALL_FRAMES, frameCount);

	/* exact capacities are accepted, including the same packet that just failed */
	failures += expectPacketCapacity(tested, segment, payload, payloadLength, prefixTypes, 4, 4 * TEST_FRAME_SIZE, 4);
	buildPayload(control, payload, 3, sidPatternA, &payloadLength);
	failures += expectPacket(control, segment, payload, payloadLength, prefixTypes, 4);
	/* expectPacketCapacity already checked the retried packet against the tested reference */

	/* history after failures: continue both instances with the same valid stream */
	failures += expectEvent(tested, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_COMFORT);
	failures += expectEvent(control, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_COMFORT);
	buildPayload(tested, payload, 2, NULL, &payloadLength);
	failures += expectPacket(tested, segment, payload, payloadLength, twoSpeech, 2);
	buildPayload(control, payload, 2, NULL, &payloadLength);
	failures += expectPacket(control, segment, payload, payloadLength, twoSpeech, 2);
	failures += expectEvent(tested, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_SPEECH);
	failures += expectEvent(control, segment, BCG729_PACKET_EVENT_ERASURE, BCG729_FRAME_PLC_SPEECH);
	if (tested->frames != control->frames) {
		printf("  rejections: frame count divergence %zu vs %zu\n", tested->frames, control->frames);
		failures++;
	} else {
		for (i = 0; i < tested->frames * TEST_FRAME_SIZE; i++) {
			if (tested->pcm[i] != control->pcm[i]) {
				printf("  rejections: history divergence from control at sample %zu\n", i);
				failures++;
				break;
			}
		}
	}

	{
		int result = failures;
		reportSegment(segment, tested, failures);
		destroySession(tested);
		destroySession(control);
		return result;
	}
}

int main(int argc, char *argv[])
{
	const char *tracePath;
	int failures = 0;

	if (argc < 2) {
		printf("usage: %s <trace file path>\n", argv[0]);
		return 2;
	}
	tracePath = argv[1];
	traceFile = fopen(tracePath, "w");
	if (traceFile == NULL) {
		printf("FATAL: cannot open trace file %s\n", tracePath);
		return 2;
	}

	printf("bcg729 packet decoder conformance test, trace: %s\n", tracePath);
	failures += segmentActiveErasure("seg1-active-erasure");
	failures += segmentSidDtxErasure("seg2-sid-dtx-erasure");
	failures += segmentRecovery("seg3-recovery");
	failures += segmentPureSid("seg4-pure-sid");
	failures += segmentSpecExample("seg5-spec-example");
	failures += segmentDualInstance("seg6-dual-instance");
	failures += segmentRejections("seg7-rejections");

	fclose(traceFile);
	if (failures != 0) {
		printf("PACKET DECODER TEST FAILED: %d failure(s)\n", failures);
		return 1;
	}
	printf("PACKET DECODER TEST PASSED\n");
	return 0;
}

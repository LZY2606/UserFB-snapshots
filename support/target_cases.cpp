//
// target_cases.cpp -- Regression cases for SMPTE division handling and
// for the PPQN/SMPTE time-base state of smf::MidiFile.
//
// This file contains no main(): it is #included by target_main.cpp, which
// drives the FB_TEST/FB_CHECK harness in case_harness.hpp.  Every input
// and write-back header is built and inspected as raw Standard MIDI File
// bytes, never only through the library's own reader/writer.
//

// Note: case_harness.hpp (providing FB_TEST/FB_CHECK) is included first by
// target_main.cpp, which #includes this translation unit.  The shipped
// harness header has no include guard, so it is deliberately not included
// again here.

#include "MidiFile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace smf;

namespace {

// Append a MIDI variable-length quantity.
void appendVlv(std::string& out, unsigned long value) {
	unsigned char buffer[8];
	int count = 0;
	buffer[count++] = (unsigned char)(value & 0x7f);
	while (value >>= 7) {
		buffer[count++] = (unsigned char)((value & 0x7f) | 0x80);
	}
	while (count > 0) {
		out += (char)buffer[--count];
	}
}

std::string u32be(unsigned long value) {
	std::string out;
	out += (char)((value >> 24) & 0xff);
	out += (char)((value >> 16) & 0xff);
	out += (char)((value >>  8) & 0xff);
	out += (char)( value        & 0xff);
	return out;
}

std::string u16be(unsigned value) {
	std::string out;
	out += (char)((value >> 8) & 0xff);
	out += (char)( value       & 0xff);
	return out;
}

std::string headerChunk(int format, int trackCount, unsigned division) {
	std::string out = "MThd";
	out += u32be(6);
	out += u16be((unsigned)format);
	out += u16be((unsigned)trackCount);
	out += u16be(division);
	return out;
}

std::string trackChunk(const std::string& trackData) {
	std::string out = "MTrk";
	out += u32be((unsigned long)trackData.size());
	out += trackData;
	return out;
}

// A tempo meta message (FF 51 03 ...) carrying microseconds per quarter.
std::string tempoEvent(unsigned long microsecondsPerQuarter) {
	std::string out;
	out += (char)0xff;
	out += (char)0x51;
	out += (char)0x03;
	out += (char)((microsecondsPerQuarter >> 16) & 0xff);
	out += (char)((microsecondsPerQuarter >>  8) & 0xff);
	out += (char)( microsecondsPerQuarter        & 0xff);
	return out;
}

std::string noteOn(int channel, int key, int velocity) {
	std::string out;
	out += (char)(0x90 | (channel & 0x0f));
	out += (char)(key & 0x7f);
	out += (char)(velocity & 0x7f);
	return out;
}

std::string noteOff(int channel, int key) {
	std::string out;
	out += (char)(0x80 | (channel & 0x0f));
	out += (char)(key & 0x7f);
	out += (char)0x00;
	return out;
}

std::string controllerEvent(int channel, int controller, int value) {
	std::string out;
	out += (char)(0xb0 | (channel & 0x0f));
	out += (char)(controller & 0x7f);
	out += (char)(value & 0x7f);
	return out;
}

std::string endOfTrack() {
	return std::string("\x00\xff\x2f\x00", 4);
}

// Prefix raw message bytes with their delta-time VLV.
std::string atDelta(unsigned long delta, const std::string& message) {
	std::string out;
	appendVlv(out, delta);
	out += message;
	return out;
}

std::string makeFile(int format, unsigned division,
		const std::vector<std::string>& tracks) {
	std::string out = headerChunk(format, (int)tracks.size(), division);
	for (std::size_t i=0; i<tracks.size(); i++) {
		out += trackChunk(tracks[i]);
	}
	return out;
}

bool readBytes(MidiFile& midiFile, const std::string& bytes) {
	std::istringstream stream(bytes);
	return midiFile.read(stream);
}

std::string writeBytes(MidiFile& midiFile) {
	std::ostringstream stream;
	FB_CHECK(midiFile.write(stream));
	return stream.str();
}

unsigned rawHeaderDivision(const std::string& bytes) {
	FB_CHECK(bytes.size() >= 14);
	return ((unsigned char)bytes[12] << 8) | (unsigned char)bytes[13];
}

unsigned rawHeaderFormat(const std::string& bytes) {
	FB_CHECK(bytes.size() >= 14);
	return ((unsigned char)bytes[8] << 8) | (unsigned char)bytes[9];
}

unsigned rawHeaderTrackCount(const std::string& bytes) {
	FB_CHECK(bytes.size() >= 14);
	return ((unsigned char)bytes[10] << 8) | (unsigned char)bytes[11];
}

unsigned smpteWord(int negativeFpsCode, int ticksPerFrame) {
	unsigned fpsByte = (unsigned)negativeFpsCode & 0x00ff;
	return (fpsByte << 8) | (unsigned)ticksPerFrame;
}

// A one-track format-1 file: tempo at tick 0, note on at tick 0 and note
// off at tick lastTick.
std::string simpleNoteFile(unsigned division, unsigned long lastTick,
		unsigned long tempoMicroseconds = 250000) {
	std::string track;
	track += atDelta(0, tempoEvent(tempoMicroseconds));
	track += atDelta(0, noteOn(0, 60, 90));
	track += atDelta(lastTick, noteOff(0, 60));
	track += endOfTrack();
	return makeFile(1, division, std::vector<std::string>(1, track));
}

bool closeEnough(double actual, double expected) {
	return std::fabs(actual - expected) <= 1e-10 * (1.0 + std::fabs(expected));
}

std::vector<std::string> twoTracks(const std::string& first,
		const std::string& second) {
	std::vector<std::string> tracks;
	tracks.push_back(first);
	tracks.push_back(second);
	return tracks;
}

} // end anonymous namespace


///////////////////////////////////////////////////////////////////////////
//
// division group: header word parsing, preservation, validation
//

FB_TEST("division",rawWordPreservedForAllFrameRates) {
	const int codes[4] = {-24, -25, -29, -30};
	const int ticksPerFrame = 40;
	for (int i=0; i<4; i++) {
		unsigned division = smpteWord(codes[i], ticksPerFrame);
		std::string input = simpleNoteFile(division, 1000);

		MidiFile midiFile;
		FB_CHECK(readBytes(midiFile, input));
		FB_CHECK(midiFile.status());
		FB_CHECK(midiFile.getRawDivision() == division);
		FB_CHECK((midiFile.getRawDivision() & 0x8000u) != 0);
		FB_CHECK(midiFile.getTPQ() == -codes[i] * ticksPerFrame);

		std::string output = writeBytes(midiFile);
		// The written MThd chunk tag, header length and division word must
		// match the hand-built input.  (A single track is re-emitted as
		// format 0, so the format bytes themselves are not compared here.)
		FB_CHECK(rawHeaderDivision(output) == division);
		FB_CHECK(rawHeaderTrackCount(output) == 1);
		FB_CHECK(output.compare(0, 8, input, 0, 8) == 0);
		FB_CHECK(output[12] == input[12]);
		FB_CHECK(output[13] == input[13]);
	}

	// PPQN division keeps its positive 15-bit word as well.
	std::string ppqnInput = simpleNoteFile(500, 1000);
	MidiFile ppqnFile;
	FB_CHECK(readBytes(ppqnFile, ppqnInput));
	FB_CHECK(ppqnFile.getRawDivision() == 500u);
	FB_CHECK(rawHeaderDivision(writeBytes(ppqnFile)) == 500u);
}

FB_TEST("division",smpteSetterValidationAndState) {
	MidiFile midiFile;
	const int codes[4] = {-24, -25, -29, -30};
	for (int i=0; i<4; i++) {
		midiFile.setSmpteDivision(codes[i], 40);
		FB_CHECK(midiFile.getRawDivision() == smpteWord(codes[i], 40));
		FB_CHECK(midiFile.getTPQ() == -codes[i] * 40);
	}

	// Illegal frame-rate codes throw and leave the current time base alone.
	unsigned before = midiFile.getRawDivision();
	int tpqBefore = midiFile.getTPQ();
	const int badCodes[4] = {0, 25, -23, -31};
	for (int i=0; i<4; i++) {
		bool threw = false;
		try {
			midiFile.setSmpteDivision(badCodes[i], 40);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		FB_CHECK(threw);
		FB_CHECK(midiFile.getRawDivision() == before);
		FB_CHECK(midiFile.getTPQ() == tpqBefore);
	}

	// Illegal ticks-per-frame values throw and preserve the time base.
	const int badTicks[3] = {0, -1, 256};
	for (int i=0; i<3; i++) {
		bool threw = false;
		try {
			midiFile.setSmpteDivision(-25, badTicks[i]);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		FB_CHECK(threw);
		FB_CHECK(midiFile.getRawDivision() == before);
	}
}

FB_TEST("division",corruptDivisionsRejectedByRead) {
	// Zero PPQN division.
	std::string zeroDivision = simpleNoteFile(0x0000, 10);
	MidiFile zeroFile;
	FB_CHECK(!readBytes(zeroFile, zeroDivision));
	FB_CHECK(!zeroFile.status());

	// Unsupported frame-rate code -31 (high byte 0xE1).
	std::string badRate = simpleNoteFile(0xe100 | 40, 10);
	MidiFile rateFile;
	FB_CHECK(!readBytes(rateFile, badRate));
	FB_CHECK(!rateFile.status());

	// Ticks per frame of zero.
	std::string zeroTpf = simpleNoteFile(0xe700, 10);
	MidiFile tpfFile;
	FB_CHECK(!readBytes(tpfFile, zeroTpf));
	FB_CHECK(!tpfFile.status());

	// High byte 0x80 is frame-rate code -128, which is not allowed.
	std::string weirdCode = simpleNoteFile(0x8028, 10);
	MidiFile weirdFile;
	FB_CHECK(!readBytes(weirdFile, weirdCode));
	FB_CHECK(!weirdFile.status());
}

FB_TEST("division",millisecondTicksIsRealSmpteTimeBase) {
	MidiFile midiFile;
	midiFile.setMillisecondTicks();

	FB_CHECK(midiFile.getRawDivision() == 0xe728u);
	FB_CHECK(midiFile.getTPQ() == 1000);
	midiFile.addNoteOn(0, 0, 0, 60, 90);
	midiFile.addNoteOff(0, 1000, 0, 60);
	midiFile.doTimeAnalysis();

	// Real millisecond semantics, not just a header label.
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 1.0));
	FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(), 1.0));
	FB_CHECK(rawHeaderDivision(writeBytes(midiFile)) == 0xe728u);
}


///////////////////////////////////////////////////////////////////////////
//
// clock group: physical-time conversions in SMPTE and PPQN modes
//

FB_TEST("clock",smpte25FortyOneThousandTicksOneSecond) {
	// Required example: -25/40, 1000 ticks must be exactly one second,
	// exercised through the public read/analysis/write path with a
	// hand-built SMF byte stream.
	std::string input = simpleNoteFile(0xe728, 1000, 250000);
	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, input));
	midiFile.doTimeAnalysis();

	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 1.0));
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(0, 2), 1.0));
	FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(), 1.0));
	FB_CHECK(closeEnough(midiFile.getAbsoluteTickTime(1.0), 1000.0));
	FB_CHECK(rawHeaderDivision(writeBytes(midiFile)) == 0xe728u);
}

FB_TEST("clock",smpte29DropFrameRationalTiming) {
	// Required example: -29/100 means 30000/1001 frames per second and
	// 30000 ticks cover 10.01 seconds.  The rational ratio must survive
	// to the final double.
	std::string input = simpleNoteFile(smpteWord(-29, 100), 30000);
	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, input));
	midiFile.doTimeAnalysis();

	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(30000), 10.01));
	FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(), 10.01));
	FB_CHECK(closeEnough(midiFile.getAbsoluteTickTime(10.01), 30000.0));
	FB_CHECK(rawHeaderDivision(writeBytes(midiFile)) == smpteWord(-29, 100));

	// A long run of events is converted per event from its absolute tick,
	// never by accumulating rounded per-frame durations.
	std::string manyTrack;
	for (int i=0; i<100; i++) {
		manyTrack += atDelta(100, controllerEvent(0, 7, 10));
	}
	manyTrack += endOfTrack();
	std::string manyInput = makeFile(1, smpteWord(-29, 100),
			std::vector<std::string>(1, manyTrack));
	MidiFile manyFile;
	FB_CHECK(readBytes(manyFile, manyInput));
	manyFile.doTimeAnalysis();
	const double secondsPerTick = 1001.0 / (30000.0 * 100.0);
	for (int i=0; i<100; i++) {
		int absoluteTick = (i + 1) * 100;
		FB_CHECK(closeEnough(manyFile[0][i].seconds,
				absoluteTick * secondsPerTick));
		FB_CHECK(closeEnough(manyFile.getTimeInSeconds(absoluteTick),
				absoluteTick * secondsPerTick));
	}
	FB_CHECK(closeEnough(manyFile.getFileDurationInSeconds(),
			10000.0 * secondsPerTick));
}

FB_TEST("clock",allFourFrameRateConversions) {
	struct RateCase { int code; int tpf; int tick; double seconds; };
	const RateCase cases[4] = {
		{-24, 40, 1920, 2.0},
		{-25, 40, 1000, 1.0},
		{-29, 40, 1200, 1.001},
		{-30, 40, 2400, 2.0},
	};
	for (int i=0; i<4; i++) {
		unsigned division = smpteWord(cases[i].code, cases[i].tpf);
		std::string input = simpleNoteFile(division, cases[i].tick);
		MidiFile midiFile;
		FB_CHECK(readBytes(midiFile, input));
		midiFile.doTimeAnalysis();

		FB_CHECK(closeEnough(midiFile.getTimeInSeconds(cases[i].tick),
				cases[i].seconds));
		FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(),
				cases[i].seconds));
		FB_CHECK(closeEnough(midiFile.getAbsoluteTickTime(cases[i].seconds),
				(double)cases[i].tick));
		// SMPTE time extends past the last event.
		FB_CHECK(closeEnough(midiFile.getTimeInSeconds(cases[i].tick * 3),
				cases[i].seconds * 3.0));
		FB_CHECK(midiFile.getTimeInSeconds(cases[i].tick * 3) > 0.0);
	}
}

FB_TEST("clock",smpteTempoEventsStayButDoNotAffectTime) {
	// An extreme tempo (1 microsecond per quarter) must not move SMPTE
	// physical time, while the tempo event remains in the data.
	std::string track;
	track += atDelta(0, tempoEvent(1));
	track += atDelta(0, noteOn(0, 60, 90));
	track += atDelta(1000, noteOff(0, 60));
	track += endOfTrack();
	std::string input = makeFile(1, 0xe728,
			std::vector<std::string>(1, track));

	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, input));
	midiFile.doTimeAnalysis();
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 1.0));
	FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(), 1.0));

	int tempoCount = 0;
	for (int i=0; i<midiFile[0].getEventCount(); i++) {
		if (midiFile[0][i].isTempo()) {
			tempoCount++;
		}
	}
	FB_CHECK(tempoCount == 1);

	std::string output = writeBytes(midiFile);
	MidiFile reread;
	FB_CHECK(readBytes(reread, output));
	reread.doTimeAnalysis();
	FB_CHECK(closeEnough(reread.getTimeInSeconds(1000), 1.0));
	int rereadTempoCount = 0;
	for (int i=0; i<reread[0].getEventCount(); i++) {
		if (reread[0][i].isTempo()) {
			rereadTempoCount++;
		}
	}
	FB_CHECK(rereadTempoCount == 1);
}

FB_TEST("clock",ppqnTempoMapAndDefaultTempo) {
	// PPQN 500: 250000 us/quarter until tick 500, then 500000 us/quarter.
	std::string track;
	track += atDelta(0, tempoEvent(250000));
	track += atDelta(0, noteOn(0, 60, 90));
	track += atDelta(500, tempoEvent(500000));
	track += atDelta(500, noteOff(0, 60));
	track += endOfTrack();
	std::string input = makeFile(1, 500,
			std::vector<std::string>(1, track));

	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, input));
	midiFile.doTimeAnalysis();
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(500), 0.25));
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 0.75));
	FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(), 0.75));

	// No tempo message: default 120 BPM still applies.
	std::string plainTrack;
	plainTrack += atDelta(0, noteOn(0, 60, 90));
	plainTrack += atDelta(480, noteOff(0, 60));
	plainTrack += endOfTrack();
	std::string plainInput = makeFile(1, 480,
			std::vector<std::string>(1, plainTrack));
	MidiFile plain;
	FB_CHECK(readBytes(plain, plainInput));
	plain.doTimeAnalysis();
	// One quarter note at 120 BPM is 0.5 seconds.
	FB_CHECK(closeEnough(plain.getTimeInSeconds(480), 0.5));
}

FB_TEST("clock",trackIndexQueryUsesAbsoluteTickInDeltaMode) {
	// Two tracks; track 1 has deltas 100 then 200, so the third event's
	// absolute tick is 300.
	std::string track0;
	track0 += atDelta(0, tempoEvent(250000));
	track0 += endOfTrack();
	std::string track1;
	track1 += atDelta(0, noteOn(0, 60, 90));
	track1 += atDelta(100, noteOff(0, 60));
	track1 += atDelta(200, noteOn(0, 64, 90));
	track1 += endOfTrack();
	std::string input = makeFile(1, 500, twoTracks(track0, track1));

	MidiFile ppqn;
	FB_CHECK(readBytes(ppqn, input));
	ppqn.makeDeltaTicks();
	FB_CHECK(ppqn.isDeltaTicks());
	ppqn.doTimeAnalysis();
	// 0.25 s per quarter / 500 ticks => 0.0005 s per tick.
	FB_CHECK(closeEnough(ppqn.getTimeInSeconds(1, 0), 0.0));
	FB_CHECK(closeEnough(ppqn.getTimeInSeconds(1, 1), 0.05));
	FB_CHECK(closeEnough(ppqn.getTimeInSeconds(1, 2), 0.15));
	FB_CHECK(ppqn.isDeltaTicks());

	// Same structure under SMPTE -25/40: absolute tick 300 => 0.3 s.
	std::string smpteInput = makeFile(1, 0xe728, twoTracks(track0, track1));
	MidiFile smpte;
	FB_CHECK(readBytes(smpte, smpteInput));
	smpte.makeDeltaTicks();
	smpte.doTimeAnalysis();
	FB_CHECK(closeEnough(smpte.getTimeInSeconds(1, 2), 0.3));
	FB_CHECK(closeEnough(smpte.getTimeInSeconds(300), 0.3));
}

FB_TEST("clock",invalidQueriesFailAndInverseTickIsFractional) {
	std::string input = simpleNoteFile(0xe728, 1000);
	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, input));
	midiFile.doTimeAnalysis();

	FB_CHECK(midiFile.getTimeInSeconds(-1) == -1.0);
	FB_CHECK(midiFile.getTimeInSeconds(-100) == -1.0);
	FB_CHECK(midiFile.getAbsoluteTickTime(-1.0) == -1.0);
	FB_CHECK(midiFile.getAbsoluteTickTime(
			std::numeric_limits<double>::quiet_NaN()) == -1.0);
	FB_CHECK(midiFile.getAbsoluteTickTime(
			std::numeric_limits<double>::infinity()) == -1.0);
	FB_CHECK(midiFile.getTimeInSeconds(9, 0) == -1.0);
	FB_CHECK(midiFile.getTimeInSeconds(0, 99) == -1.0);

	// Reverse lookup returns a floating-point tick and is not pre-rounded.
	// -24/40 gives 960 ticks per second: 0.001 s => 0.96 tick.
	std::string input24 = simpleNoteFile(smpteWord(-24, 40), 1000);
	MidiFile smpte24;
	FB_CHECK(readBytes(smpte24, input24));
	smpte24.doTimeAnalysis();
	double fractional = smpte24.getAbsoluteTickTime(0.001);
	FB_CHECK(closeEnough(fractional, 0.96));
	FB_CHECK(fractional != std::floor(fractional));
}


///////////////////////////////////////////////////////////////////////////
//
// lifecycle group: object state across copies, clears, re-reads and rebasing
//

FB_TEST("lifecycle",copyAndMoveCarryTimeBaseAndAnalysis) {
	std::string input = simpleNoteFile(smpteWord(-29, 100), 30000);
	MidiFile original;
	FB_CHECK(readBytes(original, input));
	original.doTimeAnalysis();
	FB_CHECK(closeEnough(original.getTimeInSeconds(30000), 10.01));

	// Copy constructor.
	MidiFile copyConstructed(original);
	FB_CHECK(copyConstructed.getRawDivision() == smpteWord(-29, 100));
	FB_CHECK(closeEnough(copyConstructed.getTimeInSeconds(30000), 10.01));

	// Assignment into an object that already held a different time base.
	MidiFile assigned;
	assigned.setTPQ(480);
	FB_CHECK(assigned.getRawDivision() == 480u);
	assigned = original;
	FB_CHECK(assigned.getRawDivision() == smpteWord(-29, 100));
	FB_CHECK(closeEnough(assigned.getTimeInSeconds(30000), 10.01));

	// Move constructor and move assignment.
	MidiFile moveConstructed(std::move(copyConstructed));
	FB_CHECK(moveConstructed.getRawDivision() == smpteWord(-29, 100));
	FB_CHECK(closeEnough(moveConstructed.getTimeInSeconds(30000), 10.01));
	MidiFile moveAssigned;
	moveAssigned.setTPQ(240);
	moveAssigned = std::move(assigned);
	FB_CHECK(moveAssigned.getRawDivision() == smpteWord(-29, 100));
	FB_CHECK(closeEnough(moveAssigned.getTimeInSeconds(30000), 10.01));

	// Copies are independent: rebasing the original must not leak into
	// the moved-to object.
	original.setTPQ(1000);
	FB_CHECK(original.getRawDivision() == 1000u);
	FB_CHECK(moveAssigned.getRawDivision() == smpteWord(-29, 100));
	FB_CHECK(closeEnough(moveAssigned.getTimeInSeconds(30000), 10.01));
}

FB_TEST("lifecycle",clearResetsTimeBaseAndRereadWorks) {
	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, simpleNoteFile(0xe728, 1000)));
	midiFile.doTimeAnalysis();
	FB_CHECK(midiFile.getRawDivision() == 0xe728u);

	midiFile.clear();
	FB_CHECK(midiFile.getRawDivision() == 120u);
	FB_CHECK(midiFile.getTPQ() == 120);
	FB_CHECK(midiFile.getTrackCount() == 1);

	FB_CHECK(readBytes(midiFile, simpleNoteFile(500, 1000)));
	FB_CHECK(midiFile.getRawDivision() == 500u);
	midiFile.doTimeAnalysis();
	FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(), 0.5));
}

FB_TEST("lifecycle",sameObjectAlternatesBetweenTimeBases) {
	MidiFile midiFile;

	// PPQN first.
	FB_CHECK(readBytes(midiFile, simpleNoteFile(500, 1000)));
	midiFile.doTimeAnalysis();
	FB_CHECK(midiFile.getRawDivision() == 500u);
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 0.5));
	FB_CHECK(rawHeaderDivision(writeBytes(midiFile)) == 500u);

	// SMPTE -25/40.
	FB_CHECK(readBytes(midiFile, simpleNoteFile(0xe728, 1000)));
	midiFile.doTimeAnalysis();
	FB_CHECK(midiFile.getRawDivision() == 0xe728u);
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 1.0));
	FB_CHECK(rawHeaderDivision(writeBytes(midiFile)) == 0xe728u);

	// A different SMPTE rate must fully replace the previous one.
	FB_CHECK(readBytes(midiFile, simpleNoteFile(smpteWord(-29, 100), 30000)));
	midiFile.doTimeAnalysis();
	FB_CHECK(midiFile.getRawDivision() == smpteWord(-29, 100));
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(30000), 10.01));
	FB_CHECK(rawHeaderDivision(writeBytes(midiFile)) == smpteWord(-29, 100));

	// Back to PPQN: no SMPTE state may survive.
	FB_CHECK(readBytes(midiFile, simpleNoteFile(500, 1000)));
	midiFile.doTimeAnalysis();
	FB_CHECK(midiFile.getRawDivision() == 500u);
	FB_CHECK((midiFile.getRawDivision() & 0x8000u) == 0);
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 0.5));
	FB_CHECK(rawHeaderDivision(writeBytes(midiFile)) == 500u);
}

FB_TEST("lifecycle",analysisLeavesEventsAndStatesUntouched) {
	std::string track0;
	track0 += atDelta(0, tempoEvent(250000));
	track0 += endOfTrack();
	std::string track1;
	track1 += atDelta(0, noteOn(0, 60, 90));
	track1 += atDelta(250, noteOff(0, 60));
	track1 += endOfTrack();
	std::string input = makeFile(1, 500, twoTracks(track0, track1));

	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, input));
	FB_CHECK(midiFile.getTrackCount() == 2);
	FB_CHECK(midiFile.getTrackState() == TRACK_STATE_SPLIT);
	FB_CHECK(midiFile.getTickState() == TIME_STATE_ABSOLUTE);

	struct Snapshot {
		std::vector<unsigned char> bytes;
		int tick;
		int seq;
		int track;
	};
	std::vector<Snapshot> before;
	for (int t=0; t<midiFile.getTrackCount(); t++) {
		for (int i=0; i<midiFile[t].getEventCount(); i++) {
			const MidiEvent& event = midiFile[t][i];
			Snapshot snapshot;
			snapshot.bytes.assign(event.begin(), event.end());
			snapshot.tick = event.tick;
			snapshot.seq = event.seq;
			snapshot.track = event.track;
			before.push_back(snapshot);
		}
	}

	midiFile.linkNotePairs();
	const MidiEvent* linkedNote = midiFile[1][0].getLinkedEvent();
	FB_CHECK(linkedNote != NULL);
	FB_CHECK(linkedNote == &midiFile[1][1]);

	midiFile.doTimeAnalysis();

	// Structural state and event content survive the analysis.
	FB_CHECK(midiFile.getTrackCount() == 2);
	FB_CHECK(midiFile.getTrackState() == TRACK_STATE_SPLIT);
	FB_CHECK(midiFile.getTickState() == TIME_STATE_ABSOLUTE);
	std::size_t snapshotIndex = 0;
	for (int t=0; t<midiFile.getTrackCount(); t++) {
		for (int i=0; i<midiFile[t].getEventCount(); i++) {
			const MidiEvent& event = midiFile[t][i];
			FB_CHECK(before[snapshotIndex].bytes.size() == event.size());
			FB_CHECK(std::equal(before[snapshotIndex].bytes.begin(),
					before[snapshotIndex].bytes.end(), event.begin()));
			FB_CHECK(before[snapshotIndex].tick == event.tick);
			FB_CHECK(before[snapshotIndex].seq == event.seq);
			FB_CHECK(before[snapshotIndex].track == event.track);
			snapshotIndex++;
		}
	}
	FB_CHECK(snapshotIndex == before.size());
	FB_CHECK(midiFile[1][0].getLinkedEvent() == linkedNote);
	// seconds are the one field allowed to change.
	FB_CHECK(midiFile[1][1].seconds > 0.0);

	// The same guarantees hold in delta/joined state.
	std::vector<int> deltaTicks;
	midiFile.makeDeltaTicks();
	midiFile.joinTracks();
	for (int i=0; i<midiFile[0].getEventCount(); i++) {
		deltaTicks.push_back(midiFile[0][i].tick);
	}
	midiFile.doTimeAnalysis();
	FB_CHECK(midiFile.getTrackState() == TRACK_STATE_JOINED);
	FB_CHECK(midiFile.getTickState() == TIME_STATE_DELTA);
	FB_CHECK(midiFile[0].getEventCount() == (int)deltaTicks.size());
	for (int i=0; i<midiFile[0].getEventCount(); i++) {
		FB_CHECK(midiFile[0][i].tick == deltaTicks[i]);
	}
	midiFile.splitTracks();
	midiFile.makeAbsoluteTicks();
	FB_CHECK(midiFile.getTrackCount() == 2);
	FB_CHECK(midiFile[1][1].tick == 250);
}

FB_TEST("lifecycle",rebasingAfterAnalysisDropsStaleSeconds) {
	// Cache scenario from the baseline: after analysis under PPQN 500,
	// switching to PPQN 1000 must recompute rather than reuse seconds.
	std::string ppqnInput = simpleNoteFile(500, 1000, 250000);
	MidiFile ppqn;
	FB_CHECK(readBytes(ppqn, ppqnInput));
	ppqn.doTimeAnalysis();
	FB_CHECK(closeEnough(ppqn.getFileDurationInSeconds(), 0.5));
	ppqn.setTPQ(1000);
	FB_CHECK(ppqn.getRawDivision() == 1000u);
	FB_CHECK(closeEnough(ppqn.getFileDurationInSeconds(), 0.25));

	// Switching an analyzed SMPTE file to PPQN changes the physics.  The
	// hand-built file carries a 250000 us/quarter tempo message which was
	// ignored in SMPTE mode but now governs the PPQN calculation.
	MidiFile smpte;
	FB_CHECK(readBytes(smpte, simpleNoteFile(0xe728, 1000)));
	smpte.doTimeAnalysis();
	FB_CHECK(closeEnough(smpte.getTimeInSeconds(1000), 1.0));
	smpte.setTPQ(1000);
	// 250000 us per quarter, 1000 ticks == one quarter => 0.25 seconds.
	FB_CHECK(closeEnough(smpte.getTimeInSeconds(1000), 0.25));

	// And switching back to SMPTE -29/100 restores the rational timing.
	smpte.setSmpteDivision(-29, 100);
	FB_CHECK(smpte.getRawDivision() == smpteWord(-29, 100));
	FB_CHECK(closeEnough(smpte.getTimeInSeconds(30000), 10.01));
}

FB_TEST("lifecycle",queriesAndReuseAfterFailedRead) {
	MidiFile midiFile;
	FB_CHECK(!readBytes(midiFile, simpleNoteFile(0xe700, 100)));
	FB_CHECK(!midiFile.status());

	// Existing failure convention: physical-time queries return -1.
	FB_CHECK(midiFile.getTimeInSeconds(100) == -1.0);
	FB_CHECK(midiFile.getAbsoluteTickTime(1.0) == -1.0);

	// The object is still usable once a valid file is read.
	FB_CHECK(readBytes(midiFile, simpleNoteFile(500, 1000)));
	FB_CHECK(midiFile.status());
	midiFile.doTimeAnalysis();
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 0.5));
	midiFile.setTPQ(250);
	FB_CHECK(midiFile.getRawDivision() == 250u);
}


///////////////////////////////////////////////////////////////////////////
//
// compatibility group: legacy PPQN behavior and supported file formats
//

FB_TEST("compatibility",ppqnBaselineTimingUnchanged) {
	// Mirrors support/baseline.cpp: PPQN 500, tempo 250000 us, the event at
	// tick 1000 lands at 0.5 seconds.
	std::string input = simpleNoteFile(500, 1000, 250000);
	MidiFile midiFile;
	FB_CHECK(readBytes(midiFile, input));
	midiFile.doTimeAnalysis();
	FB_CHECK(closeEnough(midiFile.getFileDurationInSeconds(), 0.5));
	FB_CHECK(closeEnough(midiFile.getTimeInSeconds(1000), 0.5));
	FB_CHECK(closeEnough(midiFile.getAbsoluteTickTime(0.5), 1000.0));
	FB_CHECK(midiFile.getTPQ() == 500);
}

FB_TEST("compatibility",smpteTPQIsCompatValueOnly) {
	struct TPQCase { int code; int tpf; int expected; };
	const TPQCase cases[4] = {
		{-24,  40,  960},
		{-25,  40, 1000},
		{-29, 100, 2900},
		{-30,  40, 1200},
	};
	for (int i=0; i<4; i++) {
		MidiFile midiFile;
		midiFile.setSmpteDivision(cases[i].code, cases[i].tpf);
		FB_CHECK(midiFile.getTPQ() == cases[i].expected);
		FB_CHECK(midiFile.getRawDivision()
				== smpteWord(cases[i].code, cases[i].tpf));
	}
}

FB_TEST("compatibility",tpqSetterValidationKeepsState) {
	MidiFile midiFile;
	FB_CHECK(midiFile.getTPQ() == 120);
	const int badValues[4] = {0, -1, -120, 0x8000};
	for (int i=0; i<4; i++) {
		bool threw = false;
		try {
			midiFile.setTPQ(badValues[i]);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		FB_CHECK(threw);
		FB_CHECK(midiFile.getTPQ() == 120);
		FB_CHECK(midiFile.getRawDivision() == 120u);
	}

	// A legal value switches the time base and is accepted by both names.
	midiFile.setTicksPerQuarterNote(480);
	FB_CHECK(midiFile.getTPQ() == 480);
	FB_CHECK(midiFile.getRawDivision() == 480u);
	midiFile.setTPQ(960);
	FB_CHECK(midiFile.getTicksPerQuarterNote() == 960);
	FB_CHECK(midiFile.getRawDivision() == 960u);
}

FB_TEST("compatibility",format0And1SupportedFormat2Rejected) {
	// Format 0: one track.
	std::string format0Track;
	format0Track += atDelta(0, noteOn(0, 60, 90));
	format0Track += atDelta(100, noteOff(0, 60));
	format0Track += endOfTrack();
	std::string format0 = makeFile(0, 500,
			std::vector<std::string>(1, format0Track));
	MidiFile type0;
	FB_CHECK(readBytes(type0, format0));
	FB_CHECK(type0.status());
	FB_CHECK(type0.getTrackCount() == 1);
	std::string type0Output = writeBytes(type0);
	FB_CHECK(rawHeaderFormat(type0Output) == 0);
	FB_CHECK(rawHeaderTrackCount(type0Output) == 1);
	FB_CHECK(rawHeaderDivision(type0Output) == 500u);

	// Format 1: two tracks.
	std::string emptyTrack = endOfTrack();
	std::string format1 = makeFile(1, 500,
			twoTracks(emptyTrack, format0Track));
	MidiFile type1;
	FB_CHECK(readBytes(type1, format1));
	FB_CHECK(type1.getTrackCount() == 2);
	std::string type1Output = writeBytes(type1);
	FB_CHECK(rawHeaderFormat(type1Output) == 1);
	FB_CHECK(rawHeaderTrackCount(type1Output) == 2);
	FB_CHECK(rawHeaderDivision(type1Output) == 500u);

	// Format 2 must remain unsupported.
	std::string format2 = headerChunk(2, 1, 500) + trackChunk(endOfTrack());
	MidiFile type2;
	FB_CHECK(!readBytes(type2, format2));
	FB_CHECK(!type2.status());
}

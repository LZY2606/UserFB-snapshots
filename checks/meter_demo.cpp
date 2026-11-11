// checks/meter_demo.cpp -- Independent verification of smf::MeterMap.
//
// Covers: the worked example from the task (TPQ=480, 4/4 -> 3/8 at tick
// 1200), reverse-lookup failures, odd TPQ with denominator 32, multi-track
// same-tick merges/conflicts, joined and delta-tick inputs, snapshot
// isolation, non-mutation of the source MidiFile, and all error cases.

#include "MeterMap.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(cond) do { \
		if (!(cond)) { \
			std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n"; \
			return 1; \
		} \
	} while (0)

#define CHECK_THROW(expr) do { \
		bool threw_ = false; \
		try { expr; } catch (const smf::MeterMapError&) { threw_ = true; } \
		if (!threw_) { \
			std::cerr << "FAIL line " << __LINE__ << ": no MeterMapError from " \
				#expr "\n"; \
			return 1; \
		} \
	} while (0)

using smf::MeterMap;
using smf::MeterPosition;
using smf::MidiFile;

// Serialize everything MeterMap must not disturb: track count, tick
// state, event order, tick/seconds/seq values, message bytes and note
// links.
static std::string captureState(const MidiFile& mf) {
	std::ostringstream out;
	out << mf.getTrackCount() << "|" << mf.isDeltaTicks() << "|"
		<< mf.isAbsoluteTicks() << "|" << mf.getTicksPerQuarterNote() << "\n";
	for (int t=0; t<mf.getTrackCount(); t++) {
		out << "T" << t << ":" << mf[t].size() << "\n";
		for (int i=0; i<mf[t].size(); i++) {
			const smf::MidiEvent& ev = mf[t][i];
			unsigned long long secbits;
			static_assert(sizeof(secbits) == sizeof(ev.seconds), "size");
			std::memcpy(&secbits, &ev.seconds, sizeof(secbits));
			out << i << "," << ev.tick << "," << ev.track << "," << secbits
				<< "," << ev.seq << "," << ev.isLinked() << ","
				<< (const void*)ev.getLinkedEvent() << ":";
			for (int j=0; j<ev.size(); j++) {
				out << (int)ev[j] << ".";
			}
			out << "\n";
		}
	}
	return out.str();
}

// The worked example: TPQ=480, 4/4 at tick 0, 3/8 at tick 1200.
static MidiFile makeExampleFile(void) {
	MidiFile f;
	f.setTPQ(480);
	f.addTrack();
	f.addTimeSignature(0, 0, 4, 4);
	f.addTimeSignature(1, 1200, 3, 8);
	f.addTempo(0, 0, 120);      // tempo must not affect coordinates
	f.addTempo(1, 960, 60);
	f.addNoteOn(1, 0, 0, 60, 100);
	f.addNoteOff(1, 1680, 0, 60);
	for (int t=0; t<f.size(); t++) {
		for (int i=0; i<f[t].size(); i++) {
			f[t][i].track = t;
		}
	}
	f.sortTracks();
	f.linkNotePairs();
	f.doTimeAnalysis();
	return f;
}

int main(void) {
	//--- Section 1: the worked example -------------------------------
	{
		MidiFile f = makeExampleFile();
		std::string before = captureState(f);

		MeterMap mm(f);
		CHECK(mm.isValid());
		CHECK(mm.tickToPosition(0)    == MeterPosition(1, 1));
		CHECK(mm.tickToPosition(959)  == MeterPosition(1, 2, 479, 480));
		CHECK(mm.tickToPosition(960)  == MeterPosition(1, 3));
		CHECK(mm.tickToPosition(1199) == MeterPosition(1, 3, 239, 480));
		CHECK(mm.tickToPosition(1200) == MeterPosition(2, 1));
		CHECK(mm.tickToPosition(1440) == MeterPosition(2, 2));
		CHECK(mm.tickToPosition(1680) == MeterPosition(2, 3));
		CHECK(mm.tickToPosition(1919) == MeterPosition(2, 3, 239, 240));
		CHECK(mm.tickToPosition(1920) == MeterPosition(3, 1));
		// last time signature extends beyond the final event:
		CHECK(mm.tickToPosition(100000).bar > 100);

		CHECK(mm.positionToTick(1, 1) == 0);
		CHECK(mm.positionToTick(1, 3) == 960);
		CHECK(mm.positionToTick(2, 1) == 1200);
		CHECK(mm.positionToTick(2, 3) == 1680);
		CHECK(mm.positionToTick(3, 1) == 1920);
		CHECK(mm.positionToTick(MeterPosition(1, 1, 2, 4)) == 240);
		CHECK(mm.getBeatsInMeasure(1) == 4);
		CHECK(mm.getBeatsInMeasure(2) == 3);
		CHECK(mm.getBeatsInMeasure(50) == 3);

		// measure 1 was truncated at tick 1200: its 4th beat is gone
		CHECK_THROW(mm.positionToTick(1, 4));
		CHECK_THROW(mm.positionToTick(1, 3, 1, 2));  // lands on tick 1200
		// 3/8 has only three beats:
		CHECK_THROW(mm.positionToTick(2, 4));
		// malformed coordinates:
		CHECK_THROW(mm.positionToTick(0, 1));
		CHECK_THROW(mm.positionToTick(1, 0));
		CHECK_THROW(mm.positionToTick(-1, 1));
		CHECK_THROW(mm.positionToTick(1, 1, 3, 2));   // offset >= one beat
		CHECK_THROW(mm.positionToTick(1, 1, 1, 0));   // zero denominator
		CHECK_THROW(mm.positionToTick(1, 1, -1, 2));  // negative offset
		CHECK_THROW(mm.tickToPosition(-1));
		// overflow must throw, not wrap:
		CHECK_THROW(mm.positionToTick(0x7fffffffffffffffLL, 1));

		// exact round trip for every integer tick in a wide range:
		for (long long t=0; t<20000; t++) {
			CHECK(mm.positionToTick(mm.tickToPosition(t)) == t);
		}

		// building and querying must not modify the MidiFile:
		CHECK(captureState(f) == before);
	}

	//--- Section 2: default 4/4, no time signatures at all ------------
	{
		MidiFile f;
		f.setTPQ(480);
		f.addNoteOn(0, 0, 0, 60, 100);
		f.addNoteOff(0, 480, 0, 60);
		MeterMap mm(f);
		CHECK(mm.tickToPosition(0)    == MeterPosition(1, 1));
		CHECK(mm.tickToPosition(480)  == MeterPosition(1, 2));
		CHECK(mm.tickToPosition(1920) == MeterPosition(2, 1));
		CHECK(mm.positionToTick(5, 4) == 4*1920 + 3*480);
	}

	//--- Section 3: re-declaration must not truncate ------------------
	{
		MidiFile f;
		f.setTPQ(480);
		f.addTimeSignature(0, 0, 4, 4);
		f.addTimeSignature(0, 480, 4, 4);   // same signature, mid-measure
		MeterMap mm(f);
		CHECK(mm.tickToPosition(480)  == MeterPosition(1, 2));
		CHECK(mm.tickToPosition(1920) == MeterPosition(2, 1));
		CHECK(mm.positionToTick(1, 4) == 1440);  // measure 1 is intact
	}

	//--- Section 4: change exactly on a measure boundary --------------
	{
		MidiFile f;
		f.setTPQ(480);
		f.addTimeSignature(0, 0, 4, 4);
		f.addTimeSignature(0, 1920, 6, 8);  // exactly where measure 2 starts
		MeterMap mm(f);
		CHECK(mm.tickToPosition(1920) == MeterPosition(2, 1));  // no bar skip
		// 6/8: six beats per measure, one beat = an eighth note (240 ticks)
		CHECK(mm.getBeatsInMeasure(2) == 6);
		CHECK(mm.tickToPosition(1920 + 240)  == MeterPosition(2, 2));
		CHECK(mm.tickToPosition(1920 + 1200) == MeterPosition(2, 6));
		CHECK(mm.tickToPosition(1920 + 1440) == MeterPosition(3, 1));
		CHECK(mm.positionToTick(2, 6) == 3120);
		CHECK_THROW(mm.positionToTick(2, 7));
		for (long long t=0; t<10000; t++) {
			CHECK(mm.positionToTick(mm.tickToPosition(t)) == t);
		}
	}

	//--- Section 5: odd TPQ and denominator 32 -------------------------
	{
		MidiFile f;
		f.setTPQ(7);
		f.addTimeSignature(0, 0, 5, 32);    // beat = 7*4/32 = 7/8 tick
		MeterMap mm(f);
		CHECK(mm.getBeatsInMeasure(1) == 5);
		// measure length is 35/8 ticks; tick 7 is 8 beats in -> measure 2:
		CHECK(mm.tickToPosition(7) == MeterPosition(2, 4));
		// tick 1 is 8/7 beats in: beat 2, offset 1/7 of a beat:
		CHECK(mm.tickToPosition(1) == MeterPosition(1, 2, 1, 7));
		// exact inverse of integer-tick lookups:
		CHECK(mm.positionToTick(MeterPosition(1, 2, 1, 7)) == 1);
		CHECK(mm.positionToTick(MeterPosition(2, 4)) == 7);
		// a coordinate landing on a fractional tick is rejected:
		CHECK_THROW(mm.positionToTick(MeterPosition(1, 1, 1, 2)));  // 7/16
		CHECK_THROW(mm.positionToTick(1, 6));  // only 5 beats
		for (long long t=0; t<2000; t++) {
			CHECK(mm.positionToTick(mm.tickToPosition(t)) == t);
		}
		// tick overflow with a fractional measure length must throw:
		CHECK_THROW(mm.tickToPosition(0x7fffffffffffffffLL));
	}

	//--- Section 6: multi-track merges and conflicts -------------------
	{
		// same signature at the same tick on two tracks: merged, even
		// when the click/32nds bytes differ (they are not beat length):
		MidiFile f;
		f.setTPQ(480);
		f.addTrack();
		f.addTimeSignature(0, 0, 6, 8);              // cc=24, bb=8
		std::vector<smf::uchar> payload = {6, 3, 36, 4};  // same 6/8
		f.addMetaEvent(1, 0, 0x58, payload);         // cc=36, bb=4
		MeterMap mm(f);
		CHECK(mm.getBeatsInMeasure(1) == 6);
		CHECK(mm.tickToPosition(240) == MeterPosition(1, 2));

		// conflicting numerator or denominator at the same tick: error
		MidiFile bad1;
		bad1.setTPQ(480);
		bad1.addTrack();
		bad1.addTimeSignature(0, 480, 3, 4);
		bad1.addTimeSignature(1, 480, 6, 8);
		CHECK_THROW(MeterMap m1(bad1));

		MidiFile bad2;
		bad2.setTPQ(480);
		bad2.addTrack();
		bad2.addTimeSignature(0, 0, 4, 4);
		bad2.addTimeSignature(1, 0, 4, 8);
		CHECK_THROW(MeterMap m2(bad2));
	}

	//--- Section 7: joined tracks and delta ticks ----------------------
	{
		MidiFile f = makeExampleFile();
		MeterMap ref(f);

		MidiFile joined = f;
		joined.joinTracks();
		CHECK(joined.hasJoinedTracks());
		MeterMap jm(joined);
		CHECK(jm.tickToPosition(960)  == MeterPosition(1, 3));
		CHECK(jm.tickToPosition(1680) == MeterPosition(2, 3));
		CHECK_THROW(jm.positionToTick(1, 4));

		MidiFile delta = f;
		delta.makeDeltaTicks();
		CHECK(delta.isDeltaTicks());
		MeterMap dm(delta);
		CHECK(dm.tickToPosition(960)  == MeterPosition(1, 3));
		CHECK(dm.tickToPosition(1200) == MeterPosition(2, 1));
		CHECK(dm.tickToPosition(1680) == MeterPosition(2, 3));
		CHECK_THROW(dm.positionToTick(1, 4));

		// a file read back from disk (absolute ticks after reading):
		MidiFile out = f;
		out.makeDeltaTicks();
		std::stringstream io(std::ios::in | std::ios::out | std::ios::binary);
		CHECK(out.write(io));
		io.seekg(0);
		MidiFile g;
		CHECK(g.read(io));
		CHECK(g.isAbsoluteTicks());
		MeterMap gm(g);
		CHECK(gm.tickToPosition(960)  == MeterPosition(1, 3));
		CHECK(gm.tickToPosition(1680) == MeterPosition(2, 3));
		CHECK_THROW(gm.positionToTick(1, 4));
		for (long long t=0; t<5000; t++) {
			CHECK(gm.positionToTick(gm.tickToPosition(t)) == t);
		}
		// the same file with delta ticks: deltas accumulate per track:
		g.makeDeltaTicks();
		CHECK(g.isDeltaTicks());
		MeterMap gd(g);
		CHECK(gd.tickToPosition(960)  == MeterPosition(1, 3));
		CHECK(gd.tickToPosition(1200) == MeterPosition(2, 1));
		CHECK(gd.tickToPosition(1680) == MeterPosition(2, 3));
		CHECK_THROW(gd.positionToTick(1, 4));
	}

	//--- Section 8: snapshot isolation ---------------------------------
	{
		MidiFile f;
		f.setTPQ(480);
		f.addTimeSignature(0, 0, 4, 4);
		MeterMap snap(f);
		CHECK(snap.tickToPosition(960) == MeterPosition(1, 3));
		// modify the MidiFile after the snapshot was taken:
		f.addTimeSignature(0, 960, 3, 4);
		f.sortTracks();
		CHECK(snap.tickToPosition(960) == MeterPosition(1, 3));  // unchanged
		CHECK(snap.positionToTick(1, 4) == 1440);
		MeterMap rebuilt(f);
		CHECK(rebuilt.tickToPosition(960) == MeterPosition(2, 1));
		CHECK_THROW(rebuilt.positionToTick(1, 4));
	}

	//--- Section 9: invalid files and payloads -------------------------
	{
		// zero numerator:
		MidiFile f1;
		f1.setTPQ(480);
		std::vector<smf::uchar> p1 = {0, 2, 24, 8};
		f1.addMetaEvent(0, 0, 0x58, p1);
		CHECK_THROW(MeterMap m(f1));

		// denominator exponent larger than 7:
		MidiFile f2;
		f2.setTPQ(480);
		std::vector<smf::uchar> p2 = {4, 8, 24, 8};
		f2.addMetaEvent(0, 0, 0x58, p2);
		CHECK_THROW(MeterMap m(f2));

		// malformed payload (wrong length):
		MidiFile f3;
		f3.setTPQ(480);
		std::vector<smf::uchar> p3 = {4, 2, 24};
		f3.addMetaEvent(0, 0, 0x58, p3);
		CHECK_THROW(MeterMap m(f3));

		// non-positive TPQ:
		MidiFile f4;
		f4.setTPQ(0);
		CHECK_THROW(MeterMap m(f4));
		MidiFile f5;
		f5.setTPQ(-3);
		CHECK_THROW(MeterMap m(f5));

		// SMPTE division via setMillisecondTicks:
		MidiFile f6;
		f6.setMillisecondTicks();
		CHECK(f6.isSmpteDivision());
		CHECK_THROW(MeterMap m(f6));

		// SMPTE division read from a real file header (0xE728):
		std::string data;
		data += "MThd";
		data += std::string("\x00\x00\x00\x06", 4);
		data += std::string("\x00\x00", 2);   // format 0
		data += std::string("\x00\x01", 2);   // one track
		data += std::string("\xE7\x28", 2);   // SMPTE: 25 fps, 40 subframes
		data += "MTrk";
		data += std::string("\x00\x00\x00\x04", 4);
		data += std::string("\x00\xFF\x2F\x00", 4);
		std::stringstream input(data, std::ios::in | std::ios::binary);
		MidiFile f7;
		CHECK(f7.read(input));
		CHECK(f7.isSmpteDivision());
		CHECK_THROW(MeterMap m(f7));

		// querying an empty (never built) map:
		MeterMap empty;
		CHECK(!empty.isValid());
		CHECK_THROW(empty.tickToPosition(0));
		CHECK_THROW(empty.positionToTick(1, 1));
	}

	//--- Section 10: MeterPosition basics ------------------------------
	{
		MeterPosition p;
		CHECK(p == MeterPosition(1, 1, 0, 1));
		CHECK(MeterPosition(2, 3).toString() == "2:3+0/1");
		CHECK(MeterPosition(1, 2, 1, 7) != MeterPosition(1, 2, 1, 8));
	}

	std::cout << "METER_MAP_ASSERTIONS_OK\n";
	return 0;
}

//
// Description:   A class which maps between absolute MIDI ticks and
//                musical positions (measure / beat / within-beat offset)
//                according to the time-signature (0x58) meta messages
//                found in a MidiFile.  Only PPQN (ticks-per-quarter)
//                files are supported; SMPTE division files are rejected.
//                Tempo changes do not affect the mapping.
//
// Syntax:        C++11
// vim:           ts=3 noexpandtab
//

#include "MeterMap.h"

#include <algorithm>
#include <climits>


namespace smf {

///////////////////////////////////////////////////////////////////////////
//
// MeterPosition functions --
//

//////////////////////////////
//
// MeterPosition::MeterPosition -- Constructors.
//

MeterPosition::MeterPosition(void) :
	bar(1), beat(1), offsetNum(0), offsetDen(1) {
	// default: start of the first measure
}


MeterPosition::MeterPosition(long long aBar, long long aBeat,
		long long num, long long den) :
	bar(aBar), beat(aBeat), offsetNum(num), offsetDen(den) {
	// nothing else to do
}



//////////////////////////////
//
// MeterPosition::operator== -- Positions are equal when all four
//     coordinates are equal (offsets are stored reduced, so equal
//     offsets always have identical numerators and denominators).
//

bool MeterPosition::operator==(const MeterPosition& other) const {
	return (bar == other.bar) && (beat == other.beat) &&
		(offsetNum == other.offsetNum) && (offsetDen == other.offsetDen);
}


bool MeterPosition::operator!=(const MeterPosition& other) const {
	return !(*this == other);
}



//////////////////////////////
//
// MeterPosition::toString -- Returns "bar:beat+num/den".
//

std::string MeterPosition::toString(void) const {
	return std::to_string(bar) + ":" + std::to_string(beat) + "+" +
		std::to_string(offsetNum) + "/" + std::to_string(offsetDen);
}


///////////////////////////////////////////////////////////////////////////
//
// Local helper functions (file scope) --
//

namespace {

//////////////////////////////
//
// gcdll -- Greatest common divisor of two non-negative integers.
//

long long gcdll(long long a, long long b) {
	while (b != 0) {
		long long r = a % b;
		a = b;
		b = r;
	}
	return a;
}



//////////////////////////////
//
// checkedMul / checkedAdd -- Arithmetic on non-negative values which
//     throws MeterMapError instead of wrapping around on overflow.
//

long long checkedMul(long long a, long long b) {
	if ((a != 0) && (b > LLONG_MAX / a)) {
		throw MeterMapError("MeterMap: arithmetic overflow");
	}
	return a * b;
}


long long checkedAdd(long long a, long long b) {
	if (b > LLONG_MAX - a) {
		throw MeterMapError("MeterMap: arithmetic overflow");
	}
	return a + b;
}

} // end of anonymous namespace


///////////////////////////////////////////////////////////////////////////
//
// MeterMap functions --
//

//////////////////////////////
//
// MeterMap::MeterMap -- Constructors.  The default constructor creates
//     an empty (invalid) map; build() must be called before querying.
//

MeterMap::MeterMap(void) : m_tpq(0) {
	// empty map
}


MeterMap::MeterMap(const MidiFile& midifile) : m_tpq(0) {
	build(midifile);
}



//////////////////////////////
//
// MeterMap::clear -- Remove all contents, returning to the empty state.
//

void MeterMap::clear(void) {
	m_tpq = 0;
	m_segments.clear();
}



//////////////////////////////
//
// MeterMap::isValid -- Returns true if the map has been built.
//

bool MeterMap::isValid(void) const {
	return !m_segments.empty();
}



//////////////////////////////
//
// MeterMap::build -- Take a snapshot of the time-signature sequence of
//     the given MidiFile and construct the tick/position mapping from
//     it.  The MidiFile is only read, never modified, and no references
//     to it are kept: later changes to the MidiFile do not affect this
//     map until build() is called again.
//
//     All 0x58 meta messages on all tracks form a single time-signature
//     sequence.  If the MidiFile currently stores delta ticks, they are
//     accumulated track by track; otherwise the stored absolute ticks
//     are used as they are.  Multiple declarations of the same time
//     signature at the same absolute tick are merged; a conflicting
//     numerator or denominator at the same tick is an error.  The
//     clocks-per-click and 32nds-per-quarter bytes are ignored (they
//     describe the metronome, not the beat length).
//
//     If no time signature is present, 4/4 starting at tick 0 (measure
//     1) is assumed.  A time-signature change starts a new measure at
//     its tick: an unfinished old measure is truncated, while a change
//     landing exactly on an old measure boundary keeps that measure's
//     number.  Re-declaring the currently active time signature has no
//     effect (in particular it does not truncate the current measure).
//

void MeterMap::build(const MidiFile& midifile) {
	clear();
	if (midifile.isSmpteDivision()) {
		throw MeterMapError("MeterMap: SMPTE time division is not supported");
	}
	long long tpq = midifile.getTicksPerQuarterNote();
	if (tpq <= 0) {
		throw MeterMapError("MeterMap: ticks-per-quarter-note must be positive");
	}

	// Collect all time-signature meta messages from all tracks:
	struct RawSignature {
		long long tick;
		int       numerator;
		int       denomExponent;
	};
	std::vector<RawSignature> raw;
	bool delta = midifile.isDeltaTicks();
	int tracks = midifile.getTrackCount();
	for (int t=0; t<tracks; t++) {
		const MidiEventList& events = midifile[t];
		long long abstick = 0;
		for (int i=0; i<events.size(); i++) {
			const MidiEvent& event = events[i];
			if (delta) {
				abstick += event.tick;
			} else {
				abstick = event.tick;
			}
			if (!event.isMetaMessage()) {
				continue;
			}
			if (event.getMetaType() != 0x58) {
				continue;
			}
			if (abstick < 0) {
				throw MeterMapError("MeterMap: time signature at negative tick");
			}
			// A well-formed time signature is FF 58 04 nn dd cc bb.
			if ((event.size() != 7) || (event[2] != 4)) {
				throw MeterMapError("MeterMap: invalid time-signature payload");
			}
			int nn = event[3];
			int dd = event[4];
			if (nn == 0) {
				throw MeterMapError("MeterMap: time-signature numerator is zero");
			}
			if (dd > 7) {
				throw MeterMapError(
					"MeterMap: time-signature denominator exponent exceeds 7");
			}
			RawSignature sig;
			sig.tick          = abstick;
			sig.numerator     = nn;
			sig.denomExponent = dd;
			raw.push_back(sig);
		}
	}

	// Sort by absolute tick, then merge identical signatures that share
	// a tick; conflicting signatures at the same tick are an error:
	std::stable_sort(raw.begin(), raw.end(),
		[](const RawSignature& a, const RawSignature& b) {
			return a.tick < b.tick;
		});
	std::vector<RawSignature> sigs;
	for (const auto& sig : raw) {
		if (!sigs.empty() && (sigs.back().tick == sig.tick)) {
			if ((sigs.back().numerator != sig.numerator) ||
					(sigs.back().denomExponent != sig.denomExponent)) {
				throw MeterMapError(
					"MeterMap: conflicting time signatures at the same tick");
			}
			continue;  // identical re-declaration at the same tick: merge
		}
		sigs.push_back(sig);
	}

	// Build the segment list.  The default is 4/4 from tick 0, measure 1:
	m_tpq = tpq;
	Segment start;
	start.startTick     = 0;
	start.numerator     = 4;
	start.denomExponent = 2;
	start.firstBar      = 1;
	setMeasureLength(start);
	m_segments.push_back(start);

	for (const auto& sig : sigs) {
		Segment& last = m_segments.back();
		if ((sig.numerator == last.numerator) &&
				(sig.denomExponent == last.denomExponent)) {
			continue;  // re-declaration of the active signature: no effect
		}
		if (sig.tick == last.startTick) {
			// A signature change at the very start of the segment (only
			// possible at tick 0): replace the signature in place so that
			// measure numbering is unaffected.
			last.numerator     = sig.numerator;
			last.denomExponent = sig.denomExponent;
			setMeasureLength(last);
			continue;
		}
		// Locate the tick within the current segment's measures:
		long long offset = sig.tick - last.startTick;  // > 0
		long long prod   = checkedMul(offset, last.measureDen);
		long long index  = prod / last.measureNum;     // measures fully elapsed
		bool      exact  = (prod % last.measureNum) == 0;
		Segment next;
		next.startTick     = sig.tick;
		next.numerator     = sig.numerator;
		next.denomExponent = sig.denomExponent;
		// If the change lands exactly on a measure boundary, the measure
		// starting there keeps its number; otherwise the unfinished
		// measure is truncated and the change starts the next one:
		next.firstBar = checkedAdd(last.firstBar, index + (exact ? 0 : 1));
		setMeasureLength(next);
		m_segments.push_back(next);
	}
}



//////////////////////////////
//
// MeterMap::tickToPosition -- Convert an absolute tick into a musical
//     position.  The last time signature extends past the end of the
//     file, so any non-negative tick can be converted.
//

MeterPosition MeterMap::tickToPosition(long long tick) const {
	if (m_segments.empty()) {
		throw MeterMapError("MeterMap: map is empty (build() not called)");
	}
	if (tick < 0) {
		throw MeterMapError("MeterMap: negative tick");
	}
	const Segment& seg = m_segments[findSegmentByTick(tick)];
	long long offset = tick - seg.startTick;

	// Measures fully elapsed within the segment:
	long long prod     = checkedMul(offset, seg.measureDen);
	long long barIndex = prod / seg.measureNum;

	// Within-measure offset in ticks: wmNum/measureDen (wmNum < measureNum):
	long long wmNum = prod - barIndex * seg.measureNum;

	// Beats fully elapsed within the measure.  The beat length in ticks
	// is measureNum/(measureDen*numerator), so the beat index is
	// floor(wmNum*numerator/measureNum):
	long long beatIndex = checkedMul(wmNum, (long long)seg.numerator) /
		seg.measureNum;

	// Within-beat offset as a fraction of one beat:
	//    (wm - beatIndex*beatLen) / beatLen
	//       = (wmNum*numerator - beatIndex*measureNum) / measureNum
	long long fNum = wmNum * seg.numerator - beatIndex * seg.measureNum;
	long long fDen = seg.measureNum;
	long long g = gcdll(fNum, fDen);
	fNum /= g;
	fDen /= g;

	MeterPosition position;
	position.bar       = checkedAdd(seg.firstBar, barIndex);
	position.beat      = beatIndex + 1;
	position.offsetNum = fNum;
	position.offsetDen = fDen;
	return position;
}



//////////////////////////////
//
// MeterMap::positionToTick -- Convert a musical position back into an
//     absolute tick.  Throws MeterMapError if the position does not
//     exist (e.g. a beat beyond the end of its measure, or a position
//     inside a measure region that was truncated by a time-signature
//     change) or if the position falls on a fractional tick (the result
//     is never silently rounded).
//

long long MeterMap::positionToTick(const MeterPosition& position) const {
	if (m_segments.empty()) {
		throw MeterMapError("MeterMap: map is empty (build() not called)");
	}
	if (position.bar < 1) {
		throw MeterMapError("MeterMap: measure number must be at least 1");
	}
	if (position.beat < 1) {
		throw MeterMapError("MeterMap: beat number must be at least 1");
	}
	if (position.offsetDen < 1) {
		throw MeterMapError("MeterMap: offset denominator must be positive");
	}
	if (position.offsetNum < 0) {
		throw MeterMapError("MeterMap: offset numerator must be non-negative");
	}
	// Normalize the within-beat offset fraction:
	long long num = position.offsetNum;
	long long den = position.offsetDen;
	long long g = gcdll(num, den);
	num /= g;
	den /= g;
	if (num >= den) {
		throw MeterMapError(
			"MeterMap: offset must be smaller than one beat");
	}

	int si = findSegmentByBar(position.bar);
	const Segment& seg = m_segments[si];
	if (position.beat > seg.numerator) {
		throw MeterMapError(
			"MeterMap: beat number does not exist in this measure");
	}

	// Beats elapsed from the segment start to the requested position,
	// as the fraction bNum/bDen:
	long long k    = position.bar - seg.firstBar;  // measures into segment
	long long bNum = checkedAdd(checkedMul(k, (long long)seg.numerator),
		position.beat - 1);
	bNum = checkedAdd(checkedMul(bNum, den), num);
	long long bDen = den;

	// Beat length in ticks: measureNum/(measureDen*numerator), reduced:
	long long blNum = seg.measureNum;
	long long blDen = checkedMul(seg.measureDen, (long long)seg.numerator);
	long long bg = gcdll(blNum, blDen);
	blNum /= bg;
	blDen /= bg;

	// Tick offset from the segment start: bNum*blNum / (bDen*blDen).
	// Cross-cancel; the remaining denominators must both become 1,
	// otherwise the position falls on a fractional tick:
	long long g1 = gcdll(bNum, blDen);
	bNum  /= g1;
	blDen /= g1;
	long long g2 = gcdll(blNum, bDen);
	blNum /= g2;
	bDen  /= g2;
	if ((bDen != 1) || (blDen != 1)) {
		throw MeterMapError(
			"MeterMap: position does not fall on an integer tick");
	}
	long long tick = checkedAdd(seg.startTick, checkedMul(bNum, blNum));

	// A position inside a measure region that was cut off by the next
	// time-signature change does not exist:
	if ((si + 1 < (int)m_segments.size()) &&
			(tick >= m_segments[si+1].startTick)) {
		throw MeterMapError(
			"MeterMap: position lies beyond the truncated measure");
	}
	return tick;
}


long long MeterMap::positionToTick(long long bar, long long beat,
		long long offsetNum, long long offsetDen) const {
	return positionToTick(MeterPosition(bar, beat, offsetNum, offsetDen));
}



//////////////////////////////
//
// MeterMap::getBeatsInMeasure -- Returns the number of beats in the
//     given (1-based) measure, i.e. the time-signature numerator in
//     effect for that measure.
//

int MeterMap::getBeatsInMeasure(long long bar) const {
	if (m_segments.empty()) {
		throw MeterMapError("MeterMap: map is empty (build() not called)");
	}
	if (bar < 1) {
		throw MeterMapError("MeterMap: measure number must be at least 1");
	}
	return m_segments[findSegmentByBar(bar)].numerator;
}



//////////////////////////////
//
// MeterMap::findSegmentByTick -- Returns the index of the segment
//     containing the given (non-negative) tick.
//

int MeterMap::findSegmentByTick(long long tick) const {
	int output = 0;
	for (int i=0; i<(int)m_segments.size(); i++) {
		if (m_segments[i].startTick <= tick) {
			output = i;
		} else {
			break;
		}
	}
	return output;
}



//////////////////////////////
//
// MeterMap::findSegmentByBar -- Returns the index of the segment
//     containing the given (1-based) measure number.  Segment firstBar
//     values are strictly increasing, so each measure belongs to
//     exactly one segment.
//

int MeterMap::findSegmentByBar(long long bar) const {
	int output = 0;
	for (int i=0; i<(int)m_segments.size(); i++) {
		if (m_segments[i].firstBar <= bar) {
			output = i;
		} else {
			break;
		}
	}
	return output;
}



//////////////////////////////
//
// MeterMap::setMeasureLength -- Compute the reduced measure length in
//     ticks for a segment: numerator * TPQ * 4 / 2^denomExponent.
//

void MeterMap::setMeasureLength(Segment& segment) const {
	long long num = checkedMul(checkedMul((long long)segment.numerator,
		m_tpq), 4);
	long long den = 1LL << segment.denomExponent;
	long long g = gcdll(num, den);
	segment.measureNum = num / g;
	segment.measureDen = den / g;
}


} // end of namespace smf

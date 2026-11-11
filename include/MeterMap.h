//
// Programmer:    Craig Stuart Sapp <craig@ccrma.stanford.edu> (library)
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

#ifndef _METERMAP_H_INCLUDED
#define _METERMAP_H_INCLUDED

#include "MidiFile.h"

#include <stdexcept>
#include <string>
#include <vector>


namespace smf {

// MeterMapError == exception type thrown by MeterMap for all error
// conditions: unsupported files (SMPTE division, non-positive TPQ),
// malformed time-signature meta messages, conflicting time signatures
// at the same tick, negative ticks, positions that do not exist,
// positions that fall on a fractional tick, and arithmetic overflow.

class MeterMapError : public std::runtime_error {
	public:
		explicit MeterMapError(const std::string& message) :
			std::runtime_error(message) { }
};


// MeterPosition == a musical position: 1-based measure (bar) number,
// 1-based beat number within the measure, and a within-beat offset
// expressed as a reduced non-negative fraction of one beat
// (0 <= offsetNum < offsetDen; a zero offset is always 0/1).  One beat
// is the note value given by the time-signature denominator, so 6/8
// has six beats per measure.  All members are long long so that
// positions far beyond INT_MAX ticks cannot wrap around.

class MeterPosition {
	public:
		long long bar;        // measure number, starting from 1
		long long beat;       // beat number within the measure, starting from 1
		long long offsetNum;  // numerator of within-beat offset (reduced)
		long long offsetDen;  // denominator of within-beat offset (>= 1)

		           MeterPosition  (void);
		           MeterPosition  (long long aBar, long long aBeat,
		                           long long num = 0, long long den = 1);
		bool       operator==    (const MeterPosition& other) const;
		bool       operator!=    (const MeterPosition& other) const;
		std::string toString     (void) const;
};


class MeterMap {
	public:
		              MeterMap        (void);
		              MeterMap        (const MidiFile& midifile);

		void          clear           (void);
		void          build           (const MidiFile& midifile);
		bool          isValid         (void) const;

		MeterPosition tickToPosition  (long long tick) const;
		long long     positionToTick  (const MeterPosition& position) const;
		long long     positionToTick  (long long bar, long long beat,
		                               long long offsetNum = 0,
		                               long long offsetDen = 1) const;

		// number of beats in the measure containing the given bar number:
		int           getBeatsInMeasure(long long bar) const;

	private:
		// Segment == a span of measures sharing one time signature.
		// The measure length in ticks is the reduced fraction
		// measureNum/measureDen (it need not be a whole number of ticks).
		struct Segment {
			long long startTick;      // first tick of the segment
			int       numerator;      // time-signature numerator (beats/measure)
			int       denomExponent;  // log2 of time-signature denominator
			long long firstBar;       // 1-based number of first measure in segment
			long long measureNum;     // measure length in ticks, numerator
			long long measureDen;     // measure length in ticks, denominator
		};

		long long             m_tpq;       // ticks per quarter note (> 0)
		std::vector<Segment>  m_segments;  // ordered by startTick and firstBar

		int           findSegmentByTick (long long tick) const;
		int           findSegmentByBar  (long long bar) const;
		void          setMeasureLength  (Segment& segment) const;
};


} // end of namespace smf

#endif /* _METERMAP_H_INCLUDED */

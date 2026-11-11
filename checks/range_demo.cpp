#include "Tunings.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static const std::string baseScale =
    "Four tone tritave\n4\n5/4\n3/2\n2/1\n3/1\n! @ABL REFERENCE_PITCH 4 0 300.0\n"
    "! @ABL NOTE_NAMES A B C D\n";

static std::string withRange(const std::string &directive)
{
    return "Four tone tritave\n4\n5/4\n3/2\n2/1\n3/1\n! @ABL REFERENCE_PITCH 4 0 300.0\n"
           "! @ABL NOTE_RANGE_BY_" +
           directive + "\n! @ABL NOTE_NAMES A B C D\n";
}

static bool close(double a, double b) { return std::abs(a - b) < 1e-6 * std::max(1.0, b); }

static void expectThrow(const std::string &ascl, const char *what)
{
    try
    {
        auto a = Tunings::parseASCLData(ascl);
        (void)a;
        std::cerr << "Expected TuningError for " << what << std::endl;
        std::exit(1);
    }
    catch (const Tunings::TuningError &)
    {
    }
}

int main()
{
    // Index range (4,1) to (5,0) on the four tone tritave with reference
    // (4,0) = 300 Hz maps 375, 450, 600, 900 Hz onto MIDI 0..3.
    {
        auto a = Tunings::parseASCLData(withRange("INDEX 4 1 5 0"));
        assert(a.noteRange.kind == Tunings::NoteRange::kByIndex);
        assert(a.noteRange.hasUpperBound);
        assert(a.noteRange.minOctave == 4 && a.noteRange.minIndex == 1);
        assert(a.noteRange.maxOctave == 5 && a.noteRange.maxIndex == 0);
        assert(a.noteRange.startPosition == 17 && a.noteRange.endPosition == 20);
        assert(a.noteRange.numKeys == 4);

        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 375.0));
        assert(close(t.frequencyForMidiNote(1), 450.0));
        assert(close(t.frequencyForMidiNote(2), 600.0));
        assert(close(t.frequencyForMidiNote(3), 900.0));

        // MIDI 4 is no longer playable, but the frequency interface keeps
        // extrapolating along the scale for modulation.
        assert(!t.isMidiNoteMapped(4));
        assert(t.scalePositionForMidiNote(4) == -1);
        assert(!t.isMidiNoteMapped(-1));
        assert(t.scalePositionForMidiNote(-1) == -1);
        assert(close(t.frequencyForMidiNote(4), 1125.0));
        assert(close(t.frequencyForMidiNote(-1), 300.0));
        for (int m = 0; m < 4; ++m)
            assert(t.isMidiNoteMapped(m));

        // Note names relocate with the range: MIDI 0 is the B of octave 4.
        assert(t.scalePositionForMidiNote(0) == 1);
        assert(t.noteNameForScalePosition(t.scalePositionForMidiNote(0)) == "B");
        assert(t.midiNoteForNoteName("B", 4) == 0);
        assert(t.midiNoteForNoteName("C", 4) == 1);
        assert(t.midiNoteForNoteName("D", 4) == 2);
        assert(t.midiNoteForNoteName("A", 5) == 3);

        // Names outside the range throw instead of clamping to 0 or 127.
        for (auto [name, oct] : {std::pair{"A", 4}, {"B", 3}, {"B", 5}, {"D", 3}, {"A", 6}})
        {
            try
            {
                auto m = t.midiNoteForNoteName(name, oct);
                (void)m;
                std::cerr << "Expected TuningError for note name " << name << " octave " << oct
                          << std::endl;
                return 1;
            }
            catch (const Tunings::TuningError &)
            {
            }
        }

        // Interpolating skipped notes must not resurrect the notes outside
        // the range, nor flatten their extrapolated frequencies.
        auto ti = t.withSkippedNotesInterpolated();
        assert(!ti.isMidiNoteMapped(4));
        assert(ti.scalePositionForMidiNote(4) == -1);
        assert(close(ti.frequencyForMidiNote(4), 1125.0));
        assert(close(ti.frequencyForMidiNote(0), 375.0));
    }

    // The same four keys selected by frequency: 374 to 900 Hz.
    {
        auto a = Tunings::parseASCLData(withRange("FREQUENCY 374 900"));
        assert(a.noteRange.kind == Tunings::NoteRange::kByFrequency);
        assert(a.noteRange.hasUpperBound);
        assert(a.noteRange.minFreq == 374.0 && a.noteRange.maxFreq == 900.0);
        assert(a.noteRange.numKeys == 4);

        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 375.0));
        assert(close(t.frequencyForMidiNote(1), 450.0));
        assert(close(t.frequencyForMidiNote(2), 600.0));
        assert(close(t.frequencyForMidiNote(3), 900.0));
        assert(!t.isMidiNoteMapped(4));
        assert(t.midiNoteForNoteName("B", 4) == 0);

        // Bounds are inclusive on both ends.
        auto a2 = Tunings::parseASCLData(withRange("FREQUENCY 375 900"));
        assert(a2.noteRange.numKeys == 4);
        auto a3 = Tunings::parseASCLData(withRange("FREQUENCY 375 899"));
        assert(a3.noteRange.numKeys == 3);

        // An open ended frequency range is bounded by the 21000 Hz ceiling.
        auto a4 = Tunings::parseASCLData(withRange("FREQUENCY 374"));
        assert(!a4.noteRange.hasUpperBound);
        assert(a4.noteRange.numKeys == 15); // up to (7,3) = 16200 Hz
        Tunings::Tuning t4(a4);
        assert(close(t4.frequencyForMidiNote(14), 16200.0));
        assert(!t4.isMidiNoteMapped(15));
    }

    // Indices below the reference octave: no off-by-one-period from negative
    // division or modulo.
    {
        auto a = Tunings::parseASCLData(withRange("INDEX 2 3 3 2"));
        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 300.0 * 2.0 / 9.0)); // (2,3)
        assert(close(t.frequencyForMidiNote(1), 100.0));             // (3,0), one period down
        assert(close(t.frequencyForMidiNote(2), 125.0));
        assert(close(t.frequencyForMidiNote(3), 150.0));
        assert(!t.isMidiNoteMapped(4));
        assert(t.midiNoteForNoteName("D", 2) == 0);
        assert(t.midiNoteForNoteName("A", 3) == 1);
    }

    // The reference pitch may fall outside the playable range; every
    // frequency is still determined by it rather than clamped to a key.
    {
        auto a = Tunings::parseASCLData(withRange("INDEX 5 1 5 2"));
        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 1125.0));
        assert(close(t.frequencyForMidiNote(1), 1350.0));
        assert(!t.isMidiNoteMapped(2));
    }

    // An open ended index range is bounded by the 21000 Hz ceiling.
    {
        auto a = Tunings::parseASCLData(withRange("INDEX 0 0"));
        assert(a.noteRange.numKeys == 32); // (0,0) up to (7,3) = 16200 Hz
        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 300.0 / 81.0));
        assert(close(t.frequencyForMidiNote(31), 16200.0));
        assert(!t.isMidiNoteMapped(32));
    }

    // A range is also capped at 128 keys.
    {
        std::string big = "Big\n31\n";
        for (int i = 1; i <= 30; ++i)
            big += std::to_string(1200.0 * i / 31) + "\n";
        big += "2/1\n! @ABL REFERENCE_PITCH 4 0 300.0\n! @ABL NOTE_RANGE_BY_INDEX 0 0\n";
        auto a = Tunings::parseASCLData(big);
        assert(a.noteRange.numKeys == 128);
        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 300.0 / 16.0));
        assert(t.isMidiNoteMapped(127));
        assert(!t.isMidiNoteMapped(128));
    }

    // Malformed ranges all raise TuningError.
    expectThrow(withRange("INDEX 5 0 4 1"), "reversed index range");
    expectThrow(withRange("INDEX 4 1 5"), "half an upper bound");
    expectThrow(withRange("INDEX 4 1 5 0 6"), "extra arguments");
    expectThrow(withRange("INDEX 4"), "missing arguments");
    expectThrow(withRange("INDEX 12abc 0"), "trailing garbage in a number");
    expectThrow(withRange("INDEX 4 1.5"), "non-integer index");
    expectThrow(withRange("INDEX 100 0"), "octave above 99");
    expectThrow(withRange("INDEX -100 0"), "octave below -99");
    expectThrow(withRange("INDEX 4 4"), "index past the scale");
    expectThrow(withRange("INDEX 0 -1"), "negative index");
    expectThrow(withRange("INDEX 9 0"), "range starting above 21000 Hz is empty");
    expectThrow(withRange("FREQUENCY 900 374"), "reversed frequency range");
    expectThrow(withRange("FREQUENCY 3"), "frequency below 4 Hz");
    expectThrow(withRange("FREQUENCY 22000"), "frequency above 21000 Hz");
    expectThrow(withRange("FREQUENCY 374abc"), "trailing garbage in a frequency");
    expectThrow(withRange("FREQUENCY 374 900 1000"), "extra frequency arguments");
    expectThrow(withRange("FREQUENCY 376 449"), "frequency range selecting no notes");

    // A bare directive with no arguments at all.
    expectThrow("Four tone tritave\n4\n5/4\n3/2\n2/1\n3/1\n! @ABL REFERENCE_PITCH 4 0 300.0\n"
                "! @ABL NOTE_RANGE_BY_INDEX\n",
                "bare index directive");
    expectThrow("Four tone tritave\n4\n5/4\n3/2\n2/1\n3/1\n! @ABL REFERENCE_PITCH 4 0 300.0\n"
                "! @ABL NOTE_RANGE_BY_FREQUENCY\n",
                "bare frequency directive");

    // An index range requires the reference pitch to come first.
    expectThrow("Four tone tritave\n4\n5/4\n3/2\n2/1\n3/1\n! @ABL NOTE_RANGE_BY_INDEX 4 1\n"
                "! @ABL REFERENCE_PITCH 4 0 300.0\n",
                "index range before reference pitch");

    // Only one range directive per file, even of the same kind.
    expectThrow(withRange("INDEX 4 1 5 0") + "! @ABL NOTE_RANGE_BY_INDEX 4 1\n",
                "repeated index range");
    expectThrow(withRange("INDEX 4 1 5 0") + "! @ABL NOTE_RANGE_BY_FREQUENCY 374\n",
                "index range followed by frequency range");

    // The frequency form rejects scales which do not strictly ascend within
    // one period.
    expectThrow("Non monotonic\n3\n3/2\n5/4\n2/1\n! @ABL REFERENCE_PITCH 4 0 300.0\n"
                "! @ABL NOTE_RANGE_BY_FREQUENCY 100 1000\n",
                "non monotonic scale with frequency range");
    // ... while the index form allows them.
    {
        auto a = Tunings::parseASCLData("Non monotonic\n3\n3/2\n5/4\n2/1\n"
                                        "! @ABL REFERENCE_PITCH 4 0 300.0\n"
                                        "! @ABL NOTE_RANGE_BY_INDEX 4 0 4 2\n");
        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 300.0));
        assert(close(t.frequencyForMidiNote(1), 450.0));
        assert(close(t.frequencyForMidiNote(2), 375.0));
    }

    // Without a range directive the mapping is exactly as before.
    {
        auto a = Tunings::parseASCLData(baseScale);
        assert(a.noteRange.kind == Tunings::NoteRange::kNone);
        Tunings::Tuning t(a);
        int anchor = a.keyboardMapping.tuningConstantNote;
        assert(close(t.frequencyForMidiNote(anchor), 300.0));
        assert(t.isMidiNoteMapped(0) && t.isMidiNoteMapped(127));
        assert(t.midiNoteForNoteName("A", 4) == anchor);
        // no range, so note name queries clamp rather than throw
        assert(t.midiNoteForNoteName("A", 100) == anchor + 4 * (100 - 4));
        assert(t.midiNoteForNoteName("A", 1000) == Tunings::Tuning::N - 1);

        // And constructing a Tuning from a Scale and KeyboardMapping directly
        // is untouched by all of this.
        Tunings::Tuning t2(a.scale, Tunings::KeyboardMapping());
        assert(close(t2.frequencyForMidiNote(60), Tunings::MIDI_0_FREQ * 32.0));
        assert(t2.isMidiNoteMapped(0) && t2.isMidiNoteMapped(127));
    }

    // The same behavior arrives through a file as through memory.
    {
        auto dir = std::filesystem::temp_directory_path();
        auto path = dir / std::filesystem::path("tuning_library_range_demo_check.ascl");
        {
            std::ofstream ofs(path);
            ofs << withRange("INDEX 4 1 5 0");
        }
        auto a = Tunings::readASCLFile(path.string());
        std::filesystem::remove(path);
        assert(a.noteRange.kind == Tunings::NoteRange::kByIndex);
        assert(a.noteRange.numKeys == 4);
        Tunings::Tuning t(a);
        assert(close(t.frequencyForMidiNote(0), 375.0));
        assert(close(t.frequencyForMidiNote(3), 900.0));
        assert(!t.isMidiNoteMapped(4));
        assert(t.midiNoteForNoteName("B", 4) == 0);
    }

    std::cout << "ASCL_RANGE_OK" << std::endl;
    return 0;
}

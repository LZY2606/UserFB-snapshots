// MTS Single Note Tuning Change contract test.
//
// Uses only the public real-time API and offline PCM generation. Pitch
// evidence is taken from the actual rendered PCM: a controlled, nearly
// pure-sine instrument is installed into the player's dynamic bank, and the
// fundamental period is measured from interpolated positive zero crossings.
//
// OPL3 frequency resolution: the generic frequency table advances one fnum
// unit every 0.0169268578 semitone, so a tone value is quantized to at
// worst +-0.5 fnum unit. For the notes used below this is well below 0.1%
// in pitch (0.17% only at the very bottom of the range). A 0.6% relative
// tolerance is used for pitch ratios; PCM equivalence between two
// independently generated voices allows up to 2% RMS difference (actual
// difference is expected to be only filter/quantization noise).

#include "adlmidi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static const int SR = 48000;

/* ---------------- controlled instrument ---------------- */

// Two-operator additive (AM) voice: the carrier is a full-level sine wave,
// the modulator is attenuated to silence. The envelope holds at full level
// (instant attack, zero decay, sustain bit set so the level never decays).
// OPL multiplier field 1 means F = 1x (field 0 would be 0.5x, an octave down).
static void fillSineOperator(ADL_Operator &op, unsigned totalLevel)
{
    memset(&op, 0, sizeof(op));
    op.avekf_20 = 0x21;      // F=1x, sustain/EG hold bit, no AM/VIB/KSR
    op.ksl_l_40 = static_cast<ADL_UInt8>(totalLevel & 0x3F);
    op.atdec_60 = 0xF0;      // attack 15 (instant), decay 0
    op.susrel_80 = 0x0F;     // sustain level 0, release 15 (fast)
    op.waveform_E0 = 0x00;   // sine
}

static bool installSineInstrument(ADL_MIDIPlayer *p, bool percussive,
                                  unsigned bankMsb, unsigned bankLsb,
                                  unsigned index, ADL_SInt16 noteOffset)
{
    ADL_BankId id;
    id.percussive = percussive ? 1 : 0;
    id.msb = static_cast<ADL_UInt8>(bankMsb & 0x7F);
    id.lsb = static_cast<ADL_UInt8>(bankLsb & 0x7F);

    ADL_Bank bank;
    if(adl_getBank(p, &id, ADLMIDI_Bank_Create, &bank) != 0)
        return false;

    ADL_Instrument ins;
    memset(&ins, 0, sizeof(ins));
    ins.version = 0;
    ins.note_offset1 = noteOffset;
    ins.note_offset2 = 0;
    ins.midi_velocity_offset = 0;
    ins.second_voice_detune = 0;
    ins.percussion_key_number = 0;
    ins.inst_flags = 0; // 2-operator, not blank
    ins.fb_conn1_C0 = 0x01; // AM (additive), no feedback
    ins.fb_conn2_C0 = 0x00;
    fillSineOperator(ins.operators[0], 0x00); // carrier, full level
    fillSineOperator(ins.operators[1], 0x3F); // modulator, muted
    ins.operators[2] = ins.operators[0];
    ins.operators[3] = ins.operators[1];
    ins.delay_on_ms = 1000;
    ins.delay_off_ms = 100;

    return adl_setInstrument(p, &bank, index, &ins) == 0;
}

static ADL_MIDIPlayer *newPlayer(ADL_SInt16 noteOffset = 0)
{
    ADL_MIDIPlayer *p = adl_init(SR);
    if(!p)
        return NULL;
    if(adl_switchEmulator(p, ADLMIDI_EMU_DOSBOX) != 0)
    {
        adl_close(p);
        return NULL;
    }
    if(adl_setNumChips(p, 1) != 0)
    {
        adl_close(p);
        return NULL;
    }
    if(adl_setBank(p, 0) != 0)
    {
        adl_close(p);
        return NULL;
    }
    // A private bank (MSB 0x7E is the XG SFX range and is never selected by
    // the default patch 0 of a melodic channel), so the native bank 0 does
    // not get in the way of the controlled instrument.
    if(!installSineInstrument(p, false, 0x7E, 0, 0, noteOffset))
    {
        adl_close(p);
        return NULL;
    }
    return p;
}

// Put a melodic channel on the private controlled bank.
static void selectSineInstrument(ADL_MIDIPlayer *p, unsigned channel)
{
    adl_rt_bankChange(p, static_cast<ADL_UInt8>(channel), 0x7E00);
    adl_rt_patchChange(p, static_cast<ADL_UInt8>(channel), 0);
}

/* ---------------- PCM rendering and measurement ---------------- */

static int renderFrames(ADL_MIDIPlayer *p, std::vector<short> &out, int frames)
{
    out.resize(static_cast<size_t>(frames) * 2);
    int got = adl_generate(p, frames * 2, out.data());
    if(got < 0)
        got = 0;
    if(got != frames * 2)
        std::fprintf(stderr, "render short: asked %d samples, got %d\n", frames * 2, got);
    out.resize(static_cast<size_t>(got));
    return got / 2;
}

// Measure the fundamental frequency from positive zero crossings.
// channel: 0 = average of both channels, 1 = left, 2 = right.
static bool measureFreq(const std::vector<short> &pcm, int channel, double &freq)
{
    size_t frames = pcm.size() / 2;
    if(frames < 2000)
        return false;

    std::vector<double> s(frames);
    double mean = 0.0;
    for(size_t i = 0; i < frames; ++i)
    {
        double v;
        if(channel == 1)
            v = pcm[i * 2];
        else if(channel == 2)
            v = pcm[i * 2 + 1];
        else
            v = 0.5 * (static_cast<double>(pcm[i * 2]) + static_cast<double>(pcm[i * 2 + 1]));
        s[i] = v;
        mean += v;
    }
    mean /= static_cast<double>(frames);

    double peak = 0.0;
    for(size_t i = 0; i < frames; ++i)
    {
        s[i] -= mean;
        double a = std::fabs(s[i]);
        if(a > peak)
            peak = a;
    }

    if(peak < 500.0) // No meaningful signal
        return false;

    std::vector<double> crossings;
    crossings.reserve(256);
    for(size_t i = 1; i < frames; ++i)
    {
        if(s[i - 1] <= 0.0 && s[i] > 0.0)
        {
            double d = s[i] - s[i - 1];
            double t = static_cast<double>(i - 1) + (-s[i - 1]) / d;
            crossings.push_back(t);
        }
    }

    if(crossings.size() < 4)
        return false;

    double span = crossings.back() - crossings.front();
    if(span <= 0.0)
        return false;

    freq = static_cast<double>(SR) *
           static_cast<double>(crossings.size() - 1) / span;
    return true;
}

static double expectedRatio(double semitones)
{
    return std::pow(2.0, semitones / 12.0);
}

// Render a warm-up, then a measured window.
static bool captureFreq(ADL_MIDIPlayer *p, int channel, double &freq,
                        int warmFrames = 9600, int captureFrames = 14400)
{
    std::vector<short> pcm;
    renderFrames(p, pcm, warmFrames);
    renderFrames(p, pcm, captureFrames);
    return measureFreq(pcm, channel, freq);
}

/* ---------------- SysEx helpers ---------------- */

struct Sysex
{
    //! F0 7F <dev> 08 02 <tt> <count> <records...> F7
    unsigned dev;
    unsigned bank;
    std::vector<ADL_UInt8> records;
    //! When set, the count byte is overridden (for malformed-message tests)
    int countOverride;

    explicit Sysex(unsigned dev_ = 0x7F, unsigned bank_ = 0x00)
        : dev(dev_), bank(bank_), countOverride(-1)
    {
    }

    Sysex &record(unsigned key, unsigned semitone, unsigned fracMsb, unsigned fracLsb)
    {
        // Data bytes are seven-bit; the recorder must never silently wrap a
        // bad value into the seven-bit domain.
        if(key > 0x7F || semitone > 0x7F || fracMsb > 0x7F || fracLsb > 0x7F)
        {
            std::fprintf(stderr, "test bug: Sysex::record with non seven-bit data\n");
            std::abort();
        }
        records.push_back(static_cast<ADL_UInt8>(key));
        records.push_back(static_cast<ADL_UInt8>(semitone));
        records.push_back(static_cast<ADL_UInt8>(fracMsb));
        records.push_back(static_cast<ADL_UInt8>(fracLsb));
        return *this;
    }

    void append(const ADL_UInt8 *data, size_t count)
    {
        for(size_t i = 0; i < count; ++i)
            records.push_back(data[i]);
    }

    std::vector<ADL_UInt8> build(bool terminate = true) const
    {
        std::vector<ADL_UInt8> msg;
        msg.push_back(0xF0);
        msg.push_back(0x7F);
        msg.push_back(static_cast<ADL_UInt8>(dev & 0x7F));
        msg.push_back(0x08); // Universal Real-Time: MIDI Tuning Standard
        msg.push_back(0x02); // Single Note Tuning Change
        msg.push_back(static_cast<ADL_UInt8>(bank & 0x7F));
        msg.push_back(static_cast<ADL_UInt8>(countOverride >= 0 ?
                                            static_cast<unsigned>(countOverride) :
                                            records.size() / 4));
        msg.insert(msg.end(), records.begin(), records.end());
        if(terminate)
            msg.push_back(0xF7);
        return msg;
    }

    int send(ADL_MIDIPlayer *p) const
    {
        std::vector<ADL_UInt8> msg = build();
        int r = adl_rt_systemExclusive(p, msg.data(), msg.size());
        if(r != 1)
        {
            std::fprintf(stderr, "sysex rejected: len=%u bytes=", (unsigned)msg.size());
            for(size_t i = 0; i < msg.size(); ++i)
                std::fprintf(stderr, " %02X", (unsigned)msg[i]);
            std::fprintf(stderr, " -> %d\n", r);
        }
        return r;
    }
};

// Build a message without the trailing F7, for malformed-message tests.
static int sendRaw(ADL_MIDIPlayer *p, const std::vector<ADL_UInt8> &bytes)
{
    std::vector<ADL_UInt8> msg = bytes;
    return adl_rt_systemExclusive(p, msg.data(), msg.size());
}

// Standard single-note tuning message for tuning bank 0.
static int sendTuning(ADL_MIDIPlayer *p, unsigned dev,
                      unsigned key, unsigned semitone,
                      unsigned fracMsb, unsigned fracLsb)
{
    Sysex sx(dev);
    sx.record(key, semitone, fracMsb, fracLsb);
    return sx.send(p);
}

// fraction: absolute pitch fraction part in 1/16384 semitones
static int sendAbsolute(ADL_MIDIPlayer *p, unsigned dev,
                        unsigned key, unsigned semitone, unsigned fraction16384)
{
    unsigned msb = (fraction16384 >> 7) & 0x7F;
    unsigned lsb = fraction16384 & 0x7F;
    return sendTuning(p, dev, key, semitone, msb, lsb);
}

static double ratioError(double measured, double base, double semitones)
{
    return std::fabs(measured / (base * expectedRatio(semitones)) - 1.0);
}

static bool closeEnough(double measured, double base, double semitones, double tol = 0.006)
{
    return ratioError(measured, base, semitones) <= tol;
}

static bool samePitch(double a, double b, double tol = 0.003)
{
    return b > 0.0 && std::fabs(a / b - 1.0) <= tol;
}

/* ---------------- ACTIVE group ---------------- */

static bool groupActive()
{
    bool ok = true;

    // Baseline frequency of key 69 on a fresh player.
    double f69 = 0.0;
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) { std::fprintf(stderr, "active: init failed\n"); return false; }
        ADL_MIDIPlayer *q = p;
        selectSineInstrument(q, 0);
        adl_rt_noteOn(q, 0, 69, 127);
        ok = ok && captureFreq(q, 0, f69);
        adl_rt_noteOff(q, 0, 69);
        adl_close(q);
    }
    if(!ok || f69 <= 0.0)
    {
        std::fprintf(stderr, "active: no baseline tone\n");
        return false;
    }

    // A1: a sounding key is re-pitched immediately by key 69 -> absolute 70.5
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        adl_rt_noteOn(p, 0, 69, 127);

        double f = 0.0;
        if(!captureFreq(p, 0, f) || !samePitch(f, f69, 0.01))
        {
            std::fprintf(stderr, "active A1: base pitch mismatch %f vs %f\n", f, f69);
            ok = false;
        }

        if(sendAbsolute(p, 0x7F, 69, 70, 8192) != 1)
        {
            std::fprintf(stderr, "active A1: message rejected\n");
            ok = false;
        }
        else if(!captureFreq(p, 0, f) || !closeEnough(f, f69, 1.5))
        {
            std::fprintf(stderr, "active A1: pitch not shifted (%f vs expected %f)\n",
                         f, f69 * expectedRatio(1.5));
            ok = false;
        }
        adl_close(p);
    }

    // A2: new notes after the update use the table; key 71 tuned to absolute 69
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        if(sendAbsolute(p, 0x7F, 71, 69, 0) != 1)
        {
            std::fprintf(stderr, "active A2: message rejected\n");
            ok = false;
        }
        adl_rt_noteOn(p, 0, 71, 127);

        double f = 0.0;
        if(!captureFreq(p, 0, f) || !closeEnough(f, f69, 0.0, 0.01))
        {
            std::fprintf(stderr, "active A2: new note not tuned (%f vs %f)\n", f, f69);
            ok = false;
        }
        adl_close(p);
    }

    // A3: non-center pitch bend stacks with the tuning offset.
    // bend raw 10240 = +0.5 semitone (default +-2 semitone range);
    // key 60 absolute 61 adds +1 semitone => tone 61.5 relative to key 60.
    {
        double f60 = 0.0;
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        adl_rt_noteOn(p, 0, 60, 127);
        if(!captureFreq(p, 0, f60)) ok = false;
        adl_close(p);

        p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        adl_rt_pitchBend(p, 0, 10240);
        if(sendAbsolute(p, 0x7F, 60, 61, 0) != 1)
        {
            std::fprintf(stderr, "active A3: message rejected\n");
            ok = false;
        }
        adl_rt_noteOn(p, 0, 60, 127);

        double f = 0.0;
        if(!captureFreq(p, 0, f) || !closeEnough(f, f60, 1.5))
        {
            std::fprintf(stderr, "active A3: bend+tuning stack mismatch (%f vs %f)\n",
                         f, f60 * expectedRatio(1.5));
            ok = false;
        }
        adl_close(p);
    }

    // A4: the same key sounding on two channels is updated on both.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        selectSineInstrument(p, 1);
        adl_rt_controllerChange(p, 0, 10, 0);   // left
        adl_rt_controllerChange(p, 1, 10, 127); // right
        adl_rt_noteOn(p, 0, 62, 127);
        adl_rt_noteOn(p, 1, 62, 127);

        double fl = 0.0, fr = 0.0;
        if(!captureFreq(p, 1, fl) || !captureFreq(p, 2, fr))
        {
            std::fprintf(stderr, "active A4: could not measure channels\n");
            ok = false;
        }

        if(sendAbsolute(p, 0x7F, 62, 63, 8192) != 1)
        {
            std::fprintf(stderr, "active A4: message rejected\n");
            ok = false;
        }

        double fl2 = 0.0, fr2 = 0.0;
        if(!captureFreq(p, 1, fl2) || !captureFreq(p, 2, fr2))
        {
            std::fprintf(stderr, "active A4: could not measure channels after update\n");
            ok = false;
        }
        if(!closeEnough(fl2, fl, 1.5, 0.01) || !closeEnough(fr2, fr, 1.5, 0.01))
        {
            std::fprintf(stderr, "active A4: channel not tuned (L %f->%f, R %f->%f)\n",
                         fl, fl2, fr, fr2);
            ok = false;
        }
        adl_close(p);
    }

    // A5: changing one key does not move a neighboring key.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        selectSineInstrument(p, 1);
        adl_rt_controllerChange(p, 0, 10, 0);
        adl_rt_controllerChange(p, 1, 10, 127);
        adl_rt_noteOn(p, 0, 60, 127);
        adl_rt_noteOn(p, 1, 64, 127);

        double fl = 0.0, fr = 0.0;
        if(!captureFreq(p, 1, fl) || !captureFreq(p, 2, fr))
        {
            std::fprintf(stderr, "active A5: could not measure channels\n");
            ok = false;
        }

        sendAbsolute(p, 0x7F, 60, 61, 8192);

        double fl2 = 0.0, fr2 = 0.0;
        if(!captureFreq(p, 1, fl2) || !captureFreq(p, 2, fr2))
        {
            std::fprintf(stderr, "active A5: could not measure channels after update\n");
            ok = false;
        }
        if(!closeEnough(fl2, fl, 1.5, 0.01))
        {
            std::fprintf(stderr, "active A5: target key not tuned (%f->%f)\n", fl, fl2);
            ok = false;
        }
        if(!samePitch(fr2, fr, 0.003))
        {
            std::fprintf(stderr, "active A5: neighbor key moved (%f->%f)\n", fr, fr2);
            ok = false;
        }
        adl_close(p);
    }

    // A6: percussion channel is not affected and the table is indexed by the
    // original key, not by the instrument's transposed tone.
    {
        // Percussion channel: the very same key must not move.
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        ADL_BankId pid;
        pid.percussive = 1;
        pid.msb = 0;
        pid.lsb = 0;
        ADL_Bank bank;
        if(adl_getBank(p, &pid, ADLMIDI_Bank_Create, &bank) != 0)
        {
            std::fprintf(stderr, "active A6: percussion bank missing\n");
            adl_close(p);
            return false;
        }

        ADL_Instrument ins;
        memset(&ins, 0, sizeof(ins));
        ins.version = 0;
        ins.percussion_key_number = 0;
        ins.inst_flags = 0;
        ins.fb_conn1_C0 = 0x01;
        fillSineOperator(ins.operators[0], 0x00);
        fillSineOperator(ins.operators[1], 0x3F);
        ins.operators[2] = ins.operators[0];
        ins.operators[3] = ins.operators[1];
        ins.delay_on_ms = 1000;
        ins.delay_off_ms = 100;
        if(adl_setInstrument(p, &bank, 69, &ins) != 0)
        {
            std::fprintf(stderr, "active A6: percussion instrument not set\n");
            adl_close(p);
            return false;
        }

        adl_rt_noteOn(p, 9, 69, 127);
        double fd = 0.0;
        if(!captureFreq(p, 0, fd))
        {
            std::fprintf(stderr, "active A6: percussion produced no tone\n");
            ok = false;
        }
        sendAbsolute(p, 0x7F, 69, 71, 0);
        double fd2 = 0.0;
        if(!captureFreq(p, 0, fd2) || !samePitch(fd2, fd, 0.005))
        {
            std::fprintf(stderr, "active A6: percussion moved (%f->%f)\n", fd, fd2);
            ok = false;
        }
        adl_close(p);

        // Raw-key indexing: instrument transposes +1 semitone. Tuning key 69
        // to 70.5 must apply the key-69 value, not the key-70 value.
        double f = 0.0;
        p = newPlayer(1); // note_offset1 = +1
        if(!p) return false;
        selectSineInstrument(p, 0);
        adl_rt_noteOn(p, 0, 69, 127);
        if(!captureFreq(p, 0, f)) ok = false;

        if(sendAbsolute(p, 0x7F, 70, 75, 0) != 1) // key 70 absolute 75
        {
            std::fprintf(stderr, "active A6: key70 message rejected\n");
            ok = false;
        }
        if(sendAbsolute(p, 0x7F, 69, 70, 8192) != 1) // key 69 absolute 70.5
        {
            std::fprintf(stderr, "active A6: key69 message rejected\n");
            ok = false;
        }

        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !closeEnough(f2, f, 1.5, 0.006))
        {
            std::fprintf(stderr, "active A6: raw key indexing mismatch (%f->%f)\n", f, f2);
            ok = false;
        }
        adl_close(p);
    }

    if(ok)
        std::printf("MTS_ACTIVE_BASE_HZ=%.3f\n", f69);
    return ok;
}

/* ---------------- PEDAL group ---------------- */

static bool groupPedal()
{
    bool ok = true;

    // P1: CC64 pedal-held voice is re-pitched immediately.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        adl_rt_noteOn(p, 0, 69, 127);

        double f = 0.0;
        if(!captureFreq(p, 0, f)) ok = false;

        adl_rt_controllerChange(p, 0, 64, 127); // sustain on
        adl_rt_noteOff(p, 0, 69);
        std::vector<short> warm;
        renderFrames(p, warm, 960);

        if(sendAbsolute(p, 0x7F, 69, 70, 8192) != 1)
        {
            std::fprintf(stderr, "pedal P1: message rejected\n");
            ok = false;
        }
        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !closeEnough(f2, f, 1.5))
        {
            std::fprintf(stderr, "pedal P1: held voice not tuned (%f->%f)\n", f, f2);
            ok = false;
        }

        // A second identical message must not accumulate drift.
        sendAbsolute(p, 0x7F, 69, 70, 8192);
        double f3 = 0.0;
        if(!captureFreq(p, 0, f3) || !samePitch(f3, f2, 0.004))
        {
            std::fprintf(stderr, "pedal P1: repeated update drifted (%f->%f)\n", f2, f3);
            ok = false;
        }
        adl_close(p);
    }

    // P2: CC66 sostenuto-held voice is re-pitched.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        adl_rt_noteOn(p, 0, 69, 127);

        double f = 0.0;
        if(!captureFreq(p, 0, f)) ok = false;

        adl_rt_controllerChange(p, 0, 66, 127); // sostenuto on while held
        adl_rt_noteOff(p, 0, 69);
        std::vector<short> warm;
        renderFrames(p, warm, 960);

        if(sendAbsolute(p, 0x7F, 69, 68, 0) != 1) // absolute 68 => -1 semitone
        {
            std::fprintf(stderr, "pedal P2: message rejected\n");
            ok = false;
        }
        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !closeEnough(f2, f, -1.0))
        {
            std::fprintf(stderr, "pedal P2: sostenuto voice not tuned (%f->%f)\n", f, f2);
            ok = false;
        }
        adl_close(p);
    }

    // P3: both pedals at once (Sustain_ANY bits), and another key is untouched.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        selectSineInstrument(p, 1);
        adl_rt_controllerChange(p, 0, 10, 0);
        adl_rt_controllerChange(p, 1, 10, 127);
        adl_rt_noteOn(p, 0, 69, 127);
        adl_rt_noteOn(p, 1, 72, 127);

        adl_rt_controllerChange(p, 0, 64, 127);
        adl_rt_controllerChange(p, 0, 66, 127);
        adl_rt_controllerChange(p, 1, 64, 127);
        adl_rt_controllerChange(p, 1, 66, 127);
        adl_rt_noteOff(p, 0, 69);
        adl_rt_noteOff(p, 1, 72);

        double fl = 0.0, fr = 0.0;
        std::vector<short> warm;
        renderFrames(p, warm, 960);
        if(!captureFreq(p, 1, fl) || !captureFreq(p, 2, fr)) ok = false;

        if(sendAbsolute(p, 0x7F, 69, 70, 8192) != 1)
        {
            std::fprintf(stderr, "pedal P3: message rejected\n");
            ok = false;
        }
        double fl2 = 0.0, fr2 = 0.0;
        if(!captureFreq(p, 1, fl2) || !captureFreq(p, 2, fr2)) ok = false;
        if(!closeEnough(fl2, fl, 1.5, 0.008))
        {
            std::fprintf(stderr, "pedal P3: pedal-held key not tuned (%f->%f)\n", fl, fl2);
            ok = false;
        }
        if(!samePitch(fr2, fr, 0.003))
        {
            std::fprintf(stderr, "pedal P3: other key moved (%f->%f)\n", fr, fr2);
            ok = false;
        }
        adl_close(p);
    }

    // P4: after the pedal is released and the channel is reused, the old key
    // identity must not pull the new sound. A key that still holds its pedal
    // must keep the last pitch value (no drift), and a same-key retrigger
    // must land exactly on the tuned pitch.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);

        adl_rt_noteOn(p, 0, 69, 127);
        adl_rt_controllerChange(p, 0, 64, 127);
        adl_rt_noteOff(p, 0, 69);

        // While the voice is still pedal-held, tune key 72 (a key that has no
        // voice at all): the held key 69 voice must not be dragged along.
        double f = 0.0;
        if(!captureFreq(p, 0, f)) ok = false;
        if(sendAbsolute(p, 0x7F, 72, 74, 0) != 1)
        {
            std::fprintf(stderr, "pedal P4: message rejected\n");
            ok = false;
        }
        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !samePitch(f2, f, 0.004))
        {
            std::fprintf(stderr, "pedal P4: stale key pulled by unrelated key (%f->%f)\n",
                         f, f2);
            ok = false;
        }

        adl_rt_controllerChange(p, 0, 64, 0); // release the held voice
        adl_rt_noteOn(p, 0, 72, 127);         // reuse the chip channel
        double h = 0.0;
        // Key 72 was tuned to absolute 74, i.e. +5 semitones above key 69.
        if(!captureFreq(p, 0, h) || !closeEnough(h, f, 5.0, 0.008))
        {
            std::fprintf(stderr, "pedal P4: reused channel not at key-72 tuning (%f)\n", h);
            ok = false;
        }

        // Retrigger key 69 while its tuning is active; a repeated update must
        // not change the pitch further (no accumulation).
        sendAbsolute(p, 0x7F, 69, 70, 8192);
        adl_rt_noteOff(p, 0, 72);
        adl_rt_noteOn(p, 0, 69, 127);
        double g = 0.0;
        if(!captureFreq(p, 0, g)) ok = false;
        sendAbsolute(p, 0x7F, 69, 70, 8192);
        double g2 = 0.0;
        if(!captureFreq(p, 0, g2) || !samePitch(g2, g, 0.004))
        {
            std::fprintf(stderr, "pedal P4: retrigger/tuning drift (%f->%f)\n", g, g2);
            ok = false;
        }
        if(!closeEnough(g, f, 1.5, 0.006))
        {
            std::fprintf(stderr, "pedal P4: retriggered key not tuned (%f)\n", g);
            ok = false;
        }
        adl_close(p);
    }

    return ok;
}

/* ---------------- VALIDATE group ---------------- */

static bool expectReject(ADL_MIDIPlayer *p, const std::vector<ADL_UInt8> &bytes,
                         const char *what, bool &ok)
{
    if(sendRaw(p, bytes) != 0)
    {
        std::fprintf(stderr, "validate: %s was accepted\n", what);
        ok = false;
        return false;
    }
    return true;
}

// A fully formed, terminated message, so a "reject" result can only come
// from the defect under test and never from a missing terminator.
static std::vector<ADL_UInt8> tuningBytes(unsigned dev, unsigned bank,
                                          const std::vector<unsigned> &recs)
{
    std::vector<ADL_UInt8> b;
    b.push_back(0xF0);
    b.push_back(0x7F);
    b.push_back(static_cast<ADL_UInt8>(dev));
    b.push_back(0x08);
    b.push_back(0x02);
    b.push_back(static_cast<ADL_UInt8>(bank));
    b.push_back(static_cast<ADL_UInt8>(recs.size() / 4));
    for(size_t i = 0; i < recs.size(); ++i)
        b.push_back(static_cast<ADL_UInt8>(recs[i]));
    b.push_back(0xF7);
    return b;
}

// Index layout of tuningBytes(): F0 7F dev 08 02 bank count rec0.. F7
enum { IdxDev = 2, IdxBank = 5, IdxCount = 6, IdxRec0 = 7 };

static bool groupValidate()
{
    bool ok = true;

    // Player with a sounding, untuned key: every rejected message must leave
    // its pitch exactly where it was, and a partial message must not apply
    // any of its leading records.
    ADL_MIDIPlayer *p = newPlayer();
    if(!p) return false;
    selectSineInstrument(p, 0);
    adl_rt_noteOn(p, 0, 69, 127);

    double base = 0.0;
    if(!captureFreq(p, 0, base))
    {
        std::fprintf(stderr, "validate: no baseline\n");
        adl_close(p);
        return false;
    }

    // count = 0 is a successful empty update.
    {
        Sysex sx(0x7F);
        if(sx.send(p) != 1)
        {
            std::fprintf(stderr, "validate: count=0 rejected\n");
            ok = false;
        }
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !samePitch(f, base, 0.002))
        {
            std::fprintf(stderr, "validate: count=0 changed sound\n");
            ok = false;
        }
    }

    // Wrong device (configured id is 0).
    expectReject(p, tuningBytes(3, 0, {69, 70, 64, 0}), "wrong device", ok);

    // Non-realtime universal SysEx.
    {
        std::vector<ADL_UInt8> b;
        b.push_back(0xF0); b.push_back(0x7E); b.push_back(0x7F);
        b.push_back(0x08); b.push_back(0x02); b.push_back(0x00);
        b.push_back(0x01); b.push_back(69); b.push_back(70); b.push_back(64); b.push_back(0);
        b.push_back(0xF7);
        expectReject(p, b, "non-realtime 7E", ok);
    }

    // Non-zero tuning bank.
    expectReject(p, tuningBytes(0x7F, 1, {69, 70, 64, 0}), "tuning bank 1", ok);

    // Count mismatch: count 2, only one record. The first record is valid but
    // must not be applied.
    {
        std::vector<ADL_UInt8> b = tuningBytes(0x7F, 0, {69, 70, 64, 0});
        b[IdxCount] = 2; // claimed record count
        expectReject(p, b, "count greater than records", ok);
    }

    // Truncated record: the declared count matches, but the last record is
    // missing its fraction LSB.
    {
        std::vector<ADL_UInt8> b = tuningBytes(0x7F, 0, {69, 70, 64, 0});
        b.erase(b.end() - 2); // drop record LSB, keep F7
        expectReject(p, b, "truncated record", ok);
    }

    // Extra trailing data after the declared records.
    {
        std::vector<ADL_UInt8> b = tuningBytes(0x7F, 0, {69, 70, 64, 0});
        b.insert(b.end() - 1, 0x00); // extra data byte before F7
        b[IdxCount] = 1;
        expectReject(p, b, "extra tail", ok);
    }

    // Non-seven-bit data byte, even with otherwise valid records.
    {
        std::vector<ADL_UInt8> b = tuningBytes(0x7F, 0, {69, 70, 64, 0});
        b[IdxRec0 + 1] = 0x80; // semitone byte is not seven-bit
        expectReject(p, b, "non seven-bit data", ok);
    }

    // Missing terminating F7.
    {
        std::vector<ADL_UInt8> b = tuningBytes(0x7F, 0, {69, 70, 64, 0});
        b.pop_back(); // drop F7
        expectReject(p, b, "missing F7", ok);
    }

    // Non-zero bank with a full valid payload.
    {
        std::vector<ADL_UInt8> b = tuningBytes(0x7F, 0, {69, 70, 64, 0});
        b[IdxBank] = 0x02; // tuning bank 2
        expectReject(p, b, "bank 2", ok);
    }

    // After all of the above the sound must be unchanged.
    {
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !samePitch(f, base, 0.002))
        {
            std::fprintf(stderr, "validate: rejected messages changed sound (%f vs %f)\n",
                         f, base);
            ok = false;
        }
    }

    // Device id configured through adl_setDeviceIdentifier is accepted,
    // broadcast 0x7F is accepted.
    {
        if(adl_setDeviceIdentifier(p, 5) != 0)
        {
            std::fprintf(stderr, "validate: setDeviceIdentifier failed\n");
            ok = false;
        }
        if(sendAbsolute(p, 5, 69, 70, 0) != 1)
        {
            std::fprintf(stderr, "validate: configured device rejected\n");
            ok = false;
        }
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !samePitch(f, base * expectedRatio(1.0), 0.004))
        {
            std::fprintf(stderr, "validate: configured device not applied\n");
            ok = false;
        }
        if(sendAbsolute(p, 0x7F, 69, 69, 0) != 1) // reset to absolute 69
        {
            std::fprintf(stderr, "validate: broadcast rejected\n");
            ok = false;
        }
        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !samePitch(f2, base, 0.004))
        {
            std::fprintf(stderr, "validate: broadcast not applied\n");
            ok = false;
        }
    }

    // "No change" triplet keeps the existing value.
    {
        sendAbsolute(p, 0x7F, 69, 70, 8192); // absolute 70.5
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !closeEnough(f, base, 1.5, 0.004))
        {
            std::fprintf(stderr, "validate: reserved setup failed\n");
            ok = false;
        }
        Sysex sx(0x7F);
        sx.record(69, 0x7F, 0x7F, 0x7F); // no change
        if(sx.send(p) != 1)
        {
            std::fprintf(stderr, "validate: no-change record rejected\n");
            ok = false;
        }
        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !samePitch(f2, f, 0.002))
        {
            std::fprintf(stderr, "validate: no-change record altered pitch (%f->%f)\n", f, f2);
            ok = false;
        }
    }

    // Duplicate key in one message: the last valid record wins, in order.
    {
        Sysex sx(0x7F);
        sx.record(69, 68, 0, 0);        // key 69 absolute 68 (-1)
        sx.record(69, 70, 8192 >> 7, 0); // key 69 absolute 70.5 (+1.5) wins
        if(sx.send(p) != 1)
        {
            std::fprintf(stderr, "validate: duplicate message rejected\n");
            ok = false;
        }
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !closeEnough(f, base, 1.5, 0.004))
        {
            std::fprintf(stderr, "validate: duplicate order wrong (%f)\n", f);
            ok = false;
        }

        // And a later no-change record does not overwrite the earlier value.
        Sysex sx2(0x7F);
        sx2.record(69, 68, 0, 0);       // -1 first
        sx2.record(69, 0x7F, 0x7F, 0x7F); // no change: must keep -1
        if(sx2.send(p) != 1)
        {
            std::fprintf(stderr, "validate: mixed message rejected\n");
            ok = false;
        }
        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !closeEnough(f2, base, -1.0, 0.004))
        {
            std::fprintf(stderr, "validate: no-change overwrote earlier value (%f)\n", f2);
            ok = false;
        }
    }

    adl_close(p);
    return ok;
}

/* ---------------- RESET group ---------------- */

static bool groupReset()
{
    bool ok = true;

    // Baseline for key 69 on this instrument.
    double base = 0.0;
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        adl_rt_noteOn(p, 0, 69, 127);
        if(!captureFreq(p, 0, base)) ok = false;
        adl_close(p);
    }

    // R1: adl_rt_resetState restores 12-TET.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        if(sendAbsolute(p, 0x7F, 69, 70, 8192) != 1)
        {
            std::fprintf(stderr, "reset R1: message rejected\n");
            ok = false;
        }
        adl_rt_noteOn(p, 0, 69, 127);
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !closeEnough(f, base, 1.5, 0.004))
        {
            std::fprintf(stderr, "reset R1: setup tuning not applied (%f)\n", f);
            ok = false;
        }
        adl_rt_resetState(p);
        adl_rt_noteOn(p, 0, 69, 127);
        double f2 = 0.0;
        if(!captureFreq(p, 0, f2) || !samePitch(f2, base, 0.004))
        {
            std::fprintf(stderr, "reset R1: resetState did not restore (%f vs %f)\n", f2, base);
            ok = false;
        }
        adl_close(p);
    }

    // R2: GM System On restores 12-TET.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        sendAbsolute(p, 0x7F, 69, 70, 8192);

        std::vector<ADL_UInt8> gm;
        gm.push_back(0xF0); gm.push_back(0x7E); gm.push_back(0x7F);
        gm.push_back(0x09); gm.push_back(0x01); gm.push_back(0xF7);
        if(adl_rt_systemExclusive(p, gm.data(), gm.size()) != 1)
        {
            std::fprintf(stderr, "reset R2: GM System On rejected\n");
            ok = false;
        }
        adl_rt_noteOn(p, 0, 69, 127);
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !samePitch(f, base, 0.004))
        {
            std::fprintf(stderr, "reset R2: GM On did not restore (%f vs %f)\n", f, base);
            ok = false;
        }
        adl_close(p);
    }

    // R3: adl_reset restores 12-TET.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        sendAbsolute(p, 0x7F, 69, 70, 8192);
        adl_reset(p);
        selectSineInstrument(p, 0);
        adl_rt_noteOn(p, 0, 69, 127);
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !samePitch(f, base, 0.004))
        {
            std::fprintf(stderr, "reset R3: adl_reset did not restore (%f vs %f)\n", f, base);
            ok = false;
        }
        adl_close(p);
    }

    // R4: CC121 resets controllers only, not the tuning table.
    {
        ADL_MIDIPlayer *p = newPlayer();
        if(!p) return false;
        selectSineInstrument(p, 0);
        sendAbsolute(p, 0x7F, 69, 70, 8192);
        adl_rt_controllerChange(p, 0, 121, 0);
        adl_rt_noteOn(p, 0, 69, 127);
        double f = 0.0;
        if(!captureFreq(p, 0, f) || !closeEnough(f, base, 1.5, 0.004))
        {
            std::fprintf(stderr, "reset R4: CC121 cleared the tuning (%f)\n", f);
            ok = false;
        }
        adl_close(p);
    }

    // R5: two players interleaved do not share tuning state.
    {
        ADL_MIDIPlayer *a = newPlayer();
        ADL_MIDIPlayer *b = newPlayer();
        if(!a || !b)
        {
            if(a) adl_close(a);
            if(b) adl_close(b);
            return false;
        }
        selectSineInstrument(a, 0);
        selectSineInstrument(b, 0);

        if(sendAbsolute(a, 0x7F, 69, 70, 8192) != 1)
        {
            std::fprintf(stderr, "reset R5: player A message rejected\n");
            ok = false;
        }
        if(sendAbsolute(b, 0x7F, 69, 68, 0) != 1)
        {
            std::fprintf(stderr, "reset R5: player B message rejected\n");
            ok = false;
        }

        adl_rt_noteOn(a, 0, 69, 127);
        adl_rt_noteOn(b, 0, 69, 127);

        // Interleave rendering so both instances are active together.
        std::vector<short> pa, pb, tmp;
        renderFrames(a, tmp, 9600);
        renderFrames(b, tmp, 9600);
        renderFrames(a, pa, 14400);
        renderFrames(b, pb, 14400);

        double fa = 0.0, fb = 0.0;
        if(!measureFreq(pa, 0, fa) || !closeEnough(fa, base, 1.5, 0.006))
        {
            std::fprintf(stderr, "reset R5: player A tuning wrong/mixed (%f)\n", fa);
            ok = false;
        }
        if(!measureFreq(pb, 0, fb) || !closeEnough(fb, base, -1.0, 0.006))
        {
            std::fprintf(stderr, "reset R5: player B tuning wrong/mixed (%f)\n", fb);
            ok = false;
        }
        adl_close(a);
        adl_close(b);
    }

    return ok;
}

/* ---------------- equivalent-sound comparison ---------------- */

// One group in ACTIVE compares MTS-tuned sound against an independently
// generated equivalent transposition. This helper performs that comparison
// and reports the allowed OPL quantization tolerance.
static bool equivalentSoundCheck()
{
    ADL_MIDIPlayer *a = newPlayer();
    ADL_MIDIPlayer *b = newPlayer();
    if(!a || !b)
    {
        if(a) adl_close(a);
        if(b) adl_close(b);
        return false;
    }

    selectSineInstrument(a, 0);
    selectSineInstrument(b, 0);

    // A: note 69, tuned to absolute 70.5
    if(sendAbsolute(a, 0x7F, 69, 70, 8192) != 1)
    {
        std::fprintf(stderr, "equiv: MTS message rejected\n");
        adl_close(a);
        adl_close(b);
        return false;
    }
    adl_rt_noteOn(a, 0, 69, 127);

    // B: note 70 plus +0.5 semitone of pitch bend (raw 10240), untuned.
    adl_rt_pitchBend(b, 0, 10240);
    adl_rt_noteOn(b, 0, 70, 127);

    std::vector<short> tmp, pa, pb;
    renderFrames(a, tmp, 9600);
    renderFrames(b, tmp, 9600);
    renderFrames(a, pa, 14400);
    renderFrames(b, pb, 14400);

    double fa = 0.0, fb = 0.0;
    bool measured = measureFreq(pa, 0, fa) && measureFreq(pb, 0, fb);

    double energyA = 0.0, diff = 0.0;
    size_t n = pa.size() < pb.size() ? pa.size() : pb.size();
    for(size_t i = 0; i < n; ++i)
    {
        double x = pa[i];
        double y = pb[i];
        energyA += x * x;
        double d = x - y;
        diff += d * d;
    }
    double rmsDiff = energyA > 0.0 ? std::sqrt(diff / energyA) : 1.0;

    std::printf("MTS_EQUIVALENT_PITCH_A=%.3f B=%.3f RMS_DIFF=%.4f TOLERANCE=%.2f\n",
                fa, fb, rmsDiff, 0.02);

    bool ok = measured && samePitch(fb, fa, 0.004) && rmsDiff <= 0.02;
    if(!ok)
    {
        std::fprintf(stderr, "equiv: tuned MTS sound and equivalent transposition differ "
                             "(pitch %.3f vs %.3f, RMS diff %.4f)\n",
                     fa, fb, rmsDiff);
    }

    adl_close(a);
    adl_close(b);
    return ok;
}

/* ---------------- main ---------------- */

int main()
{
    bool active = groupActive();
    if(active)
        active = equivalentSoundCheck();
    if(!active)
    {
        std::fprintf(stderr, "MTS_ACTIVE_FAILED\n");
        return 1;
    }
    std::printf("MTS_ACTIVE_OK\n");

    if(!groupPedal())
    {
        std::fprintf(stderr, "MTS_PEDAL_FAILED\n");
        return 1;
    }
    std::printf("MTS_PEDAL_OK\n");

    if(!groupValidate())
    {
        std::fprintf(stderr, "MTS_VALIDATE_FAILED\n");
        return 1;
    }
    std::printf("MTS_VALIDATE_OK\n");

    if(!groupReset())
    {
        std::fprintf(stderr, "MTS_RESET_FAILED\n");
        return 1;
    }
    std::printf("MTS_RESET_OK\n");

    return 0;
}

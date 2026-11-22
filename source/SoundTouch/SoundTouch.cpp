//////////////////////////////////////////////////////////////////////////////
///
/// SoundTouch - main class for tempo/pitch/rate adjusting routines. 
///
/// Notes:
/// - Initialize the SoundTouch object instance by setting up the sound stream 
///   parameters with functions 'setSampleRate' and 'setChannels', then set 
///   desired tempo/pitch/rate settings with the corresponding functions.
///
/// - The SoundTouch class behaves like a first-in-first-out pipeline: The 
///   samples that are to be processed are fed into one of the pipe by calling
///   function 'putSamples', while the ready processed samples can be read 
///   from the other end of the pipeline with function 'receiveSamples'.
/// 
/// - The SoundTouch processing classes require certain sized 'batches' of 
///   samples in order to process the sound. For this reason the classes buffer 
///   incoming samples until there are enough of samples available for 
///   processing, then they carry out the processing step and consequently
///   make the processed samples available for outputting.
/// 
/// - For the above reason, the processing routines introduce a certain 
///   'latency' between the input and output, so that the samples input to
///   SoundTouch may not be immediately available in the output, and neither 
///   the amount of outputtable samples may not immediately be in direct 
///   relationship with the amount of previously input samples.
///
/// - The tempo/pitch/rate control parameters can be altered during processing.
///   Please notice though that they aren't currently protected by semaphores,
///   so in multi-thread application external semaphore protection may be
///   required.
///
/// - This class utilizes classes 'TDStretch' for tempo change (without modifying
///   pitch) and 'RateTransposer' for changing the playback rate (that is, both 
///   tempo and pitch in the same ratio) of the sound. The third available control 
///   'pitch' (change pitch but maintain tempo) is produced by a combination of
///   combining the two other controls.
///
/// Author        : Copyright (c) Olli Parviainen
/// Author e-mail : oparviai 'at' iki.fi
/// SoundTouch WWW: http://www.surina.net/soundtouch
///
////////////////////////////////////////////////////////////////////////////////
//
// Last changed  : $Date: 2014-01-05 16:40:22 -0500 (Sun, 05 Jan 2014) $
// File revision : $Revision: 4 $
//
// $Id: SoundTouch.cpp 177 2014-01-05 21:40:22Z oparviai $
//
////////////////////////////////////////////////////////////////////////////////
//
// License :
//
//  SoundTouch audio processing library
//  Copyright (c) Olli Parviainen
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
////////////////////////////////////////////////////////////////////////////////

#include <assert.h>
#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include <stdio.h>
#include <stdexcept>

#include "SoundTouch.h"
#include "TDStretch.h"
#include "RateTransposer.h"
#include "SourceTrace.h"
#include "cpu_detect.h"

using namespace soundtouch;
    
/// test if two floating point numbers are equal
#define TEST_FLOAT_EQUAL(a, b)  (fabs(a - b) < 1e-10)


/// Print library version string for autoconf
extern "C" void soundtouch_ac_test()
{
    printf("SoundTouch Version: %s\n",SOUNDTOUCH_VERSION);
} 


SoundTouch::SoundTouch()
{
    // Initialize rate transposer and tempo changer instances

    pRateTransposer = new RateTransposer();
    pTDStretch = TDStretch::newInstance();

    setOutPipe(pTDStretch);

    rate = tempo = 0;

    virtualPitch =
    virtualRate =
    virtualTempo = 1.0;

    calcEffectiveRateAndTempo();

    channels = 0;
    bSrateSet = FALSE;

    bTracing = false;
    bStreamStarted = false;
    nextTraceFrame = 0;
    bTraceExhausted = false;
}



SoundTouch::~SoundTouch()
{
    delete pRateTransposer;
    delete pTDStretch;
}



/// Get SoundTouch library version string
const char *SoundTouch::getVersionString()
{
    static const char *_version = SOUNDTOUCH_VERSION;

    return _version;
}


/// Get SoundTouch library version Id
uint SoundTouch::getVersionId()
{
    return SOUNDTOUCH_VERSION_ID;
}


// Throws std::logic_error if source tracing is currently enabled: while
// tracing, all processing parameter settings are locked until the next
// clear().
void SoundTouch::throwIfTracing() const
{
    if (bTracing)
    {
        throw std::logic_error("SoundTouch: settings are locked while source trace is enabled");
    }
}


// Sets the number of channels, 1 = mono, 2 = stereo
void SoundTouch::setChannels(uint numChannels)
{
    throwIfTracing();
    /*if (numChannels != 1 && numChannels != 2)
    {
        //ST_THROW_RT_ERROR("Illegal number of channels");
		return;
    }*/
    channels = numChannels;
    pRateTransposer->setChannels((int)numChannels);
    pTDStretch->setChannels((int)numChannels);
}



// Sets new rate control value. Normal rate = 1.0, smaller values
// represent slower rate, larger faster rates.
void SoundTouch::setRate(float newRate)
{
    throwIfTracing();
    virtualRate = newRate;
    calcEffectiveRateAndTempo();
}



// Sets new rate control value as a difference in percents compared
// to the original rate (-50 .. +100 %)
void SoundTouch::setRateChange(float newRate)
{
    throwIfTracing();
    virtualRate = 1.0f + 0.01f * newRate;
    calcEffectiveRateAndTempo();
}



// Sets new tempo control value. Normal tempo = 1.0, smaller values
// represent slower tempo, larger faster tempo.
void SoundTouch::setTempo(float newTempo)
{
    throwIfTracing();
    virtualTempo = newTempo;
    calcEffectiveRateAndTempo();
}



// Sets new tempo control value as a difference in percents compared
// to the original tempo (-50 .. +100 %)
void SoundTouch::setTempoChange(float newTempo)
{
    throwIfTracing();
    virtualTempo = 1.0f + 0.01f * newTempo;
    calcEffectiveRateAndTempo();
}



// Sets new pitch control value. Original pitch = 1.0, smaller values
// represent lower pitches, larger values higher pitch.
void SoundTouch::setPitch(float newPitch)
{
    throwIfTracing();
    virtualPitch = newPitch;
    calcEffectiveRateAndTempo();
}



// Sets pitch change in octaves compared to the original pitch
// (-1.00 .. +1.00)
void SoundTouch::setPitchOctaves(float newPitch)
{
    throwIfTracing();
    virtualPitch = (float)exp(0.69314718056f * newPitch);
    calcEffectiveRateAndTempo();
}



// Sets pitch change in semi-tones compared to the original pitch
// (-12 .. +12)
void SoundTouch::setPitchSemiTones(int newPitch)
{
    throwIfTracing();
    setPitchOctaves((float)newPitch / 12.0f);
}



void SoundTouch::setPitchSemiTones(float newPitch)
{
    throwIfTracing();
    setPitchOctaves(newPitch / 12.0f);
}


// Calculates 'effective' rate and tempo values from the
// nominal control values.
void SoundTouch::calcEffectiveRateAndTempo()
{
    float oldTempo = tempo;
    float oldRate = rate;

    tempo = virtualTempo / virtualPitch;
    rate = virtualPitch * virtualRate;

    if (!TEST_FLOAT_EQUAL(rate,oldRate)) pRateTransposer->setRate(rate);
    if (!TEST_FLOAT_EQUAL(tempo, oldTempo)) pTDStretch->setTempo(tempo);

#ifndef SOUNDTOUCH_PREVENT_CLICK_AT_RATE_CROSSOVER
    if (rate <= 1.0f) 
    {
        if (output != pTDStretch) 
        {
            FIFOSamplePipe *tempoOut;

            assert(output == pRateTransposer);
            // move samples in the current output buffer to the output of pTDStretch
            tempoOut = pTDStretch->getOutput();
            tempoOut->moveSamples(*output);
            // move samples in pitch transposer's store buffer to tempo changer's input
            // deprecated : pTDStretch->moveSamples(*pRateTransposer->getStore());

            output = pTDStretch;
        }
    }
    else
#endif
    {
        if (output != pRateTransposer) 
        {
            FIFOSamplePipe *transOut;

            assert(output == pTDStretch);
            // move samples in the current output buffer to the output of pRateTransposer
            transOut = pRateTransposer->getOutput();
            transOut->moveSamples(*output);
            // move samples in tempo changer's input to pitch transposer's input
            pRateTransposer->moveSamples(*pTDStretch->getInput());

            output = pRateTransposer;
        }
    } 
}


// Sets sample rate.
void SoundTouch::setSampleRate(uint srate)
{
    throwIfTracing();
    bSrateSet = TRUE;
    // set sample rate, leave other tempo changer parameters as they are.
    pTDStretch->setParameters((int)srate);
}


// Adds 'numSamples' pcs of samples from the 'samples' memory position into
// the input of the object.
void SoundTouch::putSamples(const SAMPLETYPE *samples, uint nSamples)
{
    putSamplesInternal(samples, nSamples, false);
}


// Common input routine for 'putSamples' and 'flush'. When source tracing is
// enabled, assigns source frame numbers to the incoming samples (unless
// 'silence' is set, in which case the samples get empty source rows) and
// routes the corresponding source rows through the processing pipeline
// together with the samples.
void SoundTouch::putSamplesInternal(const SAMPLETYPE *samples, uint nSamples, bool silence)
{
    std::vector<SourceRow> rows;

    if (bSrateSet == FALSE)
    {
        ST_THROW_RT_ERROR("SoundTouch : Sample rate not defined");
    }
    else if (channels == 0)
    {
        ST_THROW_RT_ERROR("SoundTouch : Number of channels not defined");
    }

    if (bTracing && nSamples > 0)
    {
        if (!silence)
        {
            // reject input that would overflow the source frame numbering,
            // before writing anything
            if (bTraceExhausted ||
                (uint64_t)(nSamples - 1) > (uint64_t)(~(uint64_t)0) - nextTraceFrame)
            {
                throw std::overflow_error("SoundTouch : Source frame number overflow");
            }
            rows.reserve(nSamples);
            for (uint i = 0; i < nSamples; i ++)
            {
                SourceRow row;
                SourceWeight term;
                term.frame = nextTraceFrame + i;
                term.weight = 1.0;
                row.push_back(term);
                rows.push_back(row);
            }
            if (nextTraceFrame + nSamples - 1 == (uint64_t)(~(uint64_t)0))
            {
                // the last assignable frame number was just used up
                bTraceExhausted = true;
            }
            nextTraceFrame += nSamples;
        }
        else
        {
            // silent padding (flush) has no source frames
            rows.resize(nSamples);
        }
    }
    if (!silence && nSamples > 0)
    {
        // real input was sent: source trace can no longer be enabled
        bStreamStarted = true;
    }

    const SourceRow *pRows = rows.empty() ? NULL : &rows[0];

#ifndef SOUNDTOUCH_PREVENT_CLICK_AT_RATE_CROSSOVER
    if (rate <= 1.0f)
    {
        // transpose the rate down, output the transposed sound to tempo changer buffer
        assert(output == pTDStretch);
        if (bTracing && nSamples > 0) pRateTransposer->inputRows().put(pRows, nSamples);
        pRateTransposer->putSamples(samples, nSamples);
        if (bTracing) pTDStretch->inputRows().moveFrom(pRateTransposer->outputRows());
        pTDStretch->moveSamples(*pRateTransposer);
    }
    else
#endif
    {
        // evaluate the tempo changer, then transpose the rate up,
        assert(output == pRateTransposer);
        if (bTracing && nSamples > 0) pTDStretch->inputRows().put(pRows, nSamples);
        pTDStretch->putSamples(samples, nSamples);
        if (bTracing) pRateTransposer->inputRows().moveFrom(pTDStretch->outputRows());
        pRateTransposer->moveSamples(*pTDStretch);
    }
}


// Flushes the last samples from the processing pipeline to the output.
// Clears also the internal processing buffers.
//
// Note: This function is meant for extracting the last samples of a sound
// stream. This function may introduce additional blank samples in the end
// of the sound stream, and thus it's not recommended to call this function
// in the middle of a sound stream.
void SoundTouch::flush()
{
    int i;
    int nUnprocessed;
    int nOut;
    SAMPLETYPE *buff=(SAMPLETYPE*)alloca(64*channels*sizeof(SAMPLETYPE));

    // check how many samples still await processing, and scale
    // that by tempo & rate to get expected output sample count
    nUnprocessed = numUnprocessedSamples();
    nUnprocessed = (int)((double)nUnprocessed / (tempo * rate) + 0.5);

    nOut = numSamples();        // ready samples currently in buffer ...
    nOut += nUnprocessed;       // ... and how many we expect there to be in the end
    
    memset(buff, 0, 64 * channels * sizeof(SAMPLETYPE));
    // "Push" the last active samples out from the processing pipeline by
    // feeding blank samples into the processing pipeline until new, 
    // processed samples appear in the output (not however, more than 
    // 8ksamples in any case)
    for (i = 0; i < 128; i ++)
    {
        // silent padding pushed by flush has no source frame numbers
        putSamplesInternal(buff, 64, true);
        if ((int)numSamples() >= nOut) 
        {
            // Enough new samples have appeared into the output!
            // As samples come from processing with bigger chunks, now truncate it
            // back to maximum "nOut" samples to improve duration accuracy 
            adjustAmountOfSamples(nOut);

            // finish
            break;  
        }
    }

    // Clear working buffers
    pRateTransposer->clear();
    pTDStretch->clearInput();
    // yet leave the 'tempoChanger' output intouched as that's where the
    // flushed samples are!
}


// Changes a setting controlling the processing system behaviour. See the
// 'SETTING_...' defines for available setting ID's.
BOOL SoundTouch::setSetting(int settingId, int value)
{
    throwIfTracing();
    int sampleRate, sequenceMs, seekWindowMs, overlapMs;

    // read current tdstretch routine parameters
    pTDStretch->getParameters(&sampleRate, &sequenceMs, &seekWindowMs, &overlapMs);

    switch (settingId) 
    {
        case SETTING_USE_AA_FILTER :
            // enables / disabless anti-alias filter
            pRateTransposer->enableAAFilter((value != 0) ? TRUE : FALSE);
            return TRUE;

        case SETTING_AA_FILTER_LENGTH :
            // sets anti-alias filter length
            pRateTransposer->getAAFilter()->setLength(value);
            return TRUE;

        case SETTING_USE_QUICKSEEK :
            // enables / disables tempo routine quick seeking algorithm
            pTDStretch->enableQuickSeek((value != 0) ? TRUE : FALSE);
            return TRUE;

        case SETTING_SEQUENCE_MS:
            // change time-stretch sequence duration parameter
            pTDStretch->setParameters(sampleRate, value, seekWindowMs, overlapMs);
            return TRUE;

        case SETTING_SEEKWINDOW_MS:
            // change time-stretch seek window length parameter
            pTDStretch->setParameters(sampleRate, sequenceMs, value, overlapMs);
            return TRUE;

        case SETTING_OVERLAP_MS:
            // change time-stretch overlap length parameter
            pTDStretch->setParameters(sampleRate, sequenceMs, seekWindowMs, value);
            return TRUE;

        default :
            return FALSE;
    }
}


// Reads a setting controlling the processing system behaviour. See the
// 'SETTING_...' defines for available setting ID's.
//
// Returns the setting value.
int SoundTouch::getSetting(int settingId) const
{
    int temp;

    switch (settingId) 
    {
        case SETTING_USE_AA_FILTER :
            return (uint)pRateTransposer->isAAFilterEnabled();

        case SETTING_AA_FILTER_LENGTH :
            return pRateTransposer->getAAFilter()->getLength();

        case SETTING_USE_QUICKSEEK :
            return (uint)   pTDStretch->isQuickSeekEnabled();

        case SETTING_SEQUENCE_MS:
            pTDStretch->getParameters(NULL, &temp, NULL, NULL);
            return temp;

        case SETTING_SEEKWINDOW_MS:
            pTDStretch->getParameters(NULL, NULL, &temp, NULL);
            return temp;

        case SETTING_OVERLAP_MS:
            pTDStretch->getParameters(NULL, NULL, NULL, &temp);
            return temp;

		case SETTING_NOMINAL_INPUT_SEQUENCE :
			return pTDStretch->getInputSampleReq();

		case SETTING_NOMINAL_OUTPUT_SEQUENCE :
			return pTDStretch->getOutputBatchSize();

		default :
            return 0;
    }
}


// Clears all the samples in the object's output and internal processing
// buffers. Also disables source contribution tracing and clears its history.
void SoundTouch::clear()
{
    pRateTransposer->clear();
    pTDStretch->clear();

    pRateTransposer->enableTrace(false);
    pTDStretch->enableTrace(false);
    bTracing = false;
    bStreamStarted = false;
    nextTraceFrame = 0;
    bTraceExhausted = false;
}



/// Returns number of samples currently unprocessed.
uint SoundTouch::numUnprocessedSamples() const
{
    FIFOSamplePipe * psp;
    if (pTDStretch)
    {
        psp = pTDStretch->getInput();
        if (psp)
        {
            return psp->numSamples();
        }
    }
    return 0;
}


// Enables optional source contribution tracing.
void SoundTouch::enableSourceTrace(uint64_t firstFrame)
{
#ifndef SOUNDTOUCH_FLOAT_SAMPLES
    // source tracing is implemented for floating point samples only
    throw std::invalid_argument("SoundTouch : Source trace requires floating point sample type");
#endif

    if (bStreamStarted)
    {
        throw std::logic_error("SoundTouch : Source trace must be enabled before sending input");
    }
    if (bSrateSet == FALSE || channels == 0)
    {
        throw std::logic_error("SoundTouch : Sample rate and channels must be set before enabling source trace");
    }
    if (bTracing)
    {
        // tracing already enabled and stream hasn't started yet: replace the
        // first frame number
        nextTraceFrame = firstFrame;
        bTraceExhausted = false;
        return;
    }

    // check that the current configuration is supported by tracing
    if (channels != 1 && channels != 2 && channels != 6)
    {
        throw std::invalid_argument("SoundTouch : Source trace supports only 1, 2 or 6 channels");
    }
    int sampleRate = 0;
    pTDStretch->getParameters(&sampleRate, NULL, NULL, NULL);
    if (sampleRate != 44100 && sampleRate != 48000)
    {
        throw std::invalid_argument("SoundTouch : Source trace supports only 44100 or 48000 Hz sample rate");
    }
    if (!(tempo >= 0.5f && tempo <= 2.0f) || !(rate >= 0.5f && rate <= 2.0f))
    {
        throw std::invalid_argument("SoundTouch : Source trace supports effective rate/tempo within [0.5, 2]");
    }
    if (TransposerBase::getAlgorithm() != TransposerBase::CUBIC)
    {
        throw std::invalid_argument("SoundTouch : Source trace supports only CUBIC interpolation");
    }
    if (pRateTransposer->getAAFilter()->getLength() != 64)
    {
        throw std::invalid_argument("SoundTouch : Source trace supports only the default 64-tap anti-alias filter");
    }

    bTracing = true;
    nextTraceFrame = firstFrame;
    bTraceExhausted = false;
    pTDStretch->enableTrace(true);
    pRateTransposer->enableTrace(true);
}


// Returns the source row queue of the current final pipeline stage.
SourceRowFifo &SoundTouch::traceOutputRows()
{
    if (output == (FIFOSamplePipe *)pRateTransposer)
    {
        return pRateTransposer->outputRows();
    }
    return pTDStretch->outputRows();
}


const SourceRowFifo &SoundTouch::traceOutputRows() const
{
    if (output == (FIFOSamplePipe *)pRateTransposer)
    {
        return pRateTransposer->outputRows();
    }
    return pTDStretch->outputRows();
}


// Output samples from beginning of the sample buffer; drops the
// corresponding source rows when tracing is enabled.
uint SoundTouch::receiveSamples(SAMPLETYPE *outBuffer, uint maxSamples)
{
    uint n = FIFOProcessor::receiveSamples(outBuffer, maxSamples);
    if (bTracing && n > 0)
    {
        traceOutputRows().receive(n);
    }
    return n;
}


// Removes samples from the beginning of the pipe without copying them;
// drops the corresponding source rows when tracing is enabled.
uint SoundTouch::receiveSamples(uint maxSamples)
{
    uint n = FIFOProcessor::receiveSamples(maxSamples);
    if (bTracing && n > 0)
    {
        traceOutputRows().receive(n);
    }
    return n;
}


// Receives processed samples together with their source contribution rows.
uint SoundTouch::receiveSamples(SAMPLETYPE *pcm, uint capacity, std::vector<SourceRow> &rows)
{
    if (!bTracing)
    {
        throw std::logic_error("SoundTouch : Source trace is not enabled");
    }
    uint n = FIFOProcessor::receiveSamples(pcm, capacity);
    traceOutputRows().receive(rows, n);
    return n;
}


// Trims the amount of samples in the pipeline; trims the source rows too.
uint SoundTouch::adjustAmountOfSamples(uint numSamples)
{
    uint adjusted = FIFOProcessor::adjustAmountOfSamples(numSamples);
    if (bTracing)
    {
        traceOutputRows().truncate(adjusted);
    }
    return adjusted;
}


// Returns the total number of SourceWeight terms currently held for tracing.
size_t SoundTouch::getTraceStorageTerms() const
{
    if (!bTracing) return 0;
    return pTDStretch->traceTerms() + pRateTransposer->traceTerms();
}

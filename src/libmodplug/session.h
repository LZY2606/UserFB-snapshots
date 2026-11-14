/*
 * This source code is public domain.
 *
 * Per-ModPlugFile rendering session state.
 *
 * The legacy mixer keeps its wave configuration, effect parameters and DSP
 * delay lines in process-global storage, so loading a second module used to
 * reconfigure the one currently playing.  Independent render sessions
 * (ModPlug_LoadWithSettings) own a copy of every piece of state that
 * survives across ModPlug_Read() calls instead.
 *
 * Temporary scratch storage that is only touched between the start and the
 * end of a single CSoundFile::Read() call stays global: calls are serialized,
 * so it never carries information from one session to another.
 *
 * Struct fields carry a trailing underscore because the legacy identifiers are
 * also preprocessor macros (defined below and in sndfile.h) routing every
 * historical reference at the active session.
 */

#ifndef __MODPLUG_SESSION_H
#define __MODPLUG_SESSION_H

#include "sndfile.h"

// DSP effect delay-line sizes, kept identical to the legacy static arrays.
#define MODPLUG_XBASS_BUFFERSIZE       64                              // 2 ms at 50KHz
#define MODPLUG_FILTER_BUFFERSIZE      64                              // 1.25 ms
#define MODPLUG_SURROUND_BUFFERSIZE    ((MAX_SAMPLE_RATE * 50) / 1000)
#define MODPLUG_REVERB_BUFFERSIZE      ((MAX_SAMPLE_RATE * 200) / 1000)
#define MODPLUG_REVERB_BUFFERSIZE2     ((MODPLUG_REVERB_BUFFERSIZE*13) / 17)
#define MODPLUG_REVERB_BUFFERSIZE3     ((MODPLUG_REVERB_BUFFERSIZE*7) / 13)
#define MODPLUG_REVERB_BUFFERSIZE4     ((MODPLUG_REVERB_BUFFERSIZE*7) / 19)

// Persistent state of the master DSP effects (snd_dsp.cpp).  Every field used
// to be a file-scope static variable, which is what made reverb/noise
// reduction history shared between all open modules.
struct ModPlugDspState
{
	// Bass Expansion: low-pass filter
	LONG nXBassSum_;
	LONG nXBassBufferPos_;
	LONG nXBassDlyPos_;
	LONG nXBassMask_;

	// Noise Reduction: simple low-pass filter
	LONG nLeftNR_;
	LONG nRightNR_;

	// Surround Encoding: 1 delay line + low-pass filter + high-pass filter
	LONG nSurroundSize_;
	LONG nSurroundPos_;
	LONG nDolbyDepth_;
	LONG nDolbyLoDlyPos_;
	LONG nDolbyLoFltPos_;
	LONG nDolbyLoFltSum_;
	LONG nDolbyHiFltPos_;
	LONG nDolbyHiFltSum_;

	// Reverb: 4 delay lines + high-pass filter + low-pass filter
	LONG nReverbSize_;
	LONG nReverbBufferPos_;
	LONG nReverbSize2_;
	LONG nReverbBufferPos2_;
	LONG nReverbSize3_;
	LONG nReverbBufferPos3_;
	LONG nReverbSize4_;
	LONG nReverbBufferPos4_;
	LONG nReverbLoFltSum_;
	LONG nReverbLoFltPos_;
	LONG nReverbLoDlyPos_;
	LONG nFilterAttn_;
	LONG gRvbLowPass_[8];
	LONG gRvbLPPos_;
	LONG gRvbLPSum_;
	LONG ReverbLoFilterBuffer_[MODPLUG_XBASS_BUFFERSIZE];
	LONG ReverbLoFilterDelay_[MODPLUG_XBASS_BUFFERSIZE];
	LONG ReverbBuffer_[MODPLUG_REVERB_BUFFERSIZE];
	LONG ReverbBuffer2_[MODPLUG_REVERB_BUFFERSIZE2];
	LONG ReverbBuffer3_[MODPLUG_REVERB_BUFFERSIZE3];
	LONG ReverbBuffer4_[MODPLUG_REVERB_BUFFERSIZE4];

	LONG XBassBuffer_[MODPLUG_XBASS_BUFFERSIZE];
	LONG XBassDelay_[MODPLUG_XBASS_BUFFERSIZE];
	LONG DolbyLoFilterBuffer_[MODPLUG_XBASS_BUFFERSIZE];
	LONG DolbyLoFilterDelay_[MODPLUG_XBASS_BUFFERSIZE];
	LONG DolbyHiFilterBuffer_[MODPLUG_FILTER_BUFFERSIZE];
	LONG SurroundBuffer_[MODPLUG_SURROUND_BUFFERSIZE];
};

// Everything the mixer remembers between Read() calls.  A null context means
// "use the legacy process-global state", which keeps ModPlug_Load() output
// byte-for-byte identical to the original implementation.
struct ModPlugSession
{
	// Former CSoundFile static members (mixer configuration)
	UINT m_nXBassDepth_;
	UINT m_nXBassRange_;
	UINT m_nReverbDepth_;
	UINT m_nReverbDelay_;
	UINT gnReverbType_;
	UINT m_nProLogicDepth_;
	UINT m_nProLogicDelay_;
	UINT m_nStereoSeparation_;
	UINT m_nMaxMixChannels_;
	LONG m_nStreamVolume_;
	DWORD gdwSysInfo_;
	DWORD gdwSoundSetup_;
	DWORD gdwMixingFreq_;
	DWORD gnBitsPerSample_;
	DWORD gnChannels_;
	UINT gnAGC_;
	UINT gnVolumeRampSamples_;
	UINT gnVUMeter_;
	UINT gnCPUUsage_;
	LPSNDMIXHOOKPROC gpSndMixHook_;

	// Former file-scope mixer state (sndmix.cpp / fastmix.cpp)
	LONG gnDryROfsVol_;
	LONG gnDryLOfsVol_;
	LONG gnRvbROfsVol_;
	LONG gnRvbLOfsVol_;
	UINT gnReverbSend_;
	int gbInitPlugins_;
	DWORD gAGCRecoverCount_;

	// Persistent master DSP history
	ModPlugDspState dsp;

	// Output frame size, cached the way ModPlug::gSampleSize was.
	int gSampleSize_;
};

namespace ModPlug
{
	// The legacy global session.  Its definition lives in sndmix.cpp and it
	// keeps the original static initializers.  Explicit visibility is needed
	// because library builds compile with -fvisibility=hidden.
	MODPLUG_EXPORT extern ModPlugSession gLegacySession;

	// Active session for the mixer call currently on the stack.  Only ever
	// switched around serialized top-level API calls, never mid-render.
	MODPLUG_EXPORT extern ModPlugSession *gActiveSession;

	MODPLUG_EXPORT inline ModPlugSession &ActiveSession() { return *gActiveSession; }

	// Give a freshly created session the same defaults the legacy statics had
	// at process startup.
	MODPLUG_EXPORT void InitSessionDefaults(ModPlugSession *session);
}

// Routing macros for the former CSoundFile static configuration members
// (m_nXBassDepth, gdwMixingFreq, gpSndMixHook, ...) are defined in sndfile.h,
// which this header includes above.

// Routing for the file-scope mixer state that used to live in sndmix.cpp,
// fastmix.cpp and snd_dsp.cpp.  Address-of still works: ActiveSession()
// returns a reference, so &gnDryROfsVol is a valid pointer into the session.
#define gnDryROfsVol        (ModPlug::ActiveSession().gnDryROfsVol_)
#define gnDryLOfsVol        (ModPlug::ActiveSession().gnDryLOfsVol_)
#define gnRvbROfsVol        (ModPlug::ActiveSession().gnRvbROfsVol_)
#define gnRvbLOfsVol        (ModPlug::ActiveSession().gnRvbLOfsVol_)
#define gnReverbSend        (ModPlug::ActiveSession().gnReverbSend_)
#define gbInitPlugins       (ModPlug::ActiveSession().gbInitPlugins_)
#define gAGCRecoverCount    (ModPlug::ActiveSession().gAGCRecoverCount_)

#define nXBassSum           (ModPlug::ActiveSession().dsp.nXBassSum_)
#define nXBassBufferPos     (ModPlug::ActiveSession().dsp.nXBassBufferPos_)
#define nXBassDlyPos        (ModPlug::ActiveSession().dsp.nXBassDlyPos_)
#define nXBassMask          (ModPlug::ActiveSession().dsp.nXBassMask_)
#define nLeftNR             (ModPlug::ActiveSession().dsp.nLeftNR_)
#define nRightNR            (ModPlug::ActiveSession().dsp.nRightNR_)
#define nSurroundSize       (ModPlug::ActiveSession().dsp.nSurroundSize_)
#define nSurroundPos        (ModPlug::ActiveSession().dsp.nSurroundPos_)
#define nDolbyDepth         (ModPlug::ActiveSession().dsp.nDolbyDepth_)
#define nDolbyLoDlyPos      (ModPlug::ActiveSession().dsp.nDolbyLoDlyPos_)
#define nDolbyLoFltPos      (ModPlug::ActiveSession().dsp.nDolbyLoFltPos_)
#define nDolbyLoFltSum      (ModPlug::ActiveSession().dsp.nDolbyLoFltSum_)
#define nDolbyHiFltPos      (ModPlug::ActiveSession().dsp.nDolbyHiFltPos_)
#define nDolbyHiFltSum      (ModPlug::ActiveSession().dsp.nDolbyHiFltSum_)
#define nReverbSize         (ModPlug::ActiveSession().dsp.nReverbSize_)
#define nReverbBufferPos    (ModPlug::ActiveSession().dsp.nReverbBufferPos_)
#define nReverbSize2        (ModPlug::ActiveSession().dsp.nReverbSize2_)
#define nReverbBufferPos2   (ModPlug::ActiveSession().dsp.nReverbBufferPos2_)
#define nReverbSize3        (ModPlug::ActiveSession().dsp.nReverbSize3_)
#define nReverbBufferPos3   (ModPlug::ActiveSession().dsp.nReverbBufferPos3_)
#define nReverbSize4        (ModPlug::ActiveSession().dsp.nReverbSize4_)
#define nReverbBufferPos4   (ModPlug::ActiveSession().dsp.nReverbBufferPos4_)
#define nReverbLoFltSum     (ModPlug::ActiveSession().dsp.nReverbLoFltSum_)
#define nReverbLoFltPos     (ModPlug::ActiveSession().dsp.nReverbLoFltPos_)
#define nReverbLoDlyPos     (ModPlug::ActiveSession().dsp.nReverbLoDlyPos_)
#define nFilterAttn         (ModPlug::ActiveSession().dsp.nFilterAttn_)
#define gRvbLowPass         (ModPlug::ActiveSession().dsp.gRvbLowPass_)
#define gRvbLPPos           (ModPlug::ActiveSession().dsp.gRvbLPPos_)
#define gRvbLPSum           (ModPlug::ActiveSession().dsp.gRvbLPSum_)
#define ReverbLoFilterBuffer (ModPlug::ActiveSession().dsp.ReverbLoFilterBuffer_)
#define ReverbLoFilterDelay  (ModPlug::ActiveSession().dsp.ReverbLoFilterDelay_)
#define ReverbBuffer        (ModPlug::ActiveSession().dsp.ReverbBuffer_)
#define ReverbBuffer2       (ModPlug::ActiveSession().dsp.ReverbBuffer2_)
#define ReverbBuffer3       (ModPlug::ActiveSession().dsp.ReverbBuffer3_)
#define ReverbBuffer4       (ModPlug::ActiveSession().dsp.ReverbBuffer4_)
#define XBassBuffer         (ModPlug::ActiveSession().dsp.XBassBuffer_)
#define XBassDelay          (ModPlug::ActiveSession().dsp.XBassDelay_)
#define DolbyLoFilterBuffer (ModPlug::ActiveSession().dsp.DolbyLoFilterBuffer_)
#define DolbyLoFilterDelay  (ModPlug::ActiveSession().dsp.DolbyLoFilterDelay_)
#define DolbyHiFilterBuffer (ModPlug::ActiveSession().dsp.DolbyHiFilterBuffer_)
#define SurroundBuffer      (ModPlug::ActiveSession().dsp.SurroundBuffer_)

#endif // __MODPLUG_SESSION_H

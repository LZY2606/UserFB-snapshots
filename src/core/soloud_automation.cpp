/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#include "soloud_internal.h"

// Frame-accurate volume automation helpers.
//
// SoLoud historically advanced faders once per mix call, before the call's
// samples had been produced. Mixing a fixed span of audio in one 512-frame
// call and in thirty-two 16-frame calls thus sampled the fader at different
// points, which made short fades and LFOs depend on the block size. The
// helpers here evaluate the fader purely against a frame clock instead:
//
//   * the engine keeps mStreamFrames (and each voice keeps its own
//     mStreamFrames) counting output frames already delivered,
//   * a control call made between two mix calls anchors the automation at
//     that position, i.e. at time anchorFrames / R,
//   * the k-th output frame produced after the call (k starts at 1) uses
//     time (anchorFrames + k) / R.
//
// Spitting the same span into any number of mix calls therefore delivers the
// same automatic value on every frame.

namespace SoLoud
{
	void Soloud::applyVoiceVolumeEnvelope(AudioSourceInstance *aVoice, float *aScratch, unsigned int aSamples, unsigned int aBufferSize, unsigned long long aStartFrame)
	{
		// aBufferSize is the per-channel stride of the scratch buffer, not the
		// number of frames to process. Only aSamples frames are touched so the
		// request length never gets rounded up to the internal stride.
		unsigned int k;
		for (k = 0; k < aSamples; k++)
		{
			// Frame k (zero-based in the block) is the (k+1)-th frame after the
			// block start, hence k + 1 in the one-based time convention.
			float v = aVoice->mVolumeFader.getAt((aStartFrame + k + 1) / (double)mSamplerate);
			unsigned int c;
			for (c = 0; c < aVoice->mChannels; c++)
				aScratch[c * aBufferSize + k] *= v;
		}
	}

	void Soloud::clip_internal_envelope(AlignedFloatBuffer &aBuffer, AlignedFloatBuffer &aDestBuffer, unsigned int aSamples, unsigned int aStride, const Fader &aVolumeFader, unsigned long long aStartFrame)
	{
		// aStride is the internal channel stride used by the mixer's planar
		// buffers. It is not the request length: only the first aSamples
		// frames of each channel are part of the delivered audio.
		unsigned int j;
		for (j = 0; j < mChannels; j++)
		{
			unsigned int c = j * aStride;
			unsigned int i;
			for (i = 0; i < aSamples; i++)
			{
				float v = aVolumeFader.getAt((aStartFrame + i + 1) / (double)mSamplerate);
				float f = aBuffer.mData[c + i] * v;
				if (mFlags & CLIP_ROUNDOFF)
				{
					if (f <= -1.65f) f = -0.9862875f;
					else if (f >= 1.65f) f = 0.9862875f;
					else f = 0.87f * f - 0.1f * f * f * f;
				}
				else
				{
					if (f <= -1) f = -1;
					else if (f >= 1) f = 1;
				}
				aDestBuffer.mData[c + i] = f * mPostClipScaler;
			}
		}
	}
};

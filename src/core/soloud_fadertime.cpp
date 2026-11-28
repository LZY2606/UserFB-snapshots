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

// Volume automation evaluated in output-frame time. Control calls happen
// between two mix() calls; the audio position already handed to the caller is
// the origin. Frame k (k = 1..N) of the following block is at time
// origin + k / samplerate, independently of how the block is split.

namespace SoLoud
{
	void fill_fader_frame_values(const Fader &aFader, time aBlockStart, unsigned int aFrames, unsigned int aSampleRate, float aScale, float *aFrameVolumes)
	{
		if (aSampleRate == 0)
			return;
		const double invRate = 1.0 / (double)aSampleRate;
		unsigned int i;
		for (i = 0; i < aFrames; i++)
		{
			double t = aBlockStart + (double)(i + 1) * invRate;
			aFrameVolumes[i] = aFader.getValue(t) * aScale;
		}
	}

	void apply_frame_volumes(float *aScratch, unsigned int aStride, unsigned int aChannels, unsigned int aFrames, const float *aFrameVolumes)
	{
		unsigned int c, i;
		for (c = 0; c < aChannels; c++)
		{
			float *channel = aScratch + (size_t)c * aStride;
			for (i = 0; i < aFrames; i++)
			{
				channel[i] *= aFrameVolumes[i];
			}
		}
	}
};

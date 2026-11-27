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

#include "soloud.h"
#include "soloud_fft.h"
#include "soloud_internal.h"
#include <climits>
#include <cstring>

namespace SoLoud
{
	BusInstance::BusInstance(Bus *aParent)
	{
		mParent = aParent;
		mFlags |= PROTECTED | INAUDIBLE_TICK;
		for (int i = 0; i < MAX_CHANNELS; i++)
			mVisualizationChannelVolume[i] = 0;
		for (int i = 0; i < 256; i++)
			mVisualizationWaveData[i] = 0;
		mScratchSize = SAMPLE_GRANULARITY;
		mScratch.init(mScratchSize * MAX_CHANNELS);
		// Sidechain caches are allocated in Soloud::setBusDucker, so scenes
		// without sidechains pay no extra memory.
		for (int slot = 0; slot < 2; slot++)
		{
			mSidechainCacheBlock[slot] = UINT_MAX;
		}
		mSidechainBlockCounter = 0;
		mSidechainReadOffset = 0;
	}

	unsigned int BusInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		int handle = mParent->mChannelHandle;
		if (handle == 0)
		{
			// Avoid reuse of scratch data if this bus hasn't played anything yet
			unsigned int i;
			for (i = 0; i < aBufferSize * mChannels; i++)
				aBuffer[i] = 0;
			return aSamplesToRead;
		}

		Soloud *s = mParent->mSoloud;

		if (s->mSidechainActive && mSidechainCache[0].mData != 0)
		{
			// Sidechain mode: serve every request from the memoized block cache.
			// At play speed 1 with point resampling the parent mixer asks for
			// block-aligned runs of at most SAMPLE_GRANULARITY frames.
			unsigned int produced = 0;
			while (produced < aSamplesToRead)
			{
				if (mSidechainReadOffset == 0)
				{
					s->sidechainBlock_internal(this, mSidechainBlockCounter);
				}
				unsigned int block = mSidechainBlockCounter;
				float *cached = mSidechainCache[block & 1].mData;
				unsigned int chunk = aSamplesToRead - produced;
				if (chunk > SAMPLE_GRANULARITY - mSidechainReadOffset)
					chunk = SAMPLE_GRANULARITY - mSidechainReadOffset;
				for (unsigned int ch = 0; ch < mChannels; ch++)
				{
					memcpy(aBuffer + ch * aBufferSize + produced,
						cached + ch * SAMPLE_GRANULARITY + mSidechainReadOffset,
						chunk * sizeof(float));
				}
				mSidechainReadOffset += chunk;
				produced += chunk;
				if (mSidechainReadOffset == SAMPLE_GRANULARITY)
				{
					mSidechainBlockCounter++;
					mSidechainReadOffset = 0;
				}
			}
		}
		else
		{
			s->mixBus_internal(aBuffer, aSamplesToRead, aBufferSize, mScratch.mData, handle, mSamplerate, mChannels, mParent->mResampler);
		}

		int i;
		if (mParent->mFlags & AudioSource::VISUALIZATION_DATA)
		{
			for (i = 0; i < MAX_CHANNELS; i++)
				mVisualizationChannelVolume[i] = 0;

			if (aSamplesToRead > 255)
			{
				for (i = 0; i < 256; i++)
				{
					int j;
					mVisualizationWaveData[i] = 0;
					for (j = 0; j < (signed)mChannels; j++)
					{
						float sample = aBuffer[i + aBufferSize * j];
						float absvol = (float)fabs(sample);
						if (absvol > mVisualizationChannelVolume[j])
							mVisualizationChannelVolume[j] = absvol;
						mVisualizationWaveData[i] += sample;
					}
				}
			}
			else
			{
				// Very unlikely failsafe branch
				for (i = 0; i < 256; i++)
				{
					int j;
					mVisualizationWaveData[i] = 0;
					for (j = 0; j < (signed)mChannels; j++)
					{
						float sample = aBuffer[(i % aSamplesToRead) + aBufferSize * j];
						float absvol = (float)fabs(sample);
						if (absvol > mVisualizationChannelVolume[j])
							mVisualizationChannelVolume[j] = absvol;
						mVisualizationWaveData[i] += sample;
					}
				}
			}
		}
		return aSamplesToRead;
	}

	bool BusInstance::hasEnded()
	{
		// Busses never stop for fear of going under 50mph.
		return 0;
	}

	bool BusInstance::isBusInstance()
	{
		return true;
	}

	BusInstance::~BusInstance()
	{
		Soloud *s = mParent->mSoloud;
		int i;
		// Detach a ducker plan that targeted this exact play instance. Plans are
		// bound to handles, not slots, so a later voice reusing the slot must not
		// inherit a stale connection.
		if (s && mParent->mChannelHandle)
		{
			int slot = (mParent->mChannelHandle & 0xfff) - 1;
			if (slot >= 0 && slot < VOICE_COUNT &&
				s->mBusDucker[slot].mActive &&
				s->mBusDucker[slot].mTarget == mParent->mChannelHandle)
			{
				s->mBusDucker[slot].mActive = false;
				s->mBusDucker[slot].mTarget = 0;
				s->mBusDucker[slot].mDetector = 0;
				bool active = false;
				for (i = 0; i < VOICE_COUNT; i++)
					if (s->mBusDucker[i].mActive)
						active = true;
				s->mSidechainActive = active;
			}
		}
		for (i = 0; i < (signed)s->mHighestVoice; i++)
		{
			if (s->mVoice[i] && s->mVoice[i]->mBusHandle == mParent->mChannelHandle)
			{
				s->stopVoice_internal(i);
			}
		}
	}

	Bus::Bus()
	{
		mChannelHandle = 0;
		mInstance = 0;
		mChannels = 2;
		mResampler = SOLOUD_DEFAULT_RESAMPLER;
		for (int i = 0; i < 256; i++)
		{
			mFFTData[i] = 0;
			mWaveData[i] = 0;
		}
	}
	
	BusInstance * Bus::createInstance()
	{
		if (mChannelHandle)
		{
			stop();
			mChannelHandle = 0;
			mInstance = 0;
		}
		mInstance = new BusInstance(this);
		return mInstance;
	}

	void Bus::findBusHandle()
	{
		if (mChannelHandle == 0)
		{
			// Find the channel the bus is playing on to calculate handle..
			int i;
			for (i = 0; mChannelHandle == 0 && i < (signed)mSoloud->mHighestVoice; i++)
			{
				if (mSoloud->mVoice[i] == mInstance)
				{
					mChannelHandle = mSoloud->getHandleFromVoice_internal(i);
				}
			}
		}
	}

	handle Bus::play(AudioSource &aSound, float aVolume, float aPan, bool aPaused)
	{
		if (!mInstance || !mSoloud)
		{
			return 0;
		}

		findBusHandle();

		if (mChannelHandle == 0)
		{
			return 0;
		}
		return mSoloud->play(aSound, aVolume, aPan, aPaused, mChannelHandle);
	}	


	handle Bus::playClocked(time aSoundTime, AudioSource &aSound, float aVolume, float aPan)
	{
		if (!mInstance || !mSoloud)
		{
			return 0;
		}

		findBusHandle();

		if (mChannelHandle == 0)
		{
			return 0;
		}

		return mSoloud->playClocked(aSoundTime, aSound, aVolume, aPan, mChannelHandle);
	}	

	handle Bus::play3d(AudioSource &aSound, float aPosX, float aPosY, float aPosZ, float aVelX, float aVelY, float aVelZ, float aVolume, bool aPaused)
	{
		if (!mInstance || !mSoloud)
		{
			return 0;
		}

		findBusHandle();

		if (mChannelHandle == 0)
		{
			return 0;
		}
		return mSoloud->play3d(aSound, aPosX, aPosY, aPosZ, aVelX, aVelY, aVelZ, aVolume, aPaused, mChannelHandle);
	}

	handle Bus::play3dClocked(time aSoundTime, AudioSource &aSound, float aPosX, float aPosY, float aPosZ, float aVelX, float aVelY, float aVelZ, float aVolume)
	{
		if (!mInstance || !mSoloud)
		{
			return 0;
		}

		findBusHandle();

		if (mChannelHandle == 0)
		{
			return 0;
		}
		return mSoloud->play3dClocked(aSoundTime, aSound, aPosX, aPosY, aPosZ, aVelX, aVelY, aVelZ, aVolume, mChannelHandle);
	}

	void Bus::annexSound(handle aVoiceHandle)
	{
		findBusHandle();
		FOR_ALL_VOICES_PRE_EXT
			mSoloud->mVoice[ch]->mBusHandle = mChannelHandle;
		FOR_ALL_VOICES_POST_EXT
	}

	void Bus::setFilter(unsigned int aFilterId, Filter *aFilter)
	{
		if (aFilterId >= FILTERS_PER_STREAM)
			return;

		mFilter[aFilterId] = aFilter;

		if (mInstance)
		{
			mSoloud->lockAudioMutex_internal();
			delete mInstance->mFilter[aFilterId];
			mInstance->mFilter[aFilterId] = 0;
		
			if (aFilter)
			{
				mInstance->mFilter[aFilterId] = mFilter[aFilterId]->createInstance();
			}
			mSoloud->unlockAudioMutex_internal();
		}
	}

	result Bus::setChannels(unsigned int aChannels)
	{
		if (aChannels == 0 || aChannels == 3 || aChannels == 5 || aChannels == 7 || aChannels > MAX_CHANNELS)
			return INVALID_PARAMETER;
		mChannels = aChannels;
		return SO_NO_ERROR;
	}

	void Bus::setVisualizationEnable(bool aEnable)
	{
		if (aEnable)
		{
			mFlags |= AudioSource::VISUALIZATION_DATA;
		}
		else
		{
			mFlags &= ~AudioSource::VISUALIZATION_DATA;
		}
	}
		
	float * Bus::calcFFT()
	{
		if (mInstance && mSoloud)
		{
			mSoloud->lockAudioMutex_internal();
			float temp[1024];
			int i;
			for (i = 0; i < 256; i++)
			{
				temp[i*2] = mInstance->mVisualizationWaveData[i];
				temp[i*2+1] = 0;
				temp[i+512] = 0;
				temp[i+768] = 0;
			}
			mSoloud->unlockAudioMutex_internal();

			SoLoud::FFT::fft1024(temp);

			for (i = 0; i < 256; i++)
			{
				float real = temp[i * 2];
				float imag = temp[i * 2 + 1];
				mFFTData[i] = (float)sqrt(real*real+imag*imag);
			}
		}

		return mFFTData;
	}

	float * Bus::getWave()
	{
		if (mInstance && mSoloud)
		{
			int i;
			mSoloud->lockAudioMutex_internal();
			for (i = 0; i < 256; i++)
				mWaveData[i] = mInstance->mVisualizationWaveData[i];
			mSoloud->unlockAudioMutex_internal();
		}
		return mWaveData;
	}

	float Bus::getApproximateVolume(unsigned int aChannel)
	{
		if (aChannel > mChannels)
			return 0;
		float vol = 0;
		if (mInstance && mSoloud)
		{
			mSoloud->lockAudioMutex_internal();
			vol = mInstance->mVisualizationChannelVolume[aChannel];
			mSoloud->unlockAudioMutex_internal();
		}
		return vol;
	}

	unsigned int Bus::getActiveVoiceCount()
	{
		int i;
		unsigned int count = 0;
		findBusHandle();
		mSoloud->lockAudioMutex_internal();
		for (i = 0; i < VOICE_COUNT; i++)
			if (mSoloud->mVoice[i] && mSoloud->mVoice[i]->mBusHandle == mChannelHandle)
				count++;
		mSoloud->unlockAudioMutex_internal();
		return count;
	}

	unsigned int Bus::getResampler()
	{
		return mResampler;
	}

	void Bus::setResampler(unsigned int aResampler)
	{
		if (aResampler <= Soloud::RESAMPLER_CATMULLROM)
			mResampler = aResampler;
	}

};

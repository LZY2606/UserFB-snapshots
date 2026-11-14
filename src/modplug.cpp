/*
 * This source code is public domain.
 *
 * Authors: Kenton Varda <temporal@gauge3d.org> (C interface wrapper)
 */

#include "stdafx.h"
#include "modplug.h"
#include "sndfile.h"
#include "libmodplug/session.h"

struct _ModPlugFile
{
	CSoundFile mSoundFile;

	// Storage for an independent render session.  Legacy files leave this
	// unused and point "session" at the process-global legacy session.  The
	// settings handed to ModPlug_LoadWithSettings() are consumed once to fill
	// this session, which is the retained private copy.
	ModPlugSession mSessionStorage;
	ModPlugSession *session;

	_ModPlugFile()
		: mSoundFile()
		, mSessionStorage()
		, session(&ModPlug::gLegacySession)
	{
	}
};

namespace ModPlug
{
	ModPlug_Settings gSettings =
	{
		MODPLUG_ENABLE_OVERSAMPLING | MODPLUG_ENABLE_NOISE_REDUCTION,

		2, // mChannels
		16, // mBits
		44100, // mFrequency
		MODPLUG_RESAMPLE_LINEAR, //mResamplingMode

		128, // mStereoSeparation
		32, // mMaxMixChannels
		0,
		0,
		0,
		0,
		0,
		0,
		0
	};

	// Runs every mixer/DSP call with the owning session active.  Calls are
	// serialized by the API contract, so swapping the single active-session
	// pointer around a top-level call is enough; nothing is reinitialized on
	// the switch.
	struct SessionGuard
	{
		ModPlugSession * const previous;
		explicit SessionGuard(ModPlugSession *target)
			: previous(gActiveSession)
		{
			gActiveSession = target;
		}
		~SessionGuard()
		{
			gActiveSession = previous;
		}
	};

	// Applies the given settings to the currently active session; the body is
	// unchanged from the legacy implementation, every CSoundFile static it
	// touches now routes through the session macros.  The global gSettings
	// itself is never written here, so an explicit load cannot change the
	// process-global defaults.
	void UpdateSettings(const ModPlug_Settings &settings, bool updateBasicConfig)
	{
		if(settings.mFlags & MODPLUG_ENABLE_REVERB)
		{
			CSoundFile::SetReverbParameters(settings.mReverbDepth,
			                                settings.mReverbDelay);
		}

		if(settings.mFlags & MODPLUG_ENABLE_MEGABASS)
		{
			CSoundFile::SetXBassParameters(settings.mBassAmount,
			                               settings.mBassRange);
		}
		else // modplug seems to ignore the SetWaveConfigEx() setting for bass boost
			CSoundFile::SetXBassParameters(0, 0);

		if(settings.mFlags & MODPLUG_ENABLE_SURROUND)
		{
			CSoundFile::SetSurroundParameters(settings.mSurroundDepth,
			                                  settings.mSurroundDelay);
		}

		if(updateBasicConfig)
		{
			CSoundFile::SetWaveConfig(settings.mFrequency,
			                                                  settings.mBits,
			                          settings.mChannels);
			CSoundFile::SetMixConfig(settings.mStereoSeparation,
			                                                 settings.mMaxMixChannels);

			ActiveSession().gSampleSize_ = settings.mBits / 8 * settings.mChannels;
		}

		CSoundFile::SetWaveConfigEx(settings.mFlags & MODPLUG_ENABLE_SURROUND,
		                            !(settings.mFlags & MODPLUG_ENABLE_OVERSAMPLING),
		                            settings.mFlags & MODPLUG_ENABLE_REVERB,
		                            true,
		                            settings.mFlags & MODPLUG_ENABLE_MEGABASS,
		                            settings.mFlags & MODPLUG_ENABLE_NOISE_REDUCTION,
		                            false);
		CSoundFile::SetResamplingMode(settings.mResamplingMode);
	}

	// Loads a module into "file" with the given settings driving a private
	// session.  The settings struct is read once here; later changes to it or
	// to the global settings cannot reach the new handle.
	ModPlugFile *LoadWithSession(ModPlugFile *file, const void *data, int size,
	                             const ModPlug_Settings *settings)
	{
		InitSessionDefaults(&file->mSessionStorage);
		file->session = &file->mSessionStorage;

		SessionGuard guard(file->session);
		UpdateSettings(*settings, true);
		if(file->mSoundFile.Create((const BYTE*)data, size))
		{
			file->mSoundFile.SetRepeatCount(settings->mLoopCount);
			return file;
		}
		return NULL;
	}
}

ModPlugFile* ModPlug_Load(const void* data, int size)
{
	ModPlugFile* result = new ModPlugFile;
	// A legacy file shares the process-global session, so the guard is a
	// no-op and the original single-track behavior is preserved exactly.
	ModPlug::SessionGuard guard(result->session);
	ModPlug::UpdateSettings(ModPlug::gSettings, true);
	if(result->mSoundFile.Create((const BYTE*)data, size))
	{
		result->mSoundFile.SetRepeatCount(ModPlug::gSettings.mLoopCount);
		return result;
	}
	else
	{
		delete result;
		return NULL;
	}
}

ModPlugFile* ModPlug_LoadWithSettings(const void* data, int size,
                                      const ModPlug_Settings* settings)
{
	if(!settings) return NULL;
	ModPlugFile* result = new ModPlugFile;
	if(!ModPlug::LoadWithSession(result, data, size, settings))
	{
		delete result;
		return NULL;
	}
	return result;
}

void ModPlug_Unload(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	file->mSoundFile.Destroy();
	delete file;
}

int ModPlug_Read(ModPlugFile* file, void* buffer, int size)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.Read(buffer, size) * file->session->gSampleSize_;
}

const char* ModPlug_GetName(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetTitle();
}

int ModPlug_GetLength(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetSongTime() * 1000;
}

void ModPlug_InitMixerCallback(ModPlugFile* file,ModPlugMixerProc proc)
{
	ModPlug::SessionGuard guard(file->session);
	gpSndMixHook = (LPSNDMIXHOOKPROC)proc ;
	return;
}

void ModPlug_UnloadMixerCallback(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	gpSndMixHook = NULL;
	return ;
}

unsigned int ModPlug_GetMasterVolume(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return (unsigned int)file->mSoundFile.m_nMasterVolume;
}

void ModPlug_SetMasterVolume(ModPlugFile* file,unsigned int cvol)
{
	ModPlug::SessionGuard guard(file->session);
	(void)file->mSoundFile.SetMasterVolume( (UINT)cvol,
						FALSE );
	return ;
}

int ModPlug_GetCurrentSpeed(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.m_nMusicSpeed;
}

int ModPlug_GetCurrentTempo(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.m_nMusicTempo;
}

int ModPlug_GetCurrentOrder(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetCurrentOrder();
}

int ModPlug_GetCurrentPattern(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetCurrentPattern();
}

int ModPlug_GetCurrentRow(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.m_nRow;
}

int ModPlug_GetPlayingChannels(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	// m_nMaxMixChannels is session configuration, not a per-file member.
	return ( file->mSoundFile.m_nMixChannels < m_nMaxMixChannels ? file->mSoundFile.m_nMixChannels : m_nMaxMixChannels );
}

void ModPlug_SeekOrder(ModPlugFile* file,int order)
{
	ModPlug::SessionGuard guard(file->session);
	file->mSoundFile.SetCurrentOrder(order);
}

int ModPlug_GetModuleType(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.m_nType;
}

char* ModPlug_GetMessage(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.m_lpszSongComments;
}

#ifndef MODPLUG_NO_FILESAVE
char ModPlug_ExportS3M(ModPlugFile* file,const char* filepath)
{
	ModPlug::SessionGuard guard(file->session);
	return (char)file->mSoundFile.SaveS3M(filepath,0);
}

char ModPlug_ExportXM(ModPlugFile* file, const char* filepath)
{
	ModPlug::SessionGuard guard(file->session);
	return (char)file->mSoundFile.SaveXM(filepath,0);
}

char ModPlug_ExportMOD(ModPlugFile* file, const char* filepath)
{
	ModPlug::SessionGuard guard(file->session);
	return (char)file->mSoundFile.SaveMod(filepath,0);
}

char ModPlug_ExportIT(ModPlugFile* file, const char* filepath)
{
	ModPlug::SessionGuard guard(file->session);
	return (char)file->mSoundFile.SaveIT(filepath,0);
}
#endif // MODPLUG_NO_FILESAVE

unsigned int ModPlug_NumInstruments(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.m_nInstruments;
}

unsigned int ModPlug_NumSamples(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.m_nSamples;
}

unsigned int ModPlug_NumPatterns(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetNumPatterns();
}

unsigned int ModPlug_NumChannels(ModPlugFile* file)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetNumChannels();
}

unsigned int ModPlug_SampleName(ModPlugFile* file,unsigned int qual,char* buff)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetSampleName(qual,buff);
}

unsigned int ModPlug_InstrumentName(ModPlugFile* file,unsigned int qual,char* buff)
{
	ModPlug::SessionGuard guard(file->session);
	return file->mSoundFile.GetInstrumentName(qual,buff);
}

ModPlugNote* ModPlug_GetPattern(ModPlugFile* file,int pattern,unsigned int* numrows) {
	ModPlug::SessionGuard guard(file->session);
	if ( pattern<MAX_PATTERNS && pattern >= 0) {
		if (file->mSoundFile.Patterns[pattern]) {
			if (numrows) *numrows=(unsigned int)file->mSoundFile.PatternSize[pattern];
			return (ModPlugNote*)file->mSoundFile.Patterns[pattern];
		}
	}
	return NULL;
}

void ModPlug_Seek(ModPlugFile* file, int millisecond)
{
	ModPlug::SessionGuard guard(file->session);
	int maxpos;
	int maxtime = file->mSoundFile.GetSongTime() * 1000;
	float postime;

	if(millisecond > maxtime)
		millisecond = maxtime;
	maxpos = file->mSoundFile.GetMaxPosition();
	postime = 0.0f;
	if (maxtime != 0)
		postime = (float)maxpos / (float)maxtime;

	file->mSoundFile.SetCurrentPos((int)(millisecond * postime));
}

void ModPlug_GetSettings(ModPlug_Settings* settings)
{
	memcpy(settings, &ModPlug::gSettings, sizeof(ModPlug_Settings));
}

void ModPlug_SetSettings(const ModPlug_Settings* settings)
{
	memcpy(&ModPlug::gSettings, settings, sizeof(ModPlug_Settings));
	// Global settings only ever touch the legacy session; independent
	// sessions keep their private copies no matter what happens here.
	ModPlug::SessionGuard guard(&ModPlug::gLegacySession);
	ModPlug::UpdateSettings(ModPlug::gSettings, false); // do not update basic config.
}

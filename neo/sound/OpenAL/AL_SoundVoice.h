/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __AL_SOUNDVOICE_H__
#define __AL_SOUNDVOICE_H__

// Reset OpenAL extension/procedure caches when the hardware context changes.
void OpenAL_ResetContextCaches();

/*
================================================
idSoundVoice_OpenAL
================================================
*/
class idSoundVoice_OpenAL : public idSoundVoice_Base
{
public:
	idSoundVoice_OpenAL();
	~idSoundVoice_OpenAL();
	
	void					SetPosition( const idVec3& p )
	{
		idSoundVoice_Base::SetPosition( p );
		
		alGetError();
		alSource3f( openalSource, AL_POSITION, -p.y, p.z, -p.x );
		if (alGetError() != AL_NO_ERROR)
		{
			coreParameterUpdateFailed = true;
		}
	}
	
	void					SetGain( float gain )
	{
		// snd_emitter.cpp already clamps ordinary sounds to 1.0f, while
		// SSF_UNCLAMPED intentionally allows values above 1.0f.  Do not
		// impose another upper clamp here. OpenAL requires AL_GAIN >= 0.
		const float openalGain = Max(0.0f, gain);
		
		idSoundVoice_Base::SetGain(openalGain);
		alGetError();
		alSourcef(openalSource, AL_GAIN, openalGain);
		if (alGetError() != AL_NO_ERROR)
		{
			coreParameterUpdateFailed = true;
		}
	}
	
	void		SetPitch( float p )
	{
		// OpenAL requires AL_PITCH > 0. Preserve the lower-bound behavior of
		// the old XAudio2 backend (XAUDIO2_MIN_FREQ_RATIO = 1 / 1024) so a
		// zero, negative, or otherwise invalid slow-motion value cannot leave
		// AL_INVALID_VALUE sticky on the source while retaining an old pitch.
		const float minPitch = 1.0f / 1024.0f;
		const float openalPitch = (p > minPitch) ? p : minPitch;
		
		idSoundVoice_Base::SetPitch(openalPitch);
		alGetError();
		alSourcef(openalSource, AL_PITCH, openalPitch);
		if (alGetError() != AL_NO_ERROR)
		{
			coreParameterUpdateFailed = true;
		}
	}

	void					SetOcclusion(float f)
	{
		idSoundVoice_Base::SetOcclusion(idMath::ClampFloat(0.0f, 1.0f, f));
		ApplyOcclusionFilter();
	}

	void					SetInnerRadius(float r)
	{
		idSoundVoice_Base::SetInnerRadius(Max(0.0f, r));
		ApplySourceRadius();
	}
	
	bool					Create(const idSoundSample* leadinSample, const idSoundSample* loopingSample);
	
	// Start playing at a particular point in the buffer. Returns false if the
	// source could not be prepared or started.
	bool					Start(int offsetMS, int ssFlags);
	
	// Stop playing.
	void					Stop();
	
	// Stop consuming buffers
	void					Pause();
	
	// Start consuming buffers again
	void					UnPause();
	
	// Sends new position/volume/pitch information to the hardware
	bool					Update();
	
	// Returns the sample amplitude envelope when amplitude tracking is enabled.
	float					GetAmplitude();
	
	// returns true if we can re-use this voice
	bool					CompatibleFormat( idSoundSample_OpenAL* s );
	
	uint32					GetSampleRate() const
	{
		return sampleRate;
	}
	
private:
	friend class idSoundHardware_OpenAL;
	
	// Returns true when all the buffers are finished processing
	bool					IsPlaying();
	
	// Stop the source and detach its static buffer/queue. Returns false if
	// OpenAL rejected any part of the cleanup.
	bool					FlushSourceBuffers();
	
	// Destroy the internal hardware resource. Returns false when OpenAL
	// rejected deletion of an owned source in the current live context.
	bool					DestroyInternal();

	// Clear cached AL object names without issuing AL calls. Used only when
	// the owning context cannot be made current during hardware shutdown.
	void					InvalidateContextObjects();
	
	// EFX low-pass filter used to reproduce the XAudio2 occlusion/muffling path.
	bool					EnsureOcclusionFilter();
	void					ApplyOcclusionFilter();
	void					DestroyOcclusionFilter();

	// AL_EXT_SOURCE_RADIUS is used when available to reproduce the original
	// near-listener omni-to-directional blend for mono sources.
	void					ApplySourceRadius();

	// Helper function used by the initial start and lead-in/loop transitions.
	int						RestartAt(int64 offsetSamples);
	
	// Helper function to submit a buffer
	int						SubmitBuffer( idSoundSample_OpenAL* sample, int bufferNumber, int offset );
	
	ALuint					openalSource;
	ALuint					openalLowPassFilter;
	
	idSoundSample_OpenAL*	leadinSample;
	idSoundSample_OpenAL*	loopingSample;
	
	// Sample format state used by the OpenAL source.
	uint16					numChannels;

	uint32					sampleRate;
	
	bool					trackAmplitude;
	bool					paused;

	// SetPosition/SetGain/SetPitch are void at the shared sound interface.
	// Latch any rejected core source update until Start()/Update() can report
	// failure to idSoundChannel and retire the voice safely.
	bool					coreParameterUpdateFailed;
};

/*
================================================
idSoundVoice
================================================
*/
class idSoundVoice : public idSoundVoice_OpenAL
{
};

#endif

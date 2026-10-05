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

#include "../../framework/precompiled.h"
#pragma hdrstop

#include "../snd_local.h"
#include <AL/efx.h>

idCVar s_debugHardware( "s_debugHardware", "0", CVAR_BOOL, "Print a message any time a hardware voice changes" );

typedef LPALGENFILTERS			openalGenFilters_t;
typedef LPALDELETEFILTERS		openalDeleteFilters_t;
typedef LPALFILTERI				openalFilteri_t;
typedef LPALFILTERF				openalFilterf_t;

static openalGenFilters_t		qalGenFilters = NULL;
static openalDeleteFilters_t	qalDeleteFilters = NULL;
static openalFilteri_t			qalFilteri = NULL;
static openalFilterf_t			qalFilterf = NULL;

// The XAudio2 backend used a low-pass cutoff of 1000 / occlusion Hz.
// EFX exposes high-frequency gain instead of the same cutoff control, so use
// a smooth HF attenuation that gives a similar muffling effect without
// changing the source's overall gain.
static const float OPENAL_OCCLUSION_HF_ATTENUATION_DB = -16.0f;

// The bundled OpenAL headers predate AL_EXT_SOURCE_RADIUS. Query the enum at
// runtime instead of hard-coding a newer header dependency. Cache per context
// so a device/context restart can select a different OpenAL implementation.
static ALCcontext * openalSourceRadiusContext = NULL;
static ALenum openalSourceRadiusEnum = AL_NONE;

static ALenum OpenAL_GetSourceRadiusEnum()
{
	ALCcontext * context = alcGetCurrentContext();
	if( context == NULL )
	{
		openalSourceRadiusContext = NULL;
		openalSourceRadiusEnum = AL_NONE;
		return AL_NONE;
	}
	
	if( context == openalSourceRadiusContext )
	{
		return openalSourceRadiusEnum;
	}
	
	openalSourceRadiusContext = context;
	openalSourceRadiusEnum = AL_NONE;
	
	CheckALErrors();
	if( alIsExtensionPresent("AL_EXT_SOURCE_RADIUS") != AL_TRUE )
	{
		CheckALErrors();
		return AL_NONE;
	}
	
	const ALenum sourceRadius = alGetEnumValue( "AL_SOURCE_RADIUS" );
	if( CheckALErrors() == AL_NO_ERROR && sourceRadius != AL_NONE )
	{
		openalSourceRadiusEnum = sourceRadius;
	}
	
	return openalSourceRadiusEnum;
}

static bool OpenAL_LoadEfxFilterProcs()
{
	ALCcontext * context = alcGetCurrentContext();
	if (context == NULL)
	{
		return false;
	}
	
	ALCdevice * device = alcGetContextsDevice( context );
	if (device == NULL || alcIsExtensionPresent( device, ALC_EXT_EFX_NAME) != ALC_TRUE )
	{
		return false;
	}
	
	qalGenFilters = reinterpret_cast<openalGenFilters_t>( alGetProcAddress( "alGenFilters" ) );
	qalDeleteFilters = reinterpret_cast<openalDeleteFilters_t>( alGetProcAddress("alDeleteFilters" ) );
	qalFilteri = reinterpret_cast<openalFilteri_t>(alGetProcAddress( "alFilteri" ) );
	qalFilterf = reinterpret_cast<openalFilterf_t>(alGetProcAddress( "alFilterf" ) );
	
	return qalGenFilters != NULL &&
	qalDeleteFilters != NULL &&
	qalFilteri != NULL &&
	qalFilterf != NULL;
}


/*
========================
idSoundVoice_OpenAL::idSoundVoice_OpenAL
========================
*/
idSoundVoice_OpenAL::idSoundVoice_OpenAL()
	:
	openalSource(0),
	openalLowPassFilter(0),
	leadinSample(NULL),
	loopingSample(NULL),
	numChannels(0),
	sampleRate(0),
	trackAmplitude(false),
	paused(true)
{
}

/*
========================
idSoundVoice_OpenAL::~idSoundVoice_OpenAL
========================
*/
idSoundVoice_OpenAL::~idSoundVoice_OpenAL()
{
	DestroyInternal();
}

/*
========================
idSoundVoice_OpenAL::CompatibleFormat
========================
*/
bool idSoundVoice_OpenAL::CompatibleFormat( idSoundSample_OpenAL* s )
{
	if (s == NULL)
	{
		return false;
	}

	// OpenAL sources are not tied to a PCM format the way XAudio2 source
	// voices are. A stopped source can therefore be reused for any sample.
	return !IsPlaying();
}

/*
========================
idSoundVoice_OpenAL::Create
========================
*/
bool idSoundVoice_OpenAL::Create( const idSoundSample* leadinSample_, const idSoundSample* loopingSample_ )
{
	if( IsPlaying() )
	{
		// AllocateVoice() should only hand us a free voice. Be defensive in
		// case the bookkeeping ever gets out of sync.
		Stop();
		
		if( IsPlaying() )
		{
			return false;
		}
	}
	
	leadinSample = ( idSoundSample_OpenAL* )leadinSample_;
	loopingSample = ( idSoundSample_OpenAL* )loopingSample_;

	// PC PCM/ADPCM samples are decoded as needed and uploaded by
	// idSoundSample_OpenAL::CreateOpenALBuffer(). The old CPU-streaming
	// fallback was incomplete (one of three buffers was refilled/queued and
	// playback offsets were ignored), so reject a missing hardware buffer
	// instead of entering a path that cannot play the sample correctly.
	if( leadinSample == NULL ||
		leadinSample->openalBuffer == 0 ||
		(loopingSample != NULL && loopingSample->openalBuffer == 0) )
	{
		if( alIsSource(openalSource) )
		{
			FlushSourceBuffers();
		}
		
		idLib::Warning(
			"idSoundVoice_OpenAL::Create: sample has no OpenAL buffer: %s%s%s",
			leadinSample != NULL ? leadinSample->GetName() : "<null>",
			loopingSample != NULL ? " / " : "",
			loopingSample != NULL ? loopingSample->GetName() : "");
		
		leadinSample = NULL;
		loopingSample = NULL;
		return false;
	}
	
	if( alIsSource( openalSource ) && CompatibleFormat( leadinSample ) )
	{
		// A reused OpenAL source may still have an old static buffer or queue
		// attached. Clear it before configuring the new sample.
		FlushSourceBuffers();
	}
	else
	{
		DestroyInternal();
		CheckALErrors();
		
		openalSource = 0;

		alGenSources( 1, &openalSource );

		if( CheckALErrors() != AL_NO_ERROR || !alIsSource(openalSource) )
		{
			openalSource = 0;
			leadinSample = NULL;
			loopingSample = NULL;
			return false;
		}
		
		alSourcef( openalSource, AL_ROLLOFF_FACTOR, 0.0f );
		
		alSourcei(openalSource, AL_BUFFER, 0);
		
		if( s_debugHardware.GetBool() )
		{
			if( loopingSample == NULL || loopingSample == leadinSample )
			{
				idLib::Printf( "%dms: %i created for %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>" );
			}
			else
			{
				idLib::Printf( "%dms: %i created for %s and %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>", loopingSample ? loopingSample->GetName() : "<null>" );
			}
		}
	}
	
	// Keep these fields current even when an existing OpenAL source is reused.
	numChannels = leadinSample->format.basic.numChannels;
	sampleRate = leadinSample->format.basic.samplesPerSec;

	CheckALErrors();
	
	alSourcei( openalSource, AL_SOURCE_RELATIVE, AL_TRUE );
	alSource3f( openalSource, AL_POSITION, 0.0f, 0.0f, 0.0f );
	
	alSourcef( openalSource, AL_GAIN, 1.0f );
	alSourcei(openalSource, AL_LOOPING, AL_FALSE);

	// A source may be reused from a previously occluded sound. Start each
	// allocation with an unfiltered direct path; UpdateHardware() will call
	// SetOcclusion() before playback starts.
	idSoundVoice_Base::SetOcclusion(0.0f);
	ApplyOcclusionFilter();
	
	// Reset any source-radius state inherited from a reused OpenAL source.
	// UpdateHardware() supplies the shader's actual minDistance before Start().
	idSoundVoice_Base::SetInnerRadius(0.0f);
	ApplySourceRadius();

	if( CheckALErrors() != AL_NO_ERROR )
	{
		DestroyInternal();
		return false;
	}
	
	paused = true;
	return true;
}

/*
========================
idSoundVoice_OpenAL::DestroyInternal
========================
*/
void idSoundVoice_OpenAL::DestroyInternal()
{
	if (openalSource != 0 && alIsSource(openalSource))
	{
		if( s_debugHardware.GetBool() )
		{
			idLib::Printf( "%dms: %i destroyed\n", Sys_Milliseconds(), openalSource );
		}

		FlushSourceBuffers();
		
		alDeleteSources( 1, &openalSource );
	}

	openalSource = 0;
	trackAmplitude = false;

	DestroyOcclusionFilter();
}

/*
========================
idSoundVoice_OpenAL::InvalidateContextObjects
========================
*/
void idSoundVoice_OpenAL::InvalidateContextObjects()
{
	openalSource = 0;
	openalLowPassFilter = 0;
	leadinSample = NULL;
	loopingSample = NULL;
	numChannels = 0;
	sampleRate = 0;
	trackAmplitude = false;
	paused = true;
}

/*
========================
idSoundVoice_OpenAL::EnsureOcclusionFilter
========================
*/
bool idSoundVoice_OpenAL::EnsureOcclusionFilter()
 {
	if( !OpenAL_LoadEfxFilterProcs() )
	{
		return false;
	}
	
	if( openalLowPassFilter != 0 )
	{
		return true;
	}
	
	CheckALErrors();
	qalGenFilters( 1, &openalLowPassFilter );
	if( CheckALErrors() != AL_NO_ERROR || openalLowPassFilter == 0 )
	{
		openalLowPassFilter = 0;
		return false;
	}
	
	qalFilteri( openalLowPassFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS );
	qalFilterf( openalLowPassFilter, AL_LOWPASS_GAIN, 1.0f );
	qalFilterf( openalLowPassFilter, AL_LOWPASS_GAINHF, 1.0f );
	
	if( CheckALErrors() != AL_NO_ERROR )
	{
		qalDeleteFilters(1, &openalLowPassFilter);
		openalLowPassFilter = 0;
		CheckALErrors();
		return false;
	}
	
	return true;
}

/*
========================
idSoundVoice_OpenAL::ApplyOcclusionFilter
========================
*/
void idSoundVoice_OpenAL::ApplyOcclusionFilter()
{
	if( !alIsSource(openalSource) )
	{
		return;
	}
	
	const float amount = idMath::ClampFloat( 0.0f, 1.0f, occlusion );
	
	if( amount <= 0.0f )
	{
		if( openalLowPassFilter != 0 && OpenAL_LoadEfxFilterProcs() )
		{
			CheckALErrors();
			alSourcei( openalSource, AL_DIRECT_FILTER, AL_FILTER_NULL );
			CheckALErrors();
		}
		return;
	}
	
	if( !EnsureOcclusionFilter() )
	{
		// EFX is optional. If the current OpenAL device does not expose it,
		// leave the source unfiltered rather than failing voice playback.
		return;
	}
	
	const float gainHF = idMath::ClampFloat(
		0.0f,
		1.0f,
		DBtoLinear( OPENAL_OCCLUSION_HF_ATTENUATION_DB * amount ) );
	
	CheckALErrors();
	qalFilterf( openalLowPassFilter, AL_LOWPASS_GAIN, 1.0f );
	qalFilterf( openalLowPassFilter, AL_LOWPASS_GAINHF, gainHF );
	alSourcei( openalSource, AL_DIRECT_FILTER, openalLowPassFilter );
	CheckALErrors();
	}

/*
========================
idSoundVoice_OpenAL::DestroyOcclusionFilter
========================
*/
void idSoundVoice_OpenAL::DestroyOcclusionFilter()
{
	if( openalLowPassFilter == 0 )
	{
		return;
	}
	
	if( OpenAL_LoadEfxFilterProcs() )
	{
		CheckALErrors();
		qalDeleteFilters( 1, &openalLowPassFilter );
		CheckALErrors();
	}
	
	openalLowPassFilter = 0;
}

/*
========================
idSoundVoice_OpenAL::ApplySourceRadius
========================
*/
void idSoundVoice_OpenAL::ApplySourceRadius()
{
	if( !alIsSource( openalSource ) )
	{
		return;
	}
	
	const ALenum sourceRadiusEnum = OpenAL_GetSourceRadiusEnum();
	if( sourceRadiusEnum == AL_NONE )
	{
		// AL_EXT_SOURCE_RADIUS is optional. Older OpenAL implementations keep
		// the existing point-source spatialization behavior.
		return;
	}
	
	// The original BFG surround matrix applied innerRadius blending only to
	// mono sources. Stereo samples were routed as stereo rather than treated
	// as a positionable mono point source, so do not give them a source radius.
	const float radius = ( numChannels == 1 ) ? Max( 0.0f, innerRadius ) : 0.0f;
	
	CheckALErrors();
	alSourcef( openalSource, sourceRadiusEnum, radius );
	CheckALErrors();
}

/*
========================
idSoundVoice_OpenAL::Start
========================
*/
void idSoundVoice_OpenAL::Start( int offsetMS, int ssFlags )
{
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %i starting %s @ %dms\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>", offsetMS );
	}
	
	if( !leadinSample )
	{
		return;
	}
	
	if( !alIsSource( openalSource ) )
	{
		return;
	}
	
	if( leadinSample->IsDefault() )
	{
		idLib::Warning( "Starting defaulted sound sample %s", leadinSample->GetName() );
	}
	
	trackAmplitude = ( ssFlags & SSF_NO_FLICKER ) == 0;
	
	assert( offsetMS >= 0 );
	int offsetSamples = MsecToSamples( offsetMS, leadinSample->SampleRate() );
	if( loopingSample == NULL && offsetSamples >= leadinSample->playLength )
	{
		return;
	}

	if( RestartAt( offsetSamples ) <= 0 )
	{
		return;
	}

	Update();
	UnPause();
}

/*
========================
idSoundVoice_OpenAL::RestartAt
========================
*/
int idSoundVoice_OpenAL::RestartAt( int offsetSamples )
{
	offsetSamples = Max( 0, offsetSamples );
	
	if( leadinSample == NULL || leadinSample->playLength <= 0 )
	{
		return 0;
	}
	
	idSoundSample_OpenAL* sample = leadinSample;
	if( offsetSamples >= leadinSample->playLength )
	{
		if( loopingSample == NULL || loopingSample->playLength <= 0 )
		{
			return 0;
		}
		
		if( loopingSample == leadinSample )
		{
			offsetSamples %= loopingSample->playLength;
		}
		else if( loopingSample->SampleRate() == leadinSample->SampleRate() )
		{
			// offsetSamples is measured from the start of the lead-in.
			offsetSamples = ( offsetSamples - leadinSample->playLength ) % loopingSample->playLength;
		}
		else
		{
			// Convert through time when the lead-in and loop use different
			// sample rates.
			const int elapsedMS = SamplesToMsec( offsetSamples, leadinSample->SampleRate() );
			const int loopElapsedMS = Max( 0, elapsedMS - leadinSample->LengthInMsec() );
			offsetSamples = MsecToSamples( loopElapsedMS, loopingSample->SampleRate() );
			offsetSamples %= loopingSample->playLength;
		}

		sample = loopingSample;
	}
	
	// A distinct lead-in followed by a loop is best represented as a two-buffer
	// OpenAL queue.  Start with looping disabled; Update() removes the processed
	// lead-in and enables AL_LOOPING once only the loop buffer remains.
	const bool queueLeadinAndLoop =
		sample == leadinSample &&
		loopingSample != NULL &&
		loopingSample != leadinSample &&
		leadinSample->openalBuffer != 0 &&
		loopingSample->openalBuffer != 0 &&
		leadinSample->GetOpenALBufferFormat() == loopingSample->GetOpenALBufferFormat() &&
		leadinSample->SampleRate() == loopingSample->SampleRate();

	if( queueLeadinAndLoop )
	{
		FlushSourceBuffers();
	
		ALuint queuedBuffers[2] =
		{
			leadinSample->openalBuffer,
			loopingSample->openalBuffer
		};

		CheckALErrors();
		alSourcei( openalSource, AL_LOOPING, AL_FALSE );
		alSourceQueueBuffers( openalSource, 2, queuedBuffers );
	
		// For a queued source AL_SAMPLE_OFFSET is relative to the beginning
		// of the complete queue, so an offset inside the lead-in can be
		// applied directly here.
		const int queueOffset = leadinSample->playBegin + offsetSamples;
		if( queueOffset > 0 )
		{
			alSourcei( openalSource, AL_SAMPLE_OFFSET, queueOffset );
		}
	
		if( CheckALErrors() != AL_NO_ERROR )
		{
			FlushSourceBuffers();
			return 0;
		}
	
		return Max( 1, leadinSample->totalBufferSize );
	}

	int previousNumSamples = 0;

	for( int i = 0; i < sample->buffers.Num(); i++ )
	{
		if( sample->buffers[i].numSamples > sample->playBegin + offsetSamples )
		{
			return SubmitBuffer( sample, i, sample->playBegin + offsetSamples - previousNumSamples );
		}
		previousNumSamples = sample->buffers[i].numSamples;
	}
	
	return 0;
}

/*
========================
idSoundVoice_OpenAL::SubmitBuffer
========================
*/
int idSoundVoice_OpenAL::SubmitBuffer( idSoundSample_OpenAL* sample, int bufferNumber, int offset )
{
	if( sample == NULL || ( bufferNumber < 0 ) || ( bufferNumber >= sample->buffers.Num() ) )
	{
		return 0;
	}

	if( sample->openalBuffer == 0 )
	{
		return 0;
	}

	// OpenAL keeps the first error until alGetError() consumes it.  Clear and
	// report any error left by an earlier operation so the result below only
	// reflects this buffer submission.
	CheckALErrors();
	
	alSourcei( openalSource, AL_BUFFER, sample->openalBuffer );
	alSourcei( openalSource, AL_LOOPING, (sample == loopingSample && loopingSample != NULL ? AL_TRUE : AL_FALSE) );
	
	if( offset > 0 )
	{
		alSourcei( openalSource, AL_SAMPLE_OFFSET, offset );
	}
	
	if( CheckALErrors() != AL_NO_ERROR )
	{
		return 0;
	}
	
	return sample->totalBufferSize;
}

/*
========================
idSoundVoice_OpenAL::Update
========================
*/
bool idSoundVoice_OpenAL::Update()
{
	if( !alIsSource( openalSource ) || leadinSample == NULL )
	{
		return false;
	}

	ALint state = AL_INITIAL;
	ALint sourceType = AL_UNDETERMINED;

	// Do not let a sticky error from an unrelated source operation make a
	// valid voice look dead.
	CheckALErrors();
	
	alGetSourcei( openalSource, AL_SOURCE_STATE, &state );
	alGetSourcei( openalSource, AL_SOURCE_TYPE, &sourceType );
	if( CheckALErrors() != AL_NO_ERROR )
	{
		return false;
	}

	if( loopingSample != NULL && loopingSample != leadinSample )
	{
		const bool compatibleQueuedPair =
		leadinSample->openalBuffer != 0 &&
		loopingSample->openalBuffer != 0 &&
		leadinSample->GetOpenALBufferFormat() == loopingSample->GetOpenALBufferFormat() &&
		leadinSample->SampleRate() == loopingSample->SampleRate();
		
		if( compatibleQueuedPair && sourceType == AL_STREAMING )
		{
			ALint processedBuffers = 0;
			alGetSourcei( openalSource, AL_BUFFERS_PROCESSED, &processedBuffers );
			if( CheckALErrors() != AL_NO_ERROR )
			{
				return false;
			}
			
			if( processedBuffers > 0 )
			{
				ALuint processedBuffer = 0;
				alSourceUnqueueBuffers(openalSource, 1, &processedBuffer);
				if( CheckALErrors() != AL_NO_ERROR )
				{
					return false;
				}
				
				if( processedBuffer == leadinSample->openalBuffer )
				{
					// The queue now contains only the loop buffer.  Enabling
					// source looping here repeats only that remaining buffer,
					// not the lead-in.
					alSourcei(openalSource, AL_LOOPING, AL_TRUE);
					
					alGetSourcei(openalSource, AL_SOURCE_STATE, &state);
					if( CheckALErrors() != AL_NO_ERROR )
					{
						return false;
					}
					
					// If both buffers finished between engine updates, the
					// source is stopped but the loop buffer is still queued.
					// Restart it; it will now loop indefinitely.
					if( !paused && state != AL_PLAYING )
					{
						alSourcePlay(openalSource);
						if( CheckALErrors() != AL_NO_ERROR )
						{
							return false;
						}
					}
				}
			}
		}
		else if( sourceType == AL_STATIC && state == AL_STOPPED && !paused )
		{
			// Different buffer attributes cannot share an OpenAL queue.
			// Fall back to switching to the loop after the lead-in stops.
			ALint currentBuffer = 0;
			alGetSourcei(openalSource, AL_BUFFER, &currentBuffer);
			if( CheckALErrors() != AL_NO_ERROR )
			{
				return false;
			}
			
			if( (ALuint)currentBuffer == leadinSample->openalBuffer )
			{
				if( SubmitBuffer( loopingSample, 0, loopingSample->playBegin ) <= 0 )
				{
					return false;
				}
				
				alSourcePlay( openalSource );
				paused = false;
				return CheckALErrors() == AL_NO_ERROR;
			}
		}
	}
	
		return true;
}

/*
========================
idSoundVoice_OpenAL::IsPlaying
========================
*/
bool idSoundVoice_OpenAL::IsPlaying()
{
	if( !alIsSource( openalSource ) )
	{
		return false;
	}
	
	ALint state = AL_INITIAL;

	// Isolate this state query from any error left by a previous AL call.
	CheckALErrors();
	
	alGetSourcei( openalSource, AL_SOURCE_STATE, &state );
	
	if( CheckALErrors() != AL_NO_ERROR )
	{
		return false;
	}
	
	return ( state == AL_PLAYING || state == AL_PAUSED );
}

/*
========================
idSoundVoice_OpenAL::FlushSourceBuffers
========================
*/
void idSoundVoice_OpenAL::FlushSourceBuffers()
{
	if( !alIsSource( openalSource ) )
	{
		return;
	}

	// AL_BUFFER = AL_NONE is legal on a stopped/initial source and releases
	// the complete source queue, including a streaming queue.  This is both
	// simpler and safer than manually unqueueing every processed buffer.
	CheckALErrors();
	alSourceStop( openalSource );
	alSourcei( openalSource, AL_BUFFER, 0 );
	alSourcei( openalSource, AL_LOOPING, AL_FALSE );

	CheckALErrors();

	paused = true;
}

/*
========================
idSoundVoice_OpenAL::Pause
========================
*/
void idSoundVoice_OpenAL::Pause()
{
	if (!alIsSource(openalSource) || paused)
	{
		return;
	}

	if (s_debugHardware.GetBool())
	{
		idLib::Printf("%dms: %i pausing %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>");
	}

	CheckALErrors();
	alSourcePause(openalSource);

	if (CheckALErrors() == AL_NO_ERROR)
	{
		paused = true;
	}
}
/*
========================
idSoundVoice_OpenAL::UnPause
========================
*/
void idSoundVoice_OpenAL::UnPause()
{
	if( !alIsSource( openalSource ) || !paused )
	{
		return;
	}
	
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %i unpausing %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>" );
	}
	
	CheckALErrors();
	alSourcePlay( openalSource );

	if (CheckALErrors() == AL_NO_ERROR)
	{
		paused = false;
	}
}

/*
========================
idSoundVoice_OpenAL::Stop
========================
*/
void idSoundVoice_OpenAL::Stop()
{
	if( !alIsSource( openalSource ) )
	{
		return;
	}
	
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %i stopping %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>" );
	}

	// Flush even when our bookkeeping already says "paused".  A paused source
	// may still have a static buffer or a lead-in/loop queue attached.
	FlushSourceBuffers();
}

/*
========================
idSoundVoice_OpenAL::GetAmplitude
========================
*/
float idSoundVoice_OpenAL::GetAmplitude()
{
	if( !trackAmplitude )
	{
		return 1.0f;
	}
	
	if( !alIsSource( openalSource ) || leadinSample == NULL )
	{
		return 0.0f;
	}
	
	ALint state = AL_INITIAL;
	ALint sourceType = AL_UNDETERMINED;
	ALint sampleOffset = 0;

	// Keep an unrelated sticky error from suppressing a valid amplitude query.
	CheckALErrors();
	
	alGetSourcei( openalSource, AL_SOURCE_STATE, &state );
	alGetSourcei( openalSource, AL_SOURCE_TYPE, &sourceType );
	alGetSourcei( openalSource, AL_SAMPLE_OFFSET, &sampleOffset );
	
	if( CheckALErrors() != AL_NO_ERROR )
	{
		return 0.0f;
	}
	
	// A stopped or paused voice is not currently producing audible output.
	if( state != AL_PLAYING )
	{
		return 0.0f;
	}
	
	idSoundSample_OpenAL* currentSample = leadinSample;
	int currentSampleOffset = Max( 0, sampleOffset );
	
	if( sourceType == AL_STATIC )
	{
		ALint currentBuffer = 0;
		alGetSourcei( openalSource, AL_BUFFER, &currentBuffer );
		if( CheckALErrors() != AL_NO_ERROR )
		{
			return 0.0f;
		}

		if( loopingSample != NULL &&
			loopingSample != leadinSample &&
			(ALuint)currentBuffer == loopingSample->openalBuffer )
		{
			currentSample = loopingSample;
		}
	}
	else if( sourceType == AL_STREAMING )
	{
		// The static lead-in + static loop implementation uses a two-buffer
		// OpenAL queue. AL_SAMPLE_OFFSET is relative to the beginning of the
		// currently queued buffers, so use the queue size to determine whether
		// the lead-in has already been removed by Update().
		const bool queuedLeadinAndLoop =
		loopingSample != NULL &&
		loopingSample != leadinSample &&
		leadinSample->openalBuffer != 0 &&
		loopingSample->openalBuffer != 0 &&
		leadinSample->GetOpenALBufferFormat() == loopingSample->GetOpenALBufferFormat() &&
		leadinSample->SampleRate() == loopingSample->SampleRate();
		
		if( !queuedLeadinAndLoop )
		 {
			// The legacy CPU streaming fallback does not currently retain
			// enough source-sample position state for an accurate envelope
			// lookup. Preserve the old non-flickering fallback rather than
			// reporting a false zero amplitude.
			return 1.0f;
		}
		
		ALint queuedBuffers = 0;
		alGetSourcei( openalSource, AL_BUFFERS_QUEUED, &queuedBuffers );
		if( CheckALErrors() != AL_NO_ERROR || queuedBuffers <= 0 )
		{
			return 0.0f;
		}
		
		if( queuedBuffers == 1 )
		{
			// Update() has removed the processed lead-in, leaving only the
			// looping buffer in the queue.
			currentSample = loopingSample;
		}
		else if( leadinSample->buffers.Num() > 0 )
		{
			const int leadinBufferSamples =
			leadinSample->buffers[leadinSample->buffers.Num() - 1].numSamples;
			
			if( currentSampleOffset >= leadinBufferSamples )
			{
				currentSampleOffset -= leadinBufferSamples;
				currentSample = loopingSample;
			}
		}
	}
	else
	{
		return 0.0f;
	}
	
	if( currentSample == NULL )
	{
		return 0.0f;
	}
	
	// Generated samples normally carry a precomputed 60 Hz amplitude envelope.
	// Raw WAVs may not have a matching .amp file; in that case retaining 1.0f
	// is preferable to making an audible sound appear silent to the game.
	if( currentSample->IsDefault() || currentSample->amplitude.Num() == 0 )
	{
		return 1.0f;
	}
	
	const int relativeSample =
	Max( 0, currentSampleOffset - currentSample->playBegin );
	const int timeMS =
	SamplesToMsec( relativeSample, currentSample->SampleRate() );
	
	return currentSample->GetAmplitude( timeMS );
}
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Felix Rueegg

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

#include "Precompiled.h"
#include "globaldata.h"

//
// DESCRIPTION:
//	System interface for sound.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <sys/types.h>
#include <fcntl.h>
// Timer stuff. Experimental.
#include <time.h>
#include <signal.h>
#include "z_zone.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "d_main.h"
#include "doomdef.h"
#include "../timidity/timidity.h"
#include "../timidity/controls.h"

#include "sound/snd_local.h"
#include "sound/sound.h"

#pragma warning ( disable : 4244 )

#define SFX_RATE		11050
#define SFX_SAMPLETYPE		AL_FORMAT_MONO8

#define MIDI_CHANNELS		2
#define MIDI_RATE		22050
#define MIDI_SAMPLETYPE		AL_FORMAT_STEREO8
#define MIDI_FORMAT		AUDIO_U8
#define MIDI_FORMAT_BYTES	1

ALuint		alMusicSourceVoice;
ALuint		alMusicBuffer;

MidiSong* doomMusic;
byte* musicBuffer;
int		totalBufferSize;

bool		waitingForMusic;
bool		musicReady;

typedef struct {
	float x;
	float y;
	float z;
} vec3_t;

typedef struct {
	vec3_t OrientTop;
	vec3_t OrientFront;
	vec3_t Position;
} doomListener_t;

typedef struct tagActiveSound_t {
	ALuint alSourceVoice;
	int id;
	int valid;
	int start;
	int player;
	bool localSound;
	mobj_t* originator;
} activeSound_t;

// cheap little struct to hold a sound
typedef struct {
	int vol;
	int player;
	int pitch;
	int priority;
	mobj_t* originator;
	mobj_t* listener;
} soundEvent_t;

// array of all the possible sounds
// in split screen we only process the loudest sound of each type per frame
soundEvent_t soundEvents[128];
extern int PLAYERCOUNT;

// Source voice settings for all sound effects
const ALfloat		SFX_MAX_DISTANCE = 1200.f;
const ALfloat		SFX_REFERENCE_DISTANCE = 100.f;
const ALfloat		SFX_ROLLOFF_FACTOR = 0.2f;

// Real volumes
const float		GLOBAL_VOLUME_MULTIPLIER = 0.5f;

float			x_SoundVolume = GLOBAL_VOLUME_MULTIPLIER;
float			x_MusicVolume = GLOBAL_VOLUME_MULTIPLIER;

// The actual lengths of all sound effects.
static int 		lengths[NUMSFX];
ALuint			alBuffers[NUMSFX];
activeSound_t		activeSounds[NUM_SOUNDBUFFERS] = { 0 };

int			S_initialized = 0;
bool			Music_initialized = false;
static bool		soundHardwareInitialized = false;
static int		numOutputChannels = 0;

doomListener_t		doom_Listener;

void			I_InitSoundChannel(int channel, int numOutputChannels_);

/*
======================
getsfx
======================
*/
// This function loads the sound data from the WAD lump,
// for single sound.
//
void* getsfx(const char* sfxname, int* len)
{
	if (len == NULL) {
		return NULL;
	}
	*len = 0;
	
	if (sfxname == NULL || sfxname[0] == '\0') {
		printf("[doomclassic] invalid empty SFX name\n");
		return NULL;
	}

	unsigned char* sfx;
	unsigned char* sfxmem;
	char name[20];
	int sfxlump;
	//float scale = 1.0f;

	// Get the sound data from the WAD. Doom SFX names are short, but keep
	// this bounded so malformed metadata cannot overflow the stack buffer.
	const int nameLength = idStr::snPrintf(name, static_cast<int>(sizeof(name)), "ds%s", sfxname);
	if (nameLength < 0 || nameLength >= (int)sizeof(name)) {
		printf("[doomclassic] SFX name is too long: '%s'\n", sfxname);
		return NULL;
	}

	// Scale down the plasma gun, it clips
	//if ( strcmp( sfxname, "plasma" ) == 0 ) {
	//	scale = 0.75f;
	//}
	//if ( strcmp( sfxname, "itemup" ) == 0 ) {
	//	scale = 1.333f;
	//}

	// If sound requested is not found in current WAD, use pistol as default
	if (W_CheckNumForName(name) == -1)
		sfxlump = W_GetNumForName("dspistol");
	else
		sfxlump = W_GetNumForName(name);

	// Sound lump headers are 8 bytes.
	const int SOUND_LUMP_HEADER_SIZE_IN_BYTES = 8;

	const int lumpLength = W_LumpLength(sfxlump);
	if (lumpLength <= SOUND_LUMP_HEADER_SIZE_IN_BYTES) {
		printf("[doomclassic] invalid SFX lump '%s' length: %d\n", name, lumpLength);
		return NULL;
	}

	const int size = lumpLength - SOUND_LUMP_HEADER_SIZE_IN_BYTES;

	sfx = (unsigned char*)W_CacheLumpNum(sfxlump, PU_CACHE_SHARED);
	if (sfx == NULL) {
		printf("[doomclassic] failed to cache SFX lump '%s'\n", name);
		return NULL;
	}

	const unsigned char* sfxSampleStart = sfx + SOUND_LUMP_HEADER_SIZE_IN_BYTES;

	// Allocate the persistent copy used by Classic Doom sound playback.
	sfxmem = (unsigned char*)malloc((size_t)size);
	if (sfxmem == NULL) {
		printf("[doomclassic] failed to allocate %d bytes for SFX '%s'\n", size, name);
		Z_Free(sfx);
		return NULL;
	}

	memcpy(sfxmem, sfxSampleStart, (size_t)size);

	// Remove the cached lump.
	Z_Free(sfx);

	// Set length only after the complete copy succeeds.
	*len = size;

	return (void*)(sfxmem);
}

/*
======================
I_SetChannels
======================
*/
void I_SetChannels()
{
	// Original Doom set up lookup tables here
}

/*
======================
I_SetSfxVolume
======================
*/
void I_SetSfxVolume(int volume)
{
	x_SoundVolume = ((float)volume / 15.f) * GLOBAL_VOLUME_MULTIPLIER;
}

/*
======================
I_GetSfxLumpNum
======================
*/
//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
	char namebuf[9];
	sprintf(namebuf, "ds%s", sfx->name);
	return W_GetNumForName(namebuf);
}

/*
======================
I_StartSound2
======================
*/
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback) is set
//
int I_StartSound2(int id, int player, mobj_t* origin, mobj_t* listener_origin, int pitch, int priority)
{
	if (!soundHardwareInitialized || id <= 0 || id >= NUMSFX || alBuffers[id] == 0) {
		return id;
	}

	int i;
	activeSound_t* sound = 0;
	int oldest = 0, oldestnum = -1;

	// these id's should not overlap
	if (id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful || id == sfx_sawhit || id == sfx_stnmov) {
		// Loop all channels, check.
		for (i = 0; i < NUM_SOUNDBUFFERS; i++)
		{
			sound = &activeSounds[i];

			if (sound->valid && (sound->id == id && sound->player == player)) {
				I_StopSound(sound->id, player);
				break;
			}
		}
	}

	// find a valid channel, or one that has finished playing
	for (i = 0; i < NUM_SOUNDBUFFERS; i++) {
		sound = &activeSounds[i];

		// A source can be unavailable if OpenAL source creation failed during
		// hardware initialization. Never issue AL calls against source 0.
		if (sound->alSourceVoice == 0)
			continue;

		if (!sound->valid)
			break;

		if (!oldest || oldest > sound->start) {
			oldestnum = i;
			oldest = sound->start;
		}

		ALint sourceState = AL_INITIAL;
		alGetError();
		alGetSourcei(sound->alSourceVoice, AL_SOURCE_STATE, &sourceState);
		const ALenum stateError = alGetError();
		if (stateError != AL_NO_ERROR) {
			// Do not reuse a source whose state cannot be queried. Leaving the
			// channel valid prevents us from issuing additional setup calls to a
			// source that OpenAL has already rejected.
			continue;
		}
		if (sourceState == AL_STOPPED) {
			break;
		}
	}

	// none found, so use the oldest one
	if (i == NUM_SOUNDBUFFERS) {
		if (oldestnum < 0) {
			return id;
		}

		i = oldestnum;
		sound = &activeSounds[i];
	}

	// Isolate this start sequence from any sticky error left by an earlier
	// OpenAL operation. CPU-side channel state is committed only after the
	// complete source setup and alSourcePlay() succeed.
	alGetError();
	alSourceStop(sound->alSourceVoice);

	// Attach the source voice to the correct buffer
	if (sound->id != id) {
		alSourcei(sound->alSourceVoice, AL_BUFFER, 0);
		alSourcei(sound->alSourceVoice, AL_BUFFER, alBuffers[id]);
	}

	// Set the source voice volume
	alSourcef(sound->alSourceVoice, AL_GAIN, x_SoundVolume);

	// Set the source voice pitch
	float sourcePitch = 1.0f + ((float)pitch - 128.0f) / 95.0f;
	if (sourcePitch <= 0.0f) {
		sourcePitch = 0.01f;
	}
	alSourcef(sound->alSourceVoice, AL_PITCH, sourcePitch);

	// Set the source voice position
	ALfloat x = 0.f;
	ALfloat y = 0.f;
	ALfloat z = 0.f;
	bool localSound = false;
	if (origin) {
		if (origin == listener_origin) {
			localSound = true;
		}
		else {
			x = (ALfloat)(origin->x >> FRACBITS);
			z = (ALfloat)(origin->y >> FRACBITS);
		}
	}
	else {
		localSound = true;
	}
	if (localSound) {
		x = doom_Listener.Position.x;
		z = doom_Listener.Position.z;
	}
	alSource3f(sound->alSourceVoice, AL_POSITION, x, y, z);

	alSourcePlay(sound->alSourceVoice);

	const ALenum startError = alGetError();
	if (startError != AL_NO_ERROR) {
		printf("[doomclassic] failed to start SFX %d: 0x%X\n", id, startError);
		
		// The old sound was already stopped, so make the CPU-side slot free as
		// well. Detach the buffer to ensure the next use performs a fresh bind.
		alSourceStop(sound->alSourceVoice);
		alSourcei(sound->alSourceVoice, AL_BUFFER, 0);
		alGetError();
		
		sound->id = 0;
		sound->start = 0;
		sound->valid = 0;
		sound->player = -1;
		sound->localSound = false;
		sound->originator = NULL;
		return id;
	}
	
	// Publish the channel only after OpenAL accepted the complete start.
	sound->id = id;
	sound->start = ::g->gametic;
	sound->valid = 1;
	sound->player = player;
	sound->localSound = localSound;
	sound->originator = origin;

	return id;
}

/*
======================
I_ProcessSoundEvents
======================
*/
void I_ProcessSoundEvents(void)
{
	for (int i = 0; i < 128; i++) {
		if (soundEvents[i].pitch) {
			I_StartSound2(i, soundEvents[i].player, soundEvents[i].originator, soundEvents[i].listener,
				soundEvents[i].pitch, soundEvents[i].priority);
		}
	}
	memset(soundEvents, 0, sizeof(soundEvents));
}

/*
======================
I_StartSound
======================
*/
int I_StartSound(int id, mobj_t* origin, mobj_t* listener_origin, int vol, int pitch, int priority)
{

	// I_StartSound2() also validates the id, but split-screen event coalescing
	// indexes soundEvents[id] before reaching that function.
	if (id <= 0 || id >= NUMSFX) {
		return 0;
	}

	// only allow player 0s sounds in intermission and finale screens
	if (::g->gamestate != GS_LEVEL && DoomLib::GetPlayer() != 0) {
		return 0;
	}

	// if we're only one player or we're trying to play the chainsaw sound, do it normal
	// otherwise only allow one sound of each type per frame
	if (PLAYERCOUNT == 1 || id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful || id == sfx_sawhit) {
		return I_StartSound2(id, ::g->consoleplayer, origin, listener_origin, pitch, priority);
	}
	else {
		if (soundEvents[id].vol < vol) {
			soundEvents[id].player = DoomLib::GetPlayer();
			soundEvents[id].pitch = pitch;
			soundEvents[id].priority = priority;
			soundEvents[id].vol = vol;
			soundEvents[id].originator = origin;
			soundEvents[id].listener = listener_origin;
		}
		return id;
	}
}

/*
======================
I_StopSound
======================
*/
void I_StopSound(int handle, int player)
{
	// You need the handle returned by StartSound.
	// Would be looping all channels,
	// tracking down the handle,
	// and setting the channel to zero.
	int i;
	activeSound_t* sound = 0;

	for (i = 0; i < NUM_SOUNDBUFFERS; ++i) {
		sound = &activeSounds[i];
		if (!sound->valid || sound->id != handle || (player >= 0 && sound->player != player))
			continue;
		break;
	}

	if (i == NUM_SOUNDBUFFERS)
		return;

	// Stop the sound. CPU-side state is cleared regardless of whether the
	// OpenAL stop succeeds so a stale source cannot remain logically active.
	if (sound->alSourceVoice != 0) {
		alGetError();
		alSourceStop(sound->alSourceVoice);
		alGetError();
	}

	sound->id = 0;
	sound->valid = 0;
	sound->start = 0;
	sound->player = -1;
	sound->localSound = false;
	sound->originator = NULL;
}

/*
======================
I_SoundIsPlaying
======================
*/
int I_SoundIsPlaying(int handle)
{
	if (!soundHardwareInitialized) {
		return 0;
	}

	int i;
	activeSound_t* sound;

	for (i = 0; i < NUM_SOUNDBUFFERS; ++i) {
		sound = &activeSounds[i];
		if (!sound->valid || sound->id != handle)
			continue;

		if(sound->alSourceVoice == 0) {
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
			continue;
		}
		
		ALint sourceState = AL_STOPPED;
		alGetError();
		alGetSourcei(sound->alSourceVoice, AL_SOURCE_STATE, &sourceState);
		const ALenum stateError = alGetError();
		if (stateError != AL_NO_ERROR) {
			// A source whose state cannot be queried is no longer trustworthy.
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
			continue;
		}

		if (sourceState == AL_PLAYING) {
			return 1;
		}

		if (sourceState == AL_STOPPED || sourceState == AL_INITIAL) {
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
		}
	}

	return 0;
}

/*
======================
I_UpdateSound
======================
*/
// Update listener position and go through all the
// channels and update sound positions.
void I_UpdateSound(void)
{
	if (!soundHardwareInitialized) {
		return;
	}

	// Update listener orientation and position
	mobj_t* playerObj = ::g->players[0].mo;
	if (playerObj) {
		angle_t	pAngle = playerObj->angle;
		fixed_t fx, fz;

		pAngle >>= ANGLETOFINESHIFT;

		fx = finecosine[pAngle];
		fz = finesine[pAngle];

		doom_Listener.OrientFront.x = (float)(fx) / 65535.f;
		doom_Listener.OrientFront.y = 0.f;
		doom_Listener.OrientFront.z = (float)(fz) / 65535.f;
		doom_Listener.Position.x = (float)(playerObj->x >> FRACBITS);
		doom_Listener.Position.y = 0.f;
		doom_Listener.Position.z = (float)(playerObj->y >> FRACBITS);
	}
	else {
		doom_Listener.OrientFront.x = 0.f;
		doom_Listener.OrientFront.y = 0.f;
		doom_Listener.OrientFront.z = 1.f;

		doom_Listener.Position.x = 0.f;
		doom_Listener.Position.y = 0.f;
		doom_Listener.Position.z = 0.f;
	}

	ALfloat listenerOrientation[] = { doom_Listener.OrientFront.x, doom_Listener.OrientFront.y,
		doom_Listener.OrientFront.z, doom_Listener.OrientTop.x, doom_Listener.OrientTop.y,
		doom_Listener.OrientTop.z };
	alGetError();
	alListenerfv(AL_ORIENTATION, listenerOrientation);
	alListener3f(AL_POSITION, doom_Listener.Position.x, doom_Listener.Position.y, doom_Listener.Position.z);
	// Do not let a listener-update error contaminate the per-source queries.
	alGetError();

	// Update playing source voice positions
	int i;
	activeSound_t* sound;
	for (i = 0; i < NUM_SOUNDBUFFERS; i++) {
		sound = &activeSounds[i];

		if (!sound->valid) {
			continue;
		}

		if (sound->alSourceVoice == 0) {
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
			continue;
		}
		
		ALint sourceState = AL_STOPPED;
		alGetError();
		alGetSourcei(sound->alSourceVoice, AL_SOURCE_STATE, &sourceState);
		const ALenum stateError = alGetError();
		if (stateError != AL_NO_ERROR) {
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
			continue;
		}

		if (sourceState == AL_STOPPED || sourceState == AL_INITIAL) {
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
			continue;
		}
		
		if (sourceState != AL_PLAYING) {
			continue;
		}
		
		if (sound->localSound) {
			alSource3f(sound->alSourceVoice, AL_POSITION, doom_Listener.Position.x,
				doom_Listener.Position.y, doom_Listener.Position.z);
		} else {
			if (sound->originator == NULL) {
				// A non-local source without an originator cannot be positioned
				// safely. Stop it before retiring the CPU-side channel so it
				// cannot continue as an orphaned "ghost" sound.
				alGetError();
				alSourceStop(sound->alSourceVoice);
				alGetError();

				sound->id = 0;
				sound->valid = 0;
				sound->start = 0;
				sound->player = -1;
				sound->localSound = false;
				sound->originator = NULL;
				continue;
			}
			
			ALfloat x = (ALfloat)(sound->originator->x >> FRACBITS);
			ALfloat y = 0.f;
			ALfloat z = (ALfloat)(sound->originator->y >> FRACBITS);
			
			alSource3f(sound->alSourceVoice, AL_POSITION, x, y, z);
		}
	}
}

/*
======================
I_UpdateSoundParams
======================
*/
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{}

/*
======================
I_ShutdownSound
======================
*/
void I_ShutdownSound(void)
{
	int done = 0;
	int i;

	if (S_initialized) 
	{
		// Stop and detach every source. I_StopSound() filters by player, which
		// can leave split-screen sounds active, and a stopped OpenAL source still
		// keeps its buffer attached. The buffers are reused by I_InitSound(), so
		// they must all be detached before alBufferData() repopulates them.

		for (i = 0; i < NUM_SOUNDBUFFERS; i++)
		{
			activeSound_t* sound = &activeSounds[i];

			if (soundHardwareInitialized && sound->alSourceVoice)
			{
				alSourceStop(sound->alSourceVoice);
				alSourcei(sound->alSourceVoice, AL_BUFFER, 0);
			}

			// Always clear CPU-side channel state, even if the OpenAL hardware
			// is already unavailable or this channel has no source handle.
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
		}

		memset(soundEvents, 0, sizeof(soundEvents));
			
		// Free allocated sound memory and clear all data pointers, including
		// aliases that point at one of the allocations freed above.

		for (i = 1; i < NUMSFX; i++) 
		{
			if (S_sfx[i].data && !(S_sfx[i].link)) 
			{
				free(S_sfx[i].data);
			}
			S_sfx[i].data = NULL;
			lengths[i] = 0;
		}
	}

	I_StopSong(0);

	S_initialized = 0;
}

/*
======================
I_InitSoundHardware

Called from the tech4x initialization code. Sets up Doom classic's
sound channels.
======================
*/
void I_InitSoundHardware(int numOutputChannels_, int channelMask)
{
	::numOutputChannels = numOutputChannels_;
	// Debug: announce entry to doomclassic sound hardware init
	printf("[doomclassic] I_InitSoundHardware: numOutputChannels=%d channelMask=0x%X\n", numOutputChannels_, channelMask);

	// Initialize source voices
	for (int i = 0; i < NUM_SOUNDBUFFERS; i++) {
		I_InitSoundChannel(i, numOutputChannels);
	}

	// Create OpenAL buffers for all sounds
	for (int i = 1; i < NUMSFX; i++) {
		alBuffers[i] = 0;
		
		// Isolate each buffer creation so one failure cannot contaminate the
		// diagnostics for every buffer generated after it.
		alGetError();
		alGenBuffers(1, &alBuffers[i]);
		
		const ALenum bufferError = alGetError();
		if (bufferError != AL_NO_ERROR || alBuffers[i] == 0) {
			printf("[doomclassic] failed to create SFX buffer %d: 0x%X\n", i, bufferError);
			alBuffers[i] = 0;
		}
	}

	// Print the active OpenAL implementation independently of individual
	// source/buffer creation failures.
	{
		const char* vendor = (const char*)alGetString(AL_VENDOR);
		const char* version = (const char*)alGetString(AL_VERSION);
		if (vendor) {
			printf("[doomclassic] OpenAL vendor: %s\n", vendor);
		} else {
			printf("[doomclassic] alGetString(AL_VENDOR) returned NULL\n");
		
		}
		if (version) {
			printf("[doomclassic] OpenAL version: %s\n", version);
		}
		else {
			printf("[doomclassic] alGetString(AL_VERSION) returned NULL\n");
		}
	}

	// I_InitSound() owns the decoded Doom sound data and normally uploads it
	// the first time Classic Doom starts. A tech4 sound restart recreates the
	// OpenAL context without re-running I_InitSound(), so repopulate the newly
	// generated buffers when the sound data is already resident.
	if( S_initialized ) 
	{
		for( int i = 1; i < NUMSFX; i++ ) 
		{
			if( S_sfx[i].data && alBuffers[i] != 0 ) 
			{
				alGetError();
				alBufferData( alBuffers[i], SFX_SAMPLETYPE, (byte*)S_sfx[i].data, lengths[i], SFX_RATE );
				
				ALenum aerr = alGetError();
				if( aerr != AL_NO_ERROR ) 
				{
					printf("[doomclassic] alBufferData restart error for buffer %d: 0x%X\n", i, aerr);
					alDeleteBuffers(1, &alBuffers[i]);
					alBuffers[i] = 0;
				}
			}
		}
	}

	I_InitMusic();

	soundHardwareInitialized = true;

	// Debug: finished
	printf("[doomclassic] I_InitSoundHardware: completed, soundHardwareInitialized=%d\n", soundHardwareInitialized);
}

/*
======================
I_ShutdownSoundHardware

Called from the tech4x shutdown code. Tears down Doom classic's
sound channels.
======================
*/
void I_ShutdownSoundHardware()
{
	soundHardwareInitialized = false;

	I_ShutdownMusic();

	// Delete all source voices
	for (int i = 0; i < NUM_SOUNDBUFFERS; ++i) {
		activeSound_t* sound = &activeSounds[i];

		if (!sound) {
			continue;
		}

		if (sound->alSourceVoice) 
		{
			alSourceStop(sound->alSourceVoice);
			alSourcei(sound->alSourceVoice, AL_BUFFER, 0);
			alDeleteSources(1, &sound->alSourceVoice);
		}

		sound->alSourceVoice = 0;
		sound->id = 0;
		sound->valid = 0;
		sound->start = 0;
		sound->player = -1;
		sound->localSound = false;
		sound->originator = NULL;
	}

	// Delete OpenAL buffers for all sounds
	for (int i = 0; i < NUMSFX; i++) 
	{
		if (alBuffers[i]) 
		{
			alDeleteBuffers(1, &alBuffers[i]);
			alBuffers[i] = 0;
			
		}
	}
}

/*
======================
I_InvalidateSoundHardware

Called when the owning OpenAL context cannot be made current during
shutdown. The context will reclaim its AL objects when it is destroyed,
so only clear Classic Doom's cached handles and CPU-side music state here.
No OpenAL calls are made from this path.
======================
*/
void I_InvalidateSoundHardware()
{
	soundHardwareInitialized = false;

	for (int i = 0; i < NUM_SOUNDBUFFERS; ++i) {
		activeSound_t* sound = &activeSounds[i];
		sound->alSourceVoice = 0;
		sound->id = 0;
		sound->valid = 0;
		sound->start = 0;
		sound->player = -1;
		sound->localSound = false;
		sound->originator = NULL;

	}

	for (int i = 0; i < NUMSFX; ++i) {
		alBuffers[i] = 0;

	}

	alMusicSourceVoice = 0;
	alMusicBuffer = 0;

	if (musicBuffer) {
		free(musicBuffer);
		musicBuffer = NULL;

	}

	if (Music_initialized) {
		Timidity_Shutdown();

	}

	doomMusic = NULL;
	totalBufferSize = 0;
	waitingForMusic = false;
	musicReady = false;
	Music_initialized = false;
}

/*
======================
I_InitSoundChannel
======================
*/
void I_InitSoundChannel(int channel, int numOutputChannels_)
{
	activeSound_t* soundchannel = &activeSounds[channel];

	soundchannel->alSourceVoice = 0;
	soundchannel->id = 0;
	soundchannel->valid = 0;
	soundchannel->start = 0;
	soundchannel->player = -1;
	soundchannel->localSound = false;
	soundchannel->originator = NULL;

	// Isolate this source's setup from sticky errors produced elsewhere.
	alGetError();
	alGenSources(1, &soundchannel->alSourceVoice);
	ALenum alError = alGetError();
	if (alError != AL_NO_ERROR || soundchannel->alSourceVoice == 0) {
		printf("[doomclassic] failed to create SFX source %d: 0x%X\n", channel, alError);
		soundchannel->alSourceVoice = 0;
		return;
	}

	alSource3f(soundchannel->alSourceVoice, AL_VELOCITY, 0.f, 0.f, 0.f);
	alSourcei(soundchannel->alSourceVoice, AL_LOOPING, AL_FALSE);
	alSourcef(soundchannel->alSourceVoice, AL_MAX_DISTANCE, SFX_MAX_DISTANCE);
	alSourcef(soundchannel->alSourceVoice, AL_REFERENCE_DISTANCE, SFX_REFERENCE_DISTANCE);
	alSourcef(soundchannel->alSourceVoice, AL_ROLLOFF_FACTOR, SFX_ROLLOFF_FACTOR);

	alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to configure SFX source %d: 0x%X\n", channel, alError);
		alDeleteSources(1, &soundchannel->alSourceVoice);
		soundchannel->alSourceVoice = 0;
	}
}

/*
======================
I_InitSound
======================
*/
void I_InitSound()
{
	if (S_initialized == 0) {
		// Debug: announce entry to doomclassic I_InitSound
		printf("[doomclassic] I_InitSound: entry\n");
		// Set up listener parameters
		doom_Listener.OrientFront.x = 0.f;
		doom_Listener.OrientFront.y = 0.f;
		doom_Listener.OrientFront.z = 1.f;

		doom_Listener.OrientTop.x = 0.f;
		doom_Listener.OrientTop.y = -1.f;
		doom_Listener.OrientTop.z = 0.f;

		doom_Listener.Position.x = 0.f;
		doom_Listener.Position.y = 0.f;
		doom_Listener.Position.z = 0.f;

		for (int i = 1; i < NUMSFX; i++) {
			// Alias? Example is the chaingun sound linked to pistol.
			if (!S_sfx[i].link) {
				// Load data from WAD file.
				S_sfx[i].data = getsfx(S_sfx[i].name, &lengths[i]);
			}
			else {
				// Previously loaded already?
				S_sfx[i].data = S_sfx[i].link->data;
				lengths[i] = lengths[S_sfx[i].link - S_sfx];
			}
			if (S_sfx[i].data) {
				if (alBuffers[i] != 0) {
					alGetError();
					alBufferData(alBuffers[i], SFX_SAMPLETYPE, (byte*)S_sfx[i].data, lengths[i], SFX_RATE);
					
					ALenum aerr = alGetError();
					if (aerr != AL_NO_ERROR) {
						printf("[doomclassic] alBufferData error for buffer %d: 0x%X\n", i, aerr);
						alDeleteBuffers(1, &alBuffers[i]);
						alBuffers[i] = 0;
					}
				}
				else {
					printf("[doomclassic] warning: SFX buffer %d is unavailable\n", i);
				}
			} else {
				// Log missing sound data for debugging
				printf("[doomclassic] warning: S_sfx[%d] '%s' has no data\n", i, S_sfx[i].name);
			}
		}

		S_initialized = 1;
		printf("[doomclassic] I_InitSound: completed, S_initialized=%d\n", S_initialized);
	}
}

/*
======================
I_SubmitSound
======================
*/
void I_SubmitSound(void)
{
	// Only do this for player 0, it will still handle positioning
	//		for other players, but it can't be outside the game
	//		frame like the soundEvents are.
	if (DoomLib::GetPlayer() == 0) {
		// Do 3D positioning of sounds
		I_UpdateSound();

		// Change music if required
		I_UpdateMusic();
	}
}


// =========================================================
// =========================================================
// Background Music
// =========================================================
// =========================================================

/*
======================
I_SetMusicVolume
======================
*/
void I_SetMusicVolume(int volume)
{
	x_MusicVolume = (float)volume / 15.f;
}

/*
======================
I_InitMusic
======================
*/
void I_InitMusic(void)
{
	if (Music_initialized) {
		return;
	}

	musicBuffer = NULL;
	totalBufferSize = 0;
	waitingForMusic = false;
	musicReady = false;
	alMusicSourceVoice = 0;
	alMusicBuffer = 0;

	const int timidityResult = Timidity_Init(MIDI_RATE, MIDI_FORMAT, MIDI_CHANNELS, MIDI_RATE, "classicmusic/gravis.cfg");
	if (timidityResult != 0) {
		printf("[doomclassic] Timidity_Init failed: %d\n", timidityResult);
		return;
	}

	// Isolate music initialization from any sticky OpenAL error left by
	// earlier Classic Doom sound setup.
	alGetError();

	alGenSources(1, &alMusicSourceVoice);
	ALenum alError = alGetError();
	if (alError != AL_NO_ERROR || alMusicSourceVoice == 0) {
		printf("[doomclassic] failed to create music source: 0x%X\n", alError);
		alMusicSourceVoice = 0;
		Timidity_Shutdown();
		return;
	}

	alSourcef(alMusicSourceVoice, AL_PITCH, 1.0f);
	// I_PlaySong() applies the requested per-song looping state before
	// playback starts. Keep initialization neutral.
	alSourcei(alMusicSourceVoice, AL_LOOPING, AL_FALSE);
	alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to configure music source: 0x%X\n", alError);
		alDeleteSources(1, &alMusicSourceVoice);
		alMusicSourceVoice = 0;
		Timidity_Shutdown();
		return;
	}

	alGenBuffers(1, &alMusicBuffer);
	alError = alGetError();
	if (alError != AL_NO_ERROR || alMusicBuffer == 0) {
		printf("[doomclassic] failed to create music buffer: 0x%X\n", alError);
		if (alMusicBuffer != 0) {
			alDeleteBuffers(1, &alMusicBuffer);
			alMusicBuffer = 0;
		}
		 alDeleteSources(1, &alMusicSourceVoice);
		alMusicSourceVoice = 0;
		Timidity_Shutdown();
		return;
	}
	
	Music_initialized = true;
}

/*
======================
I_ShutdownMusic
======================
*/
void I_ShutdownMusic(void)
{
	if (Music_initialized) {
		if (alMusicSourceVoice) {
			I_StopSong(0);
			alSourcei(alMusicSourceVoice, AL_BUFFER, 0);
			alDeleteSources(1, &alMusicSourceVoice);
			alMusicSourceVoice = 0;
		}

		if (alMusicBuffer) {
			alDeleteBuffers(1, &alMusicBuffer);
			alMusicBuffer = 0;
		}

		if (musicBuffer) {
			free(musicBuffer);
			musicBuffer = NULL;
		}

		Timidity_Shutdown();
	}

	doomMusic = NULL;
	totalBufferSize = 0;
	waitingForMusic = false;
	musicReady = false;

	Music_initialized = false;
}

namespace {
	const int MaxMidiConversionSize = 1024 * 1024;
	unsigned char midiConversionBuffer[MaxMidiConversionSize];
	const int MusicBytesPerFrame = MIDI_CHANNELS * MIDI_FORMAT_BYTES;
	const int MusicRenderChunkBytes = MIDI_RATE * MusicBytesPerFrame;
}

/*
======================
I_LoadSong
======================
*/
void I_LoadSong(const char* songname)
{
	idStr lumpName = "d_";
	lumpName += static_cast<const char*>(songname);

	const int lumpNum = W_GetNumForName(lumpName.c_str());
	const int musLength = W_LumpLength(lumpNum);
	const unsigned char* musFile = static_cast<const unsigned char*>(W_CacheLumpNum(lumpNum, PU_STATIC_SHARED));

	int length = 0;

	if (!Mus2Midi(musFile, musLength, midiConversionBuffer, MaxMidiConversionSize, &length) || length <= 0)
	{
		printf("[doomclassic] failed to convert music lump '%s' to MIDI\n", lumpName.c_str());
		musicReady = false;
		return;
	}

	doomMusic = Timidity_LoadSongMem(midiConversionBuffer, length);

	if (doomMusic == NULL)
	{
		printf("[doomclassic] Timidity failed to load converted MIDI for '%s'\n", lumpName.c_str());
		totalBufferSize = 0;
		musicReady = false;
		return;
	}

	if (doomMusic->samples <= 0 || doomMusic->samples > INT_MAX / MusicBytesPerFrame)
	{
		printf("[doomclassic] invalid decoded music length for '%s': %d samples\n", lumpName.c_str(), doomMusic->samples);
		Timidity_FreeSong(doomMusic);
		doomMusic = NULL;
		totalBufferSize = 0;
		musicReady = false;
		return;
	}

	totalBufferSize = doomMusic->samples * MusicBytesPerFrame;
	musicBuffer = static_cast<byte*>(malloc(totalBufferSize));
	if (musicBuffer == NULL)
	{
		printf("[doomclassic] failed to allocate %d bytes for music '%s'\n", totalBufferSize, lumpName.c_str());
		Timidity_FreeSong(doomMusic);
		doomMusic = NULL;
		totalBufferSize = 0;
		musicReady = false;
		return;
	}

	byte renderChunk[MusicRenderChunkBytes];
	int rc = RC_NO_RETURN_VALUE;
	int offset = 0;
	bool decodeFailed = false;
	
	Timidity_Start(doomMusic);
	
	do
	{
		int numBytes = 0;
		rc = Timidity_PlaySome(renderChunk, MIDI_RATE, &numBytes);
		
		if (numBytes < 0 || numBytes > MusicRenderChunkBytes || numBytes > totalBufferSize - offset)
		{
			printf("[doomclassic] Timidity produced an invalid byte count for '%s': %d\n", lumpName.c_str(), numBytes);
			decodeFailed = true;
			break;
		}
		
		if (numBytes > 0)
		{
			memcpy(musicBuffer + offset, renderChunk, numBytes);
			offset += numBytes;
		}
		
		if (rc != RC_NO_RETURN_VALUE && rc != RC_JUMP && rc != RC_TUNE_END)
		{
			printf("[doomclassic] Timidity decode failed for '%s' with code %d\n", lumpName.c_str(), rc);
			decodeFailed = true;
			break;
		}
	} while (rc != RC_TUNE_END);
	
	Timidity_Stop();
	Timidity_FreeSong(doomMusic);
	doomMusic = NULL;
	
	if (decodeFailed || offset != totalBufferSize)
	{
		if (!decodeFailed)
		{
			printf("[doomclassic] decoded music size mismatch for '%s': expected %d, got %d\n",
			lumpName.c_str(), totalBufferSize, offset);
		}
		
		free(musicBuffer);
		musicBuffer = NULL;
		totalBufferSize = 0;
		musicReady = false;
		return;
	}

	musicReady = true;
}

/*
======================
I_PlaySong
======================
*/
void I_PlaySong(const char* songname, int looping)
{
	if (!Music_initialized) {
		return;
	}

	I_StopSong(0);

	// Clear old state
	if (musicBuffer) {
		free(musicBuffer);
		musicBuffer = 0;
	}

	totalBufferSize = 0;

	// Honor the caller's per-song looping request. S_StartMusic() deliberately
	// requests non-looping playback while normal level music can request loops.
	alGetError();
	alSourcei(alMusicSourceVoice, AL_LOOPING, looping ? AL_TRUE : AL_FALSE);
	const ALenum loopingError = alGetError();
	if (loopingError != AL_NO_ERROR) {
		printf("[doomclassic] failed to set music looping state: 0x%X\n", loopingError);
	}

	musicReady = false;
	I_LoadSong(songname);
	waitingForMusic = musicReady;

	if (DoomLib::GetPlayer() >= 0) {
		::g->mus_looping = looping;
	}
}

/*
======================
I_UpdateMusic
======================
*/
void I_UpdateMusic(void)
{
	if (!Music_initialized) {
		return;
	}

	// A decoded song can be waiting for its OpenAL upload when the game is
	// paused. Keep it pending rather than starting playback underneath the
	// pause state; I_ResumeSong() leaves pending songs for this path to start.
	if (DoomLib::GetPlayer() >= 0 && ::g->mus_paused) {
		return;
	}

	if (alMusicSourceVoice) {
		// Set the volume
		alSourcef(alMusicSourceVoice, AL_GAIN, x_MusicVolume * GLOBAL_VOLUME_MULTIPLIER);
	}

	if (!waitingForMusic) {
		return;
	}

	if (!musicReady || !alMusicSourceVoice || !alMusicBuffer || !musicBuffer || totalBufferSize <= 0) {
		musicReady = false;
		waitingForMusic = false;
		return;
	}
	
	// Keep each OpenAL operation isolated so a stale error cannot be
	// misattributed to the music upload/start sequence.
	alGetError();
	alSourcei(alMusicSourceVoice, AL_BUFFER, 0);
	ALenum alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to detach previous music buffer: 0x%X\n", alError);
		musicReady = false;
		waitingForMusic = false;
		return;
	}
	
	alBufferData(alMusicBuffer, MIDI_SAMPLETYPE, musicBuffer, totalBufferSize, MIDI_RATE);
	alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to upload music buffer: 0x%X\n", alError);
		musicReady = false;
		waitingForMusic = false;
		return;
	}
	
	alSourcei(alMusicSourceVoice, AL_BUFFER, alMusicBuffer);
	alSourcePlay(alMusicSourceVoice);
	alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to start music playback: 0x%X\n", alError);
		alSourceStop(alMusicSourceVoice);
		alSourcei(alMusicSourceVoice, AL_BUFFER, 0);
		alGetError();
		musicReady = false;
		waitingForMusic = false;
		return;
	}
	
	waitingForMusic = false;
}

/*
======================
I_PauseSong
======================
*/
void I_PauseSong(int handle)
{
	if (!Music_initialized || !alMusicSourceVoice) {
		return;
	}

	// A pending song has not been attached or started yet. I_UpdateMusic()
	// observes ::g->mus_paused and will leave it pending until resume.
	if (waitingForMusic) {
		return;
	}

	alSourcePause(alMusicSourceVoice);
}

/*
======================
I_ResumeSong
======================
*/
void I_ResumeSong(int handle)
{
	if (!Music_initialized || !alMusicSourceVoice) {
		return;
	}

	// Pending music will be started by I_UpdateMusic() once S_ResumeSound()
	// clears ::g->mus_paused.
	if (waitingForMusic) {
		return;
	}

	alSourcePlay(alMusicSourceVoice);
}

/*
======================
I_StopSong
======================
*/
void I_StopSong(int handle)
{
	if (!Music_initialized) {
		return;
	}
	
	// Cancel a decoded-but-not-yet-uploaded song as well as an already playing
	// source. Otherwise a later I_UpdateMusic() could start music after Stop.
	waitingForMusic = false;
	musicReady = false;
	
	if (!alMusicSourceVoice) {
		return;
	}

	alSourceStop(alMusicSourceVoice);
}

/*
======================
I_UnRegisterSong
======================
*/
void I_UnRegisterSong(int handle)
{
	// does nothing
}

/*
======================
I_RegisterSong
======================
*/
int I_RegisterSong(void* data, int length)
{
	// does nothing
	return 0;
}

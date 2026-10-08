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

#define SFX_RATE		11025
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

static idStr	currentMusicName;
static int	currentMusicLooping = 0;
static bool	restoreMusicAfterHardwareRestart = false;
static bool	musicHardwareRestartAttempted = false;
static bool	restoringMusicAfterHardwareRestart = false;
static bool	musicInitRetryAttempted = false;
static bool	musicInitCleanupRestartAttempted = false;
static bool	musicNeedsExplicitRetry = false;

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
	int handle;
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
	int handle;
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
//
// The original X3DAudio emitter used pVolumeCurve == NULL with
// CurveDistanceScaler == 1200.  That produces full gain through 1200 world
// units and then an inverse 1200 / distance falloff.  OpenAL's default
// AL_INVERSE_DISTANCE_CLAMPED model reproduces that curve with reference
// distance 1200, rolloff 1, and the source's default FLT_MAX max distance.
const ALfloat		SFX_REFERENCE_DISTANCE = 1200.f;
const ALfloat		SFX_ROLLOFF_FACTOR = 1.0f;

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
static uint32		nextSoundHandle = 1;

doomListener_t		doom_Listener;

void			I_InvalidateSoundHardware();
void			I_InitSoundChannel(int channel, int numOutputChannels_);

/*
======================
I_ValidateSoundHardwareContext

Classic Doom sound code can run before the main sound-system Render() for the
frame. If the shared OpenAL context has been lost or another context became
current, invalidate Classic's cached context-local names immediately and let
the normal full sound restart rebuild them. No AL calls are made by the
invalidation path.
======================
*/
static bool I_ValidateSoundHardwareContext()
{
	if (soundSystemLocal.hardware.IsContextCurrent()) {
		return true;
	}
	
	if (soundHardwareInitialized || Music_initialized) {
		I_InvalidateSoundHardware();
		soundSystemLocal.SetNeedsRestart();
	}
	return false;
}

/*
======================
I_AllocateSoundHandle
======================
*/
static int I_AllocateSoundHandle()
{
	// Handles are signed ints in the Doom sound API. Keep them positive and
	// avoid collisions with active or pending sounds if the counter ever wraps.
	for (int attempt = 0; attempt < NUM_SOUNDBUFFERS + 129; ++attempt)
	{
		if (nextSoundHandle == 0 || nextSoundHandle > 0x7FFFFFFFu)
		{
			nextSoundHandle = 1;
		}
		
		const int candidate = (int)nextSoundHandle++;
		bool inUse = false;
		
		for (int i = 0; i < NUM_SOUNDBUFFERS; ++i)
		{
			if (activeSounds[i].valid && activeSounds[i].handle == candidate)
			{
				inUse = true;
				break;
			}
		}
		
		if (!inUse)
		{
			for (int i = 0; i < (int)(sizeof(soundEvents) / sizeof(soundEvents[0])); ++i)
			{
				if (soundEvents[i].handle == candidate)
				{
					inUse = true;
					break;
				}
			}
		}
		
		if (!inUse)
		{
			return candidate;
		}
	}
	
	return 0;
}

/*
======================
I_FindPendingSoundEvent
======================
*/
static int I_FindPendingSoundEvent(int handle, int player)
{
	if (handle <= 0)
	{
		return -1;
	}
	
	for (int i = 0; i < (int)(sizeof(soundEvents) / sizeof(soundEvents[0])); ++i)
	{
		if (soundEvents[i].handle != handle)
		{
			continue;
		}
		
		if (player >= 0 && soundEvents[i].player != player)
		{
			continue;
		}
		
		return i;
	}
	
	return -1;
}

/*
======================
I_RetireFailedSoundSource

An OpenAL source that has produced a state/setup error is no longer trusted.
Release it when possible and always clear the cached name so the lazy Classic
allocator can create a fresh source for this logical channel later.
======================
*/
static void I_RetireFailedSoundSource(activeSound_t* sound)
{
	if (sound == NULL) {
		return;
	}

	if (sound->alSourceVoice != 0 && soundHardwareInitialized && soundSystemLocal.hardware.IsContextCurrent()) {
		ALuint source = sound->alSourceVoice;
		alGetError();
		if (alIsSource(source) == AL_TRUE) {
			alSourceStop(source);
			alSourcei(source, AL_BUFFER, 0);
			alDeleteSources(1, &source);
		}

		const ALenum cleanupError = alGetError();
		if (cleanupError != AL_NO_ERROR) {
			printf("[doomclassic] failed to retire SFX source: 0x%X; requesting sound restart\n", cleanupError);
			// The cached name is discarded below. Rebuild the shared context
			// so any object the driver failed to release is reclaimed.
			soundSystemLocal.SetNeedsRestart();
		}
	}

	sound->alSourceVoice = 0;
	sound->handle = 0;
	sound->id = 0;
	sound->valid = 0;
	sound->start = 0;
	sound->player = -1;
	sound->localSound = false;
	sound->originator = NULL;
}

/*
======================
I_RetireFailedSfxBuffer

Discard a Classic SFX buffer after creation/upload failure. Unlike source
retirement, this helper is also used while I_InitSoundHardware() is rebuilding
buffers into a freshly-created context, before soundHardwareInitialized is
published. Rely on ownership of the current context rather than that flag.
======================
*/
static void I_RetireFailedSfxBuffer(ALuint & buffer, int id)
{
	if (buffer == 0) {
		return;
	}
	
	bool cleanupFailed = false;
	
	if (soundSystemLocal.hardware.IsContextCurrent()) {
		alGetError();
		const ALboolean validBuffer = alIsBuffer(buffer);
		const ALenum validationError = alGetError();
		
		if (validationError != AL_NO_ERROR) {
			cleanupFailed = true;
		}
		else if (validBuffer == AL_TRUE) {
			alDeleteBuffers(1, &buffer);
			if (alGetError() != AL_NO_ERROR) {
				cleanupFailed = true;
			}
		}
	}
	else {
		// The numeric name is being discarded without a trustworthy owning
		// context. Rebuild the shared context so it can reclaim the object.
		cleanupFailed = true;
	}
	
	if (cleanupFailed) {
		printf("[doomclassic] failed to retire SFX buffer %d; requesting sound restart\n", id);
		soundSystemLocal.SetNeedsRestart();
	}
	buffer = 0;
}

/*
======================
I_CalculateRelativeSoundPosition

The original XAudio2 backend calculated a separate output matrix for each
Classic Doom sound using the player stored in activeSound_t as that sound's
listener. OpenAL has one global listener, so reproduce the same split-screen
behavior by keeping SFX sources listener-relative and transforming each
emitter into its assigned player's coordinate frame.

OpenAL's default listener looks down -Z with +X to the right. Doom stores its
2D world in X/Y, mapped here to OpenAL X/Z. The transform below therefore
produces:
  relative X = right/left
  relative Z = negative-forward/positive-back
======================
*/
static bool I_CalculateRelativeSoundPosition( const mobj_t * source, const mobj_t * listener, ALfloat & x, ALfloat & y, ALfloat & z)
{
	if (source == NULL || listener == NULL)
	{
		return false;
	}
	
	const angle_t listenerAngle = listener->angle >> ANGLETOFINESHIFT;
	const float frontX = (float)finecosine[listenerAngle] / (float)FRACUNIT;
	const float frontZ = (float)finesine[listenerAngle] / (float)FRACUNIT;
	
	// Use 64-bit differences before converting from Doom fixed-point so an
	// extreme pair of legal fixed_t positions cannot overflow subtraction.
	const float deltaX = (float)((int64)source->x - (int64)listener->x) / (float)FRACUNIT;
	const float deltaZ = (float)((int64)source->y - (int64)listener->y) / (float)FRACUNIT;
	
	// right = (frontZ, -frontX) in Doom's X/Y plane.
	x = deltaX * frontZ - deltaZ * frontX;
	y = 0.0f;
	z = -(deltaX * frontX + deltaZ * frontZ);
	return true;
}

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

	// New sounds pick up x_SoundVolume in I_StartSound2(), but existing
	// OpenAL sources otherwise keep the gain they had when they were started.
	// Apply master-volume changes to currently active Classic Doom sounds too.
	if (!soundHardwareInitialized || !I_ValidateSoundHardwareContext()) {
		return;
	}
	
	for (int i = 0; i < NUM_SOUNDBUFFERS; ++i) {
		activeSound_t * sound = &activeSounds[i];
		if (!sound->valid || sound->alSourceVoice == 0) {
			continue;
		}
		
		alGetError();
		alSourcef(sound->alSourceVoice, AL_GAIN, x_SoundVolume);
	
		const ALenum volumeError = alGetError();
		if (volumeError != AL_NO_ERROR) {
			printf("[doomclassic] failed to update SFX source %d volume: 0x%X\n", i, volumeError);
			I_RetireFailedSoundSource(sound);
		}
	}
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
I_EnsureSfxBuffer

Classic SFX buffers are normally created with the OpenAL context and populated
when Classic Doom initializes. If creation or upload failed transiently, retry
that one buffer lazily when the sound is actually requested instead of leaving
the SFX unavailable until the next full sound-system restart.
======================
*/
static bool I_EnsureSfxBuffer(int id)
{
	if (id <= 0 || id >= NUMSFX) {
		return false;
	}
	
	if (alBuffers[id] != 0) {
		return true;
	}
	
	if (!soundHardwareInitialized || !I_ValidateSoundHardwareContext() || S_sfx[id].data == NULL || lengths[id] <= 0) {
		return false;
	}
	
	ALuint buffer = 0;
	alGetError();
	alGenBuffers(1, &buffer);
	ALenum alError = alGetError();
	if (alError != AL_NO_ERROR || buffer == 0) {
		printf("[doomclassic] failed to recreate SFX buffer %d: 0x%X\n", id, alError);
		I_RetireFailedSfxBuffer(buffer, id);
		return false;
	}
	
	alBufferData(buffer, SFX_SAMPLETYPE, (byte*)S_sfx[id].data, lengths[id], SFX_RATE);
	alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to repopulate SFX buffer %d: 0x%X\n", id, alError);
		I_RetireFailedSfxBuffer(buffer, id);
		return false;
	}
	
	alBuffers[id] = buffer;
	return true;
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
int I_StartSound2(int id, int player, mobj_t* origin, mobj_t* listener_origin, int pitch, int priority, int handle)
{
	if (!soundHardwareInitialized || !I_ValidateSoundHardwareContext() || id <= 0 || id >= NUMSFX || handle <= 0) {
		return 0;
	}
	
	if (!I_EnsureSfxBuffer(id)) {
		return 0;
	}

	int i;
	activeSound_t* sound = 0;
	int oldest = 0, oldestnum = -1;
	bool sourceCreationFailed = false;

	// these id's should not overlap
	if (id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful || id == sfx_sawhit || id == sfx_stnmov) {
		// Loop all channels, check.
		for (i = 0; i < NUM_SOUNDBUFFERS; i++)
		{
			sound = &activeSounds[i];

			if (sound->valid && (sound->id == id && sound->player == player)) {
				I_StopSound(sound->handle, player);
				break;
			}
		}
	}

	// find a valid channel, or one that has finished playing
	for (i = 0; i < NUM_SOUNDBUFFERS; i++) {
		sound = &activeSounds[i];

		// Classic Doom has 64 logical SFX channels, but reserving 64 OpenAL
		// sources at engine startup can starve the main BFG voice pool on
		// implementations with a modest source limit. Create Classic sources
		// only as channels are actually needed. After one allocation failure,
		// keep scanning already-created sources instead of hammering
		// alGenSources() for every remaining logical slot.
		if (sound->alSourceVoice == 0 && !sourceCreationFailed) {
			I_InitSoundChannel(i, numOutputChannels);
			if (sound->alSourceVoice == 0) {
				sourceCreationFailed = true;
			}
		}
		
		// A source can still be unavailable if the device source limit was
		// reached. Never issue AL calls against source 0.
		if (sound->alSourceVoice == 0)
			continue;

		if (!sound->valid) {
			// A logically free slot can still carry a stale source name after
			// an earlier driver/source failure. Validate it before selecting
			// the slot so the current start can recreate it immediately.
			alGetError();
			const ALboolean sourceValid = alIsSource(sound->alSourceVoice);
			const ALenum sourceError = alGetError();
			if (sourceValid != AL_TRUE || sourceError != AL_NO_ERROR) {
				I_RetireFailedSoundSource(sound);
				I_InitSoundChannel(i, numOutputChannels);
				if (sound->alSourceVoice == 0) {
					sourceCreationFailed = true;
					continue;
				}
			}
			break;
		}

		ALint sourceState = AL_INITIAL;
		alGetError();
		alGetSourcei(sound->alSourceVoice, AL_SOURCE_STATE, &sourceState);
		const ALenum stateError = alGetError();
		if (stateError != AL_NO_ERROR) {
			// Do not let one rejected source permanently poison this logical
			// slot. Retire it and continue looking for another usable channel.
			I_RetireFailedSoundSource(sound);
			continue;
		}
		if (sourceState == AL_STOPPED) {
			break;
		}

		// Only successfully queried, still-active sources are candidates for
		// oldest-channel stealing. Use oldestnum as the initialization sentinel
		// so a sound that started at gametic 0 is handled correctly.
		if (oldestnum < 0 || sound->start < oldest) {
			oldestnum = i;
			oldest = sound->start;
		}
	}

	// none found, so use the oldest one
	if (i == NUM_SOUNDBUFFERS) {
		if (oldestnum < 0) {
			return 0;
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
	if (origin == NULL || origin == listener_origin) {
		localSound = true;
	}
	else if (!I_CalculateRelativeSoundPosition(origin, listener_origin, x, y, z)) {
		// A non-local sound without a valid listener cannot be spatialized
		// correctly. Treat it as local for this start rather than placing it
		// in an unrelated global-listener coordinate system.
		localSound = true;
		x = 0.0f;
		y = 0.0f;
		z = 0.0f;
	}

	alSource3f(sound->alSourceVoice, AL_POSITION, x, y, z);

	alSourcePlay(sound->alSourceVoice);

	const ALenum startError = alGetError();
	if (startError != AL_NO_ERROR) {
		printf("[doomclassic] failed to start SFX %d: 0x%X\n", id, startError);
		
		I_RetireFailedSoundSource(sound);
		return 0;
	}
	
	// Publish the channel only after OpenAL accepted the complete start.
	sound->handle = handle;
	sound->id = id;
	sound->start = ::g->gametic;
	sound->valid = 1;
	sound->player = player;
	sound->localSound = localSound;
	sound->originator = origin;

	return handle;
}

/*
======================
I_ProcessSoundEvents
======================
*/
void I_ProcessSoundEvents(void)
{
	for (int i = 0; i < 128; i++) {
		if (soundEvents[i].handle != 0) {
			I_StartSound2(i, soundEvents[i].player, soundEvents[i].originator, soundEvents[i].listener,
				soundEvents[i].pitch, soundEvents[i].priority, soundEvents[i].handle);
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
	
	const int handle = I_AllocateSoundHandle();
	if (handle == 0) {
		return 0;
	}

	// if we're only one player or we're trying to play the chainsaw sound, do it normal
	// otherwise only allow one sound of each type per frame
	if (PLAYERCOUNT == 1 || id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful || id == sfx_sawhit) {
		return I_StartSound2(id, ::g->consoleplayer, origin, listener_origin, pitch, priority, handle);
	}
	else {
		if (soundEvents[id].vol < vol) {
			soundEvents[id].handle = handle;
			soundEvents[id].player = DoomLib::GetPlayer();
			soundEvents[id].pitch = pitch;
			soundEvents[id].priority = priority;
			soundEvents[id].vol = vol;
			soundEvents[id].originator = origin;
			soundEvents[id].listener = listener_origin;
		}
		return handle;
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
		if (!sound->valid || sound->handle != handle || (player >= 0 && sound->player != player))
			continue;
		break;
	}

	if (i == NUM_SOUNDBUFFERS)
	{
		// A split-screen sound may still be waiting in soundEvents[] and not
		// have an OpenAL source yet. Cancel that deferred start when the Doom
		// logical channel is stopped before I_ProcessSoundEvents() runs.
		const int pendingEvent = I_FindPendingSoundEvent(handle, player);
		if (pendingEvent >= 0)
		{
			memset(&soundEvents[pendingEvent], 0, sizeof(soundEvents[pendingEvent]));
		}
		return;
	}

	if (!I_ValidateSoundHardwareContext()) {
		return;
	}

	// Stop the sound. CPU-side state is cleared regardless of whether the
	// OpenAL stop succeeds so a stale source cannot remain logically active.
	if (sound->alSourceVoice != 0) {
		alGetError();
		alSourceStop(sound->alSourceVoice);
		const ALenum stopError = alGetError();
		if (stopError != AL_NO_ERROR) {
			printf("[doomclassic] failed to stop SFX source: 0x%X; retiring source\n", stopError);
			// A rejected stop can leave audible ghost playback after the
			// logical channel is gone. Retire the untrusted source so the
			// context either releases it immediately or is rebuilt if
			// cleanup itself fails.
			I_RetireFailedSoundSource(sound);
			return;
		}
	}

	sound->handle = 0;
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
	if (!soundHardwareInitialized || !I_ValidateSoundHardwareContext()) {
		return 0;
	}

	// Split-screen coalescing defers the actual OpenAL start until
	// I_ProcessSoundEvents(). Keep the logical Doom channel alive while its
	// unique handle is still queued for that deferred start.
	if (I_FindPendingSoundEvent(handle, -1) >= 0)
	{
		return 1;
	}

	int i;
	activeSound_t* sound;

	for (i = 0; i < NUM_SOUNDBUFFERS; ++i) {
		sound = &activeSounds[i];
		if (!sound->valid || sound->handle != handle)
			continue;

		if(sound->alSourceVoice == 0) {
			sound->handle = 0;
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
			I_RetireFailedSoundSource(sound);
			continue;
		}

		if (sourceState == AL_PLAYING) {
			return 1;
		}

		if (sourceState == AL_STOPPED || sourceState == AL_INITIAL) {
			sound->handle = 0;
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
	if (!soundHardwareInitialized || !I_ValidateSoundHardwareContext()) {
		return;
	}

	// Update playing source voice positions
	int i;
	activeSound_t* sound;
	for (i = 0; i < NUM_SOUNDBUFFERS; i++) {
		sound = &activeSounds[i];

		if (!sound->valid) {
			continue;
		}

		if (sound->alSourceVoice == 0) {
			sound->handle = 0;
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
			I_RetireFailedSoundSource(sound);
			continue;
		}

		if (sourceState == AL_STOPPED || sourceState == AL_INITIAL) {
			sound->handle = 0;
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
			alGetError();
			alSource3f(sound->alSourceVoice, AL_POSITION, 0.0f, 0.0f, 0.0f);
			const ALenum positionError = alGetError();
			if (positionError != AL_NO_ERROR) {
				printf("[doomclassic] failed to update local SFX source position: 0x%X\n", positionError);
				I_RetireFailedSoundSource(sound);
				continue;
			}
		} else {
			if (sound->originator == NULL || sound->player < 0 || sound->player >= MAXPLAYERS) {
				// A non-local source without an originator cannot be positioned
				// safely. Stop it before retiring the CPU-side channel so it
				// cannot continue as an orphaned "ghost" sound.
				I_RetireFailedSoundSource(sound);
				continue;
			}
			
			mobj_t* playerObj = ::g->players[sound->player].mo;
			if (playerObj == NULL) {
				I_RetireFailedSoundSource(sound);
				continue;
			}
			
			ALfloat x = 0.0f;
			ALfloat y = 0.f;
			ALfloat z = 0.0f;
			if (!I_CalculateRelativeSoundPosition(sound->originator, playerObj, x, y, z)) {
				continue;
			}
			alGetError();
			alSource3f(sound->alSourceVoice, AL_POSITION, x, y, z);
			const ALenum positionError = alGetError();
			if (positionError != AL_NO_ERROR) {
				printf("[doomclassic] failed to update spatial SFX source position: 0x%X\n", positionError);
				I_RetireFailedSoundSource(sound);
				continue;
			}
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
I_ReleaseMusicForClassicShutdown

I_ShutdownSound() leaves the shared OpenAL context alive for the main BFG
sound system. Release Classic's music source/buffer and CPU-side decoder state
instead of keeping them dormant until the next hardware restart. Unlike
I_ShutdownMusic(), which runs immediately before context destruction, this
path must verify live-context cleanup and request a rebuild if the driver
refuses to release an object.
======================
*/
static void I_ReleaseMusicForClassicShutdown()
{
	bool cleanupFailed = false;
	
	if (soundSystemLocal.hardware.IsContextCurrent()) {
		if (alMusicSourceVoice != 0) {
			alGetError();
			const ALboolean validSource = alIsSource(alMusicSourceVoice);
			const ALenum validationError = alGetError();
			
			if (validationError != AL_NO_ERROR) {
				cleanupFailed = true;
			}
			else if (validSource == AL_TRUE) {
				alDeleteSources(1, &alMusicSourceVoice);
				if (alGetError() != AL_NO_ERROR) {
					cleanupFailed = true;
				}
			}
		}
		
		if (alMusicBuffer != 0) {
			alGetError();
			const ALboolean validBuffer = alIsBuffer(alMusicBuffer);
			const ALenum validationError = alGetError();
			
			if (validationError != AL_NO_ERROR) {
				cleanupFailed = true;
			}
			else if (validBuffer == AL_TRUE) {
				alDeleteBuffers(1, &alMusicBuffer);
				if (alGetError() != AL_NO_ERROR) {
					cleanupFailed = true;
				}
			}
		}
	}
	else if (alMusicSourceVoice != 0 || alMusicBuffer != 0) {
		cleanupFailed = true;
	}
	
	alMusicSourceVoice = 0;
	alMusicBuffer = 0;
	
	if (musicBuffer != NULL) {
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
	currentMusicName.Clear();
	currentMusicLooping = 0;
	restoreMusicAfterHardwareRestart = false;
	restoringMusicAfterHardwareRestart = false;
	musicHardwareRestartAttempted = false;
	musicInitRetryAttempted = false;
	musicInitCleanupRestartAttempted = false;
	musicNeedsExplicitRetry = false;
	
	if (cleanupFailed) {
		printf("[doomclassic] failed to release Classic music hardware; requesting sound restart\n");
		soundSystemLocal.SetNeedsRestart();
	}
}

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
		I_ValidateSoundHardwareContext();

		bool hardwareCleanupFailed = false;
		// Stop and detach every source. I_StopSound() filters by player, which
		// can leave split-screen sounds active, and a stopped OpenAL source still
		// keeps its buffer attached. The buffers are reused by I_InitSound(), so
		// they must all be detached before alBufferData() repopulates them.

		for (i = 0; i < NUM_SOUNDBUFFERS; i++)
		{
			activeSound_t* sound = &activeSounds[i];

			if (soundHardwareInitialized && sound->alSourceVoice)
			{
				alGetError();
				alSourceStop(sound->alSourceVoice);
				alSourcei(sound->alSourceVoice, AL_BUFFER, 0);
				alDeleteSources(1, &sound->alSourceVoice);
				sound->alSourceVoice = 0;
				const ALenum cleanupError = alGetError();
				if (cleanupError != AL_NO_ERROR)
				{
					printf("[doomclassic] failed to release SFX source %d: 0x%X\n", i, cleanupError);
					hardwareCleanupFailed = true;
				}
			}

			// Always clear CPU-side channel state, even if the OpenAL hardware
			// is already unavailable or this channel has no source handle.
			sound->handle = 0;
			sound->id = 0;
			sound->valid = 0;
			sound->start = 0;
			sound->player = -1;
			sound->localSound = false;
			sound->originator = NULL;
		}

		// Classic owns its SFX OpenAL buffers only while Classic is active.
		// Releasing them here returns those hardware objects to the main BFG
		// sound system instead of keeping an entire dormant Classic buffer
		// set alive after switching games.
		for (i = 1; i < NUMSFX; ++i)
		{
			if (soundHardwareInitialized && alBuffers[i] != 0)
			{
				alGetError();
				alDeleteBuffers(1, &alBuffers[i]);
				const ALenum deleteError = alGetError();
				if (deleteError != AL_NO_ERROR)
				{
					printf("[doomclassic] failed to delete SFX buffer %d: 0x%X\n", i, deleteError);
					hardwareCleanupFailed = true;
				}
			}
			alBuffers[i] = 0;
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

		// Classic has intentionally discarded all source/buffer names above.
		// If the driver rejected any cleanup operation, rebuild the OpenAL
		// context so it can reclaim those otherwise unreachable objects before
		// the main BFG sound system continues using the shared device.
		if (hardwareCleanupFailed)
			soundSystemLocal.SetNeedsRestart();
	}

	// Return all Classic music resources to BFG. Source deletion itself stops
	// playback, so do not call I_StopSong() first: a rejected alSourceStop()
	// would request a redundant full restart even if deletion succeeds here.
	I_ReleaseMusicForClassicShutdown();

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

	// SFX sources are created lazily by I_StartSound2(). Keeping the 64
	// logical Classic channels source-free until they are used prevents
	// dormant Classic Doom audio from consuming the BFG hardware voice pool.

	// Keep Classic SFX buffers dormant too. During ordinary Doom 3 startup
	// Classic has no resident SFX data and does not need to reserve OpenAL
	// buffer objects. If Classic is already active, this is a hardware restart
	// and its resident SFX buffers must be rebuilt into the new context.
	for (int i = 1; i < NUMSFX; i++) {
		alBuffers[i] = 0;
		
		if (!S_initialized) {
			continue;
		}

		// Isolate each buffer creation so one failure cannot contaminate the
		// diagnostics for every buffer generated after it.
		alGetError();
		alGenBuffers(1, &alBuffers[i]);
		
		const ALenum bufferError = alGetError();
		if (bufferError != AL_NO_ERROR || alBuffers[i] == 0) {
			printf("[doomclassic] failed to create SFX buffer %d: 0x%X\n", i, bufferError);
			I_RetireFailedSfxBuffer(alBuffers[i], i);
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
					I_RetireFailedSfxBuffer(alBuffers[i], i);
				}
			}
		}
	}

	// If Classic Doom is already active, this is a hardware restart and its
	// music state may need to be restored into the new context. On ordinary
	// Doom 3 startup, defer the Classic music source until I_InitSound().
	if (S_initialized) {
		I_InitMusic();
	}

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
		sound->handle = 0;
		sound->id = 0;
		sound->valid = 0;
		sound->start = 0;
		sound->player = -1;
		sound->localSound = false;
		sound->originator = NULL;
	}

	// Deferred split-screen events belong to the old OpenAL context/lifecycle.
	// Never replay them after a hardware restart.
	memset(soundEvents, 0, sizeof(soundEvents));

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

	// The context is unavailable, so its source state cannot be queried.
	// Preserve a selected track for the next successful hardware init.
	if (!currentMusicName.IsEmpty()) {
		restoreMusicAfterHardwareRestart = true;
	}

	for (int i = 0; i < NUM_SOUNDBUFFERS; ++i) {
		activeSound_t* sound = &activeSounds[i];
		sound->alSourceVoice = 0;
		sound->handle = 0;
		sound->id = 0;
		sound->valid = 0;
		sound->start = 0;
		sound->player = -1;
		sound->localSound = false;
		sound->originator = NULL;

	}

	// The old context can no longer service deferred starts either.
	memset(soundEvents, 0, sizeof(soundEvents));

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

	// I_ShutdownMusic() normally resets the bounded initialization retry, but
	// this invalid-context path cannot call it. A replacement OpenAL context
	// should get the same one post-init retry if its early Classic music
	// source/buffer creation attempt fails. Keep musicHardwareRestartAttempted
	// intact so a music-triggered restart still cannot become a restart loop.
	musicInitRetryAttempted = false;
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
	soundchannel->handle = 0;
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
		I_RetireFailedSoundSource(soundchannel);
		return;
	}

	alSource3f(soundchannel->alSourceVoice, AL_VELOCITY, 0.f, 0.f, 0.f);
	// Classic Doom previously used X3DAudio to calculate a separate speaker
	// matrix for each sound's assigned split-screen player. OpenAL exposes
	// only one listener, so keep every SFX source listener-relative and feed
	// it coordinates transformed into that player's frame instead.
	alSourcei(soundchannel->alSourceVoice, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcei(soundchannel->alSourceVoice, AL_LOOPING, AL_FALSE);
	alSourcef(soundchannel->alSourceVoice, AL_REFERENCE_DISTANCE, SFX_REFERENCE_DISTANCE);
	alSourcef(soundchannel->alSourceVoice, AL_ROLLOFF_FACTOR, SFX_ROLLOFF_FACTOR);

	alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to configure SFX source %d: 0x%X\n", channel, alError);
		I_RetireFailedSoundSource(soundchannel);
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
		// A newly-entered Classic session gets a fresh bounded cleanup
		// recovery episode for partial music initialization failures.
		musicInitCleanupRestartAttempted = false;

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

		// I_InitSound() can be entered outside the main sound-system Render()
		// path. Retire stale Classic AL names before any BFG-source handoff,
		// music creation, or SFX buffer upload.
		I_ValidateSoundHardwareContext();

		// Doom 3 keeps stopped OpenAL source objects cached for fast voice
		// reuse. Release only those idle BFG sources before Classic allocates
		// its music/SFX sources so both backends can share devices with a
		// limited source count. Active BFG voices remain untouched.
		if (soundHardwareInitialized) {
			soundSystemLocal.hardware.ReleaseFreeVoiceResources();
		}

		// Classic music gets the first Classic-owned OpenAL source. SFX
		// sources are allocated lazily afterward, so a limited-source device
		// cannot lose music merely because 64 dormant SFX channels were
		// created first.
		if (soundHardwareInitialized) {
			I_InitMusic();
		}

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
				// Normal engine startup no longer reserves dormant Classic
				// buffers. Create/populate them now that Classic is actually
				// active; I_StartSound2() will retry an individual buffer
				// later if this allocation fails transiently.
				if (!I_EnsureSfxBuffer(i)) {
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
I_MusicNeedsExplicitRetry
======================
*/
bool I_MusicNeedsExplicitRetry(void)
 {
	return musicNeedsExplicitRetry;
}

/*
======================
I_CleanupFailedMusicInitObjects

Music initialization can run either after Classic Doom is already active or
inside idSoundHardware_OpenAL::Init(), before soundHardwareInitialized is
published. Clean up against the owned current context rather than relying on
that flag. If the driver refuses to release a partially-created object, allow
one follow-up full sound restart to reclaim it through context destruction.
The guard survives that restart so a persistently broken replacement context
cannot create an every-frame restart loop.
======================
*/
static void I_CleanupFailedMusicInitObjects()
{
	bool cleanupFailed = false;
	
	if (soundSystemLocal.hardware.IsContextCurrent()) {
		if (alMusicSourceVoice != 0) {
			alGetError();
			const ALboolean validSource = alIsSource(alMusicSourceVoice);
			const ALenum validationError = alGetError();
			
			if (validationError != AL_NO_ERROR) {
				cleanupFailed = true;
			}
			else if (validSource == AL_TRUE) {
				alDeleteSources(1, &alMusicSourceVoice);
				if (alGetError() != AL_NO_ERROR) {
					cleanupFailed = true;
				}
			}
		}
		
		if (alMusicBuffer != 0) {
			alGetError();
			const ALboolean validBuffer = alIsBuffer(alMusicBuffer);
			const ALenum validationError = alGetError();
			
			if (validationError != AL_NO_ERROR) {
				cleanupFailed = true;
			}
			else if (validBuffer == AL_TRUE) {
				alDeleteBuffers(1, &alMusicBuffer);
				if (alGetError() != AL_NO_ERROR) {
					cleanupFailed = true;
				}
			}
		}
	}
	else if (alMusicSourceVoice != 0 || alMusicBuffer != 0) {
		cleanupFailed = true;
	}
	
	alMusicSourceVoice = 0;
	alMusicBuffer = 0;
	
	if (cleanupFailed && !musicInitCleanupRestartAttempted) {
		printf("[doomclassic] failed to retire partial music initialization objects; requesting one cleanup restart\n");
		musicInitCleanupRestartAttempted = true;
		soundSystemLocal.SetNeedsRestart();
	}
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
		I_CleanupFailedMusicInitObjects();
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
		I_CleanupFailedMusicInitObjects();
		Timidity_Shutdown();
		return;
	}

	alGenBuffers(1, &alMusicBuffer);
	alError = alGetError();
	if (alError != AL_NO_ERROR || alMusicBuffer == 0) {
		printf("[doomclassic] failed to create music buffer: 0x%X\n", alError);
		I_CleanupFailedMusicInitObjects();
		Timidity_Shutdown();
		return;
	}
	
	Music_initialized = true;
	musicInitCleanupRestartAttempted = false;

	// A hardware-only restart destroys the OpenAL music objects while the
	// Classic Doom game still considers its current track selected. Requeue
	// that track into the newly created context when shutdown marked it active.
	if (restoreMusicAfterHardwareRestart && !currentMusicName.IsEmpty()) {
		const idStr restartSongName = currentMusicName;
		const int restartLooping = currentMusicLooping;
		restoreMusicAfterHardwareRestart = false;
		restoringMusicAfterHardwareRestart = true;
		I_PlaySong(restartSongName.c_str(), restartLooping);
		restoringMusicAfterHardwareRestart = false;
	}
}

/*
======================
I_ShutdownMusic
======================
*/
void I_ShutdownMusic(void)
{
	if (Music_initialized) {
		// Preserve only music that was actually pending, playing, or paused.
		// A naturally completed non-looping track should not be restarted.
		// Keep an explicit restore request made by I_UpdateMusic() after an
		// OpenAL upload/start failure; the failed path has already cleared
		// waitingForMusic before the full sound restart reaches this teardown.
		restoreMusicAfterHardwareRestart = restoreMusicAfterHardwareRestart || (waitingForMusic && !currentMusicName.IsEmpty());

		if (alMusicSourceVoice) {
			if (!currentMusicName.IsEmpty() && !restoreMusicAfterHardwareRestart) {
				ALint sourceState = AL_STOPPED;
				alGetError();
				alGetSourcei(alMusicSourceVoice, AL_SOURCE_STATE, &sourceState);
				const ALenum stateError = alGetError();
				
				if (stateError != AL_NO_ERROR ||
					sourceState == AL_PLAYING ||
					sourceState == AL_PAUSED) {
					restoreMusicAfterHardwareRestart = true;
				}
			}
			
			// Stop directly here rather than through I_StopSong(); I_StopSong()
			// intentionally clears the retained logical-track state.
			alGetError();
			alSourceStop(alMusicSourceVoice);
			alSourcei(alMusicSourceVoice, AL_BUFFER, 0);
			alGetError();
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
	musicInitRetryAttempted = false;
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

static void I_HandleMusicHardwareFailure();

/*
======================
I_PlaySong
======================
*/
void I_PlaySong(const char* songname, int looping)
{
	const bool hardwareContextCurrent = I_ValidateSoundHardwareContext();

	const bool retryingAbandonedTrack = !restoringMusicAfterHardwareRestart && musicNeedsExplicitRetry;

	// An explicit request begins a new attempt even if the requested track
	// matches the logical track that a previous hardware failure abandoned.
	if (!restoringMusicAfterHardwareRestart) {
		musicNeedsExplicitRetry = false;
	}

	if (!Music_initialized) {
		const idStr requestedSong = songname != NULL ? songname : "";
		const bool explicitNewRequest = !restoringMusicAfterHardwareRestart && (requestedSong.Icmp(currentMusicName.c_str()) != 0 || currentMusicLooping != looping);
		// S_ChangeMusic() records its logical selection even when the hardware
		// start fails, and then suppresses repeated requests for that same
		// track. Preserve the requested song here so a transient failure to
		// create the Classic music source/buffer does not make the selected
		// track permanently silent.
		currentMusicName = requestedSong;
		currentMusicLooping = looping;
		restoreMusicAfterHardwareRestart = !currentMusicName.IsEmpty();
		if (!restoringMusicAfterHardwareRestart) {
			musicInitRetryAttempted = false;
			musicInitCleanupRestartAttempted = false;
			if (retryingAbandonedTrack || explicitNewRequest) {
				// A different track or an explicit retry of an abandoned
				// same track begins a fresh bounded hardware-recovery
				// episode.
				musicHardwareRestartAttempted = false;
			}
		}
		if (hardwareContextCurrent && soundHardwareInitialized && S_initialized) {
			I_InitMusic();
		}
		// A successful I_InitMusic() re-enters I_PlaySong() through its
		// retained-track restore path, so either way this request is complete.
		return;
	}

	if (!hardwareContextCurrent) {
		return;
	}

	const idStr requestedSong = songname != NULL ? songname : "";
	if (!I_StopSong(0)) {
		// The old source could not be stopped reliably. Do not continue
		// configuring/uploading the replacement track against that source.
		// I_StopSong() has already requested a full context restart; retain
		// the new logical selection so the replacement context starts it.
		currentMusicName = requestedSong;
		currentMusicLooping = looping;
		restoreMusicAfterHardwareRestart = !currentMusicName.IsEmpty();
		musicNeedsExplicitRetry = false;
		if (!restoringMusicAfterHardwareRestart) {
			musicHardwareRestartAttempted = false;
			musicInitRetryAttempted = false;
		}
		if (DoomLib::GetPlayer() >= 0) {
			::g->mus_looping = looping;
		}
		 return;
		
	}

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
		// Do not continue with whatever AL_LOOPING state the previous track
		// left behind. Preserve this requested track for the bounded hardware
		// recovery path just like detach/upload/start failures.
		currentMusicName = songname != NULL ? songname : "";
		currentMusicLooping = looping;
		I_HandleMusicHardwareFailure();
		return;
	}

	musicReady = false;
	I_LoadSong(songname);
	waitingForMusic = musicReady;

	if (musicReady) {
		currentMusicName = songname;
		currentMusicLooping = looping;
	}

	if (DoomLib::GetPlayer() >= 0) {
		::g->mus_looping = looping;
	}
}

/*
======================
I_DisableMusicHardwareAfterFailure

The selected track has already consumed its one full sound-system recovery
attempt. Retire only Classic Doom's music hardware so a persistent source
failure cannot be retried/logged every frame. Classic SFX remain active.
======================
*/
static void I_DisableMusicHardwareAfterFailure()
{
	bool cleanupFailed = false;
	
	if (soundSystemLocal.hardware.IsContextCurrent()) {
		if (alMusicSourceVoice != 0) {
			alGetError();
			if (alIsSource(alMusicSourceVoice) == AL_TRUE) {
				alDeleteSources(1, &alMusicSourceVoice);
			}
			if (alGetError() != AL_NO_ERROR) {
				cleanupFailed = true;
			}
		}
		
		if (alMusicBuffer != 0) {
			alGetError();
			if (alIsBuffer(alMusicBuffer) == AL_TRUE) {
				alDeleteBuffers(1, &alMusicBuffer);
			}
			if (alGetError() != AL_NO_ERROR) {
				cleanupFailed = true;
			}
		}
	}
	
	alMusicSourceVoice = 0;
	alMusicBuffer = 0;
	
	if (musicBuffer != NULL) {
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
	
	// No automatic retry remains for this failed track. A later explicit
	// song request may initialize music again and starts a new recovery
	// episode.
	restoreMusicAfterHardwareRestart = false;
	musicHardwareRestartAttempted = false;
	musicInitRetryAttempted = true;
	musicNeedsExplicitRetry = !currentMusicName.IsEmpty();
	
	if (cleanupFailed) {
		// The track is deliberately not marked for restoration, so this
		// restart exists only to destroy the context and reclaim an object
		// that the driver refused to delete. It cannot form a restore loop.
		soundSystemLocal.SetNeedsRestart();
	}
}

/*
======================
I_HandleMusicHardwareFailure

Give a selected Classic music track one full sound-system restart after an
OpenAL detach/upload/start failure. If the restored track fails again on the
new context, do not create an endless restart loop.
======================
*/
static void I_HandleMusicHardwareFailure()
{
	musicReady = false;
	waitingForMusic = false;
	
	if (!currentMusicName.IsEmpty() && !musicHardwareRestartAttempted) {
		restoreMusicAfterHardwareRestart = true;
		musicHardwareRestartAttempted = true;
		soundSystemLocal.SetNeedsRestart();
	}
	else {
		// The replacement context has also failed (or there is no selected
		// track to restore). Do not leave the bad music source alive for
		// I_UpdateMusic() to touch again every frame.
		if (musicHardwareRestartAttempted) {
			I_DisableMusicHardwareAfterFailure();
		}
		else {
			restoreMusicAfterHardwareRestart = false;
		}
	}
}

/*
======================
I_UpdateMusic
======================
*/
void I_UpdateMusic(void)
{
	if (!I_ValidateSoundHardwareContext()) {
		return;
	}

	if (!Music_initialized) {
		// The one post-init retry has now been consumed. Keep the
		// high-level selected-track identity, but allow a future
		// explicit S_ChangeMusic() request for that same track to reach
		// I_PlaySong() instead of being suppressed forever.
		if (musicInitRetryAttempted && !currentMusicName.IsEmpty()) {
			restoreMusicAfterHardwareRestart = false;
			musicNeedsExplicitRetry = true;
		}
		// A hardware restart initializes Classic music before
		// soundHardwareInitialized is published. If source/buffer creation
		// failed at that point, give the retained logical track one bounded
		// retry after the new context is fully live. Do not retry every frame.
		if (restoreMusicAfterHardwareRestart &&
			!musicInitRetryAttempted &&
			soundHardwareInitialized &&
			S_initialized &&
			!currentMusicName.IsEmpty()) {
			musicInitRetryAttempted = true;
			I_InitMusic();
		}
		 if (!Music_initialized) {
			return;
		}
	}

	// A decoded song can be waiting for its OpenAL upload when the game is
	// paused. Keep it pending rather than starting playback underneath the
	// pause state; I_ResumeSong() leaves pending songs for this path to start.
	if (DoomLib::GetPlayer() >= 0 && ::g->mus_paused) {
		return;
	}

	if (alMusicSourceVoice) {
		// Set the volume
		alGetError();
		alSourcef(alMusicSourceVoice, AL_GAIN, x_MusicVolume * GLOBAL_VOLUME_MULTIPLIER);
		const ALenum gainError = alGetError();
		if (gainError != AL_NO_ERROR) {
			printf("[doomclassic] failed to update music gain: 0x%X\n", gainError);
			I_HandleMusicHardwareFailure();
			return;
		}
	}

	if (!waitingForMusic) {
		// Once a selected track has started, its source should remain PLAYING
		// until a non-looping track reaches AL_STOPPED. A state-query failure,
		// or an unexpected stopped/initial/paused looping source while the
		// game itself is not paused, means the music hardware is no longer
		// trustworthy enough to leave the logical track selected silently.
		if (!currentMusicName.IsEmpty() && alMusicSourceVoice) {
			ALint sourceState = AL_INITIAL;
			alGetError();
			alGetSourcei(alMusicSourceVoice, AL_SOURCE_STATE, &sourceState);
			const ALenum stateError = alGetError();

			if (stateError != AL_NO_ERROR) {
				printf("[doomclassic] failed to query active music source state: 0x%X\n", stateError);
				I_HandleMusicHardwareFailure();
				return;
			}
			
			if (!currentMusicLooping && sourceState == AL_STOPPED) {
				// Natural completion of a one-shot track.
				currentMusicName.Clear();
				currentMusicLooping = 0;
				restoreMusicAfterHardwareRestart = false;
				musicReady = false;
			}
			else if (sourceState != AL_PLAYING) {
				printf("[doomclassic] active music source entered unexpected state 0x%X; requesting recovery\n", sourceState);
				I_HandleMusicHardwareFailure();
				return;
			}
		}
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
		I_HandleMusicHardwareFailure();
		return;
	}
	
	alBufferData(alMusicBuffer, MIDI_SAMPLETYPE, musicBuffer, totalBufferSize, MIDI_RATE);
	alError = alGetError();
	if (alError != AL_NO_ERROR) {
		printf("[doomclassic] failed to upload music buffer: 0x%X\n", alError);
		I_HandleMusicHardwareFailure();
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
		I_HandleMusicHardwareFailure();
		return;
	}
	
	waitingForMusic = false;
	restoreMusicAfterHardwareRestart = false;
	musicHardwareRestartAttempted = false;
	musicInitRetryAttempted = false;
	musicNeedsExplicitRetry = false;
}

/*
======================
I_PauseSong
======================
*/
void I_PauseSong(int handle)
{
	if (!Music_initialized || !alMusicSourceVoice || !I_ValidateSoundHardwareContext()) {
		return;
	}

	// A pending song has not been attached or started yet. I_UpdateMusic()
	// observes ::g->mus_paused and will leave it pending until resume.
	if (waitingForMusic) {
		return;
	}

	// Only a source that is actually playing can be meaningfully paused.
	// In particular, do not turn a naturally completed non-looping track
	// into something that I_ResumeSong() could accidentally restart.
	ALint sourceState = AL_INITIAL;
	alGetError();
	alGetSourcei(alMusicSourceVoice, AL_SOURCE_STATE, &sourceState);
	const ALenum stateError = alGetError();
	if (stateError != AL_NO_ERROR) {
		printf("[doomclassic] failed to query music source before pause: 0x%X\n", stateError);
		I_HandleMusicHardwareFailure();
		return;
	}
	
	if (sourceState == AL_PLAYING) {
		alGetError();
		alSourcePause(alMusicSourceVoice);
		const ALenum pauseError = alGetError();
		if (pauseError != AL_NO_ERROR) {
			printf("[doomclassic] failed to pause music source: 0x%X\n", pauseError);
			I_HandleMusicHardwareFailure();
		}
	}
}

/*
======================
I_ResumeSong
======================
*/
void I_ResumeSong(int handle)
{
	if (!Music_initialized || !alMusicSourceVoice || !I_ValidateSoundHardwareContext()) {
		return;
	}

	// Pending music will be started by I_UpdateMusic() once S_ResumeSound()
	// clears ::g->mus_paused.
	if (waitingForMusic) {
		return;
	}

	// OpenAL restarts a stopped static-buffer source from the beginning when
	// alSourcePlay() is called. Resume only a source that was genuinely
	// paused; a completed non-looping song must remain completed.
	ALint sourceState = AL_INITIAL;
	alGetError();
	alGetSourcei(alMusicSourceVoice, AL_SOURCE_STATE, &sourceState);
	const ALenum stateError = alGetError();
	if (stateError != AL_NO_ERROR) {
		printf("[doomclassic] failed to query music source before resume: 0x%X\n", stateError);
		I_HandleMusicHardwareFailure();
		return;
	}
	
	if (sourceState == AL_PAUSED) {
		alGetError();
		alSourcePlay(alMusicSourceVoice);
		const ALenum resumeError = alGetError();
		if (resumeError != AL_NO_ERROR) {
			printf("[doomclassic] failed to resume music source: 0x%X\n", resumeError);
			I_HandleMusicHardwareFailure();
		}
	}
}

/*
======================
I_StopSong
======================
*/
bool I_StopSong(int handle)
{
	// This is a logical stop, not a hardware-only teardown. Do not allow the
	// stopped track to be resurrected by a later sound-system restart.
	currentMusicName.Clear();
	currentMusicLooping = 0;
	restoreMusicAfterHardwareRestart = false;
	musicNeedsExplicitRetry = false;

	if (!restoringMusicAfterHardwareRestart) {
		musicHardwareRestartAttempted = false;
		musicInitRetryAttempted = false;
	}
	
	// Cancel a decoded-but-not-yet-uploaded song as well as an already playing
	// source. Otherwise a later I_UpdateMusic() could start music after Stop.
	waitingForMusic = false;
	musicReady = false;

	if (!Music_initialized) {
		return true;
	}

	if (!I_ValidateSoundHardwareContext()) {
		return false;
	}
	
	if (!alMusicSourceVoice) {
		return true;
	}
	alGetError();
	alSourceStop(alMusicSourceVoice);
	const ALenum stopError = alGetError();
	if (stopError != AL_NO_ERROR) {
		printf("[doomclassic] failed to stop music source: 0x%X; requesting sound restart\n", stopError);
		// This was a logical stop, so do not restore the track. A full
		// context restart is used only to guarantee that any ghost playback
		// from the failed stop is torn down.
		soundSystemLocal.SetNeedsRestart();
		return false;
	}

	return true;
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

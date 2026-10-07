/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans
Copyright (c) 2010 by Chris Robinson <chris.kcat@gmail.com> (OpenAL Info Utility)

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
#include "../../../doomclassic/doom/i_sound.h"

idCVar s_device( "s_device", "-1", CVAR_INTEGER | CVAR_ARCHIVE, "Which audio device to use (listDevices to list, -1 for default)" );
extern idCVar s_volume_dB;

// ALC_EXT_disconnect defines ALC_CONNECTED as 0x313. Keep the token local
// instead of requiring AL/alext.h; the query is only used when the device
// explicitly reports support for ALC_EXT_disconnect.
static const ALCenum ALC_CONNECTED_EXT = 0x313;

/*
========================
idSoundHardware_OpenAL::idSoundHardware_OpenAL
========================
*/
idSoundHardware_OpenAL::idSoundHardware_OpenAL()
{
	openalDevice = NULL;
	openalContext = NULL;
	disconnectExtensionAvailable = false;
	
	voices.SetNum( 0 );
	freeVoices.SetNum( 0 );
	
	lastResetTime = 0;
}

/*
========================
OpenAL_GetPlaybackDeviceList
========================
*/
static const ALCchar * OpenAL_GetPlaybackDeviceList()
{
	if( alcIsExtensionPresent( NULL, "ALC_ENUMERATE_ALL_EXT" ) != ALC_FALSE )
	{
		return alcGetString( NULL, ALC_ALL_DEVICES_SPECIFIER );
	}
	
	if( alcIsExtensionPresent( NULL, "ALC_ENUMERATION_EXT" ) != ALC_FALSE )
	{
		return alcGetString( NULL, ALC_DEVICE_SPECIFIER );
	}
	
	return NULL;
}

/*
========================
OpenAL_GetPlaybackDeviceName
========================
*/
static const ALCchar * OpenAL_GetPlaybackDeviceName( int deviceIndex )
{
	if( deviceIndex < 0 )
	{
		return NULL;
	}
	
	const ALCchar * deviceList = OpenAL_GetPlaybackDeviceList();
	if( deviceList == NULL || *deviceList == '\0' )
	{
		return NULL;
	}
	
	for( int index = 0; *deviceList != '\0'; index++ )
	{
		if( index == deviceIndex )
		{
			return deviceList;
		}
		
		deviceList += strlen(deviceList) + 1;
	}
	
	return NULL;
}

void idSoundHardware_OpenAL::PrintDeviceList( const char* list )
{
	if( !list || *list == '\0' )
	{
		idLib::Printf( "    !!! none !!!\n" );
	}
	else
	{
		int deviceIndex = 0;
		do
		{
			idLib::Printf("    %d: %s\n", deviceIndex, list);
			list += strlen( list ) + 1;
			deviceIndex++;
		}
		while( *list != '\0' );
	}
}

void idSoundHardware_OpenAL::PrintALCInfo( ALCdevice* device )
{
	ALCint major, minor;
	
	if( device )
	{
		const ALCchar* devname = NULL;
		idLib::Printf( "\n" );
		if( alcIsExtensionPresent( device, "ALC_ENUMERATE_ALL_EXT" ) != AL_FALSE )
		{
			devname = alcGetString( device, ALC_ALL_DEVICES_SPECIFIER );
		}
		
		if( CheckALCErrors( device ) != ALC_NO_ERROR || !devname )
		{
			devname = alcGetString( device, ALC_DEVICE_SPECIFIER );
		}
		
		idLib::Printf( "** Info for device \"%s\" **\n", devname );
	}
	alcGetIntegerv( device, ALC_MAJOR_VERSION, 1, &major );
	alcGetIntegerv( device, ALC_MINOR_VERSION, 1, &minor );
	
	if( CheckALCErrors( device ) == ALC_NO_ERROR )
		idLib::Printf( "ALC version: %d.%d\n", major, minor );
		
	if( device )
	{
		idLib::Printf( "OpenAL extensions: %s", alGetString( AL_EXTENSIONS ) );
		
		//idLib::Printf("ALC extensions:");
		//printList(alcGetString(device, ALC_EXTENSIONS), ' ');
		CheckALCErrors( device );
	}
}

void idSoundHardware_OpenAL::PrintALInfo()
{
	idLib::Printf( "OpenAL vendor string: %s\n", alGetString( AL_VENDOR ) );
	idLib::Printf( "OpenAL renderer string: %s\n", alGetString( AL_RENDERER ) );
	idLib::Printf( "OpenAL version string: %s\n", alGetString( AL_VERSION ) );
	idLib::Printf( "OpenAL extensions: %s", alGetString( AL_EXTENSIONS ) );
	//PrintList(alGetString(AL_EXTENSIONS), ' ');
	CheckALErrors();
}

void listDevices_f( const idCmdArgs& args )
{
	idLib::Printf( "Available playback devices:\n" );
	idSoundHardware_OpenAL::PrintDeviceList(OpenAL_GetPlaybackDeviceList());
	
	if( alcIsExtensionPresent( NULL, "ALC_ENUMERATE_ALL_EXT" ) != AL_FALSE )
	{
		idLib::Printf( "Default playback device: %s\n", alcGetString( NULL, ALC_DEFAULT_ALL_DEVICES_SPECIFIER ) );
	}
	else
	{
		idLib::Printf( "Default playback device: %s\n",  alcGetString( NULL, ALC_DEFAULT_DEVICE_SPECIFIER ) );
	}
	
	//idLib::Printf("Default capture device: %s\n", alcGetString(NULL, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER));
	
	idSoundHardware_OpenAL::PrintALCInfo( NULL );
	
	idSoundHardware_OpenAL::PrintALCInfo( ( ALCdevice* )soundSystem->GetOpenALDevice() );
}

/*
========================
idSoundHardware_OpenAL::Init
========================
*/
void idSoundHardware_OpenAL::Init()
{
	cmdSystem->AddCommand( "listDevices", listDevices_f, 0, "Lists the connected sound devices", NULL );
	
	common->Printf( "Setup OpenAL device and context... " );
	
	const int requestedDeviceIndex = s_device.GetInteger();
	const ALCchar * requestedDeviceName = NULL;
	
	if( requestedDeviceIndex >= 0 )
	{
		requestedDeviceName = OpenAL_GetPlaybackDeviceName( requestedDeviceIndex );
		if( requestedDeviceName == NULL )
		{
			idLib::Warning( "OpenAL device index %d is unavailable; using the default playback device", requestedDeviceIndex );
		}
	}
	
	openalDevice = alcOpenDevice( requestedDeviceName );

	if( openalDevice == NULL && requestedDeviceName != NULL )
	{
		idLib::Warning( "Could not open OpenAL device %d (\"%s\"); using the default playback device", requestedDeviceIndex, requestedDeviceName );
		openalDevice = alcOpenDevice(NULL);
	}

	if( openalDevice == NULL )
	{
		idLib::Warning("idSoundHardware_OpenAL::Init: alcOpenDevice() failed; sound will remain unavailable until a device can be opened");
		lastResetTime = Sys_Milliseconds();
		disconnectExtensionAvailable = false;
		return;
	}

	disconnectExtensionAvailable = (alcIsExtensionPresent(openalDevice, "ALC_EXT_disconnect") != ALC_FALSE);
	
	openalContext = alcCreateContext( openalDevice, NULL );

	if( openalContext == NULL )
	{
		alcCloseDevice( openalDevice );
		openalDevice = NULL;
		idLib::Warning("idSoundHardware_OpenAL::Init: alcCreateContext() failed; sound will retry later");
		lastResetTime = Sys_Milliseconds();
		disconnectExtensionAvailable = false;
		return;
	}
	
	if( alcMakeContextCurrent( openalContext ) == ALC_FALSE )
	{
		alcDestroyContext( openalContext );
		openalContext = NULL;
		alcCloseDevice( openalDevice );
		openalDevice = NULL;
		idLib::Warning("idSoundHardware_OpenAL::Init: alcMakeContextCurrent() failed; sound will retry later");
		lastResetTime = Sys_Milliseconds();
		disconnectExtensionAvailable = false;
		return;
	}

	// Extension availability and procedure addresses are context-dependent.
	// Start every hardware context with fresh OpenAL extension caches.
	OpenAL_ResetContextCaches();
	OpenAL_ResetSampleContextCaches();
	
	common->Printf( "Done.\n" );
	
	common->Printf( "OpenAL vendor: %s\n", alGetString( AL_VENDOR ) );
	common->Printf( "OpenAL renderer: %s\n", alGetString( AL_RENDERER ) );
	common->Printf( "OpenAL version: %s\n", alGetString( AL_VERSION ) );
	common->Printf( "OpenAL extensions: %s\n", alGetString( AL_EXTENSIONS ) );
	
	// ---------------------
	// Initialize the Doom classic sound system.
	// ---------------------
	I_InitSoundHardware(voices.Max(), 0);
		
	// OpenAL doesn't really impose a maximum number of sources
	voices.SetNum( voices.Max() );
	freeVoices.SetNum( voices.Max() );
	for( int i = 0; i < voices.Num(); i++ )
	{
		freeVoices[i] = &voices[i];
	}

	// This context now reflects the currently selected playback device.
	// Runtime changes are handled by Update() through the normal full sound
	// restart path so all context-local OpenAL buffers are rebuilt safely.
	s_device.ClearModified();
}

/*
========================
idSoundHardware_OpenAL::Shutdown
========================
*/
void idSoundHardware_OpenAL::Shutdown()
{
	// All OpenAL sources and buffers must be released while this context is
	// still valid and current.
	bool contextCurrent = false;
	
	if( openalContext != NULL )
	{
		if( alcGetCurrentContext() == openalContext )
		{
			contextCurrent = true;
		}
		else if( alcMakeContextCurrent(openalContext) != ALC_FALSE )
		{
			contextCurrent = true;
		}
		else
		{
			idLib::Warning("idSoundHardware_OpenAL::Shutdown: could not make OpenAL context current; invalidating cached AL objects");
		}
	}

	if (contextCurrent)
	{
		for (int i = 0; i < voices.Num(); i++)
		{
			voices[i].DestroyInternal();
		}
		
		// ---------------------
		// Shutdown the Doom classic sound system while the OpenAL context is
		// still current. I_ShutdownSoundHardware() deletes its OpenAL sources
		// and buffers.
		// ---------------------
		I_ShutdownSoundHardware();
	}
	else
	{
		// Never issue AL calls against no context or an unrelated context.
		// alcDestroyContext() below will reclaim the context-owned AL objects.
		for (int i = 0; i < voices.Num(); i++)
		{
			voices[i].InvalidateContextObjects();
		}
		
		I_InvalidateSoundHardware();
	}

	voices.Clear();
	freeVoices.Clear();

	// Never carry extension state or procedure pointers across a context
	// destruction/restart, even if the OpenAL implementation later reuses the
	// same ALCcontext address.
	OpenAL_ResetContextCaches();
	OpenAL_ResetSampleContextCaches();
	
	if( openalContext != NULL )
	{
		if( contextCurrent && alcGetCurrentContext() == openalContext )
		{
			alcMakeContextCurrent(NULL);
		}
		
		alcDestroyContext( openalContext );
		openalContext = NULL;
	}
	
	if( openalDevice != NULL )
	{
		alcCloseDevice( openalDevice );
		openalDevice = NULL;
	}

	disconnectExtensionAvailable = false;
}

/*
========================
idSoundHardware_OpenAL::AllocateVoice
========================
*/
idSoundVoice* idSoundHardware_OpenAL::AllocateVoice( const idSoundSample* leadinSample, const idSoundSample* loopingSample )
{
	if( leadinSample == NULL )
	{
		return NULL;
	}
	if( loopingSample != NULL )
	{
		// OpenAL sources may be rebound to a different static-buffer format
		// after they stop. RestartAt()/Update() queue compatible pairs and use
		// a static-buffer transition for incompatible channel/rate formats, so
		// preserve the requested loop instead of silently dropping it.
	}
	
	// Prefer a free voice that already owns an OpenAL source. Some OpenAL
	// devices expose fewer sources than MAX_HARDWARE_VOICES; if an unused
	// voice at the front of freeVoices hits that source limit, returning
	// immediately would hide reusable source objects later in the list and
	// could starve all subsequent allocations.
	idSoundVoice_OpenAL * unusedVoice = NULL;
	for( int i = 0; i < freeVoices.Num(); i++ )
	{
		idSoundVoice_OpenAL* candidate = freeVoices[i];
		if (candidate->IsPlaying())
		{
			continue;
		}
		if (!candidate->CompatibleFormat((idSoundSample_OpenAL*)leadinSample))
		{
			continue;
		}

		if (candidate->openalSource == 0)
		{
			if (unusedVoice == NULL)
			{
				unusedVoice = candidate;
			}
			continue;
		}
		
		if (!candidate->Create(leadinSample, loopingSample))
		{
			idLib::Warning("OpenAL failed to reuse voice for %s", leadinSample->GetName());
			continue;
		}
		
		freeVoices.Remove(candidate);
		return (idSoundVoice*)candidate;
	}
	// No reusable source was available. Try to instantiate one unused voice.
	// If source creation fails here, trying every other unused slot would hit
	// the same device source limit and only generate repeated AL errors.
	if (unusedVoice != NULL)
	{
		if (!unusedVoice->Create(leadinSample, loopingSample))
		{
			idLib::Warning("OpenAL failed to create voice for %s", leadinSample->GetName());
			return NULL;
		}
		freeVoices.Remove(unusedVoice);
		return (idSoundVoice*)unusedVoice;
	}
	
	return NULL;
}

/*
========================
idSoundHardware_OpenAL::FreeVoice
========================
*/
void idSoundHardware_OpenAL::FreeVoice( idSoundVoice* voice )
{
	if( voice == NULL )
	{
		return;
	}

	voice->Stop();
	
	// OpenAL Stop()/FlushSourceBuffers() is synchronous. The source is stopped
	// and its buffer/queue has already been detached, so the voice can be
	// recycled immediately.
	for( int i = 0; i < freeVoices.Num(); i++ )
	{
		if( freeVoices[i] == voice )
		{
			idLib::Warning( "idSoundHardware_OpenAL::FreeVoice: voice already free" );
			return;
		}
	}
	freeVoices.Append( voice );
}

/*
========================
idSoundHardware_OpenAL::ReleaseFreeVoiceResources
========================
*/
void idSoundHardware_OpenAL::ReleaseFreeVoiceResources()
{
	// Free only source objects that are not owned by an active sound channel.
	// This is used when Classic Doom starts so its lazily-created sources can
	// share the same device source budget with BFG without destroying voices
	// that are still playing. The BFG allocator recreates these source objects
	// lazily the next time the corresponding free voice is needed.
	for (int i = 0; i < freeVoices.Num(); ++i)
	{
		freeVoices[i]->DestroyInternal();
	}
}

/*
========================
idSoundHardware_OpenAL::Update
========================
*/
void idSoundHardware_OpenAL::Update()
{
	// s_device is an archived runtime CVar, but opening another OpenAL device
	// requires a new device/context pair. Never swap the context directly
	// here because every resident idSoundSample owns a context-local AL buffer
	// name. Request the sound system's normal restart, which mutes voices,
	// tears down the old context, and re-uploads resident samples afterward.
	if (s_device.IsModified())
	{
		s_device.ClearModified();
		soundSystemLocal.SetNeedsRestart();
		return;
	}

	// Match the original hardware backend's graceful "no audio device"
	// behavior. A failed OpenAL init leaves no usable context, but the rest of
	// the sound system can continue with CPU-resident samples and no hardware
	// voices. Retry through the full sound-system restart at a bounded rate so
	// all context-local sample/Classic objects are rebuilt when a device
	// becomes available.
	if (openalDevice == NULL || openalContext == NULL)
	{
		const int nowTime = Sys_Milliseconds();
		if (lastResetTime + 1000 < nowTime)
		{
			lastResetTime = nowTime;
			idLib::Warning("OpenAL device/context unavailable; requesting sound restart");
			soundSystemLocal.SetNeedsRestart();
		}
		return;
	}

	// All source/buffer names owned by this backend belong to openalContext.
	// If another/no context became current, never issue AL calls into it.
	// Recover through the same bounded full-restart path.
	if (alcGetCurrentContext() != openalContext)
	{
		const int nowTime = Sys_Milliseconds();
		if (lastResetTime + 1000 < nowTime)
		{
			lastResetTime = nowTime;
			idLib::Warning("OpenAL context is no longer current; requesting sound restart");
			soundSystemLocal.SetNeedsRestart();
		}
		return;
	}

	if (disconnectExtensionAvailable)
	{
		ALCint connected = ALC_TRUE;
		
		// ALC errors are sticky. Isolate this query so an unrelated earlier
		// error cannot make device-loss detection unreliable.
		alcGetError(openalDevice);
		alcGetIntegerv(openalDevice, ALC_CONNECTED_EXT, 1, &connected);
		const ALCenum connectionError = alcGetError(openalDevice);
		
		if (connectionError == ALC_NO_ERROR && connected == ALC_FALSE)
		{
			const int nowTime = Sys_Milliseconds();
			if (lastResetTime + 1000 < nowTime)
			{
				lastResetTime = nowTime;
				idLib::Warning("OpenAL playback device disconnected; requesting sound restart");
				soundSystemLocal.SetNeedsRestart();
			}
			return;
		}
	}

	// Isolate the listener update from sticky AL errors left by unrelated
	// operations. AL_GAIN is valid for every OpenAL listener, so an error here
	// indicates that the context/device is no longer usable enough to trust.
	CheckALErrors();
	
	if( soundSystem->IsMuted() )
	{
		alListenerf( AL_GAIN, 0.0f );
	}
	else
	{
		alListenerf( AL_GAIN, DBtoLinear( s_volume_dB.GetFloat() ) );
	}

	if (CheckALErrors() != AL_NO_ERROR)
	{
		// Treat a failed core listener operation like the other device/context
		// failure paths above. A persistently broken replacement context must
		// not turn into a full sound restart every rendered frame.
		const int nowTime = Sys_Milliseconds();
		if (lastResetTime + 1000 < nowTime)
		{
			lastResetTime = nowTime;
			idLib::Warning("OpenAL listener update failed; requesting sound restart");
			soundSystemLocal.SetNeedsRestart();
		}
		return;
	}
}



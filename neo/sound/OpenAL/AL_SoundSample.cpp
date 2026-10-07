/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans
Copyright (C) 1997-2012 Sam Lantinga <slouken@libsdl.org>  (MS ADPCM decoder)
Copyright (c) 2011 Chris Robinson <chris.kcat@gmail.com> (OpenAL helpers)

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

extern idCVar s_useCompression;
extern idCVar s_noSound;

#define GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( x ) x

const uint32 SOUND_MAGIC_IDMSA = 0x6D7A7274;

extern idCVar sys_lang;

static ALCcontext* openalMCFormatsContext = NULL;
static bool openalMCFormatsAvailable = false;
static ALenum openalQuad16Format = AL_NONE;
static ALenum openal51Chn16Format = AL_NONE;
static ALenum openal61Chn16Format = AL_NONE;
static ALenum openal71Chn16Format = AL_NONE;

/*
========================
OpenAL_ValidateExtensibleChannelMask

WAVE_FORMAT_EXTENSIBLE stores interleaved channels in ascending speaker-bit
order. Only accept layouts whose ordering matches AL_EXT_MCFORMATS directly.
For 5.1, OpenAL treats rear/surround pairs equivalently, so both common WAVE
masks are safe without reordering.
========================
*/
static bool OpenAL_ValidateExtensibleChannelMask(const idWaveFile::waveFmt_t & waveFormat)
{
	if (waveFormat.basic.formatTag != idWaveFile::FORMAT_EXTENSIBLE)
	{
		return true;
	}
	
	const uint32 mask = waveFormat.extra.extensible.channelMask;
	const uint32 frontStereo =
		idWaveFile::CHANNEL_MASK_FRONT_LEFT |
		idWaveFile::CHANNEL_MASK_FRONT_RIGHT;
	const uint32 frontCenterLFE =
		frontStereo |
		idWaveFile::CHANNEL_MASK_FRONT_CENTER |
		idWaveFile::CHANNEL_MASK_LOW_FREQUENCY;
	
	switch (waveFormat.basic.numChannels)
	{
		case 1:
		case 2:
			// Standard OpenAL mono/stereo formats do not need MCFORMATS.
			return true;
			
		case 4:
			return mask == (
				frontStereo |
				idWaveFile::CHANNEL_MASK_BACK_LEFT |
				idWaveFile::CHANNEL_MASK_BACK_RIGHT);
			
		case 6:
		{
			const uint32 rear51 =
				frontCenterLFE |
				idWaveFile::CHANNEL_MASK_BACK_LEFT |
				idWaveFile::CHANNEL_MASK_BACK_RIGHT;
			const uint32 side51 =
				frontCenterLFE |
				idWaveFile::CHANNEL_MASK_SIDE_LEFT |
				idWaveFile::CHANNEL_MASK_SIDE_RIGHT;
			return mask == rear51 || mask == side51;
		}
		
		case 7:
			return mask == (
				frontCenterLFE |
				idWaveFile::CHANNEL_MASK_BACK_CENTER |
				idWaveFile::CHANNEL_MASK_SIDE_LEFT |
				idWaveFile::CHANNEL_MASK_SIDE_RIGHT);
			
		case 8:
			return mask == (
				frontCenterLFE |
				idWaveFile::CHANNEL_MASK_BACK_LEFT |
				idWaveFile::CHANNEL_MASK_BACK_RIGHT |
				idWaveFile::CHANNEL_MASK_SIDE_LEFT |
				idWaveFile::CHANNEL_MASK_SIDE_RIGHT);
			
		default:
			return false;
	}
}

/*
========================
OpenAL_ResetSampleContextCaches
========================
*/
void OpenAL_ResetSampleContextCaches()
{
	openalMCFormatsContext = NULL;
	openalMCFormatsAvailable = false;
	openalQuad16Format = AL_NONE;
	openal51Chn16Format = AL_NONE;
	openal61Chn16Format = AL_NONE;
	openal71Chn16Format = AL_NONE;
}

/*
========================
OpenAL_GetMultichannel16Format
========================
*/
static ALenum OpenAL_GetMultichannel16Format(int numChannels)
{
	ALCcontext * context = alcGetCurrentContext();
	if (context == NULL)
	{
		OpenAL_ResetSampleContextCaches();
		return AL_NONE;
	}
	
	if (context != openalMCFormatsContext)
	{
		openalMCFormatsContext = context;
		openalMCFormatsAvailable = false;
		openalQuad16Format = AL_NONE;
		openal51Chn16Format = AL_NONE;
		openal61Chn16Format = AL_NONE;
		openal71Chn16Format = AL_NONE;
		
		CheckALErrors();
		if (alIsExtensionPresent("AL_EXT_MCFORMATS") == AL_TRUE)
		{
			openalQuad16Format = alGetEnumValue("AL_FORMAT_QUAD16");
			openal51Chn16Format = alGetEnumValue("AL_FORMAT_51CHN16");
			openal61Chn16Format = alGetEnumValue("AL_FORMAT_61CHN16");
			openal71Chn16Format = alGetEnumValue("AL_FORMAT_71CHN16");
			
			if (CheckALErrors() == AL_NO_ERROR)
			{
							// Some implementations historically returned -1 for unknown
								// enums, so require every value to be a positive AL enum.
				openalMCFormatsAvailable =
				openalQuad16Format > AL_NONE &&
				openal51Chn16Format > AL_NONE &&
				openal61Chn16Format > AL_NONE &&
				openal71Chn16Format > AL_NONE;
			}
		}
		else
		{
			CheckALErrors();
		}
		
		if (!openalMCFormatsAvailable)
		{
			openalQuad16Format = AL_NONE;
			openal51Chn16Format = AL_NONE;
			openal61Chn16Format = AL_NONE;
			openal71Chn16Format = AL_NONE;
		}
	}
	
	if (!openalMCFormatsAvailable)
	{
		return AL_NONE;
	}
	
	switch (numChannels)
	{
		case 4:
			return openalQuad16Format;
		case 6:
			return openal51Chn16Format;
		case 7:
			return openal61Chn16Format;
		case 8:
			return openal71Chn16Format;
		default:
			return AL_NONE;
	}
}

/*
========================
AllocBuffer
========================
*/
static void* AllocBuffer( int size, const char* name )
{
	return Mem_Alloc( size, TAG_AUDIO );
}

/*
========================
FreeBuffer
========================
*/
static void FreeBuffer( void* p )
{
	return Mem_Free( p );
}

/*
========================
idSoundSample_OpenAL::idSoundSample_OpenAL
========================
*/
idSoundSample_OpenAL::idSoundSample_OpenAL()
{
	timestamp = FILE_NOT_FOUND_TIMESTAMP;
	loaded = false;
	neverPurge = false;
	levelLoadReferenced = false;
	
	memset( &format, 0, sizeof( format ) );
	
	totalBufferSize = 0;
	
	playBegin = 0;
	playLength = 0;
	
	lastPlayedTime = 0;
	
	openalBuffer = 0;
	openalDataDecoded = false;
	defaultedForNoSound = false;
}

/*
========================
idSoundSample_OpenAL::~idSoundSample_OpenAL
========================
*/
idSoundSample_OpenAL::~idSoundSample_OpenAL()
{
	FreeData();
}

/*
========================
idSoundSample_OpenAL::WriteGeneratedSample
========================
*/
bool idSoundSample_OpenAL::WriteGeneratedSample(idFile* fileOut)
{
	if (fileOut == NULL)
	{
		return false;
	}

	if (fileOut->WriteBig(SOUND_MAGIC_IDMSA) != sizeof(SOUND_MAGIC_IDMSA) ||
		fileOut->WriteBig(timestamp) != sizeof(timestamp) ||
		fileOut->WriteBig(loaded) != sizeof(loaded) ||
		fileOut->WriteBig(playBegin) != sizeof(playBegin) ||
		fileOut->WriteBig(playLength) != sizeof(playLength))
	{
		return false;
	}
	
	if (!idWaveFile::WriteWaveFormatDirect(format, fileOut))
	{
		return false;
	}
	
	const int amplitudeSize = amplitude.Num();
	if (fileOut->WriteBig(amplitudeSize) != sizeof(amplitudeSize) ||
		(amplitudeSize > 0 && fileOut->Write(amplitude.Ptr(), amplitudeSize) != amplitudeSize) ||
		fileOut->WriteBig(totalBufferSize) != sizeof(totalBufferSize))
	{
		return false;
	}
	
	const int numBuffers = buffers.Num();
	if (fileOut->WriteBig(numBuffers) != sizeof(numBuffers))
	{
		return false;
	}
	
	for (int i = 0; i < numBuffers; i++)
	{
		if (fileOut->WriteBig(buffers[i].numSamples) != sizeof(buffers[i].numSamples) ||
			fileOut->WriteBig(buffers[i].bufferSize) != sizeof(buffers[i].bufferSize) ||
			buffers[i].bufferSize <= 0 ||
			buffers[i].buffer == NULL ||
			fileOut->Write(buffers[i].buffer, buffers[i].bufferSize) != buffers[i].bufferSize)
		{
			return false;
		}
	}
	
	return true;
}

/*
========================
idSoundSample_OpenAL::WriteAllSamples
========================
*/
void idSoundSample_OpenAL::WriteAllSamples( const idStr& sampleName )
{
	idSoundSample_OpenAL* samplePC = new idSoundSample_OpenAL();
	{
		idStrStatic< MAX_OSPATH > inName = sampleName;
		inName.Append( ".msadpcm" );
		idStrStatic< MAX_OSPATH > inName2 = sampleName;
		inName2.Append( ".wav" );
		
		idStrStatic< MAX_OSPATH > outName = "generated/";
		outName.Append( sampleName );
		outName.Append( ".idwav" );

		idStrStatic< MAX_OSPATH > tempName = outName;
		tempName.Append(".tmp");
		
		if( samplePC->LoadWav( inName ) || samplePC->LoadWav( inName2 ) )
		{
			idFile* fileOut = fileSystem->OpenFileWrite(tempName, "fs_basepath");
			if (fileOut == NULL)
			{
				idLib::Warning( "idSoundSample_OpenAL::WriteAllSamples: could not open temporary cache '%s' for writing", tempName.c_str());
				delete samplePC;
				return;
			}
			const bool writeSucceeded = samplePC->WriteGeneratedSample(fileOut);
			delete fileOut;

			if (!writeSucceeded)
			{
				idLib::Warning(
					"idSoundSample_OpenAL::WriteAllSamples: short write while generating '%s'",
					outName.c_str());
				fileSystem->RemoveFile(tempName);
			}
			else if (!fileSystem->RenameFile(tempName, outName, "fs_basepath"))
			{
				idLib::Warning( "idSoundSample_OpenAL::WriteAllSamples: could not replace '%s' with completed temporary cache", outName.c_str());
				fileSystem->RemoveFile(tempName);
			}
		}
	}
	delete samplePC;
}

/*
========================
idSoundSample_OpenAL::LoadGeneratedSound
========================
*/
bool idSoundSample_OpenAL::LoadGeneratedSample( const idStr& filename )
{
#if 1
	idFileLocal fileIn( fileSystem->OpenFileReadMemory( filename ) );
	if( fileIn != NULL )
	{
		uint32 magic = 0;
		if (fileIn->ReadBig(magic) != sizeof(magic))
		{
			idLib::Warning("LoadGeneratedSample( %s ): truncated generated sample header", filename.c_str());
			return false;
		}

		if (magic != SOUND_MAGIC_IDMSA)
		{
			idLib::Warning( "LoadGeneratedSample( %s ): invalid sound cache magic 0x%08x", filename.c_str(), magic);
			return false;
		}

		// The serialized "loaded" flag is legacy metadata and is not a
		// reliable validity indicator.  WriteAllSamples() historically builds
		// generated samples from a temporary object via LoadWav(), while the
		// object's loaded member may still be false.  The original sound
		// backend therefore accepted generated files regardless of this bit
		// and treated successful parsing of the payload as authoritative.
		bool serializedLoaded = false;

		if (fileIn->ReadBig(timestamp) != sizeof(timestamp) ||
			fileIn->ReadBig(serializedLoaded) != sizeof(serializedLoaded) ||
			fileIn->ReadBig(playBegin) != sizeof(playBegin) ||
			fileIn->ReadBig(playLength) != sizeof(playLength))
		{
			idLib::Warning("LoadGeneratedSample( %s ): truncated generated sample header", filename.c_str());
			loaded = false;
			return false;
		}
		
		if (!idWaveFile::ReadWaveFormatDirect(format, fileIn))
		{
			idLib::Warning( "LoadGeneratedSample( %s ): invalid generated wave format", filename.c_str());
			loaded = false;
			return false;
		}

		// The OpenAL backend has no XMA2 decoder or upload format. Reject an
		// XMA2 cache here so LoadResource() can continue to its MS ADPCM/PCM
		// fallback instead of treating an unplayable sample as successfully
		// loaded.
		if (format.basic.formatTag == idWaveFile::FORMAT_XMA2)
		{
			idLib::Warning("LoadGeneratedSample( %s ): XMA2 is unsupported by the OpenAL backend", filename.c_str());
			FreeData();
			return false;
		}

		// New generated multichannel samples retain their extensible channel
		// mask. Older generated PCM samples have no mask to validate, so keep
		// accepting them for backward compatibility.
		if (format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE && !OpenAL_ValidateExtensibleChannelMask(format))
		{
			idLib::Warning(
				"LoadGeneratedSample( %s ): unsupported %d-channel mask 0x%08x",
				filename.c_str(),
				format.basic.numChannels,
				format.extra.extensible.channelMask);
			loaded = false;
			return false;
		}

		int num;

		if (fileIn->ReadBig(num) != sizeof(num) || num < 0 || num > fileIn->Length() - fileIn->Tell())
		{
			idLib::Warning("LoadGeneratedSample( %s ): invalid amplitude byte count", filename.c_str());
			FreeData();
			return false;
		}

		amplitude.Clear();
		amplitude.SetNum( num );

		if (num > 0 && fileIn->Read(amplitude.Ptr(), num) != num)
		{
			idLib::Warning("LoadGeneratedSample( %s ): truncated amplitude data", filename.c_str());
			FreeData();
			return false;
		}
		
		if (fileIn->ReadBig(totalBufferSize) != sizeof(totalBufferSize) || totalBufferSize <= 0)
		{
			idLib::Warning("LoadGeneratedSample( %s ): invalid total buffer size", filename.c_str());
			FreeData();
			return false;
		}
		
		int numBuffers;
		if (fileIn->ReadBig(numBuffers) != sizeof(numBuffers) || numBuffers <= 0 || numBuffers > (fileIn->Length() - fileIn->Tell()) / (2 * sizeof(int)))
		{
			idLib::Warning("LoadGeneratedSample( %s ): invalid buffer count", filename.c_str());
			FreeData();
			return false;
		}
		
		if (playBegin < 0 || playLength <= 0 || format.basic.numChannels == 0 || format.basic.samplesPerSec == 0 || format.basic.samplesPerSec > INT_MAX || format.basic.blockSize == 0)
		{
			idLib::Warning("LoadGeneratedSample( %s ): invalid sample metadata", filename.c_str());
			FreeData();
			return false;
		}

		const bool isPCM =
			format.basic.formatTag == idWaveFile::FORMAT_PCM ||
			format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE;
		const bool isADPCM = format.basic.formatTag == idWaveFile::FORMAT_ADPCM;
		
		if (isPCM)
		{
			const uint64 expectedBlockSize =
			static_cast<uint64>(format.basic.numChannels) * sizeof(int16);
			
			if (format.basic.bitsPerSample != 16 ||
				expectedBlockSize != format.basic.blockSize ||
				(totalBufferSize % format.basic.blockSize) != 0)
			{
				idLib::Warning("LoadGeneratedSample( %s ): invalid PCM sample geometry", filename.c_str());
				FreeData();
				return false;
			}
		}
		else if (isADPCM)
		{
			const uint32 channels = format.basic.numChannels;
			const uint32 headerBytes = 7u * channels;

			if ((channels != 1 && channels != 2) ||
				format.basic.bitsPerSample != 4 ||
				format.extra.adpcm.numCoef == 0 ||
				format.extra.adpcm.numCoef > 7 ||
				format.extra.adpcm.samplesPerBlock < 2 ||
				format.basic.blockSize < headerBytes ||
				(totalBufferSize % format.basic.blockSize) != 0)
			{
				idLib::Warning("LoadGeneratedSample( %s ): invalid MS ADPCM sample geometry", filename.c_str());
				FreeData();
				return false;
			}

			const uint32 payloadBytes = format.basic.blockSize - headerBytes;
			const uint32 expectedSamplesPerBlock = 2u + ((payloadBytes * 2u) / channels);

			if (format.extra.adpcm.samplesPerBlock != expectedSamplesPerBlock)
			{
				idLib::Warning("LoadGeneratedSample( %s ): inconsistent MS ADPCM block metadata", filename.c_str());
				FreeData();
				return false;
			}
		}
		buffers.Clear();
		uint64 accumulatedBufferSize = 0;
		
		for (int i = 0; i < numBuffers; i++)
		{
			sampleBuffer_t sampleBuffer;
			memset(&sampleBuffer, 0, sizeof(sampleBuffer));
			
			if (fileIn->ReadBig(sampleBuffer.numSamples) != sizeof(sampleBuffer.numSamples) ||
				fileIn->ReadBig(sampleBuffer.bufferSize) != sizeof(sampleBuffer.bufferSize) ||
				sampleBuffer.numSamples <= 0 ||
				sampleBuffer.bufferSize <= 0 ||
				sampleBuffer.bufferSize > fileIn->Length() - fileIn->Tell())
			{
				idLib::Warning("LoadGeneratedSample( %s ): invalid buffer %d metadata", filename.c_str(), i);
				FreeData();
				return false;
			}
			
			accumulatedBufferSize += static_cast<uint64>(sampleBuffer.bufferSize);
			if (accumulatedBufferSize > 0x7FFFFFFFULL)
			{
				idLib::Warning("LoadGeneratedSample( %s ): generated sample is too large", filename.c_str());
				FreeData();
				return false;
			}
			
			sampleBuffer.buffer = AllocBuffer(sampleBuffer.bufferSize, GetName());
			if (fileIn->Read(sampleBuffer.buffer, sampleBuffer.bufferSize) != sampleBuffer.bufferSize)
			{
				FreeBuffer(sampleBuffer.buffer);
				idLib::Warning("LoadGeneratedSample( %s ): truncated buffer %d data", filename.c_str(), i);
				FreeData();
				return false;
			}
			
			sampleBuffer.buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS(sampleBuffer.buffer);
			buffers.Append(sampleBuffer);
			
		}
		
		if (accumulatedBufferSize != static_cast<uint64>(totalBufferSize))
		{
			idLib::Warning( "LoadGeneratedSample( %s ): buffer sizes do not match declared total", filename.c_str());
			FreeData();
			return false;
		}

		// OpenAL's PCM and ADPCM upload paths operate on one contiguous CPU
		// buffer. Generated resources produced by LoadWav() use exactly one
		// buffer for these formats; accepting more would silently upload only
		// buffers[0] in release builds.
		if ((isPCM || isADPCM) && buffers.Num() != 1)
		{
			idLib::Warning("LoadGeneratedSample( %s ): unsupported multi-buffer PCM/ADPCM sample", filename.c_str());
			FreeData();
			return false;
		}
		
		if (isPCM)
		{
			const uint64 expectedBytes =
			static_cast<uint64>(buffers[0].numSamples) *
			static_cast<uint64>(format.basic.blockSize);
			
			if (expectedBytes != static_cast<uint64>(buffers[0].bufferSize))
			{
				idLib::Warning("LoadGeneratedSample( %s ): PCM sample count does not match buffer size", filename.c_str());
				FreeData();
				return false;
			}
		}
		else if (isADPCM)
		{
			const uint64 blockCount = static_cast<uint64>(totalBufferSize) / static_cast<uint64>(format.basic.blockSize);
			const uint64 expectedSamples = blockCount * static_cast<uint64>(format.extra.adpcm.samplesPerBlock);
			
			if (expectedSamples == 0 || expectedSamples > 0x7FFFFFFFULL || static_cast<uint64>(buffers[0].numSamples) != expectedSamples)
			{
				idLib::Warning("LoadGeneratedSample( %s ): MS ADPCM sample count does not match encoded data", filename.c_str());
				FreeData();
				return false;
			}
		}
		
		const uint64 playEnd = static_cast<uint64>(playBegin) + static_cast<uint64>(playLength);
		if (playEnd > static_cast<uint64>(buffers[buffers.Num() - 1].numSamples))
		{
			idLib::Warning("LoadGeneratedSample( %s ): play range exceeds available samples", filename.c_str());
			FreeData();
			return false;
		}

		loaded = true;
		return true;
	}

#endif
	
	return false;
}
/*
========================
idSoundSample_OpenAL::Load
========================
*/
void idSoundSample_OpenAL::LoadResource()
{
	FreeData();
	
	if( idStr::Icmpn( GetName(), "_default", 8 ) == 0 )
	{
		MakeDefault();
		return;
	}
	
	if( s_noSound.GetBool() )
	{
		MakeDefault();
		// Remember that this is only a temporary placeholder. If sound is
		// enabled later, Restart() reloads the real resource instead of
		// uploading this default beep into the new OpenAL context.
		defaultedForNoSound = true;
		return;
	}
	
	loaded = false;
	
	for( int i = 0; i < 2; i++ )
	{
		idStrStatic< MAX_OSPATH > sampleName = GetName();
		if( ( i == 0 ) && !sampleName.Replace( "/vo/", va( "/vo/%s/", sys_lang.GetString() ) ) )
		{
			i++;
		}
		idStrStatic< MAX_OSPATH > generatedName = "generated/";
		generatedName.Append( sampleName );
		
		{
			if( s_useCompression.GetBool() )
			{
				sampleName.Append( ".msadpcm" );
			}
			else
			{
				sampleName.Append( ".wav" );
			}
			generatedName.Append( ".idwav" );
		}
		loaded = LoadGeneratedSample( generatedName ) || LoadWav( sampleName );
		
		if( !loaded && s_useCompression.GetBool() )
		{
			sampleName.SetFileExtension( "wav" );
			loaded = LoadWav( sampleName );
		}
		
		if( loaded )
		{
			if( cvarSystem->GetCVarBool( "fs_buildresources" ) )
			{
				fileSystem->AddSamplePreload( GetName() );
				WriteAllSamples( GetName() );
				
				if( sampleName.Find( "/vo/" ) >= 0 )
				{
					for( int i = 0; i < Sys_NumLangs(); i++ )
					{
						const char* lang = Sys_Lang( i );
						if( idStr::Icmp( lang, ID_LANG_ENGLISH ) == 0 )
						{
							continue;
						}
						idStrStatic< MAX_OSPATH > locName = GetName();
						locName.Replace( "/vo/", va( "/vo/%s/", Sys_Lang( i ) ) );
						WriteAllSamples( locName );
					}
				}
			}
			
			// upload PCM data to OpenAL
			CreateOpenALBuffer();
			
			return;
		}
	}
	
	if( !loaded )
	{
		// make it default if everything else fails
		MakeDefault();
	}
	return;
}

void idSoundSample_OpenAL::CreateOpenALBuffer()
{
	if (!loaded || buffers.Num() == 0)
	{
		return;
	}

	// Resource loading can legitimately happen while sound hardware is
	// disabled (s_noSound) or before an OpenAL context is available. Keep the
	// CPU sample data resident and defer the hardware upload until a context
	// exists; Restart() will rebuild loaded samples when sound is enabled.
	if( alcGetCurrentContext() == NULL )
	{
		openalBuffer = 0;
		return;
	}

	const ALenum alFormat = GetOpenALBufferFormat();
	if (alFormat == AL_NONE)
	{
		idLib::Warning(
			"idSoundSample_OpenAL::CreateOpenALBuffer: unsupported %d-channel format for '%s'",
			NumChannels(), GetName());
		openalBuffer = 0;
		return;
	}

	// build OpenAL buffer
	CheckALErrors();
	alGenBuffers( 1, &openalBuffer );
	
	if( CheckALErrors() != AL_NO_ERROR )
	{
		common->Error( "idSoundSample_OpenAL::CreateOpenALBuffer: error generating OpenAL hardware buffer" );
	}
	
	if( alIsBuffer( openalBuffer ) )
	{
		CheckALErrors();
		
		void* buffer = NULL;
		uint32 bufferSize = 0;
		
		if( format.basic.formatTag == idWaveFile::FORMAT_ADPCM )
		{
			// Decode ADPCM only once. The decoder replaces the compressed CPU
			// buffer with 16-bit PCM, which can then be uploaded again after an
			// OpenAL context restart without decoding the PCM a second time.
			if (!openalDataDecoded)
			{
				buffer = buffers[0].buffer;
				bufferSize = buffers[0].bufferSize;
				
				if (MS_ADPCM_decode((uint8**)&buffer, &bufferSize) < 0)
				{
					common->Error("idSoundSample_OpenAL::CreateOpenALBuffer: could not decode ADPCM '%s' to 16 bit format", GetName());
				}
				
				buffers[0].buffer = buffer;
				buffers[0].bufferSize = bufferSize;
				totalBufferSize = bufferSize;
				openalDataDecoded = true;
			}
			
			buffer = buffers[0].buffer;
			bufferSize = buffers[0].bufferSize;
		}
		else if( format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE )
		{
			// Extensible PCM has already been validated by the loader. Its
			// sample payload is ordinary interleaved 16-bit PCM.
			assert(buffers.Num() == 1);
			buffer = buffers[0].buffer;
			bufferSize = buffers[0].bufferSize;
		}
		else
		{
			// TODO concatenate buffers
			
			assert( buffers.Num() == 1 );
			
			buffer = buffers[0].buffer;
			bufferSize = buffers[0].bufferSize;
		}
		
#if 0 //#if defined(AL_SOFT_buffer_samples)
		if( alIsExtensionPresent( "AL_SOFT_buffer_samples" ) )
		{
			ALenum type = AL_SHORT_SOFT;
			
			if( format.basic.bitsPerSample != 16 )
			{
				//common->Error( "idSoundSample_OpenAL::LoadResource: '%s' not a 16 bit format", GetName() );
			}
			
			ALenum channels = NumChannels() == 1 ? AL_MONO_SOFT : AL_STEREO_SOFT;
			ALenum alFormat = GetOpenALSoftFormat( channels, type );
			
			alBufferSamplesSOFT( openalBuffer, format.basic.samplesPerSec, alFormat, BytesToFrames( bufferSize, channels, type ), channels, type, buffer );
		}
		else
#endif
		{
			alBufferData(openalBuffer, alFormat, buffer, bufferSize, format.basic.samplesPerSec);
		}
		
		if( CheckALErrors() != AL_NO_ERROR )
		{
			common->Error( "idSoundSample_OpenAL::CreateOpenALBuffer: error loading data into OpenAL hardware buffer" );
		}
	}
}

/*
========================
idSoundSample_OpenAL::RecreateOpenALBuffer
========================
*/
void idSoundSample_OpenAL::RecreateOpenALBuffer()
{
	if (!loaded || buffers.Num() == 0)
	{
		openalBuffer = 0;
		return;
	}
	
	openalBuffer = 0;
	CreateOpenALBuffer();
}

/*
========================
idSoundSample_OpenAL::LoadWav
========================
*/
bool idSoundSample_OpenAL::LoadWav( const idStr& filename )
{

	// load the wave
	idWaveFile wave;
	if( !wave.Open( filename ) )
	{
		return false;
	}
	
	idStrStatic< MAX_OSPATH > sampleName = filename;
	sampleName.SetFileExtension( "amp" );
	LoadAmplitude( sampleName );
	
	const char* formatError = wave.ReadWaveFormat( format );
	if( formatError != NULL )
	{
		idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), formatError );
		FreeData();
		return false;
	}

	// XMA2 was supported by the old platform-specific backends, but this
	// OpenAL backend has neither an XMA2 decoder nor an OpenAL XMA2 buffer
	// format. Fail this candidate cleanly so LoadResource() can try its
	// existing fallback asset rather than leaving a loaded sample with no
	// hardware buffer.
	if (format.basic.formatTag == idWaveFile::FORMAT_XMA2)
	{
		idLib::Warning("LoadWav( %s ): XMA2 is unsupported by the OpenAL backend", filename.c_str());
		wave.Close();
		FreeData();
		return false;
	}

	if (format.basic.formatTag == idWaveFile::FORMAT_PCM && format.basic.numChannels > 2)
	{
		idLib::Warning( "LoadWav( %s ): multichannel PCM requires WAVE_FORMAT_EXTENSIBLE channel-mask metadata", filename.c_str());
		FreeData();
		return false;
	}
	
	if (format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE && !OpenAL_ValidateExtensibleChannelMask(format))
	{
		idLib::Warning(
			"LoadWav( %s ): unsupported %d-channel mask 0x%08x",
			filename.c_str(),
			format.basic.numChannels,
			format.extra.extensible.channelMask);
		FreeData();
		return false;
	}

	// These fields drive divisions, buffer sizing, and OpenAL upload. Reject
	// malformed format headers before using them for any arithmetic.
	if (format.basic.numChannels == 0 || format.basic.samplesPerSec == 0 || format.basic.samplesPerSec > INT_MAX || format.basic.blockSize == 0)
	{
		idLib::Warning("LoadWav( %s ): invalid wave format field", filename.c_str());
		FreeData();
		return false;
	}
	
	if (format.basic.formatTag == idWaveFile::FORMAT_ADPCM)
	{
		const uint32 channels = format.basic.numChannels;
		const uint32 headerBytes = 7u * channels;
		
		if ((channels != 1 && channels != 2) || 
			format.basic.bitsPerSample != 4 ||
			format.extra.adpcm.numCoef == 0 ||
			format.extra.adpcm.numCoef > 7 ||
			format.extra.adpcm.samplesPerBlock < 2 ||
			format.basic.blockSize < headerBytes)
		{
			idLib::Warning("LoadWav( %s ): invalid MS ADPCM format metadata", filename.c_str());
			FreeData();
			return false;
		}
	
		// MS ADPCM stores a 7-byte header per channel followed by packed
		// 4-bit samples. The decoder advances through each block according to
		// samplesPerBlock, so this relationship must be exact or the next block
		// would begin at the wrong byte.
		const uint32 payloadBytes = format.basic.blockSize - headerBytes;
		const uint32 expectedSamplesPerBlock = 2u + ((payloadBytes * 2u) / channels);
	
		if (format.extra.adpcm.samplesPerBlock != expectedSamplesPerBlock)
		{
			idLib::Warning(
				"LoadWav( %s ): inconsistent MS ADPCM block metadata (%u samples, expected %u)",
				filename.c_str(),
				format.extra.adpcm.samplesPerBlock,
				expectedSamplesPerBlock);
			FreeData();
			return false;
		}
	}

	timestamp = wave.Timestamp();
	
	totalBufferSize = wave.SeekToChunk( 'data' );

	if (totalBufferSize <= 0)
	{
		idLib::Warning("LoadWav( %s ): missing or empty data chunk", filename.c_str());
		FreeData();
		return false;
	}
	
	if( format.basic.formatTag == idWaveFile::FORMAT_PCM || format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE )
	{
	
		if( format.basic.bitsPerSample != 16 )
		{
			idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), "Not a 16 bit PCM wav file" );
			FreeData();
			return false;
		}

		const uint32 expectedBlockSize =
		(uint32)format.basic.numChannels * (uint32)sizeof(int16);
		if (format.basic.blockSize != expectedBlockSize || (totalBufferSize % format.basic.blockSize) != 0)
		{
			idLib::Warning("LoadWav( %s ): invalid PCM block alignment", filename.c_str());
			FreeData();
			return false;
		}
		
		playBegin = 0;
		playLength = ( totalBufferSize ) / format.basic.blockSize;
		
		buffers.SetNum( 1 );
		buffers[0].bufferSize = totalBufferSize;
		buffers[0].numSamples = playLength;
		buffers[0].buffer = AllocBuffer( totalBufferSize, GetName() );
		
		
		if (wave.Read(buffers[0].buffer, totalBufferSize) != (size_t)totalBufferSize)
		{
			idLib::Warning("LoadWav( %s ): truncated PCM sample data", filename.c_str());
			FreeData();
			return false;
		}
		
		if( format.basic.bitsPerSample == 16 )
		{
			idSwap::LittleArray( ( short* )buffers[0].buffer, totalBufferSize / sizeof( short ) );
		}
		
		buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );
		
	}
	else if( format.basic.formatTag == idWaveFile::FORMAT_ADPCM )
	{

		// The decoder operates on complete ADPCM blocks only. Reject a partial
		// trailing block instead of silently ignoring truncated encoded data.
		if ((totalBufferSize % format.basic.blockSize) != 0)
		{
			idLib::Warning("LoadWav( %s ): MS ADPCM data is not block aligned", filename.c_str());
			FreeData();
			return false;
		}
		
		const uint64 blockCount = (uint64)totalBufferSize / (uint64)format.basic.blockSize;
		const uint64 decodedSampleCount = blockCount * (uint64)format.extra.adpcm.samplesPerBlock;
		const uint64 decodedByteCount = decodedSampleCount * (uint64)format.basic.numChannels * (uint64)sizeof(int16);
		
		// playLength, totalBufferSize, and the legacy decoder's encoded length
		// are all represented with signed 32-bit engine fields. Keep both the
		// sample count and decoded PCM allocation within that range.
		if (decodedSampleCount == 0 || decodedSampleCount > 0x7FFFFFFFULL || decodedByteCount > 0x7FFFFFFFULL)
		{
			idLib::Warning("LoadWav( %s ): MS ADPCM sample is too large", filename.c_str());
			FreeData();
			return false;
		}
	
		playBegin = 0;
		playLength = (int)decodedSampleCount;
		
		buffers.SetNum( 1 );
		buffers[0].bufferSize = totalBufferSize;
		buffers[0].numSamples = playLength;
		buffers[0].buffer  = AllocBuffer( totalBufferSize, GetName() );
		
		if (wave.Read(buffers[0].buffer, totalBufferSize) != (size_t)totalBufferSize)
		{
			idLib::Warning("LoadWav( %s ): truncated MS ADPCM sample data", filename.c_str());
			FreeData();
			return false;
		}
		
		buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );
		
	}
	else
	{
		idLib::Warning( "LoadWav( %s ) : Unsupported wave format %d", filename.c_str(), format.basic.formatTag );
		FreeData();
		return false;
	}
	
	wave.Close();
	
	// sanity check...
	assert( buffers[buffers.Num() - 1].numSamples == playBegin + playLength );
	
	return true;
}


/*
========================
idSoundSample_OpenAL::MakeDefault
========================
*/
void idSoundSample_OpenAL::MakeDefault()
{
	FreeData();
	openalDataDecoded = false;
	defaultedForNoSound = false;
	
	static const int DEFAULT_NUM_SAMPLES = 4096;
	
	timestamp = FILE_NOT_FOUND_TIMESTAMP;
	loaded = true;
	
	memset( &format, 0, sizeof( format ) );
	format.basic.formatTag = idWaveFile::FORMAT_PCM;
	format.basic.numChannels = 1;
	format.basic.bitsPerSample = 16;
	format.basic.samplesPerSec = 22050;
	format.basic.blockSize = format.basic.numChannels * format.basic.bitsPerSample / 8;
	format.basic.avgBytesPerSec = format.basic.samplesPerSec * format.basic.blockSize;
	
	assert( format.basic.blockSize == 2 );
	
	totalBufferSize = DEFAULT_NUM_SAMPLES * 2;// * sizeof( short );
	
	short* defaultBuffer = ( short* )AllocBuffer( totalBufferSize, GetName() );
	for( int i = 0; i < DEFAULT_NUM_SAMPLES; i += 2 )
	{
		float v = sin( idMath::PI * 2 * i / 64 );
		int sample = v * 0x4000;
		defaultBuffer[i + 0] = sample;
		defaultBuffer[i + 1] = sample;
		
		//defaultBuffer[i + 0] = SHRT_MIN;
		//defaultBuffer[i + 1] = SHRT_MAX;
	}
	
	buffers.SetNum( 1 );
	buffers[0].buffer = defaultBuffer;
	buffers[0].bufferSize = totalBufferSize;
	buffers[0].numSamples = DEFAULT_NUM_SAMPLES;
	buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );
	
	playBegin = 0;
	playLength = DEFAULT_NUM_SAMPLES;
	
	// Use the same context-aware upload path as normal samples. In s_noSound
	// mode this leaves openalBuffer at zero while retaining the CPU-side beep.
	CreateOpenALBuffer();
}

/*
========================
idSoundSample_OpenAL::FreeData

Called before deleting the object and at the start of LoadResource()
========================
*/
void idSoundSample_OpenAL::FreeData()
{
	if( buffers.Num() > 0 )
	{
		soundSystemLocal.StopVoicesWithSample( ( idSoundSample* )this );
		for( int i = 0; i < buffers.Num(); i++ )
		{
			FreeBuffer( buffers[i].buffer );
		}
		buffers.Clear();
	}
	amplitude.Clear();
	
	timestamp = FILE_NOT_FOUND_TIMESTAMP;
	memset( &format, 0, sizeof( format ) );
	loaded = false;
	totalBufferSize = 0;
	playBegin = 0;
	playLength = 0;
	openalDataDecoded = false;
	defaultedForNoSound = false;
	
	if (openalBuffer != 0 && alcGetCurrentContext() != NULL && alIsBuffer(openalBuffer))
	{
		CheckALErrors();
		
		alDeleteBuffers( 1, &openalBuffer );
		if( CheckALErrors() != AL_NO_ERROR )
		{
			common->Error( "idSoundSample_OpenAL::FreeData: error unloading data from OpenAL hardware buffer" );
		}
		else
		{
			openalBuffer = 0;
		}
	}

	// Always clear the cached name. It may refer to an object from a context
	// that has already been destroyed.
	openalBuffer = 0;
}

/*
========================
idSoundSample_OpenAL::LoadAmplitude
========================
*/
bool idSoundSample_OpenAL::LoadAmplitude( const idStr& name )
{
	amplitude.Clear();
	idFileLocal f( fileSystem->OpenFileRead( name ) );
	if( f == NULL )
	{
		return false;
	}
	const int64 amplitudeBytes = f->Length();
	if (amplitudeBytes <= 0 || amplitudeBytes > INT_MAX)
	{
		return false;
	}
	
	amplitude.SetNum((int)amplitudeBytes);
	if (f->Read(amplitude.Ptr(), amplitude.Num()) != amplitude.Num())
	{
		amplitude.Clear();
		return false;
	}
	return true;
}

/*
========================
idSoundSample_OpenAL::GetAmplitude
========================
*/
float idSoundSample_OpenAL::GetAmplitude( int timeMS ) const
{
	if( timeMS < 0 || timeMS > LengthInMsec() )
	{
		return 0.0f;
	}
	if( IsDefault() )
	{
		return 1.0f;
	}
	const int64 index = (static_cast<int64>(timeMS) * 60) / 1000;
	if( index < 0 || index >= amplitude.Num() )
	{
		return 0.0f;
	}
	return (float)amplitude[static_cast<int>(index)] / 255.0f;
}


#if 0 //defined(AL_SOFT_buffer_samples)
const char* idSoundSample_OpenAL::OpenALSoftChannelsName( ALenum chans ) const
{
	switch( chans )
	{
		case AL_MONO_SOFT:
			return "Mono";
		case AL_STEREO_SOFT:
			return "Stereo";
		case AL_REAR_SOFT:
			return "Rear";
		case AL_QUAD_SOFT:
			return "Quadraphonic";
		case AL_5POINT1_SOFT:
			return "5.1 Surround";
		case AL_6POINT1_SOFT:
			return "6.1 Surround";
		case AL_7POINT1_SOFT:
			return "7.1 Surround";
	}
	
	return "Unknown Channels";
}

const char* idSoundSample_OpenAL::OpenALSoftTypeName( ALenum type ) const
{
	switch( type )
	{
		case AL_BYTE_SOFT:
			return "S8";
		case AL_UNSIGNED_BYTE_SOFT:
			return "U8";
		case AL_SHORT_SOFT:
			return "S16";
		case AL_UNSIGNED_SHORT_SOFT:
			return "U16";
		case AL_INT_SOFT:
			return "S32";
		case AL_UNSIGNED_INT_SOFT:
			return "U32";
		case AL_FLOAT_SOFT:
			return "Float32";
		case AL_DOUBLE_SOFT:
			return "Float64";
	}
	
	return "Unknown Type";
}

ALsizei idSoundSample_OpenAL::FramesToBytes( ALsizei size, ALenum channels, ALenum type ) const
{
	switch( channels )
	{
		case AL_MONO_SOFT:
			size *= 1;
			break;
		case AL_STEREO_SOFT:
			size *= 2;
			break;
		case AL_REAR_SOFT:
			size *= 2;
			break;
		case AL_QUAD_SOFT:
			size *= 4;
			break;
		case AL_5POINT1_SOFT:
			size *= 6;
			break;
		case AL_6POINT1_SOFT:
			size *= 7;
			break;
		case AL_7POINT1_SOFT:
			size *= 8;
			break;
	}
	
	switch( type )
	{
		case AL_BYTE_SOFT:
			size *= sizeof( ALbyte );
			break;
		case AL_UNSIGNED_BYTE_SOFT:
			size *= sizeof( ALubyte );
			break;
		case AL_SHORT_SOFT:
			size *= sizeof( ALshort );
			break;
		case AL_UNSIGNED_SHORT_SOFT:
			size *= sizeof( ALushort );
			break;
		case AL_INT_SOFT:
			size *= sizeof( ALint );
			break;
		case AL_UNSIGNED_INT_SOFT:
			size *= sizeof( ALuint );
			break;
		case AL_FLOAT_SOFT:
			size *= sizeof( ALfloat );
			break;
		case AL_DOUBLE_SOFT:
			size *= sizeof( ALdouble );
			break;
	}
	
	return size;
}

ALsizei idSoundSample_OpenAL::BytesToFrames( ALsizei size, ALenum channels, ALenum type ) const
{
	return size / FramesToBytes( 1, channels, type );
}

ALenum idSoundSample_OpenAL::GetOpenALSoftFormat( ALenum channels, ALenum type ) const
{
	ALenum format = AL_NONE;
	
	/* If using AL_SOFT_buffer_samples, try looking through its formats */
	if( alIsExtensionPresent( "AL_SOFT_buffer_samples" ) )
	{
		/* AL_SOFT_buffer_samples is more lenient with matching formats. The
		 * specified sample type does not need to match the returned format,
		 * but it is nice to try to get something close. */
		if( type == AL_UNSIGNED_BYTE_SOFT || type == AL_BYTE_SOFT )
		{
			if( channels == AL_MONO_SOFT ) format = AL_MONO8_SOFT;
			else if( channels == AL_STEREO_SOFT ) format = AL_STEREO8_SOFT;
			else if( channels == AL_QUAD_SOFT ) format = AL_QUAD8_SOFT;
			else if( channels == AL_5POINT1_SOFT ) format = AL_5POINT1_8_SOFT;
			else if( channels == AL_6POINT1_SOFT ) format = AL_6POINT1_8_SOFT;
			else if( channels == AL_7POINT1_SOFT ) format = AL_7POINT1_8_SOFT;
		}
		else if( type == AL_UNSIGNED_SHORT_SOFT || type == AL_SHORT_SOFT )
		{
			if( channels == AL_MONO_SOFT ) format = AL_MONO16_SOFT;
			else if( channels == AL_STEREO_SOFT ) format = AL_STEREO16_SOFT;
			else if( channels == AL_QUAD_SOFT ) format = AL_QUAD16_SOFT;
			else if( channels == AL_5POINT1_SOFT ) format = AL_5POINT1_16_SOFT;
			else if( channels == AL_6POINT1_SOFT ) format = AL_6POINT1_16_SOFT;
			else if( channels == AL_7POINT1_SOFT ) format = AL_7POINT1_16_SOFT;
		}
		else if( type == AL_UNSIGNED_BYTE3_SOFT || type == AL_BYTE3_SOFT ||
				 type == AL_UNSIGNED_INT_SOFT || type == AL_INT_SOFT ||
				 type == AL_FLOAT_SOFT || type == AL_DOUBLE_SOFT )
		{
			if( channels == AL_MONO_SOFT ) format = AL_MONO32F_SOFT;
			else if( channels == AL_STEREO_SOFT ) format = AL_STEREO32F_SOFT;
			else if( channels == AL_QUAD_SOFT ) format = AL_QUAD32F_SOFT;
			else if( channels == AL_5POINT1_SOFT ) format = AL_5POINT1_32F_SOFT;
			else if( channels == AL_6POINT1_SOFT ) format = AL_6POINT1_32F_SOFT;
			else if( channels == AL_7POINT1_SOFT ) format = AL_7POINT1_32F_SOFT;
		}
		
		if( format != AL_NONE && !alIsBufferFormatSupportedSOFT( format ) )
			format = AL_NONE;
			
		/* A matching format was not found or supported. Try 32-bit float. */
		if( format == AL_NONE )
		{
			if( channels == AL_MONO_SOFT ) format = AL_MONO32F_SOFT;
			else if( channels == AL_STEREO_SOFT ) format = AL_STEREO32F_SOFT;
			else if( channels == AL_QUAD_SOFT ) format = AL_QUAD32F_SOFT;
			else if( channels == AL_5POINT1_SOFT ) format = AL_5POINT1_32F_SOFT;
			else if( channels == AL_6POINT1_SOFT ) format = AL_6POINT1_32F_SOFT;
			else if( channels == AL_7POINT1_SOFT ) format = AL_7POINT1_32F_SOFT;
			
			if( format != AL_NONE && !alIsBufferFormatSupportedSOFT( format ) )
				format = AL_NONE;
		}
		/* 32-bit float not supported. Try 16-bit int. */
		if( format == AL_NONE )
		{
			if( channels == AL_MONO_SOFT ) format = AL_MONO16_SOFT;
			else if( channels == AL_STEREO_SOFT ) format = AL_STEREO16_SOFT;
			else if( channels == AL_QUAD_SOFT ) format = AL_QUAD16_SOFT;
			else if( channels == AL_5POINT1_SOFT ) format = AL_5POINT1_16_SOFT;
			else if( channels == AL_6POINT1_SOFT ) format = AL_6POINT1_16_SOFT;
			else if( channels == AL_7POINT1_SOFT ) format = AL_7POINT1_16_SOFT;
			
			if( format != AL_NONE && !alIsBufferFormatSupportedSOFT( format ) )
				format = AL_NONE;
		}
		/* 16-bit int not supported. Try 8-bit int. */
		if( format == AL_NONE )
		{
			if( channels == AL_MONO_SOFT ) format = AL_MONO8_SOFT;
			else if( channels == AL_STEREO_SOFT ) format = AL_STEREO8_SOFT;
			else if( channels == AL_QUAD_SOFT ) format = AL_QUAD8_SOFT;
			else if( channels == AL_5POINT1_SOFT ) format = AL_5POINT1_8_SOFT;
			else if( channels == AL_6POINT1_SOFT ) format = AL_6POINT1_8_SOFT;
			else if( channels == AL_7POINT1_SOFT ) format = AL_7POINT1_8_SOFT;
			
			if( format != AL_NONE && !alIsBufferFormatSupportedSOFT( format ) )
				format = AL_NONE;
		}
		
		return format;
	}
	
	/* We use the AL_EXT_MCFORMATS extension to provide output of Quad, 5.1,
	 * and 7.1 channel configs, AL_EXT_FLOAT32 for 32-bit float samples, and
	 * AL_EXT_DOUBLE for 64-bit float samples. */
	if( type == AL_UNSIGNED_BYTE_SOFT )
	{
		if( channels == AL_MONO_SOFT )
			format = AL_FORMAT_MONO8;
		else if( channels == AL_STEREO_SOFT )
			format = AL_FORMAT_STEREO8;
		else if( alIsExtensionPresent( "AL_EXT_MCFORMATS" ) )
		{
			if( channels == AL_QUAD_SOFT )
				format = alGetEnumValue( "AL_FORMAT_QUAD8" );
			else if( channels == AL_5POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_51CHN8" );
			else if( channels == AL_6POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_61CHN8" );
			else if( channels == AL_7POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_71CHN8" );
		}
	}
	else if( type == AL_SHORT_SOFT )
	{
		if( channels == AL_MONO_SOFT )
			format = AL_FORMAT_MONO16;
		else if( channels == AL_STEREO_SOFT )
			format = AL_FORMAT_STEREO16;
		else if( alIsExtensionPresent( "AL_EXT_MCFORMATS" ) )
		{
			if( channels == AL_QUAD_SOFT )
				format = alGetEnumValue( "AL_FORMAT_QUAD16" );
			else if( channels == AL_5POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_51CHN16" );
			else if( channels == AL_6POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_61CHN16" );
			else if( channels == AL_7POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_71CHN16" );
		}
	}
	else if( type == AL_FLOAT_SOFT && alIsExtensionPresent( "AL_EXT_FLOAT32" ) )
	{
		if( channels == AL_MONO_SOFT )
			format = alGetEnumValue( "AL_FORMAT_MONO_FLOAT32" );
		else if( channels == AL_STEREO_SOFT )
			format = alGetEnumValue( "AL_FORMAT_STEREO_FLOAT32" );
		else if( alIsExtensionPresent( "AL_EXT_MCFORMATS" ) )
		{
			if( channels == AL_QUAD_SOFT )
				format = alGetEnumValue( "AL_FORMAT_QUAD32" );
			else if( channels == AL_5POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_51CHN32" );
			else if( channels == AL_6POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_61CHN32" );
			else if( channels == AL_7POINT1_SOFT )
				format = alGetEnumValue( "AL_FORMAT_71CHN32" );
		}
	}
	else if( type == AL_DOUBLE_SOFT && alIsExtensionPresent( "AL_EXT_DOUBLE" ) )
	{
		if( channels == AL_MONO_SOFT )
			format = alGetEnumValue( "AL_FORMAT_MONO_DOUBLE" );
		else if( channels == AL_STEREO_SOFT )
			format = alGetEnumValue( "AL_FORMAT_STEREO_DOUBLE" );
	}
	
	/* NOTE: It seems OSX returns -1 from alGetEnumValue for unknown enums, as
	 * opposed to 0. Correct it. */
	if( format == -1 )
		format = 0;
		
	return format;
}
#endif // #if defined(AL_SOFT_buffer_samples)

ALenum idSoundSample_OpenAL::GetOpenALBufferFormat() const
{
	if (format.basic.formatTag == idWaveFile::FORMAT_PCM || format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE)
	{
		switch (NumChannels())
		{
			case 1:
				return AL_FORMAT_MONO16;
			case 2:
				return AL_FORMAT_STEREO16;
			case 4:
			case 6:
			case 7:
			case 8:
				return OpenAL_GetMultichannel16Format(NumChannels());
			default:
				return AL_NONE;
		}
	}
	else if( format.basic.formatTag == idWaveFile::FORMAT_ADPCM )
	{
		// The MS ADPCM decoder supports mono and stereo and produces 16-bit PCM.
		if (NumChannels() == 1)
		{
			return AL_FORMAT_MONO16;
		}
		if (NumChannels() == 2)
		{
			return AL_FORMAT_STEREO16;
		}
		return AL_NONE;
	}

	return AL_NONE;
}

int32 idSoundSample_OpenAL::MS_ADPCM_nibble( MS_ADPCM_decodeState_t* state, int8 nybble )
{
	const int32 max_audioval = ( ( 1 << ( 16 - 1 ) ) - 1 );
	const int32 min_audioval = -( 1 << ( 16 - 1 ) );
	const int32 adaptive[] =
	{
		230, 230, 230, 230, 307, 409, 512, 614,
		768, 614, 512, 409, 307, 230, 230, 230
	};
	
	int32 new_sample, delta;
	
	new_sample = ( ( state->iSamp1 * state->coef1 ) +
				   ( state->iSamp2 * state->coef2 ) ) / 256;
				   
	if( nybble & 0x08 )
	{
		new_sample += state->iDelta * ( nybble - 0x10 );
	}
	else
	{
		new_sample += state->iDelta * nybble;
	}
	
	if( new_sample < min_audioval )
	{
		new_sample = min_audioval;
	}
	else if( new_sample > max_audioval )
	{
		new_sample = max_audioval;
	}
	
	delta = ( ( int32 ) state->iDelta * adaptive[nybble] ) / 256;
	if( delta < 16 )
	{
		delta = 16;
	}
	
	state->iDelta = ( uint16 ) delta;
	state->iSamp2 = state->iSamp1;
	state->iSamp1 = ( int16 ) new_sample;
	
	return ( new_sample );
}

int idSoundSample_OpenAL::MS_ADPCM_decode( uint8** audio_buf, uint32* audio_len )
{
	MS_ADPCM_decodeState_t			states[2];
	MS_ADPCM_decodeState_t*			state[2];
	
	if (audio_buf == NULL || audio_len == NULL || *audio_buf == NULL)
	{
		return -1;
	}
	
	const uint32 channels = format.basic.numChannels;
	const uint32 blockSize = format.basic.blockSize;
	const uint32 samplesPerBlock = format.extra.adpcm.samplesPerBlock;
	const uint32 numCoef = format.extra.adpcm.numCoef;
	const uint32 headerBytes = 7u * channels;
	
	// Validate the complete block layout here as well as in the WAV loader.
	// Generated .idwav data can reach this decoder without passing LoadWav().
	if ((channels != 1 && channels != 2) ||
			blockSize == 0 ||
			samplesPerBlock < 2 ||
			numCoef == 0 ||
			numCoef > 7 ||
			blockSize < headerBytes ||
			*audio_len == 0 ||
			*audio_len > 0x7FFFFFFFu ||
			(*audio_len % blockSize) != 0)
	{
		return -1;
	}
	
	const uint32 payloadBytes = blockSize - headerBytes;
	const uint32 expectedSamplesPerBlock = 2u + ((payloadBytes * 2u) / channels);
	if (samplesPerBlock != expectedSamplesPerBlock)
	{
		return -1;
	}
	
	const uint64 blockCount = (uint64)*audio_len / (uint64)blockSize;
	const uint64 decodedByteCount =
		blockCount *
		(uint64)samplesPerBlock *
		(uint64)channels *
		(uint64)sizeof(int16);
	
	if (decodedByteCount == 0 || decodedByteCount > 0x7FFFFFFFULL)
	{
		return -1;
	}
	
	// Predictor values are stored in the first byte(s) of every block.
	// Validate all of them before allocating output or modifying ownership.
	const uint8 * encodedBlock = *audio_buf;
	for (uint64 block = 0; block < blockCount; ++block)
	{
		if (encodedBlock[0] >= numCoef ||
			(channels == 2 && encodedBlock[1] >= numCoef))
		{
			return -1;
		}
		
		encodedBlock += blockSize;
	}
	
	uint8 * decodedBuffer = (uint8*)Mem_Alloc((int)decodedByteCount, TAG_AUDIO);
	if (decodedBuffer == NULL)
	{
		return -1;
	}
	
	uint8 * encoded = *audio_buf;
	uint8 * decoded = decodedBuffer;
	int32 encoded_len = (int32)*audio_len;
	int32 samplesleft;
	int8 nybble;
	const int8 stereo = (channels == 2) ? 1 : 0;
	int32 new_sample;
	
	state[0] = &states[0];
	state[1] = &states[stereo];
	
	while (encoded_len >= (int32)blockSize)
	{
		// Predictor indices were preflighted for every block above.
		state[0]->hPredictor = *encoded++;
		
		state[0]->coef1 = format.extra.adpcm.aCoef[state[0]->hPredictor].coef1;
		state[0]->coef2 = format.extra.adpcm.aCoef[state[0]->hPredictor].coef2;
		
		if( stereo )
		{
			state[1]->hPredictor = *encoded++;
			
			state[1]->coef1 = format.extra.adpcm.aCoef[state[1]->hPredictor].coef1;
			state[1]->coef2 = format.extra.adpcm.aCoef[state[1]->hPredictor].coef2;
		}
		
		state[0]->iDelta = ( ( encoded[1] << 8 ) | encoded[0] );
		encoded += sizeof( int16 );
		if( stereo )
		{
			state[1]->iDelta = ( ( encoded[1] << 8 ) | encoded[0] );
			encoded += sizeof( int16 );
		}
		
		state[0]->iSamp1 = ( ( encoded[1] << 8 ) | encoded[0] );
		encoded += sizeof( int16 );
		if( stereo )
		{
			state[1]->iSamp1 = ( ( encoded[1] << 8 ) | encoded[0] );
			encoded += sizeof( int16 );
		}
		
		state[0]->iSamp2 = ( ( encoded[1] << 8 ) | encoded[0] );
		encoded += sizeof( int16 );
		if( stereo )
		{
			state[1]->iSamp2 = ( ( encoded[1] << 8 ) | encoded[0] );
			encoded += sizeof( int16 );
		}
		
		// Store the two initial samples we start with.
		decoded[0] = state[0]->iSamp2 & 0xFF;
		decoded[1] = ( state[0]->iSamp2 >> 8 ) & 0xFF;
		decoded += 2;
		if( stereo )
		{
			decoded[0] = state[1]->iSamp2 & 0xFF;
			decoded[1] = ( state[1]->iSamp2 >> 8 ) & 0xFF;
			decoded += 2;
		}
		
		decoded[0] = state[0]->iSamp1 & 0xFF;
		decoded[1] = ( state[0]->iSamp1 >> 8 ) & 0xFF;
		decoded += 2;
		if( stereo )
		{
			decoded[0] = state[1]->iSamp1 & 0xFF;
			decoded[1] = ( state[1]->iSamp1 >> 8 ) & 0xFF;
			decoded += 2;
		}
		
		// Decode and store the other samples in this block.
		samplesleft = (samplesPerBlock - 2) * channels;
		
		while( samplesleft > 0 )
		{
			nybble = ( *encoded ) >> 4;
			new_sample = MS_ADPCM_nibble( state[0], nybble );
			
			decoded[0] = new_sample & 0xFF;
			decoded[1] = ( new_sample >> 8 ) & 0xFF;
			decoded += 2;
			
			nybble = ( *encoded ) & 0x0F;
			new_sample = MS_ADPCM_nibble( state[1], nybble );
			
			decoded[0] = new_sample & 0xFF;
			decoded[1] = ( new_sample >> 8 ) & 0xFF;
			decoded += 2;
			
			++encoded;
			samplesleft -= 2;
		}
		
		encoded_len -= blockSize;
	}
	
	// Commit ownership only after the entire input has passed validation and
	// the replacement buffer was allocated successfully.
	Mem_Free(*audio_buf);
	*audio_buf = decodedBuffer;
	*audio_len = (uint32)decodedByteCount;
	
	return 0;
}


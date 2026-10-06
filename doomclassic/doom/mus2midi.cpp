/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 

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
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "idlib/sys/sys_defines.h"
// mus header


// reads a variable length integer
unsigned long ReadVarLen( char* buffer ) {
	unsigned long value;
	byte c;

	if ((value = *buffer++) & 0x80) {
		value &= 0x7f;
		do  {
			value = (value << 7) + ((c = *buffer++) & 0x7f);
		}  while (c & 0x80);
	}
	return value;
}

// Writes a variable length integer to a buffer, and returns bytes written
int WriteVarLen( long value, byte* out ) 
{
	long buffer, count = 0;

	buffer = value & 0x7f;
	while ((value >>= 7) > 0) {
		buffer <<= 8;
		buffer += 0x80;
		buffer += (value & 0x7f);
	}

	while (1) {
		++count;
		*out = (byte)buffer;
		++out;
		if (buffer & 0x80)
			buffer >>= 8;
		else
			break;
	}
	return count;
}

// writes a byte, and returns the buffer
unsigned char* WriteByte(void* buf, byte b)
{
	unsigned char* buffer = (unsigned char*)buf;
	*buffer++ = b;
	return buffer;
}

unsigned char* WriteShort(void* b, unsigned short s)
{
	unsigned char* buffer = (unsigned char*)b;
	*buffer++ = (s >> 8);
	*buffer++ = (s & 0x00FF);
	return buffer;
}

unsigned char* WriteInt(void* b, unsigned int i)
{
	unsigned char* buffer = (unsigned char*)b;
	*buffer++ = (i & 0xff000000) >> 24;
	*buffer++ = (i & 0x00ff0000) >> 16;
	*buffer++ = (i & 0x0000ff00) >> 8;
	*buffer++ = (i & 0x000000ff);
	return buffer;
}

// Format - 0(1 track only), 1(1 or more tracks, each play same time), 2(1 or more, each play seperatly)
void Midi_CreateHeader(MidiHeaderChunk_t* header, short format, short track_count,  short division)
{
	WriteInt(header->name, 'MThd');
	WriteInt(&header->length, 6);
	WriteShort(&header->format, format);
	WriteShort(&header->ntracks, track_count);
	WriteShort(&header->division, division);
}

unsigned char* Midi_WriteTempo(unsigned char* buffer, int tempo)
{
	buffer = WriteByte(buffer, 0x00);	// delta time
	buffer = WriteByte(buffer, 0xff);	// sys command
	buffer = WriteShort(buffer, 0x5103); // command - set tempo
	
	buffer = WriteByte(buffer, tempo & 0x000000ff);
	buffer = WriteByte(buffer, (tempo & 0x0000ff00) >> 8);
	buffer = WriteByte(buffer, (tempo & 0x00ff0000) >> 16);

	return buffer;
}

int Midi_UpdateBytesWritten(int* bytes_written, int to_add, int max)
{
	if (bytes_written == NULL || to_add < 0 || max < 0 || *bytes_written < 0 || *bytes_written > max - to_add)
	{
		return 0;
	}

	*bytes_written += to_add;
	return 1;
}

static bool MusCanRead(const unsigned char* cur, const unsigned char* end, int count)
{
	return cur != NULL && end != NULL && count >= 0 && cur <= end && static_cast<size_t>(end - cur) >= static_cast<size_t>(count);
}

unsigned char MidiMap[] = 
{
	0,				// prog change
	0,				// bank sel
	1,	//2			// mod pot
	0x07,	//3		// volume
	0x0A,	//4		// pan pot
	0x0B,	//5		// expression pot
	0x5B,	//6		// reverb depth
	0x5D,	//7		// chorus depth
	0x40,	//8		// sustain pedal
	0x43,	//9		// soft pedal
	0x78,	//10		// all sounds off
	0x7B,	//11		// all notes off
	0x7E,	//12		// mono(use numchannels + 1)
	0x7F,	//13		// poly
	0x79,	//14	// reset all controllers
};

// The MUS data is stored in little-endian.
namespace {
	unsigned short LittleToNative( const unsigned short value ) {
		return value;
	}
}

int Mus2Midi(const unsigned char* bytes, int inputLength, unsigned char* out, int outputCapacity, int* len)
{

	if (bytes == NULL || out == NULL || len == NULL || inputLength < static_cast<int>(sizeof(MUSheader_t)) || outputCapacity <= 0)
	{
		return 0;
	}
	
	*len = 0;

	// mus header and instruments
	MUSheader_t header;
	
	// current position in read buffer
	const unsigned char* cur = bytes;
	const unsigned char* inputEnd = bytes + inputLength;
	const unsigned char* end = NULL;

	// Midi header(format 0)
	MidiHeaderChunk_t midiHeader;
	// Midi track header, only 1 needed(format 0)
	MidiTrackChunk_t midiTrackHeader;
	// Stores the position of the midi track header(to change the size)
	byte* midiTrackHeaderOut;
	
	// Delta time for midi event
	int delta_time = 0;
	int temp;
	int channel_volume[MIDI_MAXCHANNELS] = {0};
	int bytes_written = 0;
	int channelMap[MIDI_MAXCHANNELS], currentChannel = 0;
	byte last_status = 0;

	// read the mus header
	memcpy(&header, cur, sizeof(header));
	cur += sizeof(header);

	header.scoreLen = LittleToNative( header.scoreLen );
	header.scoreStart = LittleToNative( header.scoreStart );
	header.channels = LittleToNative( header.channels );
	header.sec_channels = LittleToNative( header.sec_channels );
	header.instrCnt = LittleToNative( header.instrCnt );
	header.dummy = LittleToNative( header.dummy );

	if (header.scoreStart > inputLength || header.scoreLen > inputLength - header.scoreStart)
	{
		return 0;
	}
	
	// only 15 supported
	if (header.channels > MIDI_MAXCHANNELS - 1)
		return 0;

	// Map channel 15 to 9(percussions)
	for (temp = 0; temp < MIDI_MAXCHANNELS; ++temp) {
		channelMap[temp] = -1;
		channel_volume[temp] = 0x40;
	}
	channelMap[15] = 9;

	// Get current position, and end of position
	cur = bytes + header.scoreStart;
	end = cur + header.scoreLen;

	if (cur < bytes || cur > inputEnd || end < cur || end > inputEnd)
		return 0;

	// Write out midi header
	Midi_CreateHeader(&midiHeader, 0, 1, 0x0059);

	if (!Midi_UpdateBytesWritten(&bytes_written, MIDIHEADERSIZE, outputCapacity))
		return 0;

	memcpy(out, &midiHeader, MIDIHEADERSIZE);	// cannot use sizeof(packs it to 16 bytes)
	out += MIDIHEADERSIZE;
	 
	// Store this position, for later filling in the midiTrackHeader
	if (!Midi_UpdateBytesWritten(&bytes_written, sizeof(midiTrackHeader), outputCapacity))
		return 0;

	midiTrackHeaderOut = out;
	out += sizeof(midiTrackHeader);
	

	// microseconds per quarter note(yikes)
	if (!Midi_UpdateBytesWritten(&bytes_written, 7, outputCapacity))
		return 0;

	out = Midi_WriteTempo(out, 0x001aa309);
	
	// Percussions channel starts out at full volume
	if (!Midi_UpdateBytesWritten(&bytes_written, 4, outputCapacity))
		return 0;

	out = WriteByte(out, 0x00);
	out = WriteByte(out, 0xB9);
	out = WriteByte(out, 0x07);
	out = WriteByte(out, 127);
	
	// Main Loop
	while (cur < end) {
		byte channel; 
		byte event;
		byte temp_buffer[32];	// temp buffer for current iterator
		byte *out_local = temp_buffer;
		byte status, bit1, bit2, bitc = 2;
		
		// Read in current bit
		if (!MusCanRead(cur, end, 1))
			return 0;

		event		= *cur++;
		channel		= (event & 15);		// current channel
		
		// Write variable length delta time
		out_local += WriteVarLen(delta_time, out_local);
		
		if (channelMap[channel] < 0) {
			// Set all channels to 127 volume
			out_local = WriteByte(out_local, 0xB0 + currentChannel);
			out_local = WriteByte(out_local, 0x07);
			out_local = WriteByte(out_local, 127);
			out_local = WriteByte(out_local, 0x00);

			channelMap[channel] = currentChannel++;
			if (currentChannel == 9)
				++currentChannel;
		}

		status = channelMap[channel];

		// Handle ::g->events
		switch ((event & 122) >> 4)
		{
		default:
			return 0;
		case MUSEVENT_KEYOFF:
			if (!MusCanRead(cur, end, 1))
				return 0;

			status |=  0x80;
			bit1 = *cur++;
			bit2 = 0x40;
			break;
		case MUSEVENT_KEYON:
			if (!MusCanRead(cur, end, 1))
				return 0;

			status |= 0x90;
			bit1 = *cur & 127;
			if (*cur++ & 128) {	// volume bit?
				if (!MusCanRead(cur, end, 1))
					return 0;

				channel_volume[channelMap[channel]] = *cur++;
			}
			bit2 = channel_volume[channelMap[channel]];
			break;
		case MUSEVENT_PITCHWHEEL:
			if (!MusCanRead(cur, end, 1))
				return 0;

			status |= 0xE0;
			bit1 = (*cur & 1) >> 6;
			bit2 = (*cur++ >> 1) & 127;
			break;
		case MUSEVENT_CHANNELMODE:
			if (!MusCanRead(cur, end, 2))
				return 0;
			status |= 0xB0;
			if (*cur >= sizeof(MidiMap) / sizeof(MidiMap[0]))
				return 0;

			bit1 = MidiMap[*cur++];
			bit2 = (*cur++ == 12) ? header.channels + 1 : 0x00;
			break;
		case MUSEVENT_CONTROLLERCHANGE:
			if (!MusCanRead(cur, end, 2))
				return 0;

			if (*cur == 0) {
				cur++;
				status |= 0xC0;
				bit1 = *cur++;
				bitc = 1;
			} else {
				status |= 0xB0;
				if (*cur >= sizeof(MidiMap) / sizeof(MidiMap[0]))
					return 0;

				bit1 = MidiMap[*cur++];
				bit2 = *cur++;
			}
			break;
		case 5:	// Unknown
			return 0;
		case MUSEVENT_END:	// End
			status = 0xff;
			bit1 = 0x2f;
			bit2 = 0x00;
			if (cur != end)
				return 0;

			break;
		case 7:	// Unknown
			return 0;
		}

		// Write it out
		out_local = WriteByte(out_local, status);
		out_local = WriteByte(out_local, bit1);
		if (bitc == 2) 
			out_local = WriteByte(out_local, bit2);
			

		// Write out temp stuff
		if (out_local != temp_buffer)
		{
			const int eventBytes = static_cast<int>(out_local - temp_buffer);
			if (!Midi_UpdateBytesWritten(&bytes_written, eventBytes, outputCapacity))
				return 0;
			memcpy(out, temp_buffer, eventBytes);
			out += eventBytes;
		}

		if (event & 128) {
			delta_time = 0;
			for (;;) {
				if (!MusCanRead(cur, end, 1))
					return 0;
				
				const int deltaByte = *cur++;
				const int deltaPart = deltaByte & 127;
				if (delta_time > (0x7FFFFFFF - deltaPart) / 128)
					return 0;
				
				delta_time = delta_time * 128 + deltaPart;
				if ((deltaByte & 128) == 0)
					break;	
			}
		} else {
			delta_time = 0;
		}
	}

	// Write out track header
	WriteInt(midiTrackHeader.name, 'MTrk');
	WriteInt(&midiTrackHeader.length, out - midiTrackHeaderOut - sizeof(midiTrackHeader));
	memcpy(midiTrackHeaderOut, &midiTrackHeader, sizeof(midiTrackHeader));
	
	// Store length written
	*len = bytes_written;
	/*{
		FILE* file = f o pen("d:\\test.midi", "wb");
		fwrite(midiTrackHeaderOut - sizeof(MidiHeaderChunk_t), bytes_written, 1, file);
		fclose(file);
	}*/
	return 1;
}


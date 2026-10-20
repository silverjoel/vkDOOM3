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

#include "i_system.h"
#include "z_zone.h"
#include "p_local.h"

// State.
#include "doomstat.h"
#include "r_state.h"
#include "p_saveg.h"
#include <stddef.h>
#include <limits.h>

namespace {
struct saveArchive_t {
	byte *base;
	size_t size, position;
	Globals *owner;
	bool reading, failed;
};
saveArchive_t saveArchive = {};
}

void P_SaveRequire( bool valid, const char *message ) {
	if ( !valid ) {
		saveArchive.failed = true;
		throw idException( message );
	}
}

void P_BeginSaveArchive( byte *buffer, size_t length, bool reading ) {
	P_SaveRequire( !saveArchive.owner && buffer && length <= INT_MAX, "Invalid Classic save buffer" );
	saveArchive.base = buffer;
	saveArchive.size = length;
	saveArchive.position = 0;
	saveArchive.owner = ::g;
	saveArchive.reading = reading;
	saveArchive.failed = false;
	::g->save_p = buffer;
}

void P_EndSaveArchive() {
	saveArchive = saveArchive_t();
}

size_t P_SavePosition() { return saveArchive.position; }

byte *P_SaveBytes( size_t amount, bool reading ) {
	P_SaveRequire( saveArchive.owner == ::g && saveArchive.owner && !saveArchive.failed &&
		saveArchive.reading == reading, "Invalid Classic save operation" );
	P_SaveRequire( amount <= saveArchive.size - saveArchive.position, "Truncated or oversized Classic save" );
	byte *result = saveArchive.base + saveArchive.position;
	saveArchive.position += amount;
	::g->save_p = saveArchive.base + saveArchive.position;
	return result;
}

static void P_SavePad( bool reading ) {
	// Keep the historical four-byte *address* alignment and untouched pad bytes.
	P_SaveBytes( 0, reading );
	const size_t pad = ( uintptr_t( 4 ) - ( reinterpret_cast<uintptr_t>( ::g->save_p ) & uintptr_t( 3 ) ) ) & uintptr_t( 3 );
	P_SaveBytes( pad, reading );
}

template<class T> static T P_ReadSaveValue() {
	T value;
	memcpy( &value, P_SaveBytes( sizeof( value ), true ), sizeof( value ) );
	return value;
}

template<class T> static void P_WriteSaveValue( T value ) {
	memcpy( P_SaveBytes( sizeof( value ), false ), &value, sizeof( value ) );
}

// These bytes occupy the old pointer slots, but are integers, never pointers.
static uintptr_t P_ReadSaveIndex( const byte *record, size_t offset ) {
	static_assert( sizeof( uintptr_t ) == sizeof( state_t * ) && sizeof( uintptr_t ) == sizeof( sector_t * ), "Native save index slot width" );
	uintptr_t index;
	memcpy( &index, record + offset, sizeof( index ) );
	return index;
}

static void P_WriteSaveIndex( byte *record, size_t offset, uintptr_t index ) {
	memcpy( record + offset, &index, sizeof( index ) );
}

template<class T> static uintptr_t P_SaveArrayIndex( const T *pointer, const T *base, int count ) {
	P_SaveRequire( pointer && base && count > 0, "Missing Classic save array reference" );
	const uintptr_t address = reinterpret_cast<uintptr_t>( pointer );
	const uintptr_t start = reinterpret_cast<uintptr_t>( base );
	P_SaveRequire( address >= start, "Classic save reference precedes array" );
	const uintptr_t distance = address - start;
	P_SaveRequire( distance % sizeof( T ) == 0 && distance / sizeof( T ) < uintptr_t( count ),
		"Classic save reference outside array" );
	return distance / sizeof( T );
}

template<class T> static T *P_LoadSaveIndex( uintptr_t index, T *base, int count ) {
	P_SaveRequire( base && count > 0 && index < uintptr_t( count ), "Invalid Classic saved index" );
	return base + index;
}

static size_t P_SaveBlockCount() {
	P_SaveRequire( ::g->bmapwidth >= 0 && ::g->bmapheight >= 0, "Invalid Classic blockmap size" );
	const size_t width = size_t( ::g->bmapwidth ), height = size_t( ::g->bmapheight );
	P_SaveRequire( !height || width <= SIZE_MAX / height / sizeof( *::g->blocklinks ), "Classic blockmap size overflow" );
	return width * height;
}

void P_DiscardSaveThinkers() {
	// Only the native thinker list is followed, never a serialized spatial link.
	thinker_t *th = ::g->thinkercap.next;
	while ( th && th != &::g->thinkercap ) {
		thinker_t *next = th->next;
		Z_Free( th );
		th = next;
	}
	P_InitThinkers();
	memset( ::g->activeceilings, 0, sizeof( ::g->activeceilings ) );
	memset( ::g->activeplats, 0, sizeof( ::g->activeplats ) );
	::g->numbraintargets = 0;
	memset( ::g->braintargets, 0, sizeof( ::g->braintargets ) );
	for ( int i = 0; i < MAXPLAYERS; ++i ) {
		::g->players[i].mo = ::g->players[i].attacker = NULL;
		::g->players[i].message = NULL;
		for ( int j = 0; j < NUMPSPRITES; ++j ) ::g->players[i].psprites[j].state = NULL;
	}
	for ( int i = 0; i < ::g->numsectors; ++i ) {
		::g->sectors[i].thinglist = ::g->sectors[i].soundtarget = NULL;
		::g->sectors[i].specialdata = NULL;
	}
	if ( ::g->blocklinks ) memset( ::g->blocklinks, 0, P_SaveBlockCount() * sizeof( *::g->blocklinks ) );
}



// Pads ::g->save_p to a 4-byte boundary
//  so that the load/save works on SGI&Gecko.



//
// P_ArchivePlayers
//
void P_ArchivePlayers (void) {
	for ( int i = 0; i < MAXPLAYERS; ++i ) {
		if ( !::g->playeringame[i] ) continue;
		P_SavePad( false );
		byte *record = P_SaveBytes( sizeof( player_t ), false );
		memcpy( record, &::g->players[i], sizeof( player_t ) );
		for ( int j = 0; j < NUMPSPRITES; ++j ) {
			const state_t *state = ::g->players[i].psprites[j].state;
			const uintptr_t index = state ? P_SaveArrayIndex( state, ::g->states, NUMSTATES ) : 0;
			P_WriteSaveIndex( record, offsetof( player_t, psprites ) + size_t( j ) * sizeof( pspdef_t ) + offsetof( pspdef_t, state ), index );
		}
	}
}

void P_UnArchivePlayers (void) {
	for ( int i = 0; i < MAXPLAYERS; ++i ) {
		if ( !::g->playeringame[i] ) continue;
		P_SavePad( true );
		const byte *record = P_SaveBytes( sizeof( player_t ), true );
		player_t player;
		memcpy( &player, record, sizeof( player ) );
		player.mo = player.attacker = NULL;
		player.message = NULL;
		for ( int j = 0; j < NUMPSPRITES; ++j ) {
			const uintptr_t index = P_ReadSaveIndex( record, offsetof( player_t, psprites ) + size_t( j ) * sizeof( pspdef_t ) + offsetof( pspdef_t, state ) );
			player.psprites[j].state = index ? P_LoadSaveIndex( index, ::g->states, NUMSTATES ) : NULL;
		}
		P_SaveRequire( player.playerstate >= PST_LIVE && player.playerstate <= PST_REBORN &&
			player.readyweapon >= 0 && player.readyweapon < NUMWEAPONS &&
			player.pendingweapon >= 0 && player.pendingweapon <= wp_nochange,
			"Invalid Classic saved player state" );
		memcpy( &::g->players[i], &player, sizeof( player ) );
	}
}


//
// P_ArchiveWorld
//
static void P_CheckWorldBytes( bool reading ) {
	P_SaveBytes( 0, reading );
	P_SaveRequire( ::g->numsectors >= 0 && ::g->numlines >= 0, "Invalid Classic world counts" );
	// Map counts are int-backed; perform all sizing before narrowing or access.
	size_t bytes = size_t( ::g->numsectors ) * 14 + 4;
	P_SaveRequire( bytes <= saveArchive.size - saveArchive.position, "Truncated Classic world sectors" );
	for ( int i = 0; i < ::g->numlines; ++i ) {
		size_t lineBytes = 6;
		for ( int j = 0; j < 2; ++j ) {
			const int side = ::g->lines[i].sidenum[j];
			if ( side == -1 ) continue;
			P_SaveRequire( side >= 0 && side < ::g->numsides, "Invalid Classic world side" );
			lineBytes += 10;
		}
		P_SaveRequire( lineBytes <= saveArchive.size - saveArchive.position - bytes, "Truncated Classic world lines" );
		bytes += lineBytes;
	}
}

void P_ArchiveWorld (void)
{
	P_CheckWorldBytes( false );
    int			i;
    int			j;
    sector_t*		sec;
    line_t*		li;
    side_t*		si;

    // do ::g->sectors
    for (i=0, sec = ::g->sectors ; i < ::g->numsectors ; i++,sec++)
    {
	P_WriteSaveValue<short>( static_cast<short>( sec->floorheight >> FRACBITS ) );
	P_WriteSaveValue<short>( static_cast<short>( sec->ceilingheight >> FRACBITS ) );
	P_WriteSaveValue<short>( static_cast<short>( sec->floorpic ) );
	P_WriteSaveValue<short>( static_cast<short>( sec->ceilingpic ) );
	P_WriteSaveValue<short>( static_cast<short>( sec->lightlevel ) );
	P_WriteSaveValue<short>( static_cast<short>( sec->special ) );		// needed?
	P_WriteSaveValue<short>( static_cast<short>( sec->tag ) );		// needed?
    }

    
    // do ::g->lines
    for (i=0, li = ::g->lines ; i < ::g->numlines ; i++,li++)
    {
	P_WriteSaveValue<short>( static_cast<short>( li->flags ) );
	P_WriteSaveValue<short>( static_cast<short>( li->special ) );
	P_WriteSaveValue<short>( static_cast<short>( li->tag ) );
	for (j=0 ; j<2 ; j++)
	{
	    if (li->sidenum[j] == -1)
		continue;
	    
	    si = &::g->sides[li->sidenum[j]];

	    P_WriteSaveValue<short>( static_cast<short>( si->textureoffset >> FRACBITS ) );
	    P_WriteSaveValue<short>( static_cast<short>( si->rowoffset >> FRACBITS ) );
	    P_WriteSaveValue<short>( static_cast<short>( si->toptexture ) );
	    P_WriteSaveValue<short>( static_cast<short>( si->bottomtexture ) );
	    P_WriteSaveValue<short>( static_cast<short>( si->midtexture ) );
	}
    }

	// Doom 2 level 30 requires some global pointers, wheee!
	P_WriteSaveValue<short>( static_cast<short>( ::g->braintargeton ) );
	P_WriteSaveValue<short>( static_cast<short>( ::g->easy ) );

}



//
// P_UnArchiveWorld
//
void P_UnArchiveWorld (void)
{
	P_CheckWorldBytes( true );
    int			i;
    int			j;
    sector_t*	sec;
    line_t*		li;
    side_t*		si;

    // do ::g->sectors
    for (i=0, sec = ::g->sectors ; i < ::g->numsectors ; i++,sec++)
    {
	sec->floorheight = int( P_ReadSaveValue<short>() ) * FRACUNIT;
	sec->ceilingheight = int( P_ReadSaveValue<short>() ) * FRACUNIT;
	sec->floorpic = P_ReadSaveValue<short>();
	sec->ceilingpic = P_ReadSaveValue<short>();
	sec->lightlevel = P_ReadSaveValue<short>();
	sec->special = P_ReadSaveValue<short>();		// needed?
	sec->tag = P_ReadSaveValue<short>();		// needed?
	sec->specialdata = 0;
	sec->soundtarget = 0;
    }
    
    // do ::g->lines
    for (i=0, li = ::g->lines ; i < ::g->numlines ; i++,li++)
    {
	li->flags = P_ReadSaveValue<short>();
	li->special = P_ReadSaveValue<short>();
	li->tag = P_ReadSaveValue<short>();
	for (j=0 ; j<2 ; j++)
	{
	    if (li->sidenum[j] == -1)
		continue;
	    si = &::g->sides[li->sidenum[j]];
	    si->textureoffset = int( P_ReadSaveValue<short>() ) * FRACUNIT;
	    si->rowoffset = int( P_ReadSaveValue<short>() ) * FRACUNIT;
	    si->toptexture = P_ReadSaveValue<short>();
	    si->bottomtexture = P_ReadSaveValue<short>();
	    si->midtexture = P_ReadSaveValue<short>();
	}
    }

	// Doom 2 level 30 requires some global pointers, wheee!
	::g->braintargeton = P_ReadSaveValue<short>();
	::g->easy = P_ReadSaveValue<short>();

}





//
// Thinkers
//

int GetMOIndex( mobj_t* findme ) {
	thinker_t*	th;
	mobj_t*		mobj;
	int			index = 0;

	for (th = ::g->thinkercap.next ; th != &::g->thinkercap ; th=th->next)
	{
		if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
			index++;
			mobj = (mobj_t*)th;

			if ( mobj == findme ) {
				P_SaveRequire( index <= 1024, "Classic mobile reference exceeds save limit" );
				return index;
			}
		}
	}

	// Historical references to an object no longer in the active mobj list
	// serialize as null; identity comparison never dereferences that address.
	return 0;
}

mobj_t* GetMO( int index ) {
	thinker_t*	th;
	int			testindex = 0;

	if ( !index ) {
		return NULL;
	}

	for (th = ::g->thinkercap.next ; th != &::g->thinkercap ; th=th->next)
	{
		if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
			testindex++;

			if ( testindex == index ) {
				return (mobj_t*)th;
			}
		}
	}

	return NULL;
}

//
// P_ArchiveThinkers
//
void P_ArchiveThinkers (void)
{
	thinker_t*		th;
	mobj_t*			mobj;
	ceiling_t*		ceiling;
	vldoor_t*		door;
	floormove_t*	floor;
	plat_t*			plat;
	fireflicker_t*	fire;
	lightflash_t*	flash;
	strobe_t*		strobe;
	glow_t*			glow;

	int i;
	int savedMobjs = 0;
	byte *record;
	
	// save off the current thinkers
	//I_Printf( "Savegame on leveltime %d\n====================\n", ::g->leveltime );

	for (th = ::g->thinkercap.next ; th != &::g->thinkercap ; th=th->next)
	{
		//mobj_t*	test = (mobj_t*)th;
		//I_Printf( "%3d: %x == function\n", index++, th->function.acp1 );

		if (th->function.acp1 == (actionf_p1)P_MobjThinker)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_mobj ) );
			P_SavePad( false );

			mobj = (mobj_t *)th;
			record = P_SaveBytes( sizeof( *mobj ), false );
			memcpy (record, th, sizeof(*mobj));

			P_SaveRequire( ++savedMobjs <= 1024, "Too many Classic saved mobile objects" );
			P_WriteSaveIndex( record, offsetof( mobj_t, state ), P_SaveArrayIndex( mobj->state, ::g->states, NUMSTATES ) );

			P_WriteSaveIndex( record, offsetof( mobj_t, player ), mobj->player ? P_SaveArrayIndex( mobj->player, ::g->players, MAXPLAYERS ) + 1 : 0 );

			// Save out 'target'
			int moIndex = GetMOIndex( mobj->target );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex >> 8 ) );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex ) );

			// Save out 'tracer'
			moIndex = GetMOIndex( mobj->tracer );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex >> 8 ) );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex ) );

			moIndex = GetMOIndex( mobj->snext );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex >> 8 ) );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex ) );

			moIndex = GetMOIndex( mobj->sprev );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex >> 8 ) );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex ) );

			// Is this the head of a sector list?
			if ( mobj->subsector->sector->thinglist == (mobj_t*)th ) {
				P_WriteSaveValue<byte>( static_cast<byte>( 1 ) );
			}
			else {
				P_WriteSaveValue<byte>( static_cast<byte>( 0 ) );
			}

			moIndex = GetMOIndex( mobj->bnext );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex >> 8 ) );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex ) );

			moIndex = GetMOIndex( mobj->bprev );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex >> 8 ) );
			P_WriteSaveValue<byte>( static_cast<byte>( moIndex ) );

			// Is this the head of a block list?
			int	blockx = (mobj->x - ::g->bmaporgx)>>MAPBLOCKSHIFT;
			int	blocky = (mobj->y - ::g->bmaporgy)>>MAPBLOCKSHIFT;
			if ( blockx >= 0 && blockx < ::g->bmapwidth && blocky >= 0 && blocky < ::g->bmapheight 
				&& (mobj_t*)th == ::g->blocklinks[blocky*::g->bmapwidth+blockx] ) {

					P_WriteSaveValue<byte>( static_cast<byte>( 1 ) );
			}
			else {
				P_WriteSaveValue<byte>( static_cast<byte>( 0 ) );
			}
			continue;
		}

		if (th->function.acv == (actionf_v)NULL)
		{
			for (i = 0; i < MAXCEILINGS;i++)
				if (::g->activeceilings[i] == (ceiling_t *)th)
					break;

			if (i<MAXCEILINGS)
			{
				P_WriteSaveValue<byte>( static_cast<byte>( tc_ceiling ) );
				P_SavePad( false );
				ceiling = (ceiling_t *)th;
			record = P_SaveBytes( sizeof( *ceiling ), false );
				memcpy (record, th, sizeof(*ceiling));

				P_WriteSaveIndex( record, offsetof( ceiling_t, sector ), P_SaveArrayIndex( ceiling->sector, ::g->sectors, ::g->numsectors ) );
			}
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_MoveCeiling)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_ceiling ) );
			P_SavePad( false );
			ceiling = (ceiling_t *)th;
			record = P_SaveBytes( sizeof( *ceiling ), false );
			memcpy (record, th, sizeof(*ceiling));

			P_WriteSaveIndex( record, offsetof( ceiling_t, sector ), P_SaveArrayIndex( ceiling->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_VerticalDoor)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_door ) );
			P_SavePad( false );
			door = (vldoor_t *)th;
			record = P_SaveBytes( sizeof( *door ), false );
			memcpy (record, th, sizeof(*door));

			P_WriteSaveIndex( record, offsetof( vldoor_t, sector ), P_SaveArrayIndex( door->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_MoveFloor)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_floor ) );
			P_SavePad( false );
			floor = (floormove_t *)th;
			record = P_SaveBytes( sizeof( *floor ), false );
			memcpy (record, th, sizeof(*floor));

			P_WriteSaveIndex( record, offsetof( floormove_t, sector ), P_SaveArrayIndex( floor->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_PlatRaise)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_plat ) );
			P_SavePad( false );
			plat = (plat_t *)th;
			record = P_SaveBytes( sizeof( *plat ), false );
			memcpy (record, th, sizeof(*plat));

			P_WriteSaveIndex( record, offsetof( plat_t, sector ), P_SaveArrayIndex( plat->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_FireFlicker)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_fire ) );
			P_SavePad( false );
			fire = (fireflicker_t *)th;
			record = P_SaveBytes( sizeof( *fire ), false );
			memcpy (record, th, sizeof(*fire));

			P_WriteSaveIndex( record, offsetof( fireflicker_t, sector ), P_SaveArrayIndex( fire->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_LightFlash)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_flash ) );
			P_SavePad( false );
			flash = (lightflash_t *)th;
			record = P_SaveBytes( sizeof( *flash ), false );
			memcpy (record, th, sizeof(*flash));

			P_WriteSaveIndex( record, offsetof( lightflash_t, sector ), P_SaveArrayIndex( flash->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_StrobeFlash)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_strobe ) );
			P_SavePad( false );
			strobe = (strobe_t *)th;
			record = P_SaveBytes( sizeof( *strobe ), false );
			memcpy (record, th, sizeof(*strobe));

			P_WriteSaveIndex( record, offsetof( strobe_t, sector ), P_SaveArrayIndex( strobe->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}

		if (th->function.acp1 == (actionf_p1)T_Glow)
		{
			P_WriteSaveValue<byte>( static_cast<byte>( tc_glow ) );
			P_SavePad( false );
			glow = (glow_t *)th;
			record = P_SaveBytes( sizeof( *glow ), false );
			memcpy (record, th, sizeof(*glow));

			P_WriteSaveIndex( record, offsetof( glow_t, sector ), P_SaveArrayIndex( glow->sector, ::g->sectors, ::g->numsectors ) );
			continue;
		}
	}

	// add a terminating marker
	P_WriteSaveValue<byte>( static_cast<byte>( tc_end ) );

	sector_t* sec;

	for (i=0, sec = ::g->sectors ; i < ::g->numsectors ; i++,sec++) {
		P_WriteSaveValue<short>( static_cast<short>( GetMOIndex( sec->soundtarget ) ) );
	}



	// add a terminating marker
	P_WriteSaveValue<byte>( static_cast<byte>( tc_end ) );
}



//
// P_UnArchiveThinkers
//
template<class T> static T *P_PublishSaveThinker( const T &value ) {
	T *object = static_cast<T *>( DoomLib::Z_Malloc( sizeof( T ), PU_LEVEL, NULL ) );
	P_SaveRequire( object != NULL, "Classic restore allocation failed" );
	memcpy( object, &value, sizeof( T ) );
	object->thinker.next = object->thinker.prev = NULL;
	P_AddThinker( &object->thinker );
	return object;
}

template<class T> static T *P_LoadSectorThinker( actionf_p1 function, bool moving, bool canPause ) {
	P_SavePad( true );
	const byte *record = P_SaveBytes( sizeof( T ), true );
	T value;
	memcpy( &value, record, sizeof( T ) );
	value.sector = P_LoadSaveIndex( P_ReadSaveIndex( record, offsetof( T, sector ) ), ::g->sectors, ::g->numsectors );
	const uintptr_t active = P_ReadSaveIndex( record, offsetof( T, thinker ) + offsetof( thinker_t, function ) );
	value.thinker.function.acp1 = canPause && !active ? NULL : function;
	P_SaveRequire( !moving || !value.sector->specialdata, "Duplicate Classic sector mover" );
	T *object = P_PublishSaveThinker( value );
	if ( moving ) object->sector->specialdata = object;
	return object;
}

static int P_ReadMobileIndex() {
	const byte *bytes = P_SaveBytes( 2, true );
	return ( int( bytes[0] ) << 8 ) | bytes[1];
}

static int P_SaveBlock( const mobj_t *object ) {
	// Coordinates are fixed32, but subtraction must not overflow before shifting.
	const int64 x = ( int64( object->x ) - ::g->bmaporgx ) >> MAPBLOCKSHIFT;
	const int64 y = ( int64( object->y ) - ::g->bmaporgy ) >> MAPBLOCKSHIFT;
	if ( x < 0 || y < 0 || x >= ::g->bmapwidth || y >= ::g->bmapheight ) return -1;
	const int64 index = y * ::g->bmapwidth + x;
	P_SaveRequire( index <= INT_MAX, "Classic block index overflow" );
	return static_cast<int>( index );
}

void P_UnArchiveThinkers (void) {
	struct mobileLinks_t {
		mobj_t *object;
		int target, tracer, snext, sprev, bnext, bprev;
		byte shead, bhead;
	};
	mobileLinks_t links[1024];
	int count = 0, ceilings = 0, plats = 0;
	bool owners[MAXPLAYERS] = {};
	const size_t blocks = P_SaveBlockCount();
	P_SaveRequire( !blocks || ::g->blocklinks, "Missing Classic blockmap" );

	// Preserve removal side effects, but retire each allocation now that its
	// scheduled removal will no longer run in the replaced thinker list.
	for ( thinker_t *th = ::g->thinkercap.next; th != &::g->thinkercap; ) {
		thinker_t *next = th->next;
		if ( th->function.acp1 == (actionf_p1)P_MobjThinker ) P_RemoveMobj( (mobj_t *)th );
		Z_Free( th );
		th = next;
	}
	P_InitThinkers();
	memset( ::g->activeceilings, 0, sizeof( ::g->activeceilings ) );
	memset( ::g->activeplats, 0, sizeof( ::g->activeplats ) );
	for ( int i = 0; i < ::g->numsectors; ++i ) {
		::g->sectors[i].thinglist = ::g->sectors[i].soundtarget = NULL;
		::g->sectors[i].specialdata = NULL;
	}
	if ( blocks ) memset( ::g->blocklinks, 0, blocks * sizeof( *::g->blocklinks ) );

	for ( ;; ) {
		const byte type = P_ReadSaveValue<byte>();
		if ( type == tc_end ) break;
		switch ( type ) {
		case tc_mobj: {
			P_SaveRequire( count < 1024, "Too many Classic saved mobile objects" );
			P_SavePad( true );
			P_SaveRequire( sizeof( mobj_t ) + 14 <= saveArchive.size - saveArchive.position, "Truncated Classic mobile record" );
			const byte *record = P_SaveBytes( sizeof( mobj_t ), true );
			mobj_t value;
			memcpy( &value, record, sizeof( value ) );
			value.state = P_LoadSaveIndex( P_ReadSaveIndex( record, offsetof( mobj_t, state ) ), ::g->states, NUMSTATES );
			const uintptr_t owner = P_ReadSaveIndex( record, offsetof( mobj_t, player ) );
			value.player = owner ? P_LoadSaveIndex( owner - 1, ::g->players, MAXPLAYERS ) : NULL;
			if ( owner ) {
				P_SaveRequire( ::g->playeringame[owner-1] && !owners[owner-1], "Invalid Classic saved player owner" );
				owners[owner-1] = true;
			}
			P_SaveRequire( value.type >= 0 && value.type < NUMMOBJTYPES, "Invalid Classic saved object type" );
			value.target = value.tracer = value.snext = value.sprev = value.bnext = value.bprev = NULL;
			value.subsector = NULL;
			value.info = &mobjinfo[value.type];
			value.thinker.function.acp1 = (actionf_p1)P_MobjThinker;
			mobileLinks_t &link = links[count];
			link.target = P_ReadMobileIndex(); link.tracer = P_ReadMobileIndex();
			link.snext = P_ReadMobileIndex(); link.sprev = P_ReadMobileIndex();
			link.shead = P_ReadSaveValue<byte>();
			link.bnext = P_ReadMobileIndex(); link.bprev = P_ReadMobileIndex();
			link.bhead = P_ReadSaveValue<byte>();
			P_SaveRequire( link.shead <= 1 && link.bhead <= 1, "Invalid Classic spatial head flag" );
			mobj_t *object = P_PublishSaveThinker( value );
			link.object = object;
			++count;
			P_SetThingPosition( object );
			object->floorz = object->subsector->sector->floorheight;
			object->ceilingz = object->subsector->sector->ceilingheight;
			if ( object->player ) object->player->mo = object;
			break;
		}
		case tc_ceiling:
			P_SaveRequire( ceilings++ < MAXCEILINGS, "Too many Classic saved ceilings" );
			P_AddActiveCeiling( P_LoadSectorThinker<ceiling_t>( (actionf_p1)T_MoveCeiling, true, true ) );
			break;
		case tc_door: P_LoadSectorThinker<vldoor_t>( (actionf_p1)T_VerticalDoor, true, false ); break;
		case tc_floor: P_LoadSectorThinker<floormove_t>( (actionf_p1)T_MoveFloor, true, false ); break;
		case tc_plat:
			P_SaveRequire( plats++ < MAXPLATS, "Too many Classic saved platforms" );
			P_AddActivePlat( P_LoadSectorThinker<plat_t>( (actionf_p1)T_PlatRaise, true, true ) );
			break;
		case tc_fire: P_LoadSectorThinker<fireflicker_t>( (actionf_p1)T_FireFlicker, false, false ); break;
		case tc_flash: P_LoadSectorThinker<lightflash_t>( (actionf_p1)T_LightFlash, false, false ); break;
		case tc_strobe: P_LoadSectorThinker<strobe_t>( (actionf_p1)T_StrobeFlash, false, false ); break;
		case tc_glow: P_LoadSectorThinker<glow_t>( (actionf_p1)T_Glow, false, false ); break;
		default: P_SaveRequire( false, "Unknown Classic saved thinker class" );
		}
	}

	// Admit all saved references before linking any spatial chain. A count bound
	// alone is not enough: reciprocal links and finite traversal are also required.
	for ( int i = 0; i < MAXPLAYERS; ++i ) {
		P_SaveRequire( !::g->playeringame[i] || owners[i], "Missing Classic saved player object" );
	}
	for ( int i = 0; i < count; ++i ) {
		const mobileLinks_t &l = links[i];
		P_SaveRequire( l.target <= count && l.tracer <= count && l.snext <= count && l.sprev <= count &&
			l.bnext <= count && l.bprev <= count, "Invalid Classic saved mobile reference" );
	}
	for ( int i = 0; i < count; ++i ) {
		const mobileLinks_t &l = links[i];
		P_SaveRequire( (!l.snext || (links[l.snext-1].sprev == i+1 && links[l.snext-1].object->subsector->sector == l.object->subsector->sector)) &&
			(!l.sprev || links[l.sprev-1].snext == i+1) && (!l.shead || !l.sprev), "Inconsistent Classic sector links" );
		P_SaveRequire( (!l.bnext || (links[l.bnext-1].bprev == i+1 && P_SaveBlock( links[l.bnext-1].object ) == P_SaveBlock( l.object ))) &&
			(!l.bprev || links[l.bprev-1].bnext == i+1) && (!l.bhead || !l.bprev), "Inconsistent Classic block links" );
		for ( int chain = 0; chain < 2; ++chain ) {
			int current = i+1, steps = 0;
			while ( current ) {
				P_SaveRequire( steps++ < count, "Cyclic Classic saved spatial chain" );
				current = chain ? links[current-1].bnext : links[current-1].snext;
			}
		}
	}
	for ( int i = 0; i < ::g->numsectors; ++i ) ::g->sectors[i].thinglist = NULL;
	if ( blocks ) memset( ::g->blocklinks, 0, blocks * sizeof( *::g->blocklinks ) );
	::g->numbraintargets = 0;
	for ( int i = 0; i < count; ++i ) {
		const mobileLinks_t &l = links[i];
		mobj_t *object = l.object;
		object->target = l.target ? links[l.target-1].object : NULL;
		object->tracer = l.tracer ? links[l.tracer-1].object : NULL;
		object->snext = l.snext ? links[l.snext-1].object : NULL;
		object->sprev = l.sprev ? links[l.sprev-1].object : NULL;
		object->bnext = l.bnext ? links[l.bnext-1].object : NULL;
		object->bprev = l.bprev ? links[l.bprev-1].object : NULL;
		if ( l.shead ) {
			P_SaveRequire( !object->subsector->sector->thinglist, "Duplicate Classic sector head" );
			object->subsector->sector->thinglist = object;
		}
		if ( l.bhead ) {
			const int block = P_SaveBlock( object );
			P_SaveRequire( block >= 0 && !::g->blocklinks[block], "Invalid Classic block head" );
			::g->blocklinks[block] = object;
		}
		if ( object->type == MT_BOSSTARGET ) {
			P_SaveRequire( ::g->numbraintargets < int( sizeof( ::g->braintargets ) / sizeof( ::g->braintargets[0] ) ), "Too many Classic brain targets" );
			::g->braintargets[::g->numbraintargets++] = object;
		}
	}
	for ( int i = 0; i < ::g->numsectors; ++i ) {
		const int index = P_ReadSaveValue<short>();
		P_SaveRequire( index >= 0 && index <= count, "Invalid Classic sector sound target" );
		::g->sectors[i].soundtarget = index ? links[index-1].object : NULL;
	}
	P_SaveRequire( P_ReadSaveValue<byte>() == tc_end, "Missing Classic sound-target terminator" );
}


//
// P_ArchiveSpecials
//



//
// Things to handle:
//
// T_MoveCeiling, (ceiling_t: sector_t * swizzle), - active list
// T_VerticalDoor, (vldoor_t: sector_t * swizzle),
// T_MoveFloor, (floormove_t: sector_t * swizzle),
// T_LightFlash, (lightflash_t: sector_t * swizzle),
// T_StrobeFlash, (strobe_t: sector_t *),
// T_Glow, (glow_t: sector_t *),
// T_PlatRaise, (plat_t: sector_t *), - active list
//
void P_ArchiveSpecials (void)
{
    thinker_t*		th;
    ceiling_t*		ceiling;
    vldoor_t*		door;
    floormove_t*	floor;
    plat_t*		plat;
    lightflash_t*	flash;
    strobe_t*		strobe;
    glow_t*		glow;
    int			i;
	
    // save off the current thinkers
    for (th = ::g->thinkercap.next ; th != &::g->thinkercap ; th=th->next)
    {
	if (th->function.acv == (actionf_v)NULL)
	{
	    for (i = 0; i < MAXCEILINGS;i++)
		if (::g->activeceilings[i] == (ceiling_t *)th)
		    break;
	    
	    if (i<MAXCEILINGS)
	    {
		*::g->save_p++ = tc_ceiling;
		PADSAVEP();
		ceiling = (ceiling_t *)::g->save_p;
		memcpy (ceiling, th, sizeof(*ceiling));
		::g->save_p += sizeof(*ceiling);
		ceiling->sector = (sector_t *)(ceiling->sector - ::g->sectors);
	    }
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_MoveCeiling)
	{
	    *::g->save_p++ = tc_ceiling;
	    PADSAVEP();
	    ceiling = (ceiling_t *)::g->save_p;
	    memcpy (ceiling, th, sizeof(*ceiling));
	    ::g->save_p += sizeof(*ceiling);
	    ceiling->sector = (sector_t *)(ceiling->sector - ::g->sectors);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_VerticalDoor)
	{
	    *::g->save_p++ = tc_door;
	    PADSAVEP();
	    door = (vldoor_t *)::g->save_p;
	    memcpy (door, th, sizeof(*door));
	    ::g->save_p += sizeof(*door);
	    door->sector = (sector_t *)(door->sector - ::g->sectors);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_MoveFloor)
	{
	    *::g->save_p++ = tc_floor;
	    PADSAVEP();
	    floor = (floormove_t *)::g->save_p;
	    memcpy (floor, th, sizeof(*floor));
	    ::g->save_p += sizeof(*floor);
	    floor->sector = (sector_t *)(floor->sector - ::g->sectors);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_PlatRaise)
	{
	    *::g->save_p++ = tc_plat;
	    PADSAVEP();
	    plat = (plat_t *)::g->save_p;
	    memcpy (plat, th, sizeof(*plat));
	    ::g->save_p += sizeof(*plat);
	    plat->sector = (sector_t *)(plat->sector - ::g->sectors);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_LightFlash)
	{
	    *::g->save_p++ = tc_flash;
	    PADSAVEP();
	    flash = (lightflash_t *)::g->save_p;
	    memcpy (flash, th, sizeof(*flash));
	    ::g->save_p += sizeof(*flash);
	    flash->sector = (sector_t *)(flash->sector - ::g->sectors);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_StrobeFlash)
	{
	    *::g->save_p++ = tc_strobe;
	    PADSAVEP();
	    strobe = (strobe_t *)::g->save_p;
	    memcpy (strobe, th, sizeof(*strobe));
	    ::g->save_p += sizeof(*strobe);
	    strobe->sector = (sector_t *)(strobe->sector - ::g->sectors);
	    continue;
	}
			
	if (th->function.acp1 == (actionf_p1)T_Glow)
	{
	    *::g->save_p++ = tc_glow;
	    PADSAVEP();
	    glow = (glow_t *)::g->save_p;
	    memcpy (glow, th, sizeof(*glow));
	    ::g->save_p += sizeof(*glow);
	    glow->sector = (sector_t *)(glow->sector - ::g->sectors);
	    continue;
	}
    }
	
    // add a terminating marker
    *::g->save_p++ = tc_endspecials;	

}


//
// P_UnArchiveSpecials
//
void P_UnArchiveSpecials (void)
{
    byte		tclass;
    ceiling_t*		ceiling;
    vldoor_t*		door;
    floormove_t*	floor;
    plat_t*		plat;
    lightflash_t*	flash;
    strobe_t*		strobe;
    glow_t*		glow;

    // read in saved thinkers
    while (1)
    {
	tclass = *::g->save_p++;
	switch (tclass)
	{
	  case tc_endspecials:
	    return;	// end of list
			
	  case tc_ceiling:
	    PADSAVEP();
	    ceiling = (ceiling_t*)DoomLib::Z_Malloc(sizeof(*ceiling), PU_LEVEL, NULL);
	    memcpy (ceiling, ::g->save_p, sizeof(*ceiling));
	    ::g->save_p += sizeof(*ceiling);
	    ceiling->sector = &::g->sectors[(int)ceiling->sector];
	    ceiling->sector->specialdata = ceiling;

	    if (ceiling->thinker.function.acp1)
		ceiling->thinker.function.acp1 = (actionf_p1)T_MoveCeiling;

	    P_AddThinker (&ceiling->thinker);
	    P_AddActiveCeiling(ceiling);
	    break;
				
	  case tc_door:
	    PADSAVEP();
	    door = (vldoor_t*)DoomLib::Z_Malloc(sizeof(*door), PU_LEVEL, NULL);
	    memcpy (door, ::g->save_p, sizeof(*door));
	    ::g->save_p += sizeof(*door);
	    door->sector = &::g->sectors[(int)door->sector];
	    door->sector->specialdata = door;
	    door->thinker.function.acp1 = (actionf_p1)T_VerticalDoor;
	    P_AddThinker (&door->thinker);
	    break;
				
	  case tc_floor:
	    PADSAVEP();
	    floor = (floormove_t*)DoomLib::Z_Malloc (sizeof(*floor), PU_LEVEL, NULL);
	    memcpy (floor, ::g->save_p, sizeof(*floor));
	    ::g->save_p += sizeof(*floor);
	    floor->sector = &::g->sectors[(int)floor->sector];
	    floor->sector->specialdata = floor;
	    floor->thinker.function.acp1 = (actionf_p1)T_MoveFloor;
	    P_AddThinker (&floor->thinker);
	    break;
				
	  case tc_plat:
	    PADSAVEP();
	    plat = (plat_t*)DoomLib::Z_Malloc (sizeof(*plat), PU_LEVEL, NULL);
	    memcpy (plat, ::g->save_p, sizeof(*plat));
	    ::g->save_p += sizeof(*plat);
	    plat->sector = &::g->sectors[(int)plat->sector];
	    plat->sector->specialdata = plat;

	    if (plat->thinker.function.acp1)
		plat->thinker.function.acp1 = (actionf_p1)T_PlatRaise;

	    P_AddThinker (&plat->thinker);
	    P_AddActivePlat(plat);
	    break;
				
	  case tc_flash:
	    PADSAVEP();
	    flash = (lightflash_t*)DoomLib::Z_Malloc (sizeof(*flash), PU_LEVEL, NULL);
	    memcpy (flash, ::g->save_p, sizeof(*flash));
	    ::g->save_p += sizeof(*flash);
	    flash->sector = &::g->sectors[(int)flash->sector];
	    flash->thinker.function.acp1 = (actionf_p1)T_LightFlash;
	    P_AddThinker (&flash->thinker);
	    break;
				
	  case tc_strobe:
	    PADSAVEP();
	    strobe = (strobe_t*)DoomLib::Z_Malloc (sizeof(*strobe), PU_LEVEL, NULL);
	    memcpy (strobe, ::g->save_p, sizeof(*strobe));
	    ::g->save_p += sizeof(*strobe);
	    strobe->sector = &::g->sectors[(int)strobe->sector];
	    strobe->thinker.function.acp1 = (actionf_p1)T_StrobeFlash;
	    P_AddThinker (&strobe->thinker);
	    break;
				
	  case tc_glow:
	    PADSAVEP();
	    glow = (glow_t*)DoomLib::Z_Malloc (sizeof(*glow), PU_LEVEL, NULL);
	    memcpy (glow, ::g->save_p, sizeof(*glow));
	    ::g->save_p += sizeof(*glow);
	    glow->sector = &::g->sectors[(int)glow->sector];
	    glow->thinker.function.acp1 = (actionf_p1)T_Glow;
	    P_AddThinker (&glow->thinker);
	    break;
				
	  default:
	    I_Error ("P_UnarchiveSpecials:Unknown tclass %i "
		     "in savegame",tclass);
	}
	
    }

}



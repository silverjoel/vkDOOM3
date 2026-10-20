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

#include "m_swap.h"

#include "w_wad.h"

#include "doomdef.h"
#include "r_local.h"
#include "p_local.h"

#include "doomstat.h"
#include "r_sky.h"

#ifdef LINUX
#include  <alloca.h>
#endif


#include "r_data.h"

#include <vector>
#include <new>
#include <limits.h>

//
// Graphics.
// DOOM graphics for walls and ::g->sprites
// is stored in vertical runs of opaque pixels (posts).
// A column is composed of zero or more posts,
// a patch or sprite is composed of zero or more columns.
// 



//
// Texture definition.
// Each texture is composed of one or more patches,
// with patches being lumps stored in the WAD.
// The lumps are referenced by number, and patched
// into the rectangular texture space using origin
// and possibly other attributes.
//


//
// Texture definition.
// A DOOM wall texture is a list of patches
// which are to be combined in a predefined order.
//


// A single patch from a texture definition,
//  basically a rectangular area within
//  the texture rectangle.


// A maptexturedef_t describes a rectangular texture,
//  which is composed of one or more mappatch_t structures
//  that arrange graphic patches.






// for global animation

// needed for pre rendering



//
// MAPTEXTURE_T CACHING
// When a texture is first needed,
//  it counts the number of composite columns
//  required in the texture and allocates space
//  for a column directory and any new columns.
// The directory will simply point inside other patches
//  if there is only one patch in a given column,
//  but any columns with multiple patches
//  will have new postColumn_ts generated.
//



//
// R_DrawColumnInCache
// Clip and draw a column
//  from a patch into a cached post.
//
void
R_DrawColumnInCache
( postColumn_t*	patch,
  byte*			cache,
  int			originy,
  int			cacheheight )
{
    int		count;
    int		position;
    byte*	source;
    byte*	dest;
	
    dest = (byte *)cache + 3;
	
    while (patch->topdelta != 0xff)
    {
	source = (byte *)patch + 3;
	count = patch->length;
	position = originy + patch->topdelta;

	if (position < 0)
	{
	    count += position;
	    position = 0;
	}

	if (position + count > cacheheight)
	    count = cacheheight - position;

	if (count > 0)
	    memcpy (cache + position, source, count);
		
	patch = (postColumn_t *)(  (byte *)patch + patch->length + 4); 
    }
}



//
// R_GenerateComposite
// Using the texture definition,
//  the composite texture is created from the patches,
//  and each column is cached.
//
void R_GenerateComposite (int texnum)
{
    byte*			block;
    texture_t*		texture;
    texpatch_t*		patch;	
    patch_t*		realpatch;
    int				x;
    int				x1;
    int				x2;
    int				i;
    postColumn_t*	patchcol;
    short*			collump;
    unsigned short*	colofs;
	
    texture = ::g->s_textures[texnum];

    block = (byte*)DoomLib::Z_Malloc (::g->s_texturecompositesize[texnum],
		      PU_CACHE_SHARED, 
		      &::g->s_texturecomposite[texnum]);	

    collump = ::g->s_texturecolumnlump[texnum];
    colofs = ::g->s_texturecolumnofs[texnum];
    
    // Composite the columns together.
    patch = texture->patches;
		
    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
	realpatch = (patch_t*)W_CacheLumpNum (patch->patch, PU_CACHE_SHARED);
	x1 = patch->originx;
	x2 = x1 + SHORT(realpatch->width);

	if (x1<0)
	    x = 0;
	else
	    x = x1;
	
	if (x2 > texture->width)
	    x2 = texture->width;

	for ( ; x<x2 ; x++)
	{
	    // Column does not have multiple patches?
	    if (collump[x] >= 0)
		continue;
	    
	    patchcol = (postColumn_t *)((byte *)realpatch
				    + LONG(realpatch->columnofs[x-x1]));
	    R_DrawColumnInCache (patchcol,
				 block + colofs[x],
				 patch->originy,
				 texture->height);
	}
						
    }
}



//
// R_GenerateLookup
//
void R_GenerateLookup (int texnum)
{
    texture_t*		texture;
    texpatch_t*		patch;	
    patch_t*		realpatch;
    int			x;
    int			x1;
    int			x2;
    int			i;
    short*		collump;
    unsigned short*	colofs;
	
    texture = ::g->s_textures[texnum];

    // Composited texture not created yet.
    ::g->s_texturecomposite[texnum] = 0;
    
    ::g->s_texturecompositesize[texnum] = 0;
    collump = ::g->s_texturecolumnlump[texnum];
    colofs = ::g->s_texturecolumnofs[texnum];
    
    // Now count the number of columns
    //  that are covered by more than one patch.
    // Fill in the lump / offset, so columns
    //  with only a single patch are all done.
    std::vector<byte> patchcount(texture->width, 0);
    patch = texture->patches;
		
    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
	realpatch = (patch_t*)W_CacheLumpNum (patch->patch, PU_CACHE_SHARED);
	x1 = patch->originx;
	x2 = x1 + SHORT(realpatch->width);
	
	if (x1 < 0)
	    x = 0;
	else
	    x = x1;

	if (x2 > texture->width)
	    x2 = texture->width;
	for ( ; x<x2 ; x++)
	{
	    patchcount[x]++;
	    collump[x] = patch->patch;
	    colofs[x] = LONG(realpatch->columnofs[x-x1])+3;
	}
    }
	
    for (x=0 ; x<texture->width ; x++)
    {
	if (!patchcount[x])
	{
	    I_Printf ("R_GenerateLookup: column without a patch (%s)\n",
		    texture->name);
	    return;
	}
	// I_Error ("R_GenerateLookup: column without a patch");
	
	if (patchcount[x] > 1)
	{
	    // Use the cached block.
	    collump[x] = -1;	
	    colofs[x] = ::g->s_texturecompositesize[texnum];
	    
	    if (::g->s_texturecompositesize[texnum] > 0x10000-texture->height)
	    {
		I_Error ("R_GenerateLookup: texture %i is >64k",
			 texnum);
	    }
	    
	    ::g->s_texturecompositesize[texnum] += texture->height;
	}
    }	
}




//
// R_GetColumn
//
byte*
R_GetColumn
( int		tex,
  int		col )
{
    int		lump;
    int		ofs;
	
    col &= ::g->s_texturewidthmask[tex];
    lump = ::g->s_texturecolumnlump[tex][col];
    ofs = ::g->s_texturecolumnofs[tex][col];
    
    if (lump > 0)
	return (byte *)W_CacheLumpNum(lump,PU_CACHE_SHARED)+ofs;

    if (!::g->s_texturecomposite[tex])
	R_GenerateComposite (tex);

    return ::g->s_texturecomposite[tex] + ofs;
}




//
// R_InitTextures
// Initializes the texture list
//  with the s_textures from the world map.
//
namespace {

// WAD fields are little-endian bytes, never native pointer-containing records.
const size_t TEXTURE_HEADER_BYTES = 22;
const size_t TEXTURE_PATCH_BYTES = 10;

struct textureInitError_t {
	const char *message;
};

static void R_TextureRequire( bool valid, const char *message ) {
	if ( !valid ) {
		textureInitError_t error = { message };
		throw error;
	}
}

static unsigned int R_TextureU32( const byte *p ) {
	return unsigned( p[0] ) | ( unsigned( p[1] ) << 8 ) |
		( unsigned( p[2] ) << 16 ) | ( unsigned( p[3] ) << 24 );
}

static int R_TextureS16( const byte *p ) {
	const int value = int( p[0] ) | ( int( p[1] ) << 8 );
	return value < 0x8000 ? value : value - 0x10000;
}

static int R_TextureCount( const byte *data, size_t length, size_t entryBytes ) {
	R_TextureRequire( length >= 4, "R_InitTextures: truncated lump header" );
	const unsigned int count = R_TextureU32( data );
	R_TextureRequire( count <= INT_MAX && size_t( count ) <= ( length - 4 ) / entryBytes,
		"R_InitTextures: invalid lump directory count" );
	return static_cast<int>( count );
}

struct textureRecord_t {
	const byte *data;
	int width, height, patchCount;
	unsigned int masked;
};

static textureRecord_t R_TextureRecord( const byte *data, size_t length, int count, int index ) {
	R_TextureRequire( index >= 0 && index < count, "R_InitTextures: invalid texture number" );
	const size_t directoryEnd = 4 + size_t( count ) * 4;
	const size_t offset = R_TextureU32( data + 4 + size_t( index ) * 4 );
	R_TextureRequire( offset >= directoryEnd && offset <= length && length - offset >= TEXTURE_HEADER_BYTES,
		"R_InitTextures: bad texture directory" );
	textureRecord_t record;
	record.data = data + offset;
	record.masked = R_TextureU32( record.data + 8 );
	record.width = R_TextureS16( record.data + 12 );
	record.height = R_TextureS16( record.data + 14 );
	// Bytes 16..19 are the obsolete four-byte column-directory field.
	record.patchCount = R_TextureS16( record.data + 20 );
	R_TextureRequire( record.width > 0 && record.height > 0 && record.patchCount > 0,
		"R_InitTextures: invalid texture dimensions or patch count" );
	R_TextureRequire( size_t( record.patchCount ) <= ( length - offset - TEXTURE_HEADER_BYTES ) / TEXTURE_PATCH_BYTES,
		"R_InitTextures: truncated texture patches" );
	return record;
}

static int R_TexturePatchIndex( const byte *patch, const std::vector<int> &lookup ) {
	const int index = R_TextureS16( patch + 4 );
	R_TextureRequire( index >= 0 && size_t( index ) < lookup.size(), "R_InitTextures: invalid patch index" );
	R_TextureRequire( lookup[index] >= 0, "R_InitTextures: missing texture patch" );
	return index;
}

static int R_TextureAllocationBytes( size_t count, size_t elementBytes, size_t extraBytes = 0 ) {
	R_TextureRequire( elementBytes != 0 && count <= ( size_t( -1 ) - extraBytes ) / elementBytes,
		"R_InitTextures: allocation size overflow" );
	const size_t bytes = count * elementBytes + extraBytes;
	// Z_Malloc rounds to four bytes and then adds its native block header.
	const size_t maxBytes = ( size_t( INT_MAX ) - sizeof( memblock_t ) ) & ~size_t( 3 );
	R_TextureRequire( bytes > 0 && bytes <= maxBytes, "R_InitTextures: allocation exceeds zone size contract" );
	return static_cast<int>( bytes );
}

static bool R_TextureZoneCanAllocate( int bytes ) {
	// Avoid entering Z_Malloc's returning I_Error exhaustion path. Count only
	// contiguous free/purgable blocks; an aggregate free-byte total is insufficient.
	const size_t needed = ( ( size_t( bytes ) + 3 ) & ~size_t( 3 ) ) + sizeof( memblock_t );
	size_t available = 0;
	for ( const memblock_t *block = ::g->mainzone->blocklist.next;
		block != &::g->mainzone->blocklist; block = block->next ) {
		if ( block->user && block->tag < PU_PURGELEVEL ) {
			available = 0;
		} else {
			R_TextureRequire( block->size > 0, "R_InitTextures: invalid zone block" );
			available += size_t( block->size );
			if ( available >= needed ) {
				return true;
			}
		}
	}
	return false;
}

class textureAllocations_t {
public:
	~textureAllocations_t() { Clear(); }
	void Clear() {
		for ( size_t i = blocks.size(); i > 0; --i ) {
			Z_Free( blocks[i - 1] );
		}
		blocks.clear();
	}
	void Release() { blocks.clear(); }
	void Own( void *p ) {
		try {
			blocks.push_back( p );
		} catch ( ... ) {
			Z_Free( p );
			throw;
		}
	}
	void *Alloc( int bytes, void *owner = NULL ) {
		R_TextureRequire( R_TextureZoneCanAllocate( bytes ), "R_InitTextures: insufficient zone memory" );
		void *p = DoomLib::Z_Malloc( bytes, PU_STATIC_SHARED, owner );
		R_TextureRequire( p != NULL, "R_InitTextures: allocation failed" );
		Own( p );
		memset( p, 0, size_t( bytes ) );
		return p;
	}
private:
	std::vector<void *> blocks;
};

static const byte *R_TextureLump( int lump, size_t &length, textureAllocations_t &inputs ) {
	R_TextureRequire( lump >= 0 && lump < numlumps, "R_InitTextures: missing texture lump" );
	const lumpinfo_t &info = lumpinfo[lump];
	R_TextureRequire( info.size >= 4, "R_InitTextures: truncated texture lump" );
	length = size_t( info.size );
	const int bytes = R_TextureAllocationBytes( length, 1 );
	if ( lumpcache[lump] != NULL ) {
		// Pin both texture lumps until decoding finishes; neither may be purged
		// by subsequent zone allocations while its bytes are still in use.
		Z_ChangeTag2( &lumpcache[lump], PU_STATIC_SHARED );
		inputs.Own( lumpcache[lump] );
	} else {
		R_TextureRequire( info.handle != NULL, "R_InitTextures: missing WAD file" );
		const int fileLength = info.handle->Length();
		R_TextureRequire( info.position >= 0 && fileLength >= info.position && info.size <= fileLength - info.position,
			"R_InitTextures: lump outside WAD file" );
		void *data = inputs.Alloc( bytes, &lumpcache[lump] );
		R_TextureRequire( info.handle->Seek( info.position, FS_SEEK_SET ) == 0 &&
			info.handle->Read( data, bytes ) == bytes, "R_InitTextures: short WAD lump read" );
	}
	return static_cast<const byte *>( lumpcache[lump] );
}

static void R_LoadTextures() {
	textureAllocations_t inputs;
	size_t namesLength;
	const byte *names = R_TextureLump( W_CheckNumForName( "PNAMES" ), namesLength, inputs );
	const int nameCount = R_TextureCount( names, namesLength, 8 );
	std::vector<int> patchlookup( nameCount );
	for ( int i = 0; i < nameCount; ++i ) {
		char name[9];
		memcpy( name, names + 4 + size_t( i ) * 8, 8 );
		name[8] = 0;
		patchlookup[i] = W_CheckNumForName( name );
	}
	inputs.Clear();

	textureAllocations_t output;
	if ( ::g->s_numtextures == 0 ) {
		size_t lengths[2] = { 0, 0 };
		const byte *lumps[2] = { R_TextureLump( W_CheckNumForName( "TEXTURE1" ), lengths[0], inputs ), NULL };
		int counts[2] = { R_TextureCount( lumps[0], lengths[0], 4 ), 0 };
		const int secondLump = W_CheckNumForName( "TEXTURE2" );
		if ( secondLump >= 0 ) {
			lumps[1] = R_TextureLump( secondLump, lengths[1], inputs );
			counts[1] = R_TextureCount( lumps[1], lengths[1], 4 );
		}
		R_TextureRequire( counts[1] <= INT_MAX - counts[0], "R_InitTextures: texture count overflow" );
		const int count = counts[0] + counts[1];
		R_TextureRequire( count > 0, "R_InitTextures: no textures" );

		// Validate every variable record and patch reference before publishing or
		// allocating runtime tables. Numbering remains TEXTURE1 then TEXTURE2.
		for ( int lump = 0; lump < 2; ++lump ) {
			for ( int i = 0; i < counts[lump]; ++i ) {
				const textureRecord_t record = R_TextureRecord( lumps[lump], lengths[lump], counts[lump], i );
				for ( int j = 0; j < record.patchCount; ++j ) {
					R_TexturePatchIndex( record.data + TEXTURE_HEADER_BYTES + size_t( j ) * TEXTURE_PATCH_BYTES, patchlookup );
				}
			}
		}

		texture_t **textures = static_cast<texture_t **>( output.Alloc( R_TextureAllocationBytes( count, sizeof( *textures ) ) ) );
		short **columnLump = static_cast<short **>( output.Alloc( R_TextureAllocationBytes( count, sizeof( *columnLump ) ) ) );
		unsigned short **columnOfs = static_cast<unsigned short **>( output.Alloc( R_TextureAllocationBytes( count, sizeof( *columnOfs ) ) ) );
		byte **composite = static_cast<byte **>( output.Alloc( R_TextureAllocationBytes( count, sizeof( *composite ) ) ) );
		int *widthMask = static_cast<int *>( output.Alloc( R_TextureAllocationBytes( count, sizeof( *widthMask ) ) ) );
		fixed_t *height = static_cast<fixed_t *>( output.Alloc( R_TextureAllocationBytes( count, sizeof( *height ) ) ) );
		int *compositeSize = static_cast<int *>( output.Alloc( R_TextureAllocationBytes( count, sizeof( *compositeSize ) ) ) );
		int *translation = static_cast<int *>( output.Alloc( R_TextureAllocationBytes( size_t( count ) + 1, sizeof( *translation ) ) ) );

		int index = 0;
		for ( int lump = 0; lump < 2; ++lump ) {
			for ( int i = 0; i < counts[lump]; ++i, ++index ) {
				const textureRecord_t record = R_TextureRecord( lumps[lump], lengths[lump], counts[lump], i );
				texture_t *texture = static_cast<texture_t *>( output.Alloc(
					R_TextureAllocationBytes( size_t( record.patchCount - 1 ), sizeof( texpatch_t ), sizeof( texture_t ) ) ) );
				textures[index] = texture;
				memcpy( texture->name, record.data, sizeof( texture->name ) );
				texture->width = static_cast<short>( record.width );
				texture->height = static_cast<short>( record.height );
				texture->patchcount = static_cast<short>( record.patchCount );
				// masked, obsolete column directory, stepdir and colormap remain
				// disk-only metadata, as in the original runtime representation.
				for ( int j = 0; j < record.patchCount; ++j ) {
					const byte *patch = record.data + TEXTURE_HEADER_BYTES + size_t( j ) * TEXTURE_PATCH_BYTES;
					texture->patches[j].originx = R_TextureS16( patch );
					texture->patches[j].originy = R_TextureS16( patch + 2 );
					texture->patches[j].patch = patchlookup[R_TexturePatchIndex( patch, patchlookup )];
				}
				columnLump[index] = static_cast<short *>( output.Alloc( R_TextureAllocationBytes( record.width, sizeof( short ) ) ) );
				columnOfs[index] = static_cast<unsigned short *>( output.Alloc( R_TextureAllocationBytes( record.width, sizeof( unsigned short ) ) ) );
				int power = 1;
				while ( power * 2 <= record.width ) {
					power <<= 1;
				}
				widthMask[index] = power - 1;
				height[index] = record.height << FRACBITS;
				translation[index] = index;
			}
		}
		inputs.Clear();
		::g->s_textures = textures;
		::g->s_texturecolumnlump = columnLump;
		::g->s_texturecolumnofs = columnOfs;
		::g->s_texturecomposite = composite;
		::g->s_texturewidthmask = widthMask;
		::g->s_textureheight = height;
		::g->s_texturecompositesize = compositeSize;
		::g->texturetranslation = translation;
		::g->s_numtextures = count;
		output.Release();
		for ( int i = 0; i < count; ++i ) {
			R_GenerateLookup( i );
		}
	} else {
		R_TextureRequire( ::g->s_numtextures > 0, "R_InitTextures: invalid cached texture count" );
		int *translation = static_cast<int *>( output.Alloc(
			R_TextureAllocationBytes( size_t( ::g->s_numtextures ) + 1, sizeof( int ) ) ) );
		for ( int i = 0; i < ::g->s_numtextures; ++i ) {
			translation[i] = i;
		}
		::g->texturetranslation = translation;
		output.Release();
	}
}

} // namespace

void R_InitTextures( void ) {
	const char *errorMessage;
	try {
		R_LoadTextures();
		return;
	} catch ( const textureInitError_t &error ) {
		errorMessage = error.message;
	} catch ( const std::bad_alloc & ) {
		errorMessage = "R_InitTextures: allocation failed";
	}
	// I_Error returns; allowing its callers to resume would initialize/render
	// incomplete textures. Preserve its game-switch behavior, then unwind to
	// the existing idCommonLocal::Frame handler after local owners clean up.
	idLib::Printf( "DOOM Classic error: %s\n", errorMessage );
	DoomLib::Interface.QuitCurrentGame();
	common->SwitchToGame( DOOM3_BFG );
	throw idException( errorMessage );
}


//
// R_InitFlats
//
void R_InitFlats (void)
{
    int		i;
	
    ::g->firstflat = W_GetNumForName ("F_START") + 1;
    ::g->lastflat = W_GetNumForName ("F_END") - 1;
    ::g->numflats = ::g->lastflat - ::g->firstflat + 1;
	
    // Create translation table for global animation.
    ::g->flattranslation = (int*)DoomLib::Z_Malloc ((::g->numflats+1)*4, PU_STATIC, 0);
    
    for (i=0 ; i < ::g->numflats ; i++)
	::g->flattranslation[i] = i;
}


//
// R_InitSpriteLumps
// Finds the width and hoffset of all ::g->sprites in the wad,
//  so the sprite does not need to be cached completely
//  just for having the header info ready during rendering.
//
void R_InitSpriteLumps (void)
{
    int		i;
    patch_t	*patch;
	
    ::g->firstspritelump = W_GetNumForName ("S_START") + 1;
    ::g->lastspritelump = W_GetNumForName ("S_END") - 1;
    
    ::g->numspritelumps = ::g->lastspritelump - ::g->firstspritelump + 1;
    ::g->spritewidth = (fixed_t*)DoomLib::Z_Malloc (::g->numspritelumps*4, PU_STATIC, 0);
    ::g->spriteoffset = (fixed_t*)DoomLib::Z_Malloc (::g->numspritelumps*4, PU_STATIC, 0);
    ::g->spritetopoffset = (fixed_t*)DoomLib::Z_Malloc (::g->numspritelumps*4, PU_STATIC, 0);
	
    for (i=0 ; i< ::g->numspritelumps ; i++)
    {
	if (!(i&63))
	    I_Printf (".");

	patch = (patch_t*)W_CacheLumpNum (::g->firstspritelump+i, PU_CACHE_SHARED);
	::g->spritewidth[i] = SHORT(patch->width)<<FRACBITS;
	::g->spriteoffset[i] = SHORT(patch->leftoffset)<<FRACBITS;
	::g->spritetopoffset[i] = SHORT(patch->topoffset)<<FRACBITS;
    }
}



//
// R_InitColormaps
//
void R_InitColormaps (void)
{
    int	lump, length;
    
    // Load in the light tables, 
    //  256 byte align tables.
    lump = W_GetNumForName("COLORMAP"); 
    length = W_LumpLength (lump) + 255; 
    ::g->colormaps = (lighttable_t*)DoomLib::Z_Malloc (length, PU_STATIC, 0); 
    ::g->colormaps = reinterpret_cast<byte *>( ( reinterpret_cast<uintptr_t>( ::g->colormaps ) + uintptr_t( 255 ) ) & ~uintptr_t( 255 ) );
    W_ReadLump (lump,::g->colormaps); 
}



//
// R_InitData
// Locates all the lumps
//  that will be used by all views
// Must be called after W_Init.
//
void R_InitData (void)
{
    R_InitTextures ();
    I_Printf ("\nInitTextures");
    R_InitFlats ();
    I_Printf ("\nInitFlats");
    R_InitSpriteLumps ();
    I_Printf ("\nInitSprites");
    R_InitColormaps ();
    I_Printf ("\nInitColormaps");
}



//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
int R_FlatNumForName (const char* name)
{
    int		i;
    char	namet[9];

    i = W_CheckNumForName (name);

    if (i == -1)
    {
		namet[8] = 0;
		memcpy (namet, name,8);
		I_Error ("R_FlatNumForName: %s not found",namet);
    }
    return i - ::g->firstflat;
}




//
// R_CheckTextureNumForName
// Check whether texture is available.
// Filter out NoTexture indicator.
//
int	R_CheckTextureNumForName (const char *name)
{
    int		i;

    // "NoTexture" marker.
    if (name[0] == '-')		
	return 0;
		
    for (i=0 ; i < ::g->s_numtextures ; i++)
	if ( !idStr::Icmpn( ::g->s_textures[i]->name, name, 8 ) )
	    return i;
		
    return -1;
}



//
// R_TextureNumForName
// Calls R_CheckTextureNumForName,
//  aborts with error message.
//
int	R_TextureNumForName (const char* name)
{
    int		i;
	
    i = R_CheckTextureNumForName (name);

    if (i==-1)
    {
	I_Error ("R_TextureNumForName: %s not found",
		 name);
    }
    return i;
}




//
// R_PrecacheLevel
// Preloads all relevant graphics for the level.
//

void R_PrecacheLevel (void)
{
    int			i;
    int			j;
    int			k;
    int			lump;
    
    texture_t*		texture;
    thinker_t*		th;
    spriteframe_t*	sf;

    if (::g->demoplayback)
	return;
    
    // Precache flats.
	std::vector<char> flatpresent(::g->numflats, 0);
    
    for (i=0 ; i < ::g->numsectors ; i++)
    {
	flatpresent[::g->sectors[i].floorpic] = 1;
	flatpresent[::g->sectors[i].ceilingpic] = 1;
    }
	
    ::g->flatmemory = 0;

    for (i=0 ; i < ::g->numflats ; i++)
    {
	if (flatpresent[i])
	{
	    lump = ::g->firstflat + i;
	    ::g->flatmemory += lumpinfo[lump].size;
	    W_CacheLumpNum(lump, PU_CACHE_SHARED);
	}
    }
    
    // Precache textures.
    std::vector<char> texturepresent(::g->s_numtextures, 0);
	
    for (i=0 ; i < ::g->numsides ; i++)
    {
	texturepresent[::g->sides[i].toptexture] = 1;
	texturepresent[::g->sides[i].midtexture] = 1;
	texturepresent[::g->sides[i].bottomtexture] = 1;
    }

    // Sky texture is always present.
    // Note that F_SKY1 is the name used to
    //  indicate a sky floor/ceiling as a flat,
    //  while the sky texture is stored like
    //  a wall texture, with an episode dependend
    //  name.
    texturepresent[::g->skytexture] = 1;
	
    ::g->texturememory = 0;
    for (i=0 ; i < ::g->s_numtextures ; i++)
    {
	if (!texturepresent[i])
	    continue;

	texture = ::g->s_textures[i];
	
	for (j=0 ; j<texture->patchcount ; j++)
	{
	    lump = texture->patches[j].patch;
	    ::g->texturememory += lumpinfo[lump].size;
	    W_CacheLumpNum(lump , PU_CACHE_SHARED);
	}
    }
    
    // Precache ::g->sprites.
    std::vector<char> spritepresent(::g->numsprites, 0);
	
    for (th = ::g->thinkercap.next ; th != &::g->thinkercap ; th=th->next)
    {
	if (th->function.acp1 == (actionf_p1)P_MobjThinker)
	    spritepresent[((mobj_t *)th)->sprite] = 1;
    }
	
    ::g->spritememory = 0;
    for (i=0 ; i < ::g->numsprites ; i++)
    {
	if (!spritepresent[i])
	    continue;

	for (j=0 ; j < ::g->sprites[i].numframes ; j++)
	{
	    sf = &::g->sprites[i].spriteframes[j];
	    for (k=0 ; k<8 ; k++)
	    {
		lump = ::g->firstspritelump + sf->lump[k];
		::g->spritememory += lumpinfo[lump].size;
		W_CacheLumpNum(lump , PU_CACHE_SHARED);
	    }
	}
    }
}






/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 
Copyright (C) 2016-2017 Dustin Land

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

#pragma hdrstop
#include "../framework/precompiled.h"
#include "RenderSystem_local.h"
#include "Model_local.h"
#include "Model_md3.h"
#include <cmath>
#include <memory>

void R_FreeStaticTriSurf( srfTriangles_t *tri );
srfTriangles_t *	R_AllocStaticTriSurf();
void				R_AllocStaticTriSurfVerts( srfTriangles_t *tri, int numVerts );
void				R_AllocStaticTriSurfIndexes( srfTriangles_t *tri, int numIndexes );
void				R_BoundTriSurf( srfTriangles_t *tri );

/***********************************************************************

	idMD3Mesh

***********************************************************************/

// File records stay little-endian bytes. Only these private descriptors contain
// host-order metadata and native material pointers.
struct md3Range_t {
	size_t offset;
	size_t bytes;
};

struct md3RuntimeSurface_s {
	md3Surface_t header;
	size_t offset;
	md3Range_t shaders, triangles, texCoords, vertexes;
	const idMaterial **materials;
	int materialCount;
};

struct md3RuntimeData_s {
	md3Header_t header;
	byte *bytes;
	md3RuntimeSurface_s surfaces[MD3_MAX_SURFACES];
};

static_assert( sizeof( md3Header_t ) == 108 && sizeof( md3Surface_t ) == 108, "MD3 headers" );
static_assert( sizeof( md3Frame_t ) == 56 && sizeof( md3Tag_t ) == 112, "MD3 frame/tag records" );
static_assert( sizeof( md3Triangle_t ) == 12 && sizeof( md3St_t ) == 8 && sizeof( md3XyzNormal_t ) == 8, "MD3 geometry records" );

static void MD3_Free( md3RuntimeData_s *data ) {
	if ( data != NULL ) {
		for ( int i = 0; i < MD3_MAX_SURFACES; ++i ) {
			Mem_Free( data->surfaces[i].materials );
		}
		Mem_Free( data->bytes );
		Mem_Free( data );
	}
}

class md3LoadOwner_t {
public:
	md3LoadOwner_t() : data( NULL ) {}
	~md3LoadOwner_t() { MD3_Free( data ); }
	md3RuntimeData_s *data;
private:
	md3LoadOwner_t( const md3LoadOwner_t & );
	md3LoadOwner_t &operator=( const md3LoadOwner_t & );
};

static void MD3_Require( bool valid, const char *message ) {
	if ( !valid ) {
		throw idException( message );
	}
}

// Validate by subtraction/division before either multiplication or pointer formation.
static md3Range_t MD3_Range( size_t extent, int offset, size_t count, size_t width, size_t firstPayload ) {
	MD3_Require( offset >= 0, "negative record offset" );
	const size_t start = static_cast<size_t>( offset );
	MD3_Require( width != 0 && start <= extent, "record offset outside extent" );
	MD3_Require( count <= ( extent - start ) / width, "truncated record array" );
	MD3_Require( count == 0 || start >= firstPayload, "record overlaps header" );
	md3Range_t range = { start, count * width };
	return range;
}

static size_t MD3_Product( size_t a, size_t b ) {
	MD3_Require( b == 0 || a <= SIZE_MAX / b, "record count overflow" );
	return a * b;
}

static bool MD3_Overlap( const md3Range_t &a, const md3Range_t &b ) {
	return a.bytes != 0 && b.bytes != 0 && a.offset < b.offset + b.bytes && b.offset < a.offset + a.bytes;
}

// Call only with a previously admitted record. memcpy also supports unaligned files.
template< typename T > static T MD3_Record( const byte *bytes, size_t offset ) {
	T record;
	memcpy( &record, bytes + offset, sizeof( record ) );
	return record;
}

static size_t MD3_NameLength( const char *text, size_t capacity ) {
	size_t length = 0;
	while ( length < capacity && text[length] != '\0' ) {
		++length;
	}
	MD3_Require( length < capacity, "unterminated MD3 name" );
	return length;
}

static void MD3_DecodeHeader( md3Header_t &h ) {
	h.ident = LittleLong( h.ident );
	h.version = LittleLong( h.version );
	h.flags = LittleLong( h.flags );
	h.numFrames = LittleLong( h.numFrames );
	h.numTags = LittleLong( h.numTags );
	h.numSurfaces = LittleLong( h.numSurfaces );
	h.numSkins = LittleLong( h.numSkins );
	h.ofsFrames = LittleLong( h.ofsFrames );
	h.ofsTags = LittleLong( h.ofsTags );
	h.ofsSurfaces = LittleLong( h.ofsSurfaces );
	h.ofsEnd = LittleLong( h.ofsEnd );
}

static void MD3_DecodeSurface( md3Surface_t &s ) {
	s.ident = LittleLong( s.ident );
	s.flags = LittleLong( s.flags );
	s.numFrames = LittleLong( s.numFrames );
	s.numShaders = LittleLong( s.numShaders );
	s.numVerts = LittleLong( s.numVerts );
	s.numTriangles = LittleLong( s.numTriangles );
	s.ofsTriangles = LittleLong( s.ofsTriangles );
	s.ofsShaders = LittleLong( s.ofsShaders );
	s.ofsSt = LittleLong( s.ofsSt );
	s.ofsXyzNormals = LittleLong( s.ofsXyzNormals );
	s.ofsEnd = LittleLong( s.ofsEnd );
}

static void MD3_ValidateHeader( const md3Header_t &h ) {
	MD3_Require( h.numFrames >= 1 && h.numFrames <= MD3_MAX_FRAMES, "invalid frame count" );
	MD3_Require( h.numTags >= 0 && h.numTags <= MD3_MAX_TAGS, "invalid tag count" );
	MD3_Require( h.numSurfaces >= 0 && h.numSurfaces <= MD3_MAX_SURFACES, "invalid surface count" );
	MD3_Require( h.numSkins >= 0, "negative skin count" );
	const size_t extent = static_cast<size_t>( h.ofsEnd );
	const md3Range_t frames = MD3_Range( extent, h.ofsFrames, h.numFrames, sizeof( md3Frame_t ), sizeof( h ) );
	const md3Range_t tags = MD3_Range( extent, h.ofsTags, MD3_Product( h.numFrames, h.numTags ), sizeof( md3Tag_t ), sizeof( h ) );
	MD3_Require( !MD3_Overlap( frames, tags ), "overlapping frame/tag arrays" );
	MD3_Range( extent, h.ofsSurfaces, h.numSurfaces != 0 ? 1 : 0, sizeof( md3Surface_t ), sizeof( h ) );
}

static void MD3_Validate( md3RuntimeData_s &data ) {
	const md3Header_t &h = data.header;
	const size_t extent = static_cast<size_t>( h.ofsEnd );
	const md3Range_t frames = MD3_Range( extent, h.ofsFrames, h.numFrames, sizeof( md3Frame_t ), sizeof( h ) );
	const md3Range_t tags = MD3_Range( extent, h.ofsTags, MD3_Product( h.numFrames, h.numTags ), sizeof( md3Tag_t ), sizeof( h ) );
	size_t cursor = static_cast<size_t>( h.ofsSurfaces );
	for ( int i = 0; i < h.numSurfaces; ++i ) {
		MD3_Require( cursor <= extent && sizeof( md3Surface_t ) <= extent - cursor, "truncated surface header" );
		md3RuntimeSurface_s &surface = data.surfaces[i];
		surface.offset = cursor;
		surface.header = MD3_Record<md3Surface_t>( data.bytes, cursor );
		md3Surface_t &s = surface.header;
		MD3_DecodeSurface( s );
		MD3_Require( s.ident == MD3_IDENT, "invalid surface identifier" );
		MD3_Require( s.ofsEnd >= static_cast<int>( sizeof( s ) ) && static_cast<size_t>( s.ofsEnd ) <= extent - cursor, "invalid surface extent" );
		const md3Range_t wholeSurface = { cursor, static_cast<size_t>( s.ofsEnd ) };
		MD3_Require( !MD3_Overlap( wholeSurface, frames ) && !MD3_Overlap( wholeSurface, tags ), "surface overlaps frames/tags" );
		MD3_Require( s.numFrames == h.numFrames, "surface frame count mismatch" );
		MD3_Require( s.numShaders >= 0 && s.numShaders <= MD3_MAX_SHADERS, "invalid shader count" );
		MD3_Require( s.numVerts >= 0 && s.numVerts <= SHADER_MAX_VERTEXES, "invalid vertex count" );
		MD3_Require( s.numTriangles >= 0 && s.numTriangles <= SHADER_MAX_INDEXES / 3, "invalid triangle count" );
		surface.shaders = MD3_Range( s.ofsEnd, s.ofsShaders, s.numShaders, sizeof( md3DiskShader_t ), sizeof( s ) );
		surface.triangles = MD3_Range( s.ofsEnd, s.ofsTriangles, s.numTriangles, sizeof( md3Triangle_t ), sizeof( s ) );
		surface.texCoords = MD3_Range( s.ofsEnd, s.ofsSt, s.numVerts, sizeof( md3St_t ), sizeof( s ) );
		surface.vertexes = MD3_Range( s.ofsEnd, s.ofsXyzNormals, MD3_Product( s.numFrames, s.numVerts ), sizeof( md3XyzNormal_t ), sizeof( s ) );
		const md3Range_t ranges[] = { surface.shaders, surface.triangles, surface.texCoords, surface.vertexes };
		for ( int a = 0; a < 4; ++a ) {
			for ( int b = 0; b < a; ++b ) {
				MD3_Require( !MD3_Overlap( ranges[a], ranges[b] ), "overlapping surface records" );
			}
		}
		const size_t nameLength = MD3_NameLength( s.name, sizeof( s.name ) );
		for ( size_t j = 0; j < nameLength; ++j ) {
			s.name[j] = static_cast<char>( tolower( static_cast<unsigned char>( s.name[j] ) ) );
		}
		// Preserve the original rule: any final _X suffix, not just _1 and _2.
		if ( nameLength > 2 && s.name[nameLength - 2] == '_' ) {
			s.name[nameLength - 2] = '\0';
		}
		for ( int j = 0; j < s.numShaders; ++j ) {
			const md3DiskShader_t shader = MD3_Record<md3DiskShader_t>( data.bytes, cursor + surface.shaders.offset + static_cast<size_t>( j ) * sizeof( md3DiskShader_t ) );
			MD3_NameLength( shader.name, sizeof( shader.name ) );
		}
		for ( int j = 0; j < s.numTriangles; ++j ) {
			const md3Triangle_t tri = MD3_Record<md3Triangle_t>( data.bytes, cursor + surface.triangles.offset + static_cast<size_t>( j ) * sizeof( md3Triangle_t ) );
			for ( int k = 0; k < 3; ++k ) {
				const int vertex = LittleLong( tri.indexes[k] );
				MD3_Require( vertex >= 0 && vertex < s.numVerts, "triangle index outside vertex array" );
			}
		}
		cursor += wholeSurface.bytes;
	}
}

idRenderModelMD3::idRenderModelMD3() : index( 0 ), dataSize( 0 ), md3( NULL ), numLods( 0 ) {
	purged = true;
}

idRenderModelMD3::~idRenderModelMD3() {
	PurgeModel();
}

void idRenderModelMD3::PurgeModel() {
	MD3_Free( md3 );
	md3 = NULL;
	dataSize = 0;
	numLods = 0;
	timeStamp = 0;
	idRenderModelStatic::PurgeModel();
}

void idRenderModelMD3::InitFromFile( const char *fileName ) {
	name = fileName;
	PurgeModel();
	// A failed attempt is a defined defaulted model, not a partially loaded blob.
	defaulted = true;
	purged = false;
	md3LoadOwner_t pending;
	try {
		idFileLocal file( fileSystem->OpenFileRead( name ) );
		MD3_Require( file != NULL, "file not found" );
		const int fileLength = file->Length();
		MD3_Require( fileLength >= static_cast<int>( sizeof( md3Header_t ) ), "truncated model header" );
		md3Header_t diskHeader;
		MD3_Require( file->Read( &diskHeader, sizeof( diskHeader ) ) == sizeof( diskHeader ), "short header read" );
		md3Header_t header = diskHeader;
		MD3_DecodeHeader( header );
		MD3_Require( header.ident == MD3_IDENT, "invalid MD3 identifier" );
		MD3_Require( header.version == MD3_VERSION, "invalid MD3 version" );
		MD3_Require( header.ofsEnd >= static_cast<int>( sizeof( header ) ) && header.ofsEnd <= fileLength, "invalid model extent" );
		MD3_ValidateHeader( header );
		pending.data = static_cast<md3RuntimeData_s *>( Mem_Alloc( sizeof( md3RuntimeData_s ), TAG_MODEL ) );
		MD3_Require( pending.data != NULL, "runtime allocation failed" );
		memset( pending.data, 0, sizeof( *pending.data ) );
		pending.data->header = header;
		pending.data->bytes = static_cast<byte *>( Mem_Alloc( static_cast<size_t>( header.ofsEnd ), TAG_MODEL ) );
		MD3_Require( pending.data->bytes != NULL, "model allocation failed" );
		memcpy( pending.data->bytes, &diskHeader, sizeof( diskHeader ) );
		const int remaining = header.ofsEnd - static_cast<int>( sizeof( diskHeader ) );
		MD3_Require( file->Read( pending.data->bytes + sizeof( diskHeader ), remaining ) == remaining, "short model read" );
		MD3_Validate( *pending.data );
		// All binary records and names are admitted before material lookup begins.
		for ( int i = 0; i < header.numSurfaces; ++i ) {
			md3RuntimeSurface_s &s = pending.data->surfaces[i];
			if ( s.header.numShaders == 0 ) {
				continue;
			}
			const size_t materialBytes = MD3_Product( static_cast<size_t>( s.header.numShaders ), sizeof( *s.materials ) );
			s.materials = static_cast<const idMaterial **>( Mem_Alloc( materialBytes, TAG_MODEL ) );
			MD3_Require( s.materials != NULL, "material reference allocation failed" );
			for ( int j = 0; j < s.header.numShaders; ++j ) {
				const md3DiskShader_t shader = MD3_Record<md3DiskShader_t>( pending.data->bytes, s.offset + s.shaders.offset + static_cast<size_t>( j ) * sizeof( md3DiskShader_t ) );
				s.materials[j] = declManager->FindMaterial( shader.name );
				MD3_Require( s.materials[j] != NULL, "material lookup failed" );
				++s.materialCount;
			}
		}
		timeStamp = file->Timestamp();
		md3 = pending.data;
		pending.data = NULL;
		dataSize = header.ofsEnd;
		numLods = 1;
		defaulted = false;
	} catch ( idException &e ) {
		idLib::Warning( "InitFromFile: %s: %s", name.c_str(), e.GetError() );
	}
}

void idRenderModelMD3::TouchData() {
	if ( md3 == NULL || purged ) {
		return;
	}
	for ( int i = 0; i < md3->header.numSurfaces; ++i ) {
		const md3RuntimeSurface_s &s = md3->surfaces[i];
		if ( s.materialCount != s.header.numShaders || ( s.materialCount != 0 && s.materials == NULL ) ) {
			return;
		}
		for ( int j = 0; j < s.materialCount; ++j ) {
			if ( s.materials[j] != NULL ) {
				declManager->FindMaterial( s.materials[j]->GetName() );
			}
		}
	}
}

dynamicModel_t idRenderModelMD3::IsDynamicModel() const {
	return DM_CACHED;
}

static bool MD3_FrameIndex( float value, int frameCount, int &result ) {
	if ( !std::isfinite( value ) ) {
		return false;
	}
	const double truncated = std::trunc( static_cast<double>( value ) );
	if ( truncated < 0 || truncated >= frameCount ) {
		return false;
	}
	result = static_cast<int>( truncated );
	return true;
}

bool idRenderModelMD3::LerpMeshVertexes( srfTriangles_t *tri, const md3RuntimeSurface_s &surface, const float backlerp, const int frame, const int oldframe ) const {
	if ( md3 == NULL || frame < 0 || frame >= surface.header.numFrames ||
		oldframe < 0 || oldframe >= surface.header.numFrames || !std::isfinite( backlerp ) ) {
		return false;
	}
	const size_t frameBytes = static_cast<size_t>( surface.header.numVerts ) * sizeof( md3XyzNormal_t );
	const size_t start = surface.offset + surface.vertexes.offset;
	const size_t newOffset = start + static_cast<size_t>( frame ) * frameBytes;
	const size_t oldOffset = start + static_cast<size_t>( oldframe ) * frameBytes;
	const float newScale = MD3_XYZ_SCALE * ( 1.0 - backlerp );
	const float oldScale = MD3_XYZ_SCALE * backlerp;
	for ( int i = 0; i < surface.header.numVerts; ++i ) {
		const size_t offset = static_cast<size_t>( i ) * sizeof( md3XyzNormal_t );
		const md3XyzNormal_t current = MD3_Record<md3XyzNormal_t>( md3->bytes, newOffset + offset );
		idDrawVert &out = tri->verts[tri->numVerts++];
		if ( backlerp == 0 ) {
			for ( int axis = 0; axis < 3; ++axis ) {
				out.xyz[axis] = LittleShort( current.xyz[axis] ) * newScale;
			}
		} else {
			const md3XyzNormal_t previous = MD3_Record<md3XyzNormal_t>( md3->bytes, oldOffset + offset );
			for ( int axis = 0; axis < 3; ++axis ) {
				out.xyz[axis] = LittleShort( previous.xyz[axis] ) * oldScale + LittleShort( current.xyz[axis] ) * newScale;
			}
		}
	}
	return true;
}

idRenderModel *idRenderModelMD3::InstantiateDynamicModel( const renderEntity_t *ent, const viewDef_t *view, idRenderModel *cachedModel ) {
	delete cachedModel;
	if ( md3 == NULL || purged || ent == NULL ) {
		return NULL;
	}
	int frame, oldframe;
	const float backlerp = ent->shaderParms[SHADERPARM_MD3_BACKLERP];
	if ( !MD3_FrameIndex( ent->shaderParms[SHADERPARM_MD3_FRAME], md3->header.numFrames, frame ) ||
		!MD3_FrameIndex( ent->shaderParms[SHADERPARM_MD3_LASTFRAME], md3->header.numFrames, oldframe ) || !std::isfinite( backlerp ) ) {
		idLib::Warning( "InstantiateDynamicModel: %s: invalid MD3 animation parameters", name.c_str() );
		return NULL;
	}
	for ( int i = 0; i < md3->header.numSurfaces; ++i ) {
		const md3RuntimeSurface_s &s = md3->surfaces[i];
		if ( s.materialCount != s.header.numShaders || ( s.materialCount > 0 && ( s.materials == NULL || s.materials[0] == NULL ) ) ||
			( s.materialCount == 0 && tr.defaultMaterial == NULL ) ) {
			idLib::Warning( "InstantiateDynamicModel: %s: missing MD3 material reference", name.c_str() );
			return NULL;
		}
	}
	// Own the snapshot and any not-yet-attached surface during allocation failures.
	void *snapshotStorage = Mem_Alloc( sizeof( idRenderModelStatic ), TAG_MODEL );
	if ( snapshotStorage == NULL ) {
		idLib::Warning( "InstantiateDynamicModel: %s: MD3 snapshot allocation failed", name.c_str() );
		return NULL;
	}
	std::unique_ptr<idRenderModelStatic> snapshot( new (snapshotStorage) idRenderModelStatic );
	snapshot->bounds.Clear();
	for ( int i = 0; i < md3->header.numSurfaces; ++i ) {
		const md3RuntimeSurface_s &s = md3->surfaces[i];
		if ( s.header.numVerts == 0 ) {
			continue;
		}
		srfTriangles_t *allocated = static_cast<srfTriangles_t *>( Mem_Alloc( sizeof( srfTriangles_t ), TAG_SRFTRIS ) );
		if ( allocated == NULL ) {
			idLib::Warning( "InstantiateDynamicModel: %s: MD3 surface allocation failed", name.c_str() );
			return NULL;
		}
		memset( allocated, 0, sizeof( *allocated ) );
		std::unique_ptr<srfTriangles_t, void (*)( srfTriangles_t * )> tri( allocated, R_FreeStaticTriSurf );
		R_AllocStaticTriSurfVerts( tri.get(), s.header.numVerts );
		const int indexes = s.header.numTriangles * 3; // admitted <= SHADER_MAX_INDEXES
		R_AllocStaticTriSurfIndexes( tri.get(), indexes );
		if ( tri->verts == NULL || ( indexes != 0 && tri->indexes == NULL ) ) {
			idLib::Warning( "InstantiateDynamicModel: %s: MD3 geometry allocation failed", name.c_str() );
			return NULL;
		}
		if ( !LerpMeshVertexes( tri.get(), s, backlerp, frame, oldframe ) ) {
			return NULL;
		}
		for ( int j = 0; j < s.header.numTriangles; ++j ) {
			const md3Triangle_t diskTri = MD3_Record<md3Triangle_t>( md3->bytes, s.offset + s.triangles.offset + static_cast<size_t>( j ) * sizeof( md3Triangle_t ) );
			for ( int k = 0; k < 3; ++k ) {
				tri->indexes[j * 3 + k] = LittleLong( diskTri.indexes[k] );
			}
		}
		tri->numIndexes = indexes;
		for ( int j = 0; j < s.header.numVerts; ++j ) {
			const md3St_t st = MD3_Record<md3St_t>( md3->bytes, s.offset + s.texCoords.offset + static_cast<size_t>( j ) * sizeof( md3St_t ) );
			tri->verts[j].SetTexCoord( idVec2( LittleFloat( st.st[0] ), LittleFloat( st.st[1] ) ) );
		}
		R_BoundTriSurf( tri.get() );
		modelSurface_t output = {};
		output.id = i;
		output.geometry = tri.get();
		output.shader = s.materialCount != 0 ? s.materials[0] : tr.defaultMaterial;
		snapshot->AddSurface( output );
		tri.release();
	}
	return snapshot.release();
}

idBounds idRenderModelMD3::Bounds( const renderEntity_t *ent ) const {
	idBounds result;
	result.Clear();
	if ( ent == NULL || md3 == NULL || purged ) {
		result.AddPoint( idVec3( -10, -10, -10 ) );
		result.AddPoint( idVec3( 10, 10, 10 ) );
		return result;
	}
	const md3Frame_t frame = MD3_Record<md3Frame_t>( md3->bytes, static_cast<size_t>( md3->header.ofsFrames ) );
	for ( int i = 0; i < 2; ++i ) {
		result.AddPoint( idVec3( LittleFloat( frame.bounds[i][0] ), LittleFloat( frame.bounds[i][1] ), LittleFloat( frame.bounds[i][2] ) ) );
	}
	return result;
}

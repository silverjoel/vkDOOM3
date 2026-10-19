/* Private collision-cache decoding and native allocation contract.
 * The disk budgets describe historical allocations, not native object sizes.
 * Included after CollisionModel_local.h; no public or serialized layouts change.
 */
#pragma once
#include <cstddef>
#include <climits>
#include <new>
#include <type_traits>

namespace cmCache {

struct failure {
	const char *message;
	explicit failure( const char *text ) : message( text ) {}
};

inline void Require( bool condition, const char *message ) {
	if ( !condition ) { throw failure( message ); }
}

// The engine's global operator new can return NULL. Staging buffers must
// report allocation failure before constructing elements at NULL.
template<class T> struct allocator {
	typedef T value_type;
	allocator() {}
	template<class U> allocator( const allocator<U> & ) {}
	T *allocate( size_t count ) {
		Require( count <= SIZE_MAX / sizeof( T ), "staging size overflow" );
		void *memory = Mem_Alloc( count * sizeof( T ), TAG_TEMP );
		Require( memory != NULL, "collision staging allocation failed" );
		return static_cast<T *>( memory );
	}
	void deallocate( T *memory, size_t ) { Mem_Free( memory ); }
};

inline size_t Add( size_t a, size_t b ) {
	Require( b <= SIZE_MAX - a, "size addition overflow" );
	return a + b;
}
inline size_t Multiply( size_t a, size_t b ) {
	Require( a == 0 || b <= SIZE_MAX / a, "size multiplication overflow" );
	return a * b;
}

// No allocating noexcept constructors (including Debug iterator proxies): every
// staging allocation failure must unwind to the loader's rollback boundary.
template<class T> class array {
public:
	array() : values( NULL ), count( 0 ), capacity( 0 ) {}
	explicit array( size_t size ) : array() { resize( size ); }
	array( const array &other ) : array() {
		array copy;
		copy.resize( other.count );
		for ( size_t i = 0; i < other.count; i++ ) { copy[i] = other[i]; }
		Swap( copy );
	}
	array &operator=( const array &other ) { array copy( other ); Swap( copy ); return *this; }
	~array() { Clear(); }
	size_t size() const { return count; }
	T &operator[]( size_t index ) { assert( index < count ); return values[index]; }
	const T &operator[]( size_t index ) const { assert( index < count ); return values[index]; }
	const T *begin() const { return values; }
	const T *end() const { return count == 0 ? values : values + count; }
	void resize( size_t size ) {
		Reserve( size );
		while ( count < size ) { new ( values + count ) T(); count++; }
		while ( count > size ) { values[--count].~T(); }
	}
	void push_back( const T &value ) {
		// Callers append independent decoded values, never an element of this array.
		if ( count == capacity ) { Reserve( capacity == 0 ? 1 : Add( capacity, capacity ) ); }
		new ( values + count ) T( value ); count++;
	}
private:
	T *values;
	size_t count, capacity;
	void Clear() {
		while ( count != 0 ) { values[--count].~T(); }
		Mem_Free( values ); values = NULL; capacity = 0;
	}
	void Swap( array &other ) {
		T *v = values; values = other.values; other.values = v;
		size_t n = count; count = other.count; other.count = n;
		n = capacity; capacity = other.capacity; other.capacity = n;
	}
	void Reserve( size_t size ) {
		if ( size <= capacity ) { return; }
		array replacement;
		replacement.values = allocator<T>().allocate( size ); replacement.capacity = size;
		for ( size_t i = 0; i < count; i++ ) {
			new ( replacement.values + i ) T( values[i] ); replacement.count++;
		}
		Swap( replacement );
	}
};

class string {
public:
	string() {}
	string( size_t size, char value ) {
		bytes.resize( Add( size, 1 ) );
		for ( size_t i = 0; i < size; i++ ) { bytes[i] = value; }
	}
	const char *c_str() const { return bytes.size() == 0 ? "" : &bytes[0]; }
	bool empty() const { return bytes.size() <= 1; }
	char &operator[]( size_t index ) { return bytes[index]; }
	bool operator==( const char *text ) const { return strcmp( c_str(), text ) == 0; }
private:
	array<char> bytes;
};
inline int IntSize( size_t size ) {
	Require( size <= INT_MAX, "native allocation exceeds int capacity" );
	return static_cast<int>( size );
}
inline size_t Align( size_t size, size_t alignment ) {
	Require( alignment != 0 && ( alignment & ( alignment - 1 ) ) == 0, "invalid alignment" );
	return Add( size, alignment - 1 ) & ~( alignment - 1 );
}
inline int PolygonSize( int count ) {
	static_assert( std::is_standard_layout<cm_polygon_t>::value, "polygon offsetof requires standard layout" );
	Require( count > 0 && count <= CM_MAX_POLYGON_EDGES, "invalid polygon edge count" );
	return IntSize( Align( Add( offsetof( cm_polygon_t, edges ), Multiply( static_cast<size_t>( count ), sizeof( int ) ) ), alignof( cm_polygon_t ) ) );
}
inline int BrushSize( int count ) {
	static_assert( std::is_standard_layout<cm_brush_t>::value, "brush offsetof requires standard layout" );
	Require( count > 0, "invalid brush plane count" );
	return IntSize( Align( Add( offsetof( cm_brush_t, planes ), Multiply( static_cast<size_t>( count ), sizeof( idPlane ) ) ), alignof( cm_brush_t ) ) );
}
inline void *Take( byte *&next, int &remaining, int size, size_t alignment ) {
	Require( size > 0 && remaining >= size, "native collision block exhausted" );
	Require( ( reinterpret_cast<uintptr_t>( next ) & ( alignment - 1 ) ) == 0, "unaligned collision object" );
	byte *result = next;
	next += size;
	remaining -= size;
	return result;
}
inline void *Allocate( size_t size ) {
	IntSize( size );
	if ( size == 0 ) { return NULL; }
	void *memory = Mem_Alloc( size, TAG_COLLISION );
	Require( memory != NULL, "collision allocation failed" );
	memset( memory, 0, size );
	return memory;
}

// All reads are bounded and checked before swapping or interpreting their bytes.
class reader {
public:
	explicit reader( idFile *source ) : file( source ) {
		Require( file != NULL && file->Tell() >= 0 && file->Length() >= file->Tell(), "invalid cache offset" );
	}
	size_t Remaining() const {
		Require( file->Tell() >= 0 && file->Length() >= file->Tell(), "invalid cache offset" );
		return static_cast<size_t>( file->Length() - file->Tell() );
	}
	void Bytes( void *destination, size_t size ) {
		Require( size <= Remaining(), "truncated collision cache" );
		const int length = IntSize( size );
		Require( file->Read( destination, length ) == length, "short collision cache read" );
	}
	template<class T> void Big( T &value ) { Bytes( &value, sizeof( value ) ); idSwap::Big( value ); }
	int Integer() { int value; Big( value ); return value; }
	string String() {
		int length;
		Bytes( &length, sizeof( length ) );
		idSwap::Little( length );
		Require( length >= 0 && static_cast<size_t>( length ) <= Remaining(), "invalid cache string length" );
		string text( static_cast<size_t>( length ), '\0' );
		if ( length != 0 ) { Bytes( &text[0], static_cast<size_t>( length ) ); }
		for ( int i = 0; i < length; i++ ) { Require( text[i] != '\0', "embedded NUL in cache name" ); }
		return text;
	}
	void Count( int count, size_t minimumBytes ) const {
		Require( count >= 0 && Multiply( static_cast<size_t>( count ), minimumBytes ) <= Remaining(), "invalid cache count" );
	}
private:
	idFile *file;
};

struct polygon {
	cm_polygon_t value;
	int material;
	array<int> edges;
};
struct brush {
	cm_brush_t value;
	int material;
	array<idPlane> planes;
};
struct node {
	int planeType;
	float planeDist;
	int parent, children[2];
	array<int> polygons, brushes;
};
struct decoded {
	string name;
	idBounds bounds;
	int contents;
	bool convex;
	int counts[11];
	int diskPolygonBudget, diskBrushBudget;
	size_t polygonBytes, brushBytes;
	array<cm_vertex_t> vertices;
	array<cm_edge_t> edges;
	array<string> materials;
	array<polygon> polygons;
	array<brush> brushes;
	array<node> nodes;
	decoded() : polygonBytes( 0 ), brushBytes( 0 ) {}
};

inline int ReadNode( reader &input, decoded &data, int parent, int depth, int &polygonRefs, int &brushRefs ) {
	// Collision queries and teardown recurse too. Reject trees unsafe for those consumers.
	Require( depth <= 256 && data.nodes.size() <= static_cast<size_t>( data.counts[4] ), "invalid collision tree size/depth" );
	const int index = IntSize( data.nodes.size() );
	node item;
	item.parent = parent;
	item.children[0] = item.children[1] = -1;
	item.planeType = input.Integer();
	input.Big( item.planeDist );
	Require( item.planeType >= -1 && item.planeType <= 2, "invalid collision split axis" );
	for ( int i = input.Integer(); i != -1; i = input.Integer() ) {
		Require( i >= 0 && i < data.counts[2] && polygonRefs < data.counts[6], "invalid polygon reference" );
		item.polygons.push_back( i );
		polygonRefs++;
	}
	for ( int i = input.Integer(); i != -1; i = input.Integer() ) {
		Require( i >= 0 && i < data.counts[3] && brushRefs < data.counts[5], "invalid brush reference" );
		item.brushes.push_back( i );
		brushRefs++;
	}
	data.nodes.push_back( item );
	if ( item.planeType != -1 ) {
		const int front = ReadNode( input, data, index, depth + 1, polygonRefs, brushRefs );
		const int back = ReadNode( input, data, index, depth + 1, polygonRefs, brushRefs );
		data.nodes[index].children[0] = front;
		data.nodes[index].children[1] = back;
	}
	return index;
}

inline void Decode( reader &input, decoded &data ) {
	data.name = input.String();
	input.Big( data.bounds ); input.Big( data.contents );
	byte convex; input.Big( convex );
	Require( convex <= 1, "invalid convex flag" ); data.convex = convex != 0;
	for ( int i = 0; i < 11; i++ ) {
		data.counts[i] = input.Integer();
		Require( data.counts[i] >= 0, "negative collision count" );
	}
	input.Count( data.counts[0], 24 );
	IntSize( Multiply( static_cast<size_t>( data.counts[0] ), sizeof( cm_vertex_t ) ) );
	data.vertices.resize( data.counts[0] );
	for ( size_t i = 0; i < data.vertices.size(); i++ ) {
		cm_vertex_t &v = data.vertices[i];
		input.Big( v.p ); input.Big( v.checkcount ); input.Big( v.side ); input.Big( v.sideSet );
	}
	input.Count( data.counts[1], 36 );
	IntSize( Multiply( static_cast<size_t>( data.counts[1] ), sizeof( cm_edge_t ) ) );
	data.edges.resize( data.counts[1] );
	for ( size_t i = 0; i < data.edges.size(); i++ ) {
		cm_edge_t &e = data.edges[i];
		input.Big( e.checkcount ); input.Big( e.internal ); input.Big( e.numUsers );
		input.Big( e.side ); input.Big( e.sideSet );
		input.Big( e.vertexNum[0] ); input.Big( e.vertexNum[1] ); input.Big( e.normal );
		// Edge zero is the reserved sentinel and can exist in an empty model.
		Require( i == 0 || ( e.vertexNum[0] >= 0 && e.vertexNum[0] < data.counts[0] && e.vertexNum[1] >= 0 && e.vertexNum[1] < data.counts[0] ), "invalid edge vertex index" );
	}
	data.diskPolygonBudget = input.Integer(); data.diskBrushBudget = input.Integer();
	Require( data.diskPolygonBudget >= 0 && data.diskBrushBudget >= 0, "negative legacy allocation budget" );
	const int materialCount = input.Integer(); input.Count( materialCount, 4 );
	for ( int i = 0; i < materialCount; i++ ) { data.materials.push_back( input.String() ); }
	input.Count( data.counts[2], 60 );
	data.polygons.resize( data.counts[2] );
	for ( size_t i = 0; i < data.polygons.size(); i++ ) {
		polygon &p = data.polygons[i];
		p.material = input.Integer(); p.value.numEdges = input.Integer();
		Require( p.material >= 0 && p.material < materialCount, "invalid polygon material" );
		data.polygonBytes = Add( data.polygonBytes, PolygonSize( p.value.numEdges ) ); IntSize( data.polygonBytes );
		input.Big( p.value.bounds ); input.Big( p.value.checkcount ); input.Big( p.value.contents ); input.Big( p.value.plane );
		input.Count( p.value.numEdges, 4 ); p.edges.resize( p.value.numEdges );
		for ( size_t j = 0; j < p.edges.size(); j++ ) {
			const int edge = input.Integer();
			Require( edge != INT_MIN && edge != 0 && edge > -data.counts[1] && edge < data.counts[1], "invalid polygon edge index" );
			p.edges[j] = edge;
		}
		p.value.material = NULL; p.value.edges[0] = p.edges[0];
	}
	input.Count( data.counts[3], 60 );
	data.brushes.resize( data.counts[3] );
	for ( size_t i = 0; i < data.brushes.size(); i++ ) {
		brush &b = data.brushes[i];
		b.material = input.Integer(); b.value.numPlanes = input.Integer();
		Require( b.material >= 0 && b.material < materialCount, "invalid brush material" );
		data.brushBytes = Add( data.brushBytes, BrushSize( b.value.numPlanes ) ); IntSize( data.brushBytes );
		input.Big( b.value.checkcount ); input.Big( b.value.bounds ); input.Big( b.value.contents ); input.Big( b.value.primitiveNum );
		input.Count( b.value.numPlanes, 16 ); b.planes.resize( b.value.numPlanes );
		for ( size_t j = 0; j < b.planes.size(); j++ ) { input.Big( b.planes[j] ); }
		b.value.planes[0] = b.planes[0];
	}
	input.Count( data.counts[4], 16 );
	int polygonRefs = 0, brushRefs = 0;
	ReadNode( input, data, -1, 0, polygonRefs, brushRefs );
	// Source generation historically excludes the root; text CM includes it.
	Require( data.nodes.size() == static_cast<size_t>( data.counts[4] ) || data.nodes.size() == Add( static_cast<size_t>( data.counts[4] ), 1 ), "collision node count mismatch" );
	Require( polygonRefs == data.counts[6] && brushRefs == data.counts[5], "collision reference count mismatch" );
	array<bool> seenPolygons( data.polygons.size() ), seenBrushes( data.brushes.size() );
	for ( size_t i = 0; i < data.nodes.size(); i++ ) {
		for ( size_t j = 0; j < data.nodes[i].polygons.size(); j++ ) { seenPolygons[data.nodes[i].polygons[j]] = true; }
		for ( size_t j = 0; j < data.nodes[i].brushes.size(); j++ ) { seenBrushes[data.nodes[i].brushes[j]] = true; }
	}
	for ( size_t i = 0; i < seenPolygons.size(); i++ ) { Require( seenPolygons[i], "unreferenced polygon" ); }
	for ( size_t i = 0; i < seenBrushes.size(); i++ ) { Require( seenBrushes[i], "unreferenced brush" ); }
}

// Own every native allocation even before any tree references have been linked.
// No partial object depends on FreeTree_r finding it during rollback.
class ownedModel {
public:
	cm_model_t *model;
	ownedModel() : model( NULL ) {
		void *storage = Allocate( sizeof( cm_model_t ) );
		model = new ( storage ) cm_model_t();
	}
	~ownedModel() {
		if ( model == NULL ) { return; }
		Mem_Free( model->polygonBlock ); Mem_Free( model->brushBlock );
		Mem_Free( model->nodeBlocks ); Mem_Free( model->polygonRefBlocks ); Mem_Free( model->brushRefBlocks );
		Mem_Free( model->vertices ); Mem_Free( model->edges );
		model->~cm_model_t(); Mem_Free( model );
	}
	cm_model_t *Release() { cm_model_t *result = model; model = NULL; return result; }
private:
	ownedModel( const ownedModel & );
	ownedModel &operator=( const ownedModel & );
};

template<class Header, class Element> inline Header *Block( size_t count ) {
	static_assert( sizeof( Header ) % alignof( Element ) == 0, "unaligned block payload" );
	return static_cast<Header *>( Allocate( Add( sizeof( Header ), Multiply( count, sizeof( Element ) ) ) ) );
}

inline cm_model_t *Build( const decoded &data, const array<const idMaterial *> &materials ) {
	static_assert( sizeof( cm_polygonBlock_t ) % alignof( cm_polygon_t ) == 0, "polygon block alignment" );
	static_assert( sizeof( cm_brushBlock_t ) % alignof( cm_brush_t ) == 0, "brush block alignment" );
	Require( materials.size() == data.materials.size(), "material table mismatch" );
	// Check the complete int-backed runtime statistic before allocating any model storage.
	size_t used = Add( data.polygonBytes, data.brushBytes );
	used = Add( used, Multiply( data.vertices.size(), sizeof( cm_vertex_t ) ) );
	used = Add( used, Multiply( data.edges.size(), sizeof( cm_edge_t ) ) );
	used = Add( used, Multiply( data.nodes.size(), sizeof( cm_node_t ) ) );
	used = Add( used, Multiply( data.counts[6], sizeof( cm_polygonRef_t ) ) );
	used = Add( used, Multiply( data.counts[5], sizeof( cm_brushRef_t ) ) );
	IntSize( used );
	ownedModel owner;
	cm_model_t *model = owner.model;
	model->name = data.name.c_str(); model->bounds = data.bounds; model->contents = data.contents; model->isConvex = data.convex;
	model->numVertices = model->maxVertices = IntSize( data.vertices.size() );
	model->numEdges = model->maxEdges = IntSize( data.edges.size() );
	model->vertices = static_cast<cm_vertex_t *>( Allocate( Multiply( data.vertices.size(), sizeof( cm_vertex_t ) ) ) );
	model->edges = static_cast<cm_edge_t *>( Allocate( Multiply( data.edges.size(), sizeof( cm_edge_t ) ) ) );
	for ( size_t i = 0; i < data.vertices.size(); i++ ) { model->vertices[i] = data.vertices[i]; }
	for ( size_t i = 0; i < data.edges.size(); i++ ) { model->edges[i] = data.edges[i]; }
	model->polygonBlock = Block<cm_polygonBlock_t, byte>( data.polygonBytes );
	model->polygonBlock->next = reinterpret_cast<byte *>( model->polygonBlock + 1 );
	model->polygonBlock->bytesRemaining = IntSize( data.polygonBytes );
	model->brushBlock = Block<cm_brushBlock_t, byte>( data.brushBytes );
	model->brushBlock->next = reinterpret_cast<byte *>( model->brushBlock + 1 );
	model->brushBlock->bytesRemaining = IntSize( data.brushBytes );
	array<cm_polygon_t *> polygons( data.polygons.size() );
	array<cm_brush_t *> brushes( data.brushes.size() );
	for ( size_t i = 0; i < polygons.size(); i++ ) {
		const polygon &source = data.polygons[i];
		const int size = PolygonSize( source.value.numEdges );
		cm_polygon_t *p = static_cast<cm_polygon_t *>( Take( model->polygonBlock->next, model->polygonBlock->bytesRemaining, size, alignof( cm_polygon_t ) ) );
		new ( p ) cm_polygon_t( source.value ); p->material = materials[source.material];
		for ( size_t j = 0; j < source.edges.size(); j++ ) { p->edges[j] = source.edges[j]; }
		polygons[i] = p; model->numPolygons++; model->polygonMemory += size;
	}
	for ( size_t i = 0; i < brushes.size(); i++ ) {
		const brush &source = data.brushes[i];
		const int size = BrushSize( source.value.numPlanes );
		cm_brush_t *b = static_cast<cm_brush_t *>( Take( model->brushBlock->next, model->brushBlock->bytesRemaining, size, alignof( cm_brush_t ) ) );
		new ( b ) cm_brush_t( source.value ); b->material = materials[source.material];
		for ( size_t j = 0; j < source.planes.size(); j++ ) { b->planes[j] = source.planes[j]; }
		brushes[i] = b; model->numBrushes++; model->brushMemory += size;
	}
	model->nodeBlocks = Block<cm_nodeBlock_t, cm_node_t>( data.nodes.size() );
	model->polygonRefBlocks = Block<cm_polygonRefBlock_t, cm_polygonRef_t>( data.counts[6] );
	model->brushRefBlocks = Block<cm_brushRefBlock_t, cm_brushRef_t>( data.counts[5] );
	cm_node_t *nodes = reinterpret_cast<cm_node_t *>( model->nodeBlocks + 1 );
	cm_polygonRef_t *pr = reinterpret_cast<cm_polygonRef_t *>( model->polygonRefBlocks + 1 );
	cm_brushRef_t *br = reinterpret_cast<cm_brushRef_t *>( model->brushRefBlocks + 1 );
	for ( size_t i = 0; i < data.nodes.size(); i++ ) {
		const node &source = data.nodes[i]; cm_node_t &n = nodes[i];
		n.planeType = source.planeType; n.planeDist = source.planeDist;
		n.parent = source.parent < 0 ? NULL : &nodes[source.parent];
		for ( int child = 0; child < 2; child++ ) { n.children[child] = source.children[child] < 0 ? NULL : &nodes[source.children[child]]; }
		// Match the original reader's prepend order exactly.
		for ( size_t j = 0; j < source.polygons.size(); j++, pr++ ) {
			pr->p = polygons[source.polygons[j]]; pr->next = n.polygons; n.polygons = pr; model->numPolygonRefs++;
		}
		for ( size_t j = 0; j < source.brushes.size(); j++, br++ ) {
			br->b = brushes[source.brushes[j]]; br->next = n.brushes; n.brushes = br; model->numBrushRefs++;
		}
	}
	model->node = nodes;
	model->numNodes = IntSize( data.nodes.size() );
	model->numInternalEdges = data.counts[7]; model->numSharpEdges = data.counts[8];
	model->numRemovedPolys = data.counts[9]; model->numMergedPolys = data.counts[10];
	model->usedMemory = IntSize( used );
	Require( model->polygonBlock->bytesRemaining == 0 && model->brushBlock->bytesRemaining == 0, "collision capacity mismatch" );
	assert( model->polygonBlock->bytesRemaining == 0 );
	assert( model->brushBlock->bytesRemaining == 0 );
	return owner.Release();
}
} // namespace cmCache

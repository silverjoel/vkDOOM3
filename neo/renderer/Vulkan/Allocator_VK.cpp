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

#include "../../framework/precompiled.h"
#pragma hdrstop

#include "../RenderBackend.h"
#include "Allocator_VK.h"

#include <atomic>
#include <new>

// IDs are never recycled, even when a block's address is reused. Zero means
// invalid/exhausted; exhausting the descriptor's 32-bit namespace fails closed.
static std::atomic< uint32 > nextVulkanAllocationId( 1 );

static uint32 TakeVulkanAllocationId() {
	uint32 id = nextVulkanAllocationId.load( std::memory_order_relaxed );
	while ( id != 0 ) {
		const uint32 next = id == UINT32_MAX ? 0 : id + 1;
		if ( nextVulkanAllocationId.compare_exchange_weak( id, next, std::memory_order_relaxed ) ) {
			return id;
		}
	}
	return 0;
}

// The engine heap can return NULL. Check storage before constructing metadata.
template< typename T, typename... Args >
static T * NewVulkanMetadata( Args... args ) {
	void * storage = Mem_Alloc( sizeof( T ), TAG_NEW );
	return storage == NULL ? NULL : new ( storage ) T( args... );
}

template< typename T >
static void DeleteVulkanMetadata( T * object ) {
	if ( object != NULL ) {
		object->~T();
		Mem_Free( object );
	}
}

static bool AlignVulkanOffset( VkDeviceSize offset, VkDeviceSize alignment, VkDeviceSize & aligned ) {
	if ( alignment == 0 || ( alignment & ( alignment - 1 ) ) != 0 ) {
		return false;
	}
	const VkDeviceSize mask = alignment - 1;
	if ( offset > ~VkDeviceSize( 0 ) - mask ) {
		return false;
	}
	aligned = ( offset + mask ) & ~mask;
	return true;
}

idCVar r_vkDeviceLocalMemoryMB( "r_vkDeviceLocalMemoryMB", "128", CVAR_INTEGER | CVAR_INIT, "" );
idCVar r_vkHostVisibleMemoryMB( "r_vkHostVisibleMemoryMB", "64", CVAR_INTEGER | CVAR_INIT, "" );

static const char * memoryUsageStrings[ VULKAN_MEMORY_USAGES ] = {
	"VULKAN_MEMORY_USAGE_UNKNOWN",
	"VULKAN_MEMORY_USAGE_GPU_ONLY",
	"VULKAN_MEMORY_USAGE_CPU_ONLY",
	"VULKAN_MEMORY_USAGE_CPU_TO_GPU",
	"VULKAN_MEMORY_USAGE_GPU_TO_CPU",
};

static const char * allocationTypeStrings[ VULKAN_ALLOCATION_TYPES ] = {
	"VULKAN_ALLOCATION_TYPE_FREE",
	"VULKAN_ALLOCATION_TYPE_BUFFER",
	"VULKAN_ALLOCATION_TYPE_IMAGE",
	"VULKAN_ALLOCATION_TYPE_IMAGE_LINEAR",
	"VULKAN_ALLOCATION_TYPE_IMAGE_OPTIMAL",
};

/*
=============
FindMemoryTypeIndex
=============
*/
uint32 FindMemoryTypeIndex( const uint32 memoryTypeBits, const vulkanMemoryUsage_t usage ) {
	VkPhysicalDeviceMemoryProperties & physicalMemoryProperties = vkcontext.gpu.memProps;

	VkMemoryPropertyFlags required = 0;
	VkMemoryPropertyFlags preferred = 0;

	switch ( usage ) {
	case VULKAN_MEMORY_USAGE_GPU_ONLY:
		preferred |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		break;
	case VULKAN_MEMORY_USAGE_CPU_ONLY:
		required |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		break;
	case VULKAN_MEMORY_USAGE_CPU_TO_GPU:
		required |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		preferred |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		break;
	case VULKAN_MEMORY_USAGE_GPU_TO_CPU:
		required |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		preferred |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		break;
	default:
		idLib::FatalError( "idVulkanAllocator::AllocateFromPools: Unknown memory usage." );
	}

	for ( uint32 i = 0; i < physicalMemoryProperties.memoryTypeCount; ++i ) {
		if ( ( ( memoryTypeBits >> i ) & 1 ) == 0 ) {
			continue;
		}
		
		const VkMemoryPropertyFlags properties = physicalMemoryProperties.memoryTypes[ i ].propertyFlags;
		if ( ( properties & required ) != required ) {
			continue;
		}

		if ( ( properties & preferred ) != preferred ) {
			continue;
		}

		return i;
	}

	for ( uint32 i = 0; i < physicalMemoryProperties.memoryTypeCount; ++i ) {
		if ( ( ( memoryTypeBits >> i ) & 1 ) == 0 ) {
			continue;
		}
			
		const VkMemoryPropertyFlags properties = physicalMemoryProperties.memoryTypes[ i ].propertyFlags;
		if ( ( properties & required ) != required ) {
			continue;
		}

		return i;
	}

	return UINT32_MAX;
}

/*
================================================================================================

idVulkanAllocator

================================================================================================
*/

/*
=============
idVulkanBlock::idVulkanBlock
=============
*/
idVulkanBlock::idVulkanBlock( const uint32 memoryTypeIndex, const VkDeviceSize size, vulkanMemoryUsage_t usage ) :
	m_head( NULL ),
	m_nextBlockId( 0 ),
	m_memoryTypeIndex( memoryTypeIndex ),
	m_usage( usage ),
	m_deviceMemory( VK_NULL_HANDLE ),
	m_size( size ),
	m_allocated( 0 ),
	m_data( NULL ) {
}

/*
=============
idVulkanBlock::idVulkanBlock
=============
*/
idVulkanBlock::~idVulkanBlock() {
	Shutdown();
}

/*
=============
idVulkanBlock::Init
=============
*/
bool idVulkanBlock::Init() {
	if ( m_head != NULL || m_deviceMemory != VK_NULL_HANDLE || m_size == 0 ||
		m_memoryTypeIndex >= vkcontext.gpu.memProps.memoryTypeCount ) {
		return false;
	}
	chunk_t * head = NewVulkanMetadata< chunk_t >();
	if ( head == NULL ) {
		return false;
	}
	VkMemoryAllocateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize = m_size;
	info.memoryTypeIndex = m_memoryTypeIndex;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	if ( vkAllocateMemory( vkcontext.device, &info, NULL, &memory ) != VK_SUCCESS ) {
		DeleteVulkanMetadata( head );
		return false;
	}
	void * data = NULL;
	if ( IsHostVisible() ) {
		if ( vkMapMemory( vkcontext.device, memory, 0, m_size, 0, &data ) != VK_SUCCESS ) {
			vkFreeMemory( vkcontext.device, memory, NULL );
			DeleteVulkanMetadata( head );
			return false;
		}
	}
	m_deviceMemory = memory;
	m_data = static_cast< byte * >( data );
	m_head = head;
	m_head->id = 0;
	m_head->size = m_size;
	m_head->offset = 0;
	m_head->prev = NULL;
	m_head->next = NULL;
	m_head->type = VULKAN_ALLOCATION_TYPE_FREE;
	return true;
}

/*
=============
idVulkanBlock::Shutdown
=============
*/
void idVulkanBlock::Shutdown() {
	if ( m_data != NULL ) {
		vkUnmapMemory( vkcontext.device, m_deviceMemory );
	}
	m_data = NULL;
	if ( m_deviceMemory != VK_NULL_HANDLE ) {
		vkFreeMemory( vkcontext.device, m_deviceMemory, NULL );
	}
	m_deviceMemory = VK_NULL_HANDLE;
	chunk_t * current = m_head;
	while ( current != NULL ) {
		chunk_t * next = current->next;
		DeleteVulkanMetadata( current );
		current = next;
	}
	m_head = NULL;
	m_allocated = 0;
}

/*
=============
IsOnSamePage

Algorithm comes from the Vulkan 1.0.39 spec. "Buffer-Image Granularity"
Also known as "Linear-Optimal Granularity"
=============
*/
static bool IsOnSamePage( 
	VkDeviceSize rAOffset, VkDeviceSize rASize, 
	VkDeviceSize rBOffset, VkDeviceSize pageSize ) {

	assert( rAOffset + rASize <= rBOffset && rASize > 0 && pageSize > 0 );

	VkDeviceSize rAEnd = rAOffset + rASize - 1;
	VkDeviceSize rAEndPage = rAEnd & ~( pageSize -1 );
	VkDeviceSize rBStart = rBOffset;
	VkDeviceSize rBStartPage = rBStart & ~( pageSize - 1 );

	return rAEndPage == rBStartPage;
}

/*
=============
HasGranularityConflict

Check that allocation types obey buffer image granularity.
=============
*/
static bool HasGranularityConflict( vulkanAllocationType_t type1, vulkanAllocationType_t type2 ) {
	if ( type1 > type2 ) {
		SwapValues( type1, type2 );
	}

	switch( type1 ) {
	case VULKAN_ALLOCATION_TYPE_FREE:
		return false;
	case VULKAN_ALLOCATION_TYPE_BUFFER:
		return	type2 == VULKAN_ALLOCATION_TYPE_IMAGE || 
				type2 == VULKAN_ALLOCATION_TYPE_IMAGE_OPTIMAL;
	case VULKAN_ALLOCATION_TYPE_IMAGE:
		return  type2 == VULKAN_ALLOCATION_TYPE_IMAGE ||
				type2 == VULKAN_ALLOCATION_TYPE_IMAGE_LINEAR ||
				type2 == VULKAN_ALLOCATION_TYPE_IMAGE_OPTIMAL;
	case VULKAN_ALLOCATION_TYPE_IMAGE_LINEAR:
		return type2 == VULKAN_ALLOCATION_TYPE_IMAGE_OPTIMAL;
	case VULKAN_ALLOCATION_TYPE_IMAGE_OPTIMAL:
		return false;
	default:
		assert( false );
		return true;
	}
}

/*
=============
idVulkanBlock::Allocate
=============
*/
bool idVulkanBlock::Allocate(
	const uint32 size,
	const uint32 align,
	const VkDeviceSize granularity,
	const vulkanAllocationType_t allocType,
	vulkanAllocation_t & allocation ) {

	VkDeviceSize checked = 0;
	if ( size == 0 || !AlignVulkanOffset( 0, align, checked ) ||
		!AlignVulkanOffset( 0, granularity, checked ) ||
		allocType <= VULKAN_ALLOCATION_TYPE_FREE || allocType >= VULKAN_ALLOCATION_TYPES ||
		m_allocated > m_size || size > m_size - m_allocated || m_head == NULL ) {
		return false;
	}

	chunk_t * bestFit = NULL;
	VkDeviceSize offset = 0;
	VkDeviceSize reservedSize = 0;
	for ( chunk_t * current = m_head; current != NULL; current = current->next ) {
		if ( current->type != VULKAN_ALLOCATION_TYPE_FREE || size > current->size ) {
			continue;
		}
		if ( current->offset > m_size || current->size > m_size - current->offset ) {
			return false;
		}
		if ( !AlignVulkanOffset( current->offset, align, offset ) ) {
			continue;
		}

		// A reserved range ends exactly at its payload end, so the previous
		// occupied chunk's last page is also the resource's actual last page.
		chunk_t * previous = current->prev;
		if ( previous != NULL && granularity > 1 &&
			HasGranularityConflict( previous->type, allocType ) &&
			IsOnSamePage( previous->offset, previous->size, offset, granularity ) ) {
			if ( !AlignVulkanOffset( offset, granularity, offset ) ) {
				continue;
			}
		}

		const VkDeviceSize padding = offset - current->offset;
		if ( padding > current->size || size > current->size - padding ) {
			continue;
		}
		reservedSize = padding + size;
		if ( reservedSize > m_size - m_allocated ) {
			continue;
		}

		// The next payload starts at or after its reserved start. Using that
		// lower bound is conservative; padding is never reusable resource space.
		chunk_t * next = current->next;
		if ( next != NULL && granularity > 1 &&
			HasGranularityConflict( allocType, next->type ) &&
			IsOnSamePage( offset, size, next->offset, granularity ) ) {
			continue;
		}
		bestFit = current;
		break;
	}
	if ( bestFit == NULL ) {
		return false;
	}

	// Allocate metadata and obtain an identity before publishing any mutation.
	chunk_t * remainder = NULL;
	if ( bestFit->size > reservedSize ) {
		remainder = NewVulkanMetadata< chunk_t >();
		if ( remainder == NULL ) {
			return false;
		}
	}
	const uint32 id = TakeVulkanAllocationId();
	if ( id == 0 ) {
		DeleteVulkanMetadata( remainder );
		return false;
	}
	if ( remainder != NULL ) {
		remainder->id = 0;
		remainder->offset = bestFit->offset + reservedSize;
		remainder->size = bestFit->size - reservedSize;
		remainder->type = VULKAN_ALLOCATION_TYPE_FREE;
		remainder->prev = bestFit;
		remainder->next = bestFit->next;
		if ( remainder->next != NULL ) {
			remainder->next->prev = remainder;
		}
		bestFit->next = remainder;
	}
	bestFit->id = id;
	bestFit->type = allocType;
	bestFit->size = reservedSize;
	m_allocated += reservedSize;
	++m_nextBlockId;

	allocation.block = this;
	allocation.id = id;
	allocation.deviceMemory = m_deviceMemory;
	allocation.offset = offset;
	allocation.size = size;
	allocation.data = IsHostVisible() ? m_data + offset : NULL;
	return true;
}

/*
=============
idVulkanBlock::Free
=============
*/
void idVulkanBlock::Free( vulkanAllocation_t & allocation ) {
	chunk_t * current = NULL;
	if ( allocation.block == this && allocation.id != 0 && allocation.deviceMemory == m_deviceMemory ) {
		for ( current = m_head; current != NULL; current = current->next ) {
			if ( current->id == allocation.id && current->type != VULKAN_ALLOCATION_TYPE_FREE ) {
				break;
			}
		}
	}
	if ( current == NULL || allocation.size == 0 || allocation.offset < current->offset ||
		allocation.offset - current->offset > current->size ||
		allocation.size != current->size - ( allocation.offset - current->offset ) ||
		current->size > m_allocated ) {
		idLib::Warning( "idVulkanBlock::Free: Invalid or stale allocation. %p - %u", this, allocation.id );
		return;
	}

	m_allocated -= current->size;
	current->id = 0;
	current->type = VULKAN_ALLOCATION_TYPE_FREE;
	if ( current->prev != NULL && current->prev->type == VULKAN_ALLOCATION_TYPE_FREE ) {
		chunk_t * previous = current->prev;
		previous->size += current->size;
		previous->next = current->next;
		if ( current->next != NULL ) {
			current->next->prev = previous;
		}
		DeleteVulkanMetadata( current );
		current = previous;
	}
	if ( current->next != NULL && current->next->type == VULKAN_ALLOCATION_TYPE_FREE ) {
		chunk_t * next = current->next;
		current->size += next->size;
		current->next = next->next;
		if ( next->next != NULL ) {
			next->next->prev = current;
		}
		DeleteVulkanMetadata( next );
	}
	allocation = vulkanAllocation_t();
}

/*
=============
idVulkanBlock::Print
=============
*/
void idVulkanBlock::Print() {
	int count = 0;
	for ( chunk_t * current = m_head; current != NULL; current = current->next ) { count++; }

	idLib::Printf( "Type Index: %lu\n", m_memoryTypeIndex );
	idLib::Printf( "Usage:      %s\n", memoryUsageStrings[ m_usage ] );
	idLib::Printf( "Count:      %d\n", count );
	idLib::Printf( "Size:       %llu\n", m_size );
	idLib::Printf( "Allocated:  %llu\n", m_allocated );
	idLib::Printf( "Allocation Count: %lu\n", m_nextBlockId );
	idLib::Printf( "------------------------\n" );

	for ( chunk_t * current = m_head; current != NULL; current = current->next ) {
		idLib::Printf( "{\n" );

		idLib::Printf( "\tId:     %lu\n", current->id );
		idLib::Printf( "\tSize:   %llu\n", current->size );
		idLib::Printf( "\tOffset: %llu\n", current->offset );
		idLib::Printf( "\tType:   %s\n", allocationTypeStrings[ current->type ] );

		idLib::Printf( "}\n" );
	}

	idLib::Printf( "\n" );
}

/*
================================================================================================

idVulkanAllocator

================================================================================================
*/

#if defined( ID_USE_AMD_ALLOCATOR )
VmaAllocator vmaAllocator;
#else
idVulkanAllocator vulkanAllocator;
#endif

/*
=============
idVulkanAllocator::idVulkanAllocator
=============
*/
idVulkanAllocator::idVulkanAllocator() : 
	m_garbageIndex( 0 ),
	m_deviceLocalMemoryBytes( 0 ),
	m_hostVisibleMemoryBytes( 0 ),
	m_bufferImageGranularity( 0 ) {
	
}

/*
=============
idVulkanAllocator::Init
=============
*/
void idVulkanAllocator::Init() {
	m_deviceLocalMemoryBytes = r_vkDeviceLocalMemoryMB.GetInteger() * 1024 * 1024;
	m_hostVisibleMemoryBytes = r_vkHostVisibleMemoryMB.GetInteger() * 1024 * 1024;
	m_bufferImageGranularity = vkcontext.gpu.props.limits.bufferImageGranularity;
}

/*
=============
idVulkanAllocator::Shutdown
=============
*/
void idVulkanAllocator::Shutdown() {
	for ( int i = 0; i < NUM_FRAME_DATA; ++i ) {
		EmptyGarbage();
	}
	for ( int i = 0; i < VK_MAX_MEMORY_TYPES; ++i ) {
		idList< idVulkanBlock * > & blocks = m_blocks[ i ];
		const int numBlocks = blocks.Num();
		for ( int j = 0; j < numBlocks; ++j ) {
			DeleteVulkanMetadata( blocks[ j ] );
		}

		blocks.Clear();
	}
}

/*
=============
idVulkanAllocator::Allocate
=============
*/
vulkanAllocation_t idVulkanAllocator::Allocate( 
		const uint32 size, 
		const uint32 align, 
		const uint32 memoryTypeBits,
		const vulkanMemoryUsage_t usage,
		const vulkanAllocationType_t allocType ) {
	
	vulkanAllocation_t allocation;

	uint32 memoryTypeIndex = FindMemoryTypeIndex( memoryTypeBits, usage );
	if ( memoryTypeIndex == UINT32_MAX ) {
		idLib::FatalError( "idVulkanAllocator::Allocate: Unable to find a memoryTypeIndex for allocation request." );
	}

	idList< idVulkanBlock * > & blocks = m_blocks[ memoryTypeIndex ];
	const int numBlocks = blocks.Num();
	for ( int i = 0; i < numBlocks; ++i ) {
		idVulkanBlock * block = blocks[ i ];

		if ( block->m_memoryTypeIndex != memoryTypeIndex ) {
			continue;
		}

		if ( block->Allocate( size, align, m_bufferImageGranularity, allocType, allocation ) ) {
			return allocation;
		}
	}

	VkDeviceSize blockSize = ( usage == VULKAN_MEMORY_USAGE_GPU_ONLY ) ? m_deviceLocalMemoryBytes : m_hostVisibleMemoryBytes;

	idVulkanBlock * block = NewVulkanMetadata< idVulkanBlock >( memoryTypeIndex, blockSize, usage );
	if ( block == NULL || !block->Init() ||
		!block->Allocate( size, align, m_bufferImageGranularity, allocType, allocation ) ) {
		DeleteVulkanMetadata( block );
		idLib::FatalError( "idVulkanAllocator::Allocate: Could not satisfy allocation request." );
		return vulkanAllocation_t();
	}
	blocks.Append( block );

	return allocation;
}

/*
=============
idVulkanAllocator::Free
=============
*/
void idVulkanAllocator::Free( const vulkanAllocation_t allocation ) {
	if (allocation.block == NULL) {
		// Never-allocated (default-constructed) allocation; nothing to free.
		return;
	}
	m_garbage[m_garbageIndex].Append( allocation );
}

/*
=============
idVulkanAllocator::EmptyGarbage
=============
*/
void idVulkanAllocator::EmptyGarbage() {
	m_garbageIndex = ( m_garbageIndex + 1 ) % NUM_FRAME_DATA;

	idList< vulkanAllocation_t > & garbage = m_garbage[ m_garbageIndex ];
	
	const int numAllocations = garbage.Num();
	for (int i = 0; i < numAllocations; ++i) {
		vulkanAllocation_t allocation = garbage[i];

		// A queued descriptor may outlive a previous shutdown. Validate block
		// membership before dereferencing it; allocation IDs handle address reuse.
		bool found = false;
		for ( int type = 0; type < VK_MAX_MEMORY_TYPES && !found; ++type ) {
			for ( int j = 0; j < m_blocks[ type ].Num(); ++j ) {
				if ( m_blocks[ type ][ j ] == allocation.block ) {
					allocation.block->Free( allocation );
					found = true;
					break;
				}
			}
		}
		if ( !found ) {
			idLib::Warning( "idVulkanAllocator::EmptyGarbage: Unknown allocation block." );
		}

		// NOTE: deliberately NOT deleting emptied blocks here. Garbage
		// entries later in this list — or in other ring slots — may still
		// reference this block; eager deletion was a use-after-free that
		// corrupted the heap (manifested as vkBindImageMemory failures
		// during vid_restart). Empty 128MB pools are retained until
		// Shutdown instead.
	}

	garbage.Clear();
}

/*
=============
idVulkanAllocator::Print
=============
*/
void idVulkanAllocator::Print() {
	idLib::Printf( "Device Local MB: %d\n", int( m_deviceLocalMemoryBytes / 1024 * 1024 ) );
	idLib::Printf( "Host Visible MB: %d\n", int( m_hostVisibleMemoryBytes / 1024 * 1024 ) );
	idLib::Printf( "Buffer Granularity: %llu\n", m_bufferImageGranularity );
	idLib::Printf( "\n" );

	for ( int i = 0; i < VK_MAX_MEMORY_TYPES; ++i ) {
		idList< idVulkanBlock * > & blocksByType = m_blocks[ i ];

		const int numBlocks = blocksByType.Num();
		for ( int j = 0; j < numBlocks; ++j ) {
			blocksByType[ j ]->Print();
		}
	}
}

CONSOLE_COMMAND( Vulkan_PrintHeapInfo, "Print out the heap information for this hardware.", 0 ) {
	VkPhysicalDeviceMemoryProperties & props = vkcontext.gpu.memProps;

	idLib::Printf( "Heaps %lu\n------------------------\n", props.memoryHeapCount );
	for ( uint32 i = 0; i < props.memoryHeapCount; ++i ) {
		VkMemoryHeap heap = props.memoryHeaps[ i ];
		idLib::Printf( "id=%d, size=%llu, flags=", i, heap.size );
		if ( heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) {
			idLib::Printf( "DEVICE_LOCAL" );
		} else {
			idLib::Printf( "HOST_VISIBLE" );
		}
		idLib::Printf( "\n" );

		for ( uint32 j = 0; j < props.memoryTypeCount; ++j ) {
			VkMemoryType type = props.memoryTypes[ j ];
			if ( type.heapIndex != i ) {
				continue;
			}

			idStr properties;
			if ( type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) {
				properties += "\tDEVICE_LOCAL\n";
			}
			if ( type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) {
				properties += "\tHOST_VISIBLE\n";
			}
			if ( type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ) {
				properties += "\tHOST_COHERENT\n";
			}
			if ( type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ) {
				properties += "\tHOST_CACHED\n";
			}
			if ( type.propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ) {
				properties += "\tLAZILY_ALLOCATED\n";
			}

			if ( properties.Length() > 0 ) {
				idLib::Printf( "memory_type=%lu\n", j );
				idLib::Printf( properties.c_str() );
			}
		}

		idLib::Printf( "\n" );
	}
}

CONSOLE_COMMAND( Vulkan_PrintAllocations, "Print out all the current allocations.", 0 ) {
#if defined( ID_USE_AMD_ALLOCATOR )
	// TODO
#else
	vulkanAllocator.Print();
#endif
}
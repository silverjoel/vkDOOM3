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
#include "Staging_VK.h"

idCVar r_vkUploadBufferSizeMB( "r_vkUploadBufferSizeMB", "64", CVAR_INTEGER | CVAR_INIT, "Size of gpu upload buffer." );

/*
===========================================================================

idVulkanStagingManager

===========================================================================
*/

idVulkanStagingManager stagingManager;

/*
=============
idVulkanStagingManager::idVulkanStagingManager
=============
*/
idVulkanStagingManager::idVulkanStagingManager() :
	m_maxBufferSize( 0 ),
	m_currentBuffer( 0 ),
	m_mappedData( NULL ),
	m_memory( VK_NULL_HANDLE ),
	m_commandPool( VK_NULL_HANDLE ) {
	
}

/*
=============
idVulkanStagingManager::~idVulkanStagingManager
=============
*/
idVulkanStagingManager::~idVulkanStagingManager() {
	
}

/*
=============
idVulkanStagingManager::Init
=============
*/
void idVulkanStagingManager::Init() {
	// Publish each owned resource only after success: error teardown may
	// observe partial initialization, and failed outputs are not ownership.
	m_maxBufferSize = (size_t)( r_vkUploadBufferSizeMB.GetInteger() * 1024 * 1024 );

	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = m_maxBufferSize;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	for ( int i = 0; i < NUM_FRAME_DATA; ++i ) {
		m_buffers[ i ].offset = 0;

		VkBuffer buffer = VK_NULL_HANDLE;
		const VkResult bufferResult = vkCreateBuffer( vkcontext.device, &bufferCreateInfo, NULL, &buffer );
		if ( bufferResult != VK_SUCCESS ) {
			idLib::FatalError( "VK: %s - vkCreateBuffer", VK_ErrorToString( bufferResult ) );
			return;
		}
		m_buffers[ i ].buffer = buffer;
	}

	VkMemoryRequirements memoryRequirements;
	vkGetBufferMemoryRequirements( vkcontext.device, m_buffers[ 0 ].buffer, &memoryRequirements );

	const VkDeviceSize alignMod = memoryRequirements.size % memoryRequirements.alignment;
	const VkDeviceSize alignedSize = ( alignMod == 0 ) ? memoryRequirements.size : ( memoryRequirements.size + memoryRequirements.alignment - alignMod );

	VkMemoryAllocateInfo memoryAllocateInfo = {};
	memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memoryAllocateInfo.allocationSize = alignedSize * NUM_FRAME_DATA;
	memoryAllocateInfo.memoryTypeIndex = FindMemoryTypeIndex( memoryRequirements.memoryTypeBits, VULKAN_MEMORY_USAGE_CPU_TO_GPU );

	VkDeviceMemory memory = VK_NULL_HANDLE;
	const VkResult memoryResult = vkAllocateMemory( vkcontext.device, &memoryAllocateInfo, NULL, &memory );
	if ( memoryResult != VK_SUCCESS ) {
		idLib::FatalError( "VK: %s - vkAllocateMemory", VK_ErrorToString( memoryResult ) );
		return;
	}
	m_memory = memory;

	for ( int i = 0; i < NUM_FRAME_DATA; ++i ) {
		ID_VK_CHECK( vkBindBufferMemory( vkcontext.device, m_buffers[ i ].buffer, m_memory, i * alignedSize ) );
	}

	void * mappedData = NULL;
	const VkResult mappedDataResult = vkMapMemory( vkcontext.device, m_memory, 0, alignedSize * NUM_FRAME_DATA, 0, &mappedData );
	if ( mappedDataResult != VK_SUCCESS ) {
		idLib::FatalError( "VK: %s - vkMapMemory", VK_ErrorToString( mappedDataResult ) );
		return;
	}
	m_mappedData = static_cast< byte * >( mappedData );

	VkCommandPoolCreateInfo commandPoolCreateInfo = {};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPoolCreateInfo.queueFamilyIndex = vkcontext.graphicsFamilyIdx;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	const VkResult commandPoolResult = vkCreateCommandPool( vkcontext.device, &commandPoolCreateInfo, NULL, &commandPool );
	if ( commandPoolResult != VK_SUCCESS ) {
		idLib::FatalError( "VK: %s - vkCreateCommandPool", VK_ErrorToString( commandPoolResult ) );
		return;
	}
	m_commandPool = commandPool;

	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {};
	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.commandPool = m_commandPool;
	commandBufferAllocateInfo.commandBufferCount = 1;

	VkFenceCreateInfo fenceCreateInfo = {};
	fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

	VkCommandBufferBeginInfo commandBufferBeginInfo = {};
	commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	for ( int i = 0; i < NUM_FRAME_DATA; ++i ) {
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		const VkResult commandBufferResult = vkAllocateCommandBuffers( vkcontext.device, &commandBufferAllocateInfo, &commandBuffer );
		if ( commandBufferResult != VK_SUCCESS ) {
			idLib::FatalError( "VK: %s - vkAllocateCommandBuffers", VK_ErrorToString( commandBufferResult ) );
			return;
		}
		m_buffers[ i ].commandBuffer = commandBuffer;
		VkFence fence = VK_NULL_HANDLE;
		const VkResult fenceResult = vkCreateFence( vkcontext.device, &fenceCreateInfo, NULL, &fence );
		if ( fenceResult != VK_SUCCESS ) {
			idLib::FatalError( "VK: %s - vkCreateFence", VK_ErrorToString( fenceResult ) );
			return;
		}
		m_buffers[ i ].fence = fence;
		ID_VK_CHECK( vkBeginCommandBuffer( m_buffers[ i ].commandBuffer, &commandBufferBeginInfo ) );

		m_buffers[ i ].data = (byte *)m_mappedData + (i * alignedSize);
	}
}

/*
=============
idVulkanStagingManager::Shutdown
=============
*/
void idVulkanStagingManager::Shutdown() {
	// Caller has stopped CPU producers and completed submitted GPU work.
	// Discard unsubmitted recordings; do not submit them during teardown.
	for ( int i = 0; i < NUM_FRAME_DATA; ++i ) {
		if ( m_buffers[ i ].commandBuffer != VK_NULL_HANDLE ) {
			vkFreeCommandBuffers( vkcontext.device, m_commandPool, 1, &m_buffers[ i ].commandBuffer );
			m_buffers[ i ].commandBuffer = VK_NULL_HANDLE;
		}
		if ( m_buffers[ i ].fence != VK_NULL_HANDLE ) {
			vkDestroyFence( vkcontext.device, m_buffers[ i ].fence, NULL );
			m_buffers[ i ].fence = VK_NULL_HANDLE;
		}
		m_buffers[ i ].data = NULL;
	}
	if ( m_commandPool != VK_NULL_HANDLE ) {
		vkDestroyCommandPool( vkcontext.device, m_commandPool, NULL );
		m_commandPool = VK_NULL_HANDLE;
	}
	if ( m_mappedData != NULL ) {
		vkUnmapMemory( vkcontext.device, m_memory );
		m_mappedData = NULL;
	}
	for ( int i = 0; i < NUM_FRAME_DATA; ++i ) {
		if ( m_buffers[ i ].buffer != VK_NULL_HANDLE ) {
			vkDestroyBuffer( vkcontext.device, m_buffers[ i ].buffer, NULL );
		}
		m_buffers[ i ] = stagingBuffer_t();
	}
	if ( m_memory != VK_NULL_HANDLE ) {
		vkFreeMemory( vkcontext.device, m_memory, NULL );
		m_memory = VK_NULL_HANDLE;
	}
	m_maxBufferSize = 0;
	m_currentBuffer = 0;
}

/*
=============
idVulkanStagingManager::Stage
=============
*/
byte * idVulkanStagingManager::Stage( const int size, const int alignment, VkCommandBuffer & commandBuffer, VkBuffer & buffer, int & bufferOffset ) {
	if ( size > m_maxBufferSize ) {
		idLib::FatalError( "Can't allocate %d MB in gpu transfer buffer", (int)( size / 1024 / 1024 ) );
	}

	stagingBuffer_t * stage = &m_buffers[ m_currentBuffer ];
	const int alignMod = stage->offset % alignment;
	stage->offset = ( ( stage->offset % alignment ) == 0 ) ? stage->offset : ( stage->offset + alignment - alignMod );

	if ( ( stage->offset + size ) >= ( m_maxBufferSize ) && !stage->submitted ) {
		Flush();
	}

	stage = &m_buffers[ m_currentBuffer ];
	if ( stage->submitted ) {
		Wait( *stage );
	}

	commandBuffer = stage->commandBuffer;
	buffer = stage->buffer;
	bufferOffset = stage->offset;

	byte * data = stage->data + stage->offset;
	stage->offset += size;

	return data;
}

/*
=============
idVulkanStagingManager::Flush
=============
*/
void idVulkanStagingManager::Flush() {
	stagingBuffer_t & stage = m_buffers[ m_currentBuffer ];
	if ( stage.submitted || stage.offset == 0 ) {
		return;
	}

	VkMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
	vkCmdPipelineBarrier( 
		stage.commandBuffer, 
		VK_PIPELINE_STAGE_TRANSFER_BIT, 
		VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 
		0, 1, &barrier, 0, NULL, 0, NULL );

	vkEndCommandBuffer( stage.commandBuffer );

	VkMappedMemoryRange memoryRange = {};
	memoryRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	memoryRange.memory = m_memory;
	memoryRange.size = VK_WHOLE_SIZE;
	vkFlushMappedMemoryRanges( vkcontext.device, 1, &memoryRange );

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &stage.commandBuffer;

	vkQueueSubmit( vkcontext.graphicsQueue, 1, &submitInfo, stage.fence );

	stage.submitted = true;

	m_currentBuffer = ( m_currentBuffer + 1 ) % NUM_FRAME_DATA;
}

/*
=============
idVulkanStagingManager::Wait
=============
*/
void idVulkanStagingManager::Wait( stagingBuffer_t & stage ) {
	if ( stage.submitted == false ) {
		return;
	}

	ID_VK_CHECK( vkWaitForFences( vkcontext.device, 1, &stage.fence, VK_TRUE, UINT64_MAX ) );
	ID_VK_CHECK( vkResetFences( vkcontext.device, 1, &stage.fence ) );

	stage.offset = 0;
	stage.submitted = false;

	VkCommandBufferBeginInfo commandBufferBeginInfo = {};
	commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	ID_VK_CHECK( vkBeginCommandBuffer( stage.commandBuffer, &commandBufferBeginInfo ) );
}
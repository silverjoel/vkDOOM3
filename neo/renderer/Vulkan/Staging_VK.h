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

#ifndef __STAGING_VK__
#define __STAGING_VK__

/*
===========================================================================

idVulkanStagingManager

===========================================================================
*/

struct stagingBuffer_t {
	stagingBuffer_t() :
		submitted( false ),
		commandBuffer( VK_NULL_HANDLE ),
		buffer( VK_NULL_HANDLE ),
		fence( VK_NULL_HANDLE ),
		offset( 0 ),
		data( NULL ) {}

	bool				submitted;
	VkCommandBuffer		commandBuffer;
	VkBuffer			buffer;
	VkFence				fence;
	VkDeviceSize		offset;
	byte *				data;
};

class idVulkanStagingManager {
public:
	idVulkanStagingManager();
	~idVulkanStagingManager();

	void			Init();
	void			Shutdown();

	byte *			Stage( const int size, const int alignment, VkCommandBuffer & commandBuffer, VkBuffer & buffer, int & bufferOffset );
	void			Flush();

private:
	void			Wait( stagingBuffer_t & stage );

private:
	int				m_maxBufferSize;
	int				m_currentBuffer;
	byte *			m_mappedData;
	VkDeviceMemory	m_memory;
	VkCommandPool	m_commandPool;

	stagingBuffer_t m_buffers[ NUM_FRAME_DATA ];
};

// Inactive prerequisite: no live manager/caller uses this interface yet.
// The caller owns every Vulkan object and all source/destination lifetimes.
// It must provide an exclusively owned recording command buffer, an unsignaled
// fence, valid mapped memory, and external queue/pool synchronization. The
// mutex below protects this record only, not other users of those objects.
struct stagingSubmissionResources_t {
	VkDevice device;
	VkQueue queue;
	VkDeviceMemory memory;
	VkBuffer source;
	VkCommandBuffer commandBuffer;
	VkFence fence;
};

struct stagingSubmissionResult_t {
	enum state_t { UNINITIALIZED, RECORDING, NO_WORK, ACCEPTED, COMPLETED, FAILED } state;
	enum phase_t { NONE, END_COMMANDS, FLUSH_MEMORY, SUBMIT, WAIT, RESET_FENCE, BEGIN_COMMANDS } phase;
	enum retention_t { NONE_RETAINED, RECORDING_REFERENCES, UNSUBMITTED_REFERENCES,
		PENDING_REFERENCES, COMPLETED_REFERENCES, UNCERTAIN_REFERENCES } retention;
	VkResult result;
	uint64 generation;
	stagingSubmissionResources_t resources;
	bool submissionAccepted;
	bool completionObserved;
	bool failureReported;
	char message[96]; // owned text; copies remain valid after acknowledgment/reuse
};

// Explicitly invoked, non-owning checked batch operation. No ring mutation,
// resource destruction, retry, cancellation, logging, or framework error route.
// Keep referenced resources alive until a separate owner resolves retention.
// Destruction releases only the CPU mutex; never use it concurrently with a call.
class idVulkanStagingSubmission {
public:
	idVulkanStagingSubmission();
	bool Configure( const stagingSubmissionResources_t & resources, uint64 generation, bool hasWork );
	stagingSubmissionResult_t Submit();
	stagingSubmissionResult_t PollCompletion(); // zero-timeout; no indefinite host wait
	bool BeginReuse( uint64 expectedGeneration, uint64 nextGeneration );
	stagingSubmissionResult_t Snapshot() const;
	bool AcknowledgeFailure( uint64 expectedGeneration ); // once; does not clear failure/retention

private:
	void Fail( VkResult error, stagingSubmissionResult_t::phase_t phase, const char * message );
	idVulkanStagingSubmission( const idVulkanStagingSubmission & ) = delete;
	idVulkanStagingSubmission & operator=( const idVulkanStagingSubmission & ) = delete;
	mutable idSysMutex mutex;
	stagingSubmissionResult_t status;
};


extern idVulkanStagingManager stagingManager;

#endif
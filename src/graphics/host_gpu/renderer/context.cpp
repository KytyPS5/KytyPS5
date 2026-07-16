#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/presentation/window.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vulkan/vk_enum_string_helper.h>
namespace Libs::Graphics {
static std::atomic<uint64_t> g_command_buffer_submit_seq = 0;
class CommandPool {
public:
	CommandPool() = default;
	~CommandPool() // NOLINT
	{
		// TODO(): check if destructor is called from std::_Exit()
		// DeleteAll();
	}

	KYTY_CLASS_NO_COPY(CommandPool);

	VulkanCommandPool* GetPool(int id) {
		if (m_pool[id] == nullptr) {
			Create(id);
		}
		return m_pool[id];
	}

private:
	void Create(int id);
	void DeleteAll();

	VulkanCommandPool* m_pool[GraphicContext::QUEUES_NUM] = {};
};

RenderContext*                  g_render_ctx = nullptr;
static thread_local CommandPool g_command_pool;

FenceResourceRetainer::~FenceResourceRetainer() {
	if (!m_resources.empty()) {
		EXIT("fence resource retainer destroyed before release\n");
	}
}

void FenceResourceRetainer::Retain(std::shared_ptr<void> resource) {
	if (resource == nullptr) {
		EXIT("cannot retain a null fence resource\n");
	}
	if (std::ranges::none_of(m_resources, [&resource](const auto& retained) {
		    return retained.get() == resource.get();
	    })) {
		m_resources.push_back(std::move(resource));
	}
}

void FenceResourceRetainer::ReleaseAfterFence() noexcept {
	m_resources.clear();
}

void GraphicsRenderInit() {
	EXIT_IF(g_render_ctx != nullptr);

	g_render_ctx = new RenderContext;
}

void GraphicsRenderCreateContext() {
	EXIT_IF(g_render_ctx == nullptr);

	g_render_ctx->SetGraphicCtx(WindowGetGraphicContext());
}

void CommandPool::Create(int id) {
	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(id < 0 || id >= GraphicContext::QUEUES_NUM);
	auto*& pool = m_pool[id];
	EXIT_IF(pool != nullptr);

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(ctx->queues[id].family == static_cast<uint32_t>(-1));

	pool = new VulkanCommandPool;

	VkCommandPoolCreateInfo pool_info {};
	pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.pNext            = nullptr;
	pool_info.queueFamilyIndex = ctx->queues[id].family;
	pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	vkCreateCommandPool(ctx->device, &pool_info, nullptr, &pool->pool);

	EXIT_NOT_IMPLEMENTED(pool->pool == nullptr);

	pool->buffers_count = 8;
	pool->buffers       = std::make_unique<VkCommandBuffer[]>(pool->buffers_count);
	pool->fences        = std::make_unique<VkFence[]>(pool->buffers_count);
	pool->semaphores    = std::make_unique<VkSemaphore[]>(pool->buffers_count);
	pool->busy          = std::make_unique<bool[]>(pool->buffers_count);

	VkCommandBufferAllocateInfo alloc_info {};
	alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool        = pool->pool;
	alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = pool->buffers_count;

	if (vkAllocateCommandBuffers(ctx->device, &alloc_info, pool->buffers.get()) != VK_SUCCESS) {
		EXIT("Can't allocate command buffers");
	}

	for (uint32_t i = 0; i < pool->buffers_count; i++) {
		pool->busy[i] = false;

		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_info.pNext = nullptr;
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		if (vkCreateFence(ctx->device, &fence_info, nullptr, &pool->fences[i]) != VK_SUCCESS) {
			EXIT("Can't create fence");
		}

		VkSemaphoreCreateInfo semaphore_info {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = nullptr;
		semaphore_info.flags = 0;

		if (vkCreateSemaphore(ctx->device, &semaphore_info, nullptr, &pool->semaphores[i]) !=
		    VK_SUCCESS) {
			EXIT("Can't create semaphore");
		}

		EXIT_IF(pool->buffers[i] == nullptr);
		EXIT_IF(pool->fences[i] == nullptr);
		EXIT_IF(pool->semaphores[i] == nullptr);
	}
}

void CommandPool::DeleteAll() {
	auto* ctx = g_render_ctx->GetGraphicCtx();

	for (auto& pool: m_pool) {
		if (pool != nullptr) {
			EXIT_IF(ctx == nullptr);
			EXIT_IF(ctx->device == nullptr);

			for (uint32_t i = 0; i < pool->buffers_count; i++) {
				vkDestroySemaphore(ctx->device, pool->semaphores[i], nullptr);
				vkDestroyFence(ctx->device, pool->fences[i], nullptr);
			}

			vkFreeCommandBuffers(ctx->device, pool->pool, pool->buffers_count, pool->buffers.get());

			vkDestroyCommandPool(ctx->device, pool->pool, nullptr);

			delete pool;
			pool = nullptr;
		}
	}
}

bool CommandBuffer::IsInvalid() const {
	EXIT_IF(g_render_ctx == nullptr);

	if (m_pool != nullptr) {
		Common::LockGuard lock(m_pool->mutex);

		return (m_index == static_cast<uint32_t>(-1) || m_index >= m_pool->buffers_count);
	}

	return true;
}

void CommandBuffer::Allocate() {
	EXIT_IF(!IsInvalid());

	m_pool = g_command_pool.GetPool(m_queue);

	Common::LockGuard lock(m_pool->mutex);

	for (uint32_t i = 0; i < m_pool->buffers_count; i++) {
		if (!m_pool->busy[i]) {
			m_pool->busy[i] = true;
			vkResetCommandBuffer(m_pool->buffers[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
			m_index = i;
			break;
		}
	}

	EXIT_NOT_IMPLEMENTED(IsInvalid());
}

void CommandBuffer::Free() {
	EXIT_IF(IsInvalid());

	Common::LockGuard lock(m_pool->mutex);

	WaitForFence();

	m_host_stream.Release();

	m_pool->busy[m_index] = false;
	vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	RecycleDescriptorsAfterFence();
	m_fence_resources.ReleaseAfterFence();
	m_index = static_cast<uint32_t>(-1);

	EXIT_NOT_IMPLEMENTED(!IsInvalid());
}

void CommandBuffer::DeleteAfterFence(VulkanBuffer* buffer) {
	EXIT_IF(buffer == nullptr);

	m_delete_after_fence.push_back(buffer);
}

void CommandBuffer::RetainResourceUntilFence(std::shared_ptr<void> resource) {
	if (IsInvalid() || m_execute) {
		EXIT("cannot retain a resource on an invalid or submitted command buffer\n");
	}
	m_fence_resources.Retain(std::move(resource));
}

void CommandBuffer::RecycleDescriptorAfterFence(VulkanDescriptorSet* set) {
	EXIT_IF(set == nullptr);

	m_descriptor_sets_after_fence.push_back(set);
}

void CommandBuffer::RecycleDescriptorsAfterFence() {
	for (auto* set: m_descriptor_sets_after_fence) {
		g_render_ctx->GetDescriptorCache()->Recycle(set);
	}
	m_descriptor_sets_after_fence.clear();
}

void CommandBuffer::Begin() const {
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext            = nullptr;
	begin_info.flags            = 0;
	begin_info.pInheritanceInfo = nullptr;

	auto result = vkBeginCommandBuffer(buffer, &begin_info);

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::End() const {
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	auto result = vkEndCommandBuffer(buffer);

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

void CommandBuffer::Execute() {
	EXIT_IF(IsInvalid());
	EXIT_IF(m_execute);

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores    = nullptr;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue = g_render_ctx->GetGraphicCtx()->queues[m_queue];

	auto result = vkResetFences(g_render_ctx->GetGraphicCtx()->device, 1, &fence);
	if (result != VK_SUCCESS) {
		LOGF("vkResetFences failed before submit: %s (%d)\n", string_VkResult(result),
		     static_cast<int>(result));
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	if (queue.mutex != nullptr) {
		queue.mutex->Lock();
	}

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("vkQueueSubmit begin: queue=%d index=%u debug_op=%u debug_submit=%" PRIu64
		     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     m_queue, m_index, m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1,
		     m_debug_arg2, m_debug_arg3, m_debug_arg4);
	}

	result = vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence);

	if (queue.mutex != nullptr) {
		queue.mutex->Unlock();
	}

	m_execute    = true;
	m_submit_seq = g_command_buffer_submit_seq.fetch_add(1, std::memory_order_relaxed) + 1;

	if (result != VK_SUCCESS) {
		LOGF("vkQueueSubmit failed: %s (%d), queue=%d index=%u submit_seq=%" PRIu64
		     " debug_op=%u debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     string_VkResult(result), static_cast<int>(result), m_queue, m_index, m_submit_seq,
		     m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::ExecuteWithSemaphore(VkSemaphore signal_semaphore) {
	EXIT_IF(IsInvalid());
	EXIT_IF(m_execute);

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	if (signal_semaphore == nullptr) {
		signal_semaphore = m_pool->semaphores[m_index];
	}

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores    = &signal_semaphore;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue = g_render_ctx->GetGraphicCtx()->queues[m_queue];

	auto result = vkResetFences(g_render_ctx->GetGraphicCtx()->device, 1, &fence);
	if (result != VK_SUCCESS) {
		LOGF("vkResetFences failed before submit: %s (%d)\n", string_VkResult(result),
		     static_cast<int>(result));
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	if (queue.mutex != nullptr) {
		queue.mutex->Lock();
	}

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("vkQueueSubmit begin: queue=%d index=%u signal_semaphore=%p debug_op=%u"
		     " debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     m_queue, m_index, static_cast<void*>(signal_semaphore), m_debug_op, m_debug_submit_id,
		     m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3, m_debug_arg4);
	}

	result = vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence);

	if (queue.mutex != nullptr) {
		queue.mutex->Unlock();
	}

	m_execute    = true;
	m_submit_seq = g_command_buffer_submit_seq.fetch_add(1, std::memory_order_relaxed) + 1;

	if (result != VK_SUCCESS) {
		LOGF("vkQueueSubmit failed: %s (%d), queue=%d index=%u submit_seq=%" PRIu64
		     " debug_op=%u debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     string_VkResult(result), static_cast<int>(result), m_queue, m_index, m_submit_seq,
		     m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::ExecuteWithSemaphore(VkSemaphore          wait_semaphore,
                                         VkPipelineStageFlags wait_stage,
                                         VkSemaphore          signal_semaphore) {
	EXIT_IF(IsInvalid());
	EXIT_IF(m_execute);
	EXIT_IF(wait_semaphore == nullptr);

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	if (signal_semaphore == nullptr) {
		signal_semaphore = m_pool->semaphores[m_index];
	}

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 1;
	submit_info.pWaitSemaphores      = &wait_semaphore;
	submit_info.pWaitDstStageMask    = &wait_stage;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores    = &signal_semaphore;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue = g_render_ctx->GetGraphicCtx()->queues[m_queue];

	auto result = vkResetFences(g_render_ctx->GetGraphicCtx()->device, 1, &fence);
	if (result != VK_SUCCESS) {
		LOGF("vkResetFences failed before submit: %s (%d)\n", string_VkResult(result),
		     static_cast<int>(result));
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	if (queue.mutex != nullptr) {
		queue.mutex->Lock();
	}

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("vkQueueSubmit begin: queue=%d index=%u wait_semaphore=%p signal_semaphore=%p"
		     " debug_op=%u debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     m_queue, m_index, static_cast<void*>(wait_semaphore),
		     static_cast<void*>(signal_semaphore), m_debug_op, m_debug_submit_id, m_debug_arg0,
		     m_debug_arg1, m_debug_arg2, m_debug_arg3, m_debug_arg4);
	}

	result = vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence);

	if (queue.mutex != nullptr) {
		queue.mutex->Unlock();
	}

	m_execute    = true;
	m_submit_seq = g_command_buffer_submit_seq.fetch_add(1, std::memory_order_relaxed) + 1;

	if (result != VK_SUCCESS) {
		LOGF("vkQueueSubmit failed: %s (%d), queue=%d index=%u submit_seq=%" PRIu64
		     " debug_op=%u debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     string_VkResult(result), static_cast<int>(result), m_queue, m_index, m_submit_seq,
		     m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::WaitForFence() {
	EXIT_IF(IsInvalid());

	const bool was_executed = m_execute;
	if (m_execute) {
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		auto result = vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE, UINT64_MAX);
		if (result != VK_SUCCESS) {
			LOGF("vkWaitForFences failed: %s (%d), wait=WaitForFence queue=%d index=%u "
			     "submit_seq=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
			     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
			     string_VkResult(result), static_cast<int>(result), m_queue, m_index, m_submit_seq,
			     m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2,
			     m_debug_arg3, m_debug_arg4);
		}
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

		m_execute = false;
	}

	if (was_executed) {
		RecycleDescriptorsAfterFence();
		m_fence_resources.ReleaseAfterFence();
	}
	for (auto* buffer: m_delete_after_fence) {
		VulkanDeleteBuffer(g_render_ctx->GetGraphicCtx(), buffer);
		delete buffer;
	}
	m_delete_after_fence.clear();
}

void CommandBuffer::WaitForFenceAndReset() {
	EXIT_IF(IsInvalid());

	const bool was_executed = m_execute;
	if (m_execute) {
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		auto result = vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE, UINT64_MAX);
		if (result != VK_SUCCESS) {
			LOGF("vkWaitForFences failed: %s (%d), wait=WaitForFenceAndReset queue=%d index=%u "
			     "submit_seq=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
			     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
			     string_VkResult(result), static_cast<int>(result), m_queue, m_index, m_submit_seq,
			     m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2,
			     m_debug_arg3, m_debug_arg4);
		}
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

		m_execute = false;

		vkResetCommandBuffer(m_pool->buffers[m_index],
		                     VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	}

	m_host_stream.Reset();

	if (was_executed) {
		RecycleDescriptorsAfterFence();
		m_fence_resources.ReleaseAfterFence();
	}
	for (auto* buffer: m_delete_after_fence) {
		VulkanDeleteBuffer(g_render_ctx->GetGraphicCtx(), buffer);
		delete buffer;
	}
	m_delete_after_fence.clear();
}

void CommandBuffer::BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* colors,
                                    uint32_t requested_color_count, RenderDepthInfo* depth) const {
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	EXIT_IF(framebuffer == nullptr);
	EXIT_IF(colors == nullptr);
	EXIT_IF(requested_color_count > RENDER_COLOR_ATTACHMENTS_MAX);

	bool     with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	uint32_t color_count = 0;
	for (uint32_t i = 0; i < requested_color_count; i++) {
		if (colors[i].vulkan_buffer == nullptr) {
			break;
		}
		color_count++;
	}
	bool with_color = (color_count != 0);

	EXIT_NOT_IMPLEMENTED(!with_depth && !with_color);

	VkClearValue clears[RENDER_COLOR_ATTACHMENTS_MAX + 1] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		clears[i].color = colors[i].color_clear_value;
	}
	clears[color_count].depthStencil = {depth->depth_clear_value, depth->stencil_clear_value};

	VkExtent2D extent = (with_color ? colors[0].extent : depth->vulkan_buffer->extent);

	VkRenderPassBeginInfo render_pass_info {};
	render_pass_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass_info.pNext             = nullptr;
	render_pass_info.renderPass        = framebuffer->render_pass;
	render_pass_info.framebuffer       = framebuffer->framebuffer;
	render_pass_info.renderArea.offset = {0, 0};
	render_pass_info.renderArea.extent = extent;
	render_pass_info.clearValueCount   = color_count + (with_depth ? 1u : 0u);
	render_pass_info.pClearValues      = clears;

	for (uint32_t i = 0; i < color_count; i++) {
		const auto color_initial_layout = framebuffer->color_layout[i];
		if (colors[i].vulkan_buffer->layout != color_initial_layout) {
			if (graphics_debug_dump_enabled()) {
				LOGF("BeginRenderPass: color%u initial barrier image=%p mem=%" PRIu64 " %s -> %s\n",
				     i, reinterpret_cast<void*>(colors[i].vulkan_buffer->image),
				     colors[i].vulkan_buffer->memory.unique_id,
				     string_VkImageLayout(colors[i].vulkan_buffer->layout),
				     string_VkImageLayout(color_initial_layout));
			}

			VkImageMemoryBarrier image_memory_barrier {};
			image_memory_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			image_memory_barrier.pNext         = nullptr;
			image_memory_barrier.srcAccessMask = 0;
			image_memory_barrier.dstAccessMask =
			    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			image_memory_barrier.oldLayout                       = colors[i].vulkan_buffer->layout;
			image_memory_barrier.newLayout                       = color_initial_layout;
			image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
			image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
			image_memory_barrier.image                           = colors[i].vulkan_buffer->image;
			image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
			image_memory_barrier.subresourceRange.baseMipLevel   = 0;
			image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
			image_memory_barrier.subresourceRange.baseArrayLayer = 0;
			image_memory_barrier.subresourceRange.layerCount     = 1;

			vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
			                     nullptr, 1, &image_memory_barrier);

			colors[i].vulkan_buffer->layout = image_memory_barrier.newLayout;
		} else if (graphics_debug_dump_enabled()) {
			LOGF("BeginRenderPass: color%u initial image=%p mem=%" PRIu64 " layout=%s\n", i,
			     reinterpret_cast<void*>(colors[i].vulkan_buffer->image),
			     colors[i].vulkan_buffer->memory.unique_id,
			     string_VkImageLayout(colors[i].vulkan_buffer->layout));
		}
	}

	const auto depth_layout =
	    (with_depth && framebuffer != nullptr ? framebuffer->depth_layout
	                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	if (with_depth && depth->vulkan_buffer->layout != depth_layout) {
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext         = nullptr;
		image_memory_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.dstAccessMask =
		    (depth_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		         ? VK_ACCESS_MEMORY_READ_BIT
		         : VK_ACCESS_MEMORY_WRITE_BIT);
		image_memory_barrier.oldLayout           = depth->vulkan_buffer->layout;
		image_memory_barrier.newLayout           = depth_layout;
		image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image               = depth->vulkan_buffer->image;
		image_memory_barrier.subresourceRange.aspectMask =
		    DepthStencilAspectMask(depth->vulkan_buffer->format);
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = 1;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = 1;

		vkCmdPipelineBarrier(
		    buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
		    nullptr, 0, nullptr, 1, &image_memory_barrier);

		depth->vulkan_buffer->layout = image_memory_barrier.newLayout;
	}

	vkCmdBeginRenderPass(buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

	for (uint32_t i = 0; i < color_count; i++) {
		colors[i].vulkan_buffer->layout = RENDER_COLOR_IMAGE_LAYOUT;
	}
}

void CommandBuffer::EndRenderPass() const {
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	vkCmdEndRenderPass(buffer);
}

} // namespace Libs::Graphics

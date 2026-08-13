#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
namespace Libs::Graphics {

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint32_t slot, uint64_t submit_seq,
                       uint32_t debug_op, uint64_t debug_submit, uint32_t arg0, uint32_t arg1,
                       uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), slot=%u submit_seq=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, VulkanToString(result).c_str(), static_cast<int>(result), slot, submit_seq, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), slot=%u submit_seq=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, VulkanToString(result).c_str(), static_cast<int>(result), slot, submit_seq,
	            debug_op, debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

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

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_scheduler(scheduler), m_graphics(scheduler.Graphics()),
      m_slot(scheduler.AllocateCommandBuffer()) {}

CommandBuffer::~CommandBuffer() {
	Release();
}

bool CommandBuffer::IsInvalid() const {
	return m_slot == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());

	const auto handle = m_slot->buffer;
	EXIT_IF(handle == nullptr);
	return handle;
}

void CommandBuffer::Release() {
	EXIT_IF(IsInvalid());

	Common::LockGuard lock(*m_slot->pool_mutex);

	WaitForFence();

	m_slot->busy = false;
	m_slot->Reset();
	ReleaseResourcesAfterFence();
	m_slot = nullptr;

	EXIT_NOT_IMPLEMENTED(!IsInvalid());
}

void CommandBuffer::RetireBufferAfterFence(std::unique_ptr<VulkanBuffer> buffer) {
	if (IsInvalid() || m_execute || buffer == nullptr || buffer->buffer == nullptr) {
		EXIT("cannot retire a buffer on an invalid or submitted command buffer\n");
	}
	m_retired_buffers.push_back(std::move(buffer));
}

void CommandBuffer::RetainResourceUntilFence(std::shared_ptr<void> resource) {
	if (IsInvalid() || m_execute) {
		EXIT("cannot retain a resource on an invalid or submitted command buffer\n");
	}
	m_fence_resources.Retain(std::move(resource));
}

void CommandBuffer::RecycleDescriptorAfterFence(VulkanDescriptorSet& set) {
	m_descriptor_sets_after_fence.push_back(&set);
}

void CommandBuffer::RecycleDescriptorsAfterFence() {
	for (auto* set: m_descriptor_sets_after_fence) {
		m_context.GetDescriptorCache().Recycle(*set);
	}
	m_descriptor_sets_after_fence.clear();
}

void CommandBuffer::Begin() const {
	EXIT_IF(m_rendering);
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.sType            = vk::StructureType::eCommandBufferBeginInfo;
	begin_info.pNext            = nullptr;
	begin_info.flags            = {};
	begin_info.pInheritanceInfo = nullptr;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
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

void CommandBuffer::Execute(const SubmitInfo& submit) {
	EXIT_IF(IsInvalid());
	EXIT_IF(m_execute);
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores > SubmitInfo::MaxSemaphores);

	auto buffer = Handle();
	auto fence  = m_slot->fence;

	vk::TimelineSemaphoreSubmitInfo timeline_info {};
	timeline_info.sType                     = vk::StructureType::eTimelineSemaphoreSubmitInfo;
	timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
	timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
	timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
	timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

	vk::SubmitInfo submit_info {};
	submit_info.sType                = vk::StructureType::eSubmitInfo;
	submit_info.pNext                = &timeline_info;
	submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
	submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
	submit_info.pWaitDstStageMask    = submit.wait_stages.data();
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
	submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

	auto& graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	auto result = graphics.device.resetFences(1, &fence);
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkResetFences (before submit)", result, m_slot->id, m_submit_seq,
		                  m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2,
		                  m_debug_arg3, m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("vkQueueSubmit begin: slot=%u waits=%u signals=%u debug_op=%u debug_submit=%" PRIu64
		     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     m_slot->id, submit.num_wait_semaphores, submit.num_signal_semaphores, m_debug_op,
		     m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}

	{
		Common::LockGuard lock(graphics.queue_mutex);
		m_submit_seq = m_scheduler.NextSubmitSequence();
		result       = graphics.queue.submit(1, &submit_info, fence);
	}

	m_execute      = true;
	m_fence_waited = false;

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, m_slot->id, m_submit_seq, m_debug_op,
		                  m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		                  m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::WaitForFence() {
	FinalizeFence(false);
}

void CommandBuffer::WaitForFenceOnly() {
	EXIT_IF(IsInvalid());
	if (!m_execute || m_fence_waited) {
		return;
	}
	auto device = m_graphics.device;
	auto result = device.waitForFences(1, &m_slot->fence, VK_TRUE, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkWaitForFences", result, m_slot->id, m_submit_seq, m_debug_op,
		                  m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		                  m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	m_fence_waited = true;
}

void CommandBuffer::WaitForFenceAndReset() {
	FinalizeFence(true);
}

void CommandBuffer::FinalizeFence(bool reset_recording) {
	const bool was_executed = m_execute;
	WaitForFenceOnly();
	if (was_executed) {
		m_execute      = false;
		m_fence_waited = false;
		if (reset_recording) {
			Common::LockGuard lock(*m_slot->pool_mutex);
			m_slot->Reset();
		}
	}
	if (was_executed) {
		ReleaseResourcesAfterFence();
	}
	DeleteBuffersAfterFence();
}

void CommandBuffer::ReleaseResourcesAfterFence() {
	RecycleDescriptorsAfterFence();
	m_fence_resources.ReleaseAfterFence();
}

void CommandBuffer::DeleteBuffersAfterFence() {
	for (const auto& buffer: m_retired_buffers) {
		m_graphics.DeleteBuffer(*buffer);
	}
	m_retired_buffers.clear();
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	if (m_rendering && m_render_state == state) {
		return;
	}
	EndRendering();

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].sType        = vk::StructureType::eRenderingAttachmentInfo;
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.sType       = vk::StructureType::eRenderingAttachmentInfo;
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.sType       = vk::StructureType::eRenderingAttachmentInfo;
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.sType                = vk::StructureType::eRenderingInfo;
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	Handle().beginRendering(rendering);
	m_render_state = state;
	m_rendering    = true;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	Handle().endRendering();
	m_rendering    = false;
	m_render_state = {};
}

} // namespace Libs::Graphics

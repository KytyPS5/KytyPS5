#include "common/assert.h"
#include "common/timer.h"
#include "common/common.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());
	return m_buffer;
}

static constexpr size_t                      CHECKPOINT_RING_SIZE = 4096;
static std::array<GpuCheckpoint, CHECKPOINT_RING_SIZE> g_checkpoints {};
static std::atomic<uint64_t>                 g_checkpoint_next {0};

static GpuCheckpoint* AllocateCheckpoint(uint32_t phase) {
	const auto index = g_checkpoint_next.fetch_add(1, std::memory_order_relaxed);
	auto*      entry = &g_checkpoints[index % CHECKPOINT_RING_SIZE];
	*entry           = GpuCheckpoint {};
	entry->phase     = phase;
	return entry;
}

static void DumpDeviceFaultInfo(GraphicContext& graphics) {
	vk::DeviceFaultCountsEXT counts {};
	counts.sType = vk::StructureType::eDeviceFaultCountsEXT;
	auto result  = graphics.device.getFaultInfoEXT(&counts, nullptr);
	if (result != vk::Result::eSuccess) {
		return;
	}

	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);

	vk::DeviceFaultInfoEXT info {};
	info.sType           = vk::StructureType::eDeviceFaultInfoEXT;
	info.pAddressInfos   = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos    = vendors.empty() ? nullptr : vendors.data();
	counts.vendorBinarySize = 0; // vendor binary is a crash dump blob, not useful in the log

	result = graphics.device.getFaultInfoEXT(&counts, &info);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		return;
	}

	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		const auto& a = addresses[i];
		const uint64_t precision = a.addressPrecision != 0 ? a.addressPrecision : 1;
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		const auto& v = vendors[i];
	}
}

void GpuCrashDiagnosticsDump(GraphicContext& graphics, const char* context_text) {
	if (!graphics.diagnostic_checkpoints_enabled && !graphics.device_fault_enabled) {
		return;
	}

	if (graphics.diagnostic_checkpoints_enabled) {
		Common::LockGuard lock(graphics.queue_mutex);

		auto checkpoints2 = graphics.queue.getCheckpointData2NV();
		for (const auto& data: checkpoints2) {
			const auto* entry = static_cast<const GpuCheckpoint*>(data.pCheckpointMarker);
			const bool  ours =
			    entry >= g_checkpoints.data() && entry < g_checkpoints.data() + CHECKPOINT_RING_SIZE;
			if (!ours) {
				continue;
			}
		}

		const auto recorded = g_checkpoint_next.load(std::memory_order_relaxed);
		const auto show     = std::min<uint64_t>(recorded, 12);
		for (uint64_t i = recorded - show; i < recorded; i++) {
			const auto& entry = g_checkpoints[i % CHECKPOINT_RING_SIZE];
		}

		auto checkpoints = graphics.queue.getCheckpointDataNV();
		for (const auto& data: checkpoints) {
			const auto* entry = static_cast<const GpuCheckpoint*>(data.pCheckpointMarker);
			if (entry < g_checkpoints.data() || entry >= g_checkpoints.data() + CHECKPOINT_RING_SIZE ||
			    (reinterpret_cast<uintptr_t>(entry) - reinterpret_cast<uintptr_t>(g_checkpoints.data())) %
			            sizeof(GpuCheckpoint) !=
			        0) {
				continue;
			}
		}
	}

	if (graphics.device_fault_enabled) {
		DumpDeviceFaultInfo(graphics);
	}
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || IsInvalid());
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.sType            = vk::StructureType::eCommandBufferBeginInfo;
	begin_info.pNext            = nullptr;
	begin_info.flags            = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	begin_info.pInheritanceInfo = nullptr;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	if (m_graphics.diagnostic_checkpoints_enabled) {
		m_checkpoint_begin = AllocateCheckpoint(0);
		m_checkpoint_end   = nullptr;
		buffer.setCheckpointNV(m_checkpoint_begin);
	}
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	if (m_graphics.diagnostic_checkpoints_enabled) {
		m_checkpoint_end = AllocateCheckpoint(1);
		buffer.setCheckpointNV(m_checkpoint_end);
	}

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

	if (m_graphics.diagnostic_checkpoints_enabled && !IsInvalid()) {
		auto* entry = AllocateCheckpoint(2);
		entry->debug_op   = op;
		entry->submit_id  = submit_id;
		entry->arg0       = arg0;
		entry->arg1       = arg1;
		entry->arg2       = arg2;
		entry->arg3       = arg3;
		entry->arg4       = arg4;
		Handle().setCheckpointNV(entry);
	}
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

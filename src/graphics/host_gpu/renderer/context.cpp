#include "common/assert.h"
#include "common/common.h"
#include "common/perfCounters.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cstring>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EndComputeChain();
	return StateHandle();
}

vk::CommandBuffer CommandBuffer::StateHandle() const {
	EXIT_IF(IsInvalid());
	m_used = true;
	return m_buffer;
}

void CommandBuffer::EndComputeChain() const {
	if (!m_compute_chain) {
		return;
	}
	m_compute_chain = false;
	ShaderAccessBarrier(StateHandle(), vk::PipelineStageFlagBits::eComputeShader);
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || IsInvalid());
	m_compute_chain = false;
	auto buffer     = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	m_used = false;
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

void CommandBuffer::BeginRendering(const RenderState& wanted) const {
	if (m_rendering && m_render_state.Covers(wanted)) {
		return;
	}
	auto state = wanted;
	// The same attachments with draws outside the open area: the next pass takes both areas, so
	// that a run of draws with different scissors does not restart the pass at each one.
	if (m_rendering) {
		auto open                                   = m_render_state;
		open.depth_stencil_attachment.depth_clear   = state.depth_stencil_attachment.depth_clear;
		open.depth_stencil_attachment.stencil_clear = state.depth_stencil_attachment.stencil_clear;
		RenderState same                            = open;
		same.area_x = same.area_y = 0;
		same.area_width = same.area_height = 0;
		RenderState want                   = state;
		want.area_x = want.area_y = 0;
		want.area_width = want.area_height = 0;
		if (same == want) state.Grow(m_render_state);
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	EndRendering();
	InsertDebugLabel(m_buffer, "Pass {}x{} layers={} colors={}", state.width, state.height,
	                 state.num_layers, state.num_color_attachments);

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp       = vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp      = vk::AttachmentStoreOp::eStore;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {state.width, state.height};
	if (state.area_width != 0u && state.area_height != 0u) {
		// The area stays inside the attachments: a reused image can be a little smaller than the
		// guest's render area (dynamic resolution), and rendering past it is undefined.
		const auto x0 = std::min(static_cast<uint32_t>(std::max(state.area_x, 0)), state.width);
		const auto y0 = std::min(static_cast<uint32_t>(std::max(state.area_y, 0)), state.height);
		const auto x1 = std::min(x0 + state.area_width, state.width);
		const auto y1 = std::min(y0 + state.area_height, state.height);
		if (x1 > x0 && y1 > y0) {
			rendering.renderArea.offset =
			    vk::Offset2D {static_cast<int32_t>(x0), static_cast<int32_t>(y0)};
			rendering.renderArea.extent = vk::Extent2D {x1 - x0, y1 - y0};
		}
	}
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	Handle().beginRendering(rendering);
	PerfCounters::Add(PerfCounters::Counter::RenderPasses);
	m_render_state = state;
	// The open pass holds the cleared contents, so a later clear begins a new pass.
	m_render_state.depth_stencil_attachment.depth_clear   = false;
	m_render_state.depth_stencil_attachment.stencil_clear = false;
	m_rendering                                           = true;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	Handle().endRendering();
	m_rendering = false;
}

} // namespace Libs::Graphics

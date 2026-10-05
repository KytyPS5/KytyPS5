#include "graphics/host_gpu/renderer/rasterScale.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include <algorithm>
#include <array>

namespace Libs::Graphics {
namespace {
uint32_t ScaleDimension(uint32_t size, uint32_t percent) {
	return std::max(1u, uint32_t(uint64_t(size) * percent / 100));
}
void Transition(vk::CommandBuffer command, Image& image, vk::ImageAspectFlags aspect,
                uint32_t mip, uint32_t layer, uint32_t layers,
                vk::ImageLayout before, vk::ImageLayout after) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask = barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	barrier.oldLayout = before;
	barrier.newLayout = after;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image.backing.image;
	barrier.subresourceRange = {aspect, mip, 1, layer, layers};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers = &barrier;
	command.pipelineBarrier2(dependency);
}
}

struct RasterScaler::Impl {
	GraphicContext& graphics;
	CommandScheduler& scheduler;
	std::array<std::unique_ptr<Image>, RENDER_COLOR_ATTACHMENTS_MAX + 1> images;
	RenderState original {}, effective {};
	bool active = false;
	bool logged = false;

	bool Supported(const RenderAttachment& attachment) const {
		if (!attachment.image_view) return true;
		const auto* image = attachment.image;
		if (!image || image->backing.samples != 1 || image->backing.image_type != vk::ImageType::e2D ||
		    image->binding.is_bound || attachment.image_layout == vk::ImageLayout::eGeneral ||
		    attachment.image_layout == vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT) return false;
		const auto view = std::ranges::find(image->views, attachment.image_view, &CachedImageView::view);
		if (view == image->views.end() || view->info.format != image->backing.format ||
		    view->info.level_count != 1) return false;
		const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
		return (graphics.GetFormatProperties(image->backing.format).optimalTilingFeatures & required) == required;
	}

	vk::ImageAspectFlags Aspects(const RenderAttachment& attachment) const {
		if (attachment.has_depth || attachment.has_stencil) {
			return (attachment.has_depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlags {}) |
			       (attachment.has_stencil ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlags {});
		}
		return vk::ImageAspectFlagBits::eColor;
	}

	void Transfer(vk::CommandBuffer command, uint32_t slot, const RenderAttachment& attachment, bool store) {
		auto& guest = *attachment.image;
		auto& scaled = *images[slot];
		const auto aspects = Aspects(attachment);
		// Do not change the original Image's tracked state: a caller may already
		// have computed its next barrier before EndRendering is invoked. Restore
		// exactly the layout that barrier expects before returning.
		Transition(command, guest, aspects, attachment.mip_level, attachment.base_layer, original.num_layers,
		           attachment.image_layout, store ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal);
		scaled.Transit(store ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eTransferDstOptimal,
		               store ? vk::AccessFlagBits2::eTransferRead : vk::AccessFlagBits2::eTransferWrite, {}, command);
		for (const auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth, vk::ImageAspectFlagBits::eStencil}) {
			if (!(aspects & aspect)) continue;
			vk::ImageBlit blit {};
			const vk::ImageSubresourceLayers guest_range {aspect, attachment.mip_level, attachment.base_layer, original.num_layers};
			const vk::ImageSubresourceLayers scaled_range {aspect, 0, 0, original.num_layers};
			blit.srcSubresource = store ? scaled_range : guest_range;
			blit.dstSubresource = store ? guest_range : scaled_range;
			const vk::Offset3D full {int32_t(original.width), int32_t(original.height), 1};
			const vk::Offset3D low {int32_t(effective.width), int32_t(effective.height), 1};
			blit.srcOffsets[1] = store ? low : full;
			blit.dstOffsets[1] = store ? full : low;
			// Nearest also works for depth, stencil and integer render targets.
			command.blitImage(store ? scaled.backing.image : guest.backing.image, vk::ImageLayout::eTransferSrcOptimal,
			                  store ? guest.backing.image : scaled.backing.image, vk::ImageLayout::eTransferDstOptimal,
			                  1, &blit, vk::Filter::eNearest);
		}
		Transition(command, guest, aspects, attachment.mip_level, attachment.base_layer, original.num_layers,
		           store ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal, attachment.image_layout);
		if (!store) {
			scaled.Transit(attachment.image_layout, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite, {}, command);
		}
	}

	void Prepare(vk::CommandBuffer command, uint32_t slot, const RenderAttachment& attachment, RenderAttachment& output) {
		if (!attachment.image_view) return;
		auto& image = images[slot];
		const auto format = attachment.image->backing.format;
		if (!image || image->backing.format != format || image->backing.extent.width != effective.width ||
		    image->backing.extent.height != effective.height || image->backing.layers != original.num_layers) {
			if (image) scheduler.DeferOperation([old = std::move(image)]() mutable { old.reset(); });
			ImageInfo info {};
			info.pixel_format = format;
			info.extent = {effective.width, effective.height, 1};
			info.resources.layers = original.num_layers;
			info.pitch = effective.width;
			info.bytes_per_block = attachment.image->info.bytes_per_block;
			image = std::make_unique<Image>(graphics, scheduler, info);
		}
		Transfer(command, slot, attachment, false);
		ImageViewInfo view {};
		view.format = format;
		view.aspect = Aspects(attachment);
		view.layer_count = original.num_layers;
		view.type = original.num_layers == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
		view.usage = attachment.has_depth || attachment.has_stencil ? vk::ImageUsageFlagBits::eDepthStencilAttachment : vk::ImageUsageFlagBits::eColorAttachment;
		output.image_view = image->FindView(view);
	}
};

RasterScaler::RasterScaler(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_impl(std::make_unique<Impl>(Impl {graphics, scheduler})) {}
RasterScaler::~RasterScaler() = default; // scheduler shutdown waits for pending GPU commands

const RenderState& RasterScaler::Begin(vk::CommandBuffer command, const RenderState& state) {
	auto& impl = *m_impl;
	impl.original = impl.effective = state;
	impl.active = false;
	if (state.raster_scale_percent >= 100 || state.width < 64 || state.height < 64 ||
	    !impl.Supported(state.depth_stencil_attachment) ||
	    !std::ranges::all_of(state.color_attachments, [&](const auto& a) { return impl.Supported(a); }) ||
	    (state.num_color_attachments == 0 && !state.depth_stencil_attachment.image_view)) return impl.effective;
	impl.effective.width = ScaleDimension(state.width, state.raster_scale_percent);
	impl.effective.height = ScaleDimension(state.height, state.raster_scale_percent);
	for (uint32_t i = 0; i < state.num_color_attachments; ++i) {
		impl.Prepare(command, i, state.color_attachments[i], impl.effective.color_attachments[i]);
	}
	impl.Prepare(command, RENDER_COLOR_ATTACHMENTS_MAX, state.depth_stencil_attachment, impl.effective.depth_stencil_attachment);
	impl.active = true;
	if (!impl.logged) {
		Log::WriteToConsoleAndLog(fmt::format("Render scale active: {}%; raster attachments {}x{} -> {}x{}\n",
		    state.raster_scale_percent, state.width, state.height, impl.effective.width, impl.effective.height));
		impl.logged = true;
	}
	return impl.effective;
}

void RasterScaler::End(vk::CommandBuffer command) {
	auto& impl = *m_impl;
	if (!impl.active) return;
	impl.active = false;
	for (uint32_t i = 0; i < impl.original.num_color_attachments; ++i) {
		const auto& attachment = impl.original.color_attachments[i];
		if (attachment.image_view) impl.Transfer(command, i, attachment, true);
	}
	if (impl.original.depth_stencil_attachment.image_view) {
		impl.Transfer(command, RENDER_COLOR_ATTACHMENTS_MAX, impl.original.depth_stencil_attachment, true);
	}
}
const RenderState& RasterScaler::State() const { return m_impl->effective; }
} // namespace Libs::Graphics

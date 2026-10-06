#include "graphics/host_gpu/renderer/rasterScale.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace Libs::Graphics {
vk::Rect2D ScaleRasterScissor(vk::Rect2D scissor, float scale_x, float scale_y, vk::Extent2D extent) {
	const auto clamp = [](double value, uint32_t limit) {
		return uint32_t(std::clamp(value, 0.0, double(limit)));
	};
	const auto x = clamp(std::floor(double(scissor.offset.x) * scale_x), extent.width);
	const auto y = clamp(std::floor(double(scissor.offset.y) * scale_y), extent.height);
	// Widen the signed origin before adding the unsigned extent. Clamp before
	// conversion and preserve empty slots even when scaling rounds outward.
	const auto right = scissor.extent.width == 0 ? x :
	    clamp(std::ceil((double(scissor.offset.x) + scissor.extent.width) * scale_x), extent.width);
	const auto bottom = scissor.extent.height == 0 ? y :
	    clamp(std::ceil((double(scissor.offset.y) + scissor.extent.height) * scale_y), extent.height);
	scissor.offset = {int32_t(x), int32_t(y)};
	scissor.extent = {std::max(right, x) - x, std::max(bottom, y) - y};
	return scissor;
}
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
	bool color_sources_ready = false;
	std::array<std::pair<uint64_t, uint64_t>, RENDER_COLOR_ATTACHMENTS_MAX> color_versions {};
	// What each scaled image last stored into: (instance, version), mip and layer.
	struct Stored { std::pair<uint64_t, uint64_t> version {}; uint32_t mip = 0, layer = 0, layers = 0; };
	std::array<Stored, RENDER_COLOR_ATTACHMENTS_MAX + 1> stored {};
	bool logged = false;

	// The scaled image still equals the guest image when nothing wrote the guest
	// since our last store. The current pass's own acquire adds at most one version.
	bool Current(uint32_t slot, const RenderAttachment& attachment) const {
		const auto& guest = *attachment.image;
		const auto& last = stored[slot];
		const auto now = guest.ContentVersion();
		return last.layers != 0 && last.version.first == now.first && now.second - last.version.second <= 1 &&
		       last.mip == attachment.mip_level && last.layer == attachment.base_layer &&
		       last.layers == original.num_layers && !guest.IsCpuDirty() && !guest.IsBufferModified();
	}

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
		const bool clears_all = attachment.has_depth || attachment.has_stencil ?
		    (!attachment.has_depth || attachment.depth_clear) && (!attachment.has_stencil || attachment.stencil_clear) :
		    attachment.is_clear;
		if (!store && (clears_all || Current(slot, attachment))) {
			// Keep mixed depth/stencil LOAD operations intact. Only omit a copy
			// when the render pass clears every aspect it could have loaded, or
			// when the scaled image already holds the guest content.
			scaled.Transit(attachment.image_layout, vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite, {}, command);
			return;
		}
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
		} else {
			stored[slot] = {guest.ContentVersion(), attachment.mip_level, attachment.base_layer, original.num_layers};
		}
	}

	void Prepare(vk::CommandBuffer command, uint32_t slot, const RenderAttachment& attachment, RenderAttachment& output) {
		if (!attachment.image_view) return;
		auto& image = images[slot];
		const auto format = attachment.image->backing.format;
		if (!image || image->backing.format != format || image->backing.extent.width != effective.width ||
		    image->backing.extent.height != effective.height || image->backing.layers != original.num_layers) {
			if (image) scheduler.DeferOperation([old = std::move(image)]() mutable { old.reset(); });
			stored[slot] = {};
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
		output.image = image.get();
	}
};

RasterScaler::RasterScaler(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_impl(std::make_unique<Impl>(Impl {graphics, scheduler})) {}
RasterScaler::~RasterScaler() = default; // scheduler shutdown waits for pending GPU commands

const RenderState& RasterScaler::Begin(vk::CommandBuffer command, const RenderState& state) {
	auto& impl = *m_impl;
	impl.original = impl.effective = state;
	impl.active = false;
	impl.color_sources_ready = false;
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
	RefreshSourceVersions();
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
	impl.color_sources_ready = true;
}
const RenderState& RasterScaler::State() const { return m_impl->effective; }

void RasterScaler::RefreshSourceVersions() {
	auto& impl = *m_impl;
	if (!impl.active) return;
	for (uint32_t slot = 0; slot < impl.original.num_color_attachments; ++slot) {
		const auto* image = impl.original.color_attachments[slot].image;
		impl.color_versions[slot] = image ? image->ContentVersion() : std::pair<uint64_t, uint64_t> {};
	}
}

Image* RasterScaler::ColorSource(const Image& image) const {
	const auto& impl = *m_impl;
	if (!impl.color_sources_ready || image.IsCpuDirty() || image.IsBufferModified() ||
	    impl.original.num_layers != 1 || image.backing.layers != 1 ||
	    image.backing.extent.width != impl.original.width ||
	    image.backing.extent.height != impl.original.height) return nullptr;
	for (uint32_t slot = 0; slot < impl.original.num_color_attachments; ++slot) {
		const auto& attachment = impl.original.color_attachments[slot];
		if (attachment.image == &image && attachment.image_view && attachment.mip_level == 0 &&
		    attachment.base_layer == 0 && image.ContentVersion() == impl.color_versions[slot]) {
			return impl.images[slot].get();
		}
	}
	return nullptr;
}
} // namespace Libs::Graphics

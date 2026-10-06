#include "graphics/host_gpu/renderer/geometryMotion.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/graphicContext.h"
#include <map>
#include <vector>

namespace Libs::Graphics {
struct GeometryMotion::Impl {
	GraphicContext& graphics;
	CommandScheduler& scheduler;
	uint64_t frame = 1, bytes = 0, guide_bytes = 0, barrier_frame = 0;
	bool pending_clear = false;
	static constexpr uint64_t Budget = 64 * 1024 * 1024;
	static constexpr uint64_t GuideBudget = 128 * 1024 * 1024;
	struct History {
		std::unique_ptr<Buffer> current, previous;
		uint64_t frame = 0;
		uint32_t occurrences = 0;
	};
	std::map<std::vector<uint64_t>, History> draws;
	struct Surface {
		std::unique_ptr<Image> guide;
		std::pair<uint64_t, uint64_t> version {};
		uint64_t frame = 0;
		uint64_t bytes = 0;
		bool valid = false;
	};
	std::map<uint64_t, Surface> surfaces;
	Impl(GraphicContext& g, CommandScheduler& s): graphics(g), scheduler(s) {}
	template<class T> void Retire(std::unique_ptr<T>& value) {
		if (!value) return;
		scheduler.DeferOperation([old = std::move(value)]() mutable { old.reset(); });
	}
};
GeometryMotion::GeometryMotion(GraphicContext& g, CommandScheduler& s): m_impl(std::make_unique<Impl>(g, s)) {}
GeometryMotion::~GeometryMotion() = default; // RenderContext drains its scheduler first.

std::array<uint32_t, 14> GeometryMotion::PrepareDraw(CommandBuffer& command, std::span<const uint64_t> key,
                                                   uint32_t capacity, uint32_t first_instance, uint32_t instances) {
	std::array<uint32_t, 14> push {};
	auto& s = *m_impl;
	if (!capacity || !instances || uint64_t(capacity) > Impl::Budget / 2 / 20 / instances) return push;
	const uint64_t size = uint64_t(capacity) * instances * 20;
	auto& entry = s.draws[std::vector<uint64_t>(key.begin(), key.end())];
	if ((entry.current && entry.current->Size() != size) || (entry.previous && entry.previous->Size() != size)) {
		for (auto* buffer : {&entry.current, &entry.previous}) {
			if (*buffer) s.bytes -= (*buffer)->Size();
			s.Retire(*buffer);
		}
		entry.frame = 0;
	}
	if (entry.frame == s.frame) {
		++entry.occurrences;
		for (auto& [id, surface] : s.surfaces) surface.valid = false;
		// Identical draw identities are ambiguous. The second draw does not
		// overwrite the first capture, and next frame neither consumes it.
		return push;
	}
	const bool history = entry.frame + 1 == s.frame && entry.occurrences == 1;
	entry.occurrences = 1;
	entry.frame = s.frame;
	std::swap(entry.current, entry.previous);
	if (!entry.current) {
		if (s.bytes + size > Impl::Budget) return push;
		entry.current = std::make_unique<Buffer>(s.graphics, s.scheduler, MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst |
		    vk::BufferUsageFlagBits::eShaderDeviceAddress, size);
		s.bytes += size;
		// Records carry the frame that wrote them, so only a new buffer needs zeroing.
		entry.current->Fill(0, size, 0);
	}
	if (s.barrier_frame != s.frame) {
		// Once per frame, not per draw: earlier frames' vertex stores become
		// visible to this frame's loads, and their loads finish before reuse.
		command.EndRendering();
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eVertexShader,
		    vk::PipelineStageFlagBits::eVertexShader, {}, 1, &barrier, 0, nullptr, 0, nullptr);
		s.barrier_frame = s.frame;
	}
	const auto current = entry.current->BufferDeviceAddress();
	const auto previous = entry.previous ? entry.previous->BufferDeviceAddress() : 0;
	push[0] = uint32_t(current); push[1] = uint32_t(current >> 32);
	push[2] = uint32_t(previous); push[3] = uint32_t(previous >> 32);
	push[4] = capacity; push[5] = first_instance; push[6] = instances;
	// Bit 0: history, bit 1: inverted depth (set by the caller), bits 2+: frame tag.
	push[7] = uint32_t(history && previous != 0) | uint32_t(s.frame & 0x3fffffffu) << 2;
	return push;
}
bool GeometryMotion::SupportsSurface(const Image& color, vk::Extent2D extent) const {
	const auto& s = *m_impl;
	if (!extent.width || !extent.height || uint64_t(extent.width) > Impl::GuideBudget / 8 / extent.height) return false;
	const uint64_t bytes = uint64_t(extent.width) * extent.height * 8;
	const auto found = s.surfaces.find(color.ContentVersion().first);
	const uint64_t existing = found == s.surfaces.end() ? 0 : found->second.bytes;
	return s.guide_bytes - existing <= Impl::GuideBudget - bytes;
}
bool GeometryMotion::Attach(CommandBuffer& command, RenderState& state) {
	auto& s = *m_impl;
	auto* color = state.color_attachments[0].image;
	if (!color || state.num_layers != 1 || state.color_attachments[0].mip_level != 0 ||
	    state.color_attachments[0].base_layer != 0 || color->backing.samples != 1 ||
	    state.color_attachments[7].image_view || !SupportsSurface(*color, {state.width, state.height})) return false;
	auto& surface = s.surfaces[color->ContentVersion().first];
	if (!surface.guide || surface.guide->backing.extent.width != state.width || surface.guide->backing.extent.height != state.height) {
		s.guide_bytes -= surface.bytes;
		surface.bytes = uint64_t(state.width) * state.height * 8;
		s.guide_bytes += surface.bytes;
		s.Retire(surface.guide);
		ImageInfo info {};
		info.pixel_format = vk::Format::eR16G16B16A16Sfloat;
		info.extent = {state.width, state.height, 1}; info.pitch = state.width; info.bytes_per_block = 8;
		surface.guide = std::make_unique<Image>(s.graphics, s.scheduler, info);
		surface.frame = 0;
	}
	// A non-instrumented write invalidates all prior coverage of this surface.
	// The clear is recorded inside the pass (BeginPass), so it does not split it.
	s.pending_clear = surface.frame != s.frame || !surface.valid ||
	    surface.version.second + 1 != color->ContentVersion().second ||
	    color->IsCpuDirty() || color->IsBufferModified();
	surface.frame = s.frame;
	surface.valid = true;
	// Consecutive instrumented draws continue one pass; each acquire adds one version.
	surface.version = color->ContentVersion();
	auto& guide = *surface.guide;
	guide.Transit(vk::ImageLayout::eColorAttachmentOptimal,
	    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
	ImageViewInfo view {};
	view.format = guide.backing.format; view.aspect = vk::ImageAspectFlagBits::eColor;
	view.usage = vk::ImageUsageFlagBits::eColorAttachment;
	auto& attachment = state.color_attachments[7];
	attachment.image = &guide; attachment.image_view = guide.FindView(view);
	attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
	state.num_color_attachments = 8;
	state.geometry_motion_attachment = true;
	return true;
}
void GeometryMotion::BeginPass(CommandBuffer& command) {
	auto& s = *m_impl;
	if (!s.pending_clear) return;
	s.pending_clear = false;
	const auto& effective = command.EffectiveRenderState();
	vk::ClearAttachment clear {vk::ImageAspectFlagBits::eColor, 7, vk::ClearValue {}};
	vk::ClearRect rect {vk::Rect2D {{0, 0}, {effective.width, effective.height}}, 0, 1};
	command.Handle().clearAttachments(1, &clear, 1, &rect);
}
void GeometryMotion::EndPass(const RenderState& state) {
	auto& s = *m_impl;
	for (const auto& attachment : state.color_attachments) {
		if (!attachment.image) continue;
		const auto version = attachment.image->ContentVersion();
		const auto found = s.surfaces.find(version.first);
		if (found == s.surfaces.end()) continue;
		auto& surface = found->second;
		surface.valid = state.color_attachments[7].image == surface.guide.get();
		surface.version = version;
	}
}
Image* GeometryMotion::Source(const Image& color) const {
	const auto& s = *m_impl;
	const auto found = s.surfaces.find(color.ContentVersion().first);
	if (found == s.surfaces.end()) return nullptr;
	const auto& surface = found->second;
	if (!surface.valid || surface.frame != s.frame || surface.version != color.ContentVersion() ||
	    color.IsCpuDirty() || color.IsBufferModified() ||
	    surface.guide->backing.extent.width != color.backing.extent.width ||
	    surface.guide->backing.extent.height != color.backing.extent.height) return nullptr;
	return surface.guide.get();
}
void GeometryMotion::AdvanceFrame() {
	auto& s = *m_impl;
	++s.frame;
	for (auto it = s.draws.begin(); it != s.draws.end();) {
		if (it->second.frame + 2 < s.frame) {
			for (auto* buffer : {&it->second.current, &it->second.previous}) {
				if (*buffer) s.bytes -= (*buffer)->Size();
				s.Retire(*buffer);
			}
			it = s.draws.erase(it);
		} else ++it;
	}
	for (auto it = s.surfaces.begin(); it != s.surfaces.end();) {
		if (it->second.frame + 2 < s.frame) {
			s.guide_bytes -= it->second.bytes;
			s.Retire(it->second.guide);
			it = s.surfaces.erase(it);
		} else ++it;
	}
}
} // namespace Libs::Graphics

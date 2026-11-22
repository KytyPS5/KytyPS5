#include "graphics/host_gpu/renderer/resolveCache.h"

#include "graphics/host_gpu/renderer/renderContext.h"

#include <cstring>
#include <type_traits>

namespace Libs::Graphics {

// Registers are compared and copied as bytes: a struct copy need not keep padding, and padding
// can only make unchanged registers miss.
template <typename T>
static bool SameRegisters(const T& a, const T& b) {
	static_assert(std::is_trivially_copyable_v<T>);
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}

ResolveStamp ResolveCache::Stamp(const ImageInfo& info, size_t candidates, ImageId found) const {
	// Only the sole image at the range is found again. Textures bind a stencil plane record's
	// depth owner, so a reuse would not access the record.
	if (candidates != 1 || !found || m_context.GetTextureCache().m_slot_images[found].depth_id) {
		return {};
	}
	const auto kind = info.metadata.kind;
	return {m_epoch, m_context.GetBufferCache().DrawWindow(),
	        m_context.GetCommandScheduler().CurrentTick(),
	        kind == ImageMetadataKind::Dcc || kind == ImageMetadataKind::Cmask};
}

bool ResolveCache::Reuse(ImageId id, const ResolveStamp& stamp) {
	if (stamp.epoch != m_epoch ||
	    (stamp.clear_keys && (stamp.window != m_context.GetBufferCache().DrawWindow() ||
	                          stamp.tick != m_context.GetCommandScheduler().CurrentTick()))) {
		return false;
	}
	auto& cache                                = m_context.GetTextureCache();
	cache.m_slot_images[id].tick_accessed_last = cache.m_gc_tick;
	return true;
}

bool ResolveCache::FindColor(uint32_t slot, const HW::RenderTarget& rt, bool exact_format,
                             RenderColorInfo& r) {
	const auto& last = m_colors[slot];
	if (last.exact_format != exact_format || !SameRegisters(last.registers, rt) ||
	    !Reuse(last.target.image_id, last.target.desc.stamp)) {
		return false;
	}
	r = last.target;
	return true;
}

void ResolveCache::SetColor(uint32_t slot, const HW::RenderTarget& rt, bool exact_format,
                            const RenderColorInfo& r) {
	auto& last = m_colors[slot];
	std::memcpy(&last.registers, &rt, sizeof(rt));
	last.exact_format = exact_format;
	last.target       = r;
}

bool ResolveCache::FindDepth(const HW::DepthRenderTarget& z, TextureCache::ImageDesc& desc,
                             ImageId& id) {
	if (!SameRegisters(m_depth.registers, z) || !Reuse(m_depth.image_id, m_depth.desc.stamp)) {
		return false;
	}
	desc = m_depth.desc;
	id   = m_depth.image_id;
	return true;
}

void ResolveCache::SetDepth(const HW::DepthRenderTarget& z, const TextureCache::ImageDesc& desc,
                            ImageId id) {
	std::memcpy(&m_depth.registers, &z, sizeof(z));
	m_depth.desc     = desc;
	m_depth.image_id = id;
}

} // namespace Libs::Graphics

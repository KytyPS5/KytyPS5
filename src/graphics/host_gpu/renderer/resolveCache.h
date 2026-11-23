#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOLVECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOLVECACHE_H_

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/cache/resolveStamp.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics {

class RenderContext;

// PS5: checks kept FindImage results and keeps each render target's last resolve.
class ResolveCache {
public:
	explicit ResolveCache(RenderContext& context) noexcept: m_context(context) {}

	// The image set, a stencil plane record or the surface metadata changed.
	void Invalidate() noexcept { m_epoch++; }
	// The stamp of a FindImage whose first loop matched `found` among `candidates` images.
	[[nodiscard]] ResolveStamp Stamp(const ImageInfo& info, size_t candidates, ImageId found) const;
	// Whether FindImage, given the same inputs, would find `id` again; if so, accesses it as
	// FindImage does.
	[[nodiscard]] bool Reuse(ImageId id, const ResolveStamp& stamp);

	// The last target resolved from the same arguments, while Reuse holds.
	[[nodiscard]] bool FindColor(uint32_t slot, const HW::RenderTarget& rt, bool exact_format,
	                             RenderColorInfo& r);
	void               SetColor(uint32_t slot, const HW::RenderTarget& rt, bool exact_format,
	                            const RenderColorInfo& r);
	[[nodiscard]] bool FindDepth(const HW::DepthRenderTarget& z, TextureCache::ImageDesc& desc,
	                             ImageId& id);
	void SetDepth(const HW::DepthRenderTarget& z, const TextureCache::ImageDesc& desc, ImageId id);

private:
	struct Color {
		HW::RenderTarget registers {};
		bool             exact_format = false;
		RenderColorInfo  target;
	};
	struct Depth {
		HW::DepthRenderTarget   registers {};
		TextureCache::ImageDesc desc;
		ImageId                 image_id;
	};

	RenderContext&                                  m_context;
	uint64_t                                        m_epoch = 1;
	std::array<Color, RENDER_COLOR_ATTACHMENTS_MAX> m_colors;
	Depth                                           m_depth;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOLVECACHE_H_

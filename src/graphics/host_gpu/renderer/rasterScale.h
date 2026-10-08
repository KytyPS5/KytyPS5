#ifndef KYTY_GRAPHICS_RASTER_SCALE_H_
#define KYTY_GRAPHICS_RASTER_SCALE_H_

#include "graphics/host_gpu/renderer/renderTarget.h"

#include <memory>

namespace Libs::Graphics {
class CommandScheduler;
class Image;
struct GraphicContext;

vk::Rect2D ScaleRasterScissor(vk::Rect2D scissor, float scale_x, float scale_y,
                              vk::Extent2D extent);

// Keep the guest's memory, texture sizes and shader coordinates unchanged.
// Only raster attachments are reduced; pass completion materializes the result
// into the cache image before a texture/storage/CPU consumer can observe it.
class RasterScaler final {
public:
	RasterScaler(GraphicContext& graphics, CommandScheduler& scheduler);
	~RasterScaler();
	KYTY_CLASS_NO_COPY(RasterScaler);
	const RenderState& Begin(vk::CommandBuffer command, const RenderState& state);
	void               End(vk::CommandBuffer command);
	const RenderState& State() const;
	void               RefreshSourceVersions();
	// Only the last complete, unchanged, full-surface color pass is eligible.
	Image* ColorSource(const Image& original) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics
#endif

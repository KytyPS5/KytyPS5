#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/imageInfo.h"

namespace Libs::Graphics {

struct DepthStencilVulkanImage;
struct GpuTextureVulkanImage;
struct GraphicContext;

class Tiler final {
public:
	Tiler() = default;
	KYTY_CLASS_NO_COPY(Tiler);

	void DetileImage(GraphicContext* ctx, GpuTextureVulkanImage* image, const ImageInfo& info,
	                 const BufferImageCopySource& source, bool refresh, bool storage) const;
	void DetileImage(GraphicContext* ctx, DepthStencilVulkanImage* image,
	                 const DepthTargetInfo& info, const BufferImageCopySource& source, bool refresh,
	                 uint32_t base_layer = 0) const;
	void DetileStencil(GraphicContext* ctx, DepthStencilVulkanImage* image,
	                   const DepthTargetInfo& info, const BufferImageCopySource& source,
	                   bool refresh, uint32_t base_layer = 0) const;
	void TileImage(void* dst, const void* src, const RenderTargetInfo& info) const;
	void TileImage(void* dst, const void* src, const ImageInfo& info) const;
	void TileImage(void* dst, const void* src, const DepthTargetInfo& info) const;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TILER_H_

#include "graphics/host_gpu/renderer/tiler.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/image.h"
#include "graphics/host_gpu/utils.h"

namespace Libs::Graphics {

void Tiler::DetileImage(GraphicContext* ctx, GpuTextureVulkanImage* image,
                              const ImageInfo& info, const BufferImageCopySource& source,
                              bool refresh, bool storage) const {
	if (ctx == nullptr || image == nullptr || info.address == 0 || info.size == 0 ||
	    info.width == 0 || info.height == 0 || info.depth == 0 || info.levels == 0 ||
	    info.levels >= 16 || !source.cpu_current || source.address != info.address ||
	    source.size != info.size || (source.buffer == nullptr && source.offset != 0)) {
		EXIT("Tiler: unsupported sampled-image detile, ctx=%p image=%p source_buffer=%p "
		     "source=0x%016" PRIx64 "+0x%016" PRIx64 " offset=0x%016" PRIx64
		     " current=%d image=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%ux%u levels=%u tile=%u storage=%d\n",
		     static_cast<const void*>(ctx), static_cast<const void*>(image),
		     static_cast<const void*>(source.buffer), source.address, source.size, source.offset,
		     source.cpu_current, info.address, info.size, info.width, info.height, info.depth,
		     info.levels, info.tile, storage);
	}
	if (refresh) {
		VulkanDeviceWaitIdle(ctx);
	}
	const bool array_texture  = TextureIsLayeredTexture(info.type);
	const bool volume_texture = TextureIs3DTexture(info.type);
	auto       layout         = TextureCalcUploadLayout(
	    info.format, info.width, info.height, info.levels, info.depth, info.pitch, info.tile,
	    info.size, true, false, volume_texture, storage ? "StorageTextureCache" : "TextureCache");
	const auto slice_layout = TextureUploadSliceLayout::MipChainPerSlice;
	auto regions = TextureBuildUploadRegions(layout, image->format, info.width, info.height,
	                                         info.depth, info.levels, array_texture, volume_texture,
	                                         TextureUploadDestination::MipLevels, slice_layout);
	// Keep the buffer owner explicit at the detiling seam. The current PS5
	// backend consumes the coherent guest publication; a GPU detiler can consume source.buffer and
	// source.offset here without changing TextureCache ownership or alias classification.
	TextureUploadGuestImage(
	    ctx, image, reinterpret_cast<const void*>(source.address), info.size, regions, layout,
	    info.format, info.width, info.height, info.depth, info.levels, slice_layout,
	    storage ? "StorageTextureCache" : "TextureCache",
	    static_cast<uint64_t>(storage ? VK_IMAGE_LAYOUT_GENERAL
	                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

void Tiler::DetileImage(GraphicContext* ctx, DepthStencilVulkanImage* image,
                              const DepthTargetInfo& info, const BufferImageCopySource& source,
                              bool refresh) const {
	const bool d16 = info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm) &&
	                 info.format == VK_FORMAT_D16_UNORM && info.bytes_per_element == 2;
	const bool d32 =
	    info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	    (info.format == VK_FORMAT_D32_SFLOAT || info.format == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
	    info.bytes_per_element == 4;
	if (ctx == nullptr || image == nullptr || info.address == 0 || info.size == 0 ||
	    info.width == 0 || info.height == 0 || info.pitch < info.width ||
	    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kDepth) || (!d16 && !d32) ||
	    !source.cpu_current || source.address != info.address || source.size != info.size ||
	    (source.buffer == nullptr && source.offset != 0)) {
		EXIT("Tiler: unsupported depth detile, ctx=%p image=%p source_buffer=%p "
		     "source=0x%016" PRIx64 "+0x%016" PRIx64 " offset=0x%016" PRIx64
		     " current=%d depth=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%u pitch=%u tile=%u format=%d guest_format=%u bpe=%u\n",
		     static_cast<const void*>(ctx), static_cast<const void*>(image),
		     static_cast<const void*>(source.buffer), source.address, source.size, source.offset,
		     source.cpu_current, info.address, info.size, info.width, info.height, info.pitch,
		     info.tile_mode, static_cast<int>(info.format), info.guest_format,
		     info.bytes_per_element);
	}
	if (refresh) {
		VulkanDeviceWaitIdle(ctx);
	}
	UtilScratchBuffer linear(info.size);
	TileConvertTiledToLinearDepth(linear.Data(), reinterpret_cast<const void*>(source.address),
	                              info.guest_format, info.width, info.height, info.pitch,
	                              info.size);
	UtilFillDepthImage(ctx, image, linear.Data(), info.size, info.pitch);
}

void Tiler::TileImage(void* dst, const void* src, const RenderTargetInfo& info) const {
	const bool supported_element = info.bytes_per_element == 1 || info.bytes_per_element == 2 ||
	                               info.bytes_per_element == 4 || info.bytes_per_element == 8;
	if (dst == nullptr || src == nullptr || info.address == 0 || info.size == 0 ||
	    info.width == 0 || info.height == 0 || info.pitch < info.width ||
	    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) || info.levels != 1 ||
	    !supported_element) {
		EXIT("Tiler: unsupported render-target tile, dst=%p src=%p "
		     "addr=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%u pitch=%u levels=%u tile=%u bpe=%u\n",
		     dst, src, info.address, info.size, info.width, info.height, info.pitch, info.levels,
		     info.tile_mode, info.bytes_per_element);
	}
	TileConvertLinearToTiledRenderTarget(dst, src, info.width, info.height, info.pitch,
	                                     info.bytes_per_element, info.size);
}

void Tiler::TileImage(void* dst, const void* src, const DepthTargetInfo& info) const {
	const bool supported_format =
	    (info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm) &&
	     info.format == VK_FORMAT_D16_UNORM && info.bytes_per_element == 2) ||
	    (info.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	     info.format == VK_FORMAT_D32_SFLOAT && info.bytes_per_element == 4);
	if (dst == nullptr || src == nullptr || info.address == 0 || info.size == 0 ||
	    info.stencil_address != 0 || info.stencil_size != 0 || info.width == 0 ||
	    info.height == 0 || info.pitch < info.width ||
	    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kDepth) || !supported_format) {
		EXIT("Tiler: unsupported depth-target tile, dst=%p src=%p "
		     "depth=0x%016" PRIx64 "+0x%016" PRIx64 " stencil=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%u pitch=%u tile=%u format=%d guest_format=%u bpe=%u\n",
		     dst, src, info.address, info.size, info.stencil_address, info.stencil_size, info.width,
		     info.height, info.pitch, info.tile_mode, static_cast<int>(info.format),
		     info.guest_format, info.bytes_per_element);
	}
	TileConvertLinearToTiledDepth(dst, src, info.guest_format, info.width, info.height, info.pitch,
	                              info.size);
}

} // namespace Libs::Graphics

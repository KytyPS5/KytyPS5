#include "graphics/host_gpu/renderer/tiler.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/image.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <vector>

namespace Libs::Graphics {

template <uint32_t (*Encode)(uint16_t)>
static void UploadPromotedD16Depth(GraphicContext* ctx, DepthStencilVulkanImage* image,
                                   const DepthTargetInfo& info, const BufferImageCopySource& source,
                                   uint32_t base_layer) {
	const uint64_t slice_size       = info.size / info.layers;
	const uint64_t texels           = static_cast<uint64_t>(info.pitch) * info.height;
	const uint64_t host_slice_size  = texels * sizeof(uint32_t);
	const uint64_t host_upload_size = host_slice_size * info.layers;
	if (host_upload_size > UINT32_MAX) {
		EXIT("Tiler: invalid D16 host-promotion footprint, guest_slice=0x%016" PRIx64
		     " host_slice=0x%016" PRIx64 " layers=%u\n",
		     slice_size, host_slice_size, info.layers);
	}
	Transfer::ScratchBuffer      host_linear(host_upload_size);
	std::vector<uint16_t>        guest_linear(slice_size / sizeof(uint16_t));
	std::vector<BufferImageCopy> regions;
	regions.reserve(info.layers);
	for (uint32_t layer = 0; layer < info.layers; layer++) {
		auto* guest_slice = reinterpret_cast<const uint8_t*>(source.address) + slice_size * layer;
		TileConvertTiledToLinearDepth(guest_linear.data(), guest_slice, info.guest_format,
		                              info.width, info.height, info.pitch, slice_size);
		auto* host_slice = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(host_linear.Data()) +
		                                               host_slice_size * layer);
		for (uint64_t texel = 0; texel < texels; texel++) {
			host_slice[texel] = Encode(guest_linear[texel]);
		}
		BufferImageCopy region {};
		region.offset    = static_cast<uint32_t>(host_slice_size * layer);
		region.pitch     = info.pitch;
		region.width     = info.width;
		region.height    = info.height;
		region.dst_layer = base_layer + layer;
		region.aspect    = vk::ImageAspectFlagBits::eDepth;
		regions.push_back(region);
	}
	Transfer::UploadImage(ctx, image, host_linear.Data(), host_upload_size, regions,
	                      vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

void Tiler::DetileImage(GraphicContext* ctx, GpuTextureVulkanImage* image, const ImageInfo& info,
                        const BufferImageCopySource& source, bool refresh, bool storage) const {
	if (refresh) {
		Transfer::WaitForGraphicsIdle(ctx);
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
	TextureUploadGuestImage(
	    ctx, image, reinterpret_cast<const void*>(source.address), info.size, regions, layout,
	    info.format, info.width, info.height, info.depth, info.levels, slice_layout,
	    storage ? "StorageTextureCache" : "TextureCache",
	    storage ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal);
}

void Tiler::DetileImage(GraphicContext* ctx, DepthStencilVulkanImage* image,
                        const DepthTargetInfo& info, const BufferImageCopySource& source,
                        bool refresh, uint32_t base_layer) const {
	if (refresh) {
		Transfer::WaitForGraphicsIdle(ctx);
	}
	if (DepthAspectTransferBytes(info.format) != info.bytes_per_element) {
		switch (info.format) {
			case vk::Format::eD24UnormS8Uint:
				UploadPromotedD16Depth<EncodeD16AsD24>(ctx, image, info, source, base_layer);
				return;
			case vk::Format::eD32SfloatS8Uint:
				UploadPromotedD16Depth<EncodeD16AsD32>(ctx, image, info, source, base_layer);
				return;
			default:
				EXIT("Tiler: unsupported depth transfer conversion, format=%d guest_bpe=%u\n",
				     static_cast<int>(info.format), info.bytes_per_element);
		}
	}
	const auto                   slice_size = info.size / info.layers;
	Transfer::ScratchBuffer      linear(info.size);
	std::vector<BufferImageCopy> regions;
	regions.reserve(info.layers);
	for (uint32_t layer = 0; layer < info.layers; layer++) {
		auto* linear_slice = static_cast<uint8_t*>(linear.Data()) + slice_size * layer;
		auto* guest_slice  = reinterpret_cast<const uint8_t*>(source.address) + slice_size * layer;
		TileConvertTiledToLinearDepth(linear_slice, guest_slice, info.guest_format, info.width,
		                              info.height, info.pitch, slice_size);
		BufferImageCopy region {};
		region.offset    = static_cast<uint32_t>(slice_size * layer);
		region.pitch     = info.pitch;
		region.width     = info.width;
		region.height    = info.height;
		region.dst_layer = base_layer + layer;
		region.aspect    = vk::ImageAspectFlagBits::eDepth;
		regions.push_back(region);
	}
	Transfer::UploadImage(ctx, image, linear.Data(), info.size, regions,
	                      vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

void Tiler::DetileStencil(GraphicContext* ctx, DepthStencilVulkanImage* image,
                          const DepthTargetInfo& info, const BufferImageCopySource& source,
                          bool refresh, uint32_t base_layer) const {
	const auto stencil_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt);
	const auto stencil_pitch  = TileGetTexturePitch(
	    stencil_format, info.width, 1, Prospero::GpuEnumValue(Prospero::TileMode::kDepth));
	if (refresh) {
		Transfer::WaitForGraphicsIdle(ctx);
	}
	const auto                   slice_size = info.stencil_size / info.layers;
	Transfer::ScratchBuffer      linear(info.stencil_size);
	std::vector<BufferImageCopy> regions;
	regions.reserve(info.layers);
	for (uint32_t layer = 0; layer < info.layers; layer++) {
		auto* linear_slice = static_cast<uint8_t*>(linear.Data()) + slice_size * layer;
		auto* guest_slice  = reinterpret_cast<const uint8_t*>(source.address) + slice_size * layer;
		TileConvertTiledToLinearDepth(linear_slice, guest_slice, stencil_format, info.width,
		                              info.height, stencil_pitch, slice_size);
		BufferImageCopy region {};
		region.offset    = static_cast<uint32_t>(slice_size * layer);
		region.pitch     = stencil_pitch;
		region.width     = info.width;
		region.height    = info.height;
		region.dst_layer = base_layer + layer;
		region.aspect    = vk::ImageAspectFlagBits::eStencil;
		regions.push_back(region);
	}
	Transfer::UploadImage(ctx, image, linear.Data(), info.stencil_size, regions,
	                      vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

void Tiler::TileImage(void* dst, const void* src, const RenderTargetInfo& info) const {
	const bool standard64 = IsSupportedStandard64RenderTarget(info);
	if ((info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
	     !standard64) ||
	    info.levels != 1) {
		EXIT("Tiler: unsupported render-target tile, dst=%p src=%p "
		     "addr=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%u pitch=%u levels=%u tile=%u bpe=%u\n",
		     dst, src, info.address, info.size, info.width, info.height, info.pitch, info.levels,
		     info.tile_mode, info.bytes_per_element);
	}
	const auto slice_size = info.size / info.layers;
	for (uint32_t layer = 0; layer < info.layers; layer++) {
		auto* guest_slice  = static_cast<uint8_t*>(dst) + slice_size * layer;
		auto* linear_slice = static_cast<const uint8_t*>(src) + slice_size * layer;
		if (standard64) {
			TileConvertLinearToTiledStandard64KB32(guest_slice, linear_slice, info.width,
			                                       info.height, info.pitch, slice_size);
		} else {
			TileConvertLinearToTiledRenderTarget(guest_slice, linear_slice, info.width, info.height,
			                                     info.pitch, info.bytes_per_element, slice_size);
		}
	}
}

void Tiler::TileImage(void* dst, const void* src, const ImageInfo& info) const {
	const bool image_2d =
	    info.type == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) && info.depth == 1;
	const auto bytes_per_element = Prospero::RenderTargetBytesPerElement(info.format);
	if (!image_2d || info.levels == 0 || info.levels > 16 ||
	    info.tile != Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) ||
	    bytes_per_element == 0) {
		EXIT("Tiler: unsupported storage-texture tile, addr=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%ux%u levels=%u tile=%u format=%u\n",
		     info.address, info.size, info.width, info.height, info.depth, info.levels, info.tile,
		     info.format);
	}
	auto layout = TextureCalcUploadLayout(info.format, info.width, info.height, info.levels,
	                                      info.depth, info.pitch, info.tile, info.size, true, false,
	                                      false, "StorageTextureReadback");
	auto regions = TextureBuildUploadRegions(
	    layout, VulkanFormat(info.format), info.width, info.height, info.depth, info.levels, false,
	    false, TextureUploadDestination::MipLevels, TextureUploadSliceLayout::MipChainPerSlice);
	std::memset(dst, 0, info.size);
	for (uint32_t level = 0; level < info.levels; level++) {
		const auto& level_size = layout.level_sizes[level];
		const auto  guest_offset =
		    level_size.src_size != 0 ? level_size.src_offset : level_size.offset;
		auto*       guest  = static_cast<uint8_t*>(dst) + guest_offset;
		const auto* linear = static_cast<const uint8_t*>(src) + regions[level].offset;
		TileConvertLinearToTiledRenderTarget(
		    guest, linear, regions[level].width, regions[level].height, regions[level].pitch,
		    bytes_per_element, level_size.size, level_size.src_size, level_size.x, level_size.y);
	}
}

void Tiler::TileImage(void* dst, const void* src, const DepthTargetInfo& info) const {
	if (info.stencil_address != 0 || info.stencil_size != 0 ||
	    info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kDepth) ||
	    !IsSupportedDepthTargetFormat(info)) {
		EXIT("Tiler: unsupported depth-target tile, dst=%p src=%p "
		     "depth=0x%016" PRIx64 "+0x%016" PRIx64 " stencil=0x%016" PRIx64 "+0x%016" PRIx64
		     " extent=%ux%u pitch=%u tile=%u format=%d guest_format=%u bpe=%u\n",
		     dst, src, info.address, info.size, info.stencil_address, info.stencil_size, info.width,
		     info.height, info.pitch, info.tile_mode, static_cast<int>(info.format),
		     info.guest_format, info.bytes_per_element);
	}
	const auto slice_size = info.size / info.layers;
	for (uint32_t layer = 0; layer < info.layers; layer++) {
		auto* guest_slice  = static_cast<uint8_t*>(dst) + slice_size * layer;
		auto* linear_slice = static_cast<const uint8_t*>(src) + slice_size * layer;
		TileConvertLinearToTiledDepth(guest_slice, linear_slice, info.guest_format, info.width,
		                              info.height, info.pitch, slice_size);
	}
}

} // namespace Libs::Graphics

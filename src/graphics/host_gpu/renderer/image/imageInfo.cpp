#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include "common/alignment.h"

namespace Libs::Graphics {
namespace {

uint64_t Multiply(uint64_t left, uint64_t right) {
	EXIT_IF(right != 0 && left > UINT64_MAX / right);
	return left * right;
}

uint64_t Add(uint64_t left, uint64_t right) {
	EXIT_IF(right > UINT64_MAX - left);
	return left + right;
}

bool IsDepthPlane(const ImageInfo& info) {
	return (info.IsDepth() || info.pixel_format == vk::Format::eS8Uint) &&
	       (info.tile_mode == Prospero::TileMode::kDepth ||
	        info.tile_mode == Prospero::TileMode::kLinear);
}

} // namespace

vk::Extent3D ImageInfo::MipExtent(uint32_t level) const {
	EXIT_IF(level >= resources.levels || level >= mip_layout.size());
	return {std::max(extent.width >> level, 1u), std::max(extent.height >> level, 1u),
	        IsVolume() ? std::max(extent.depth >> level, 1u) : resources.layers};
}

void ImageInfo::UpdateSize() {
	EXIT_IF(guest_format == Prospero::BufferFormat::kInvalid || extent.width == 0 ||
	        extent.height == 0 || extent.depth == 0 || resources.levels == 0 ||
	        resources.levels > mip_layout.size() || resources.layers == 0 || samples == 0 ||
	        !std::has_single_bit(samples));
	TileTextureElementLayout element {};
	EXIT_NOT_IMPLEMENTED(!TileGetTextureElementLayout(guest_format, element));
	if (bytes_per_block == 0) {
		bytes_per_block = element.bytes;
	}
	EXIT_IF(bytes_per_block != element.bytes);
	mip_layout                     = {};
	tiling                         = {};
	tiling.texel_width             = element.texel_width;
	tiling.texel_height            = element.texel_height;
	tiling.block.bytes_per_element = element.bytes;
	tiled_slice_stride             = 0;
	linear_slice_stride            = 0;
	first_tail_level               = 16;

	if (samples > 1) {
		EXIT_NOT_IMPLEMENTED(IsVolume() || resources.levels != 1 ||
		                     (tile_mode != Prospero::TileMode::kDepth &&
		                      tile_mode != Prospero::TileMode::kRenderTarget));
		const auto fragments = std::countr_zero(samples);
		if (pitch == 0) {
			pitch = tile_mode == Prospero::TileMode::kDepth
			            ? TileGetDepthPitch(extent.width, bytes_per_block, fragments)
			            : TileGetRenderTargetPitch(extent.width, bytes_per_block, fragments);
		}
		EXIT_NOT_IMPLEMENTED(!TileGetTextureBlockLayout(guest_format, tile_mode, false, tiling));
		if (data.size == 0) {
			TileSizeAlign slice {};
			EXIT_NOT_IMPLEMENTED(!TileGetRenderTargetSize(extent.width, extent.height, pitch,
			                                              bytes_per_block, slice, fragments));
			data.size = Multiply(slice.size, resources.layers);
		}
		EXIT_IF(data.size % resources.layers != 0);
		tiled_slice_stride = data.size / resources.layers;
		mip_layout[0]      = {0, data.size, pitch, extent.height};
		return;
	}

	if (pitch == 0) {
		pitch = TileGetTexturePitch(guest_format, extent.width, tile_mode);
	}
	EXIT_IF(pitch == 0);
	if (IsDepthPlane(*this) && resources.levels == 1) {
		EXIT_NOT_IMPLEMENTED(IsVolume());
		if (IsTiled()) {
			EXIT_NOT_IMPLEMENTED(
			    !TileGetTextureBlockLayout(guest_format, tile_mode, false, tiling));
		}
		if (data.size == 0) {
			TileSizeAlign slice {};
			TileGetTextureSize(guest_format, extent.width, extent.height, 1, tile_mode, &slice,
			                   nullptr, nullptr);
			data.size = Multiply(slice.size, resources.layers);
		}
		EXIT_IF(data.size % resources.layers != 0);
		const auto slice = data.size / resources.layers;
		EXIT_IF(Multiply(Multiply(pitch, extent.height), bytes_per_block) > slice);
		linear_slice_stride = slice;
		tiled_slice_stride  = IsTiled() ? slice : 0;
		mip_layout[0]       = {0, data.size, pitch, extent.height, 0, slice, pitch};
		return;
	}
	if (!IsTiled()) {
		TileSizeAlign  total {};
		TileSizeOffset offsets[16] {};
		TilePaddedSize padded[16] {};
		TileGetTextureSize(guest_format, extent.width, extent.height, resources.levels, tile_mode,
		                   &total, offsets, padded);
		if (data.size == 0) {
			data.size = Multiply(total.size, TransferLayers());
		}
		for (uint32_t level = 0; level < resources.levels; ++level) {
			auto& mip         = mip_layout[level];
			mip.offset        = offsets[level].offset;
			mip.size          = Multiply(offsets[level].size, MipExtent(level).depth);
			mip.pitch         = padded[level].width / element.texel_width;
			mip.height        = padded[level].height / element.texel_height;
			mip.linear_offset = offsets[level].offset;
			mip.linear_size   = offsets[level].size;
			mip.linear_pitch  = mip.pitch;
			linear_slice_stride =
			    std::max(linear_slice_stride, Add(mip.linear_offset, mip.linear_size));
		}
		if (TransferLayers() > 1 && data.size % TransferLayers() == 0) {
			linear_slice_stride = std::max(linear_slice_stride, data.size / TransferLayers());
		}
	} else {
		TileSurfaceLayout            surface {};
		const TileSurfaceDescription description {guest_format,
		                                          tile_mode,
		                                          IsVolume() ? TileSurfaceDimension::Dim3D
		                                                     : TileSurfaceDimension::Dim2D,
		                                          extent.width,
		                                          extent.height,
		                                          IsVolume() ? extent.depth : 1u,
		                                          resources.levels,
		                                          resources.layers};
		EXIT_NOT_IMPLEMENTED(!TileGetTiledTextureLayout(description, surface));
		tiling             = surface.texture;
		first_tail_level   = surface.first_tail_level;
		tiled_slice_stride = surface.block_slice_size;
		if (data.size == 0) {
			data.size = surface.total_size;
		}
		if (!IsVolume() && resources.layers > 1 && data.size % resources.layers == 0) {
			tiled_slice_stride = std::max(tiled_slice_stride, data.size / resources.layers);
		}
		uint64_t linear_offset = 0;
		uint32_t mip_pitch     = IsVolume() ? extent.width : pitch;
		for (uint32_t level = 0; level < resources.levels; ++level) {
			const auto& source = surface.mips[level];
			auto&       mip    = mip_layout[level];
			const auto  active = MipExtent(level);
			mip.offset         = source.offset;
			mip.size           = Multiply(source.size, IsVolume() ? 1u : resources.layers);
			mip.pitch          = source.padded_width;
			mip.height         = source.padded_height;
			mip.tail_x         = source.tail_x;
			mip.tail_y         = source.tail_y;
			mip.linear_pitch =
			    Common::AlignUp(mip_pitch, element.texel_width) / element.texel_width;
			const auto rows =
			    Common::AlignUp(active.height, element.texel_height) / element.texel_height;
			mip.linear_size      = Multiply(Multiply(mip.linear_pitch, rows), element.bytes);
			const auto alignment = std::max(element.bytes, 4u);
			EXIT_IF(linear_offset > UINT64_MAX - (alignment - 1u));
			mip.linear_offset = Common::AlignUp<uint64_t>(linear_offset, alignment);
			linear_offset     = Add(mip.linear_offset, Multiply(mip.linear_size, active.depth));
			mip_pitch         = std::max(mip_pitch >> 1u, 1u);
		}
	}
}

uint64_t ImageInfo::LinearSize(uint32_t levels) const {
	levels = levels == 0 ? resources.levels : levels;
	EXIT_IF(levels == 0 || levels > resources.levels || levels > mip_layout.size());
	if (linear_slice_stride == 0) {
		const auto& mip = mip_layout[levels - 1u];
		return Add(mip.linear_offset, Multiply(mip.linear_size, MipExtent(levels - 1u).depth));
	}
	uint64_t size = 0;
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip    = mip_layout[level];
		const auto  slices = MipExtent(level).depth;
		const auto  stride = linear_slice_stride != 0 ? linear_slice_stride : mip.linear_size;
		size               = std::max(
		    size, Add(Add(mip.linear_offset, Multiply(slices - 1u, stride)), mip.linear_size));
	}
	return size;
}

std::vector<vk::BufferImageCopy> ImageInfo::BufferCopies(uint64_t buffer_offset,
                                                         uint32_t levels) const {
	levels = levels == 0 ? resources.levels : levels;
	EXIT_IF(levels > resources.levels || levels > mip_layout.size() || tiling.texel_width == 0 ||
	        tiling.texel_height == 0);
	const auto aspect = IsDepth()                             ? vk::ImageAspectFlagBits::eDepth
	                    : pixel_format == vk::Format::eS8Uint ? vk::ImageAspectFlagBits::eStencil
	                                                          : vk::ImageAspectFlagBits::eColor;
	std::vector<vk::BufferImageCopy> copies;
	std::array<uint32_t, 16>         image_heights {};
	uint64_t                         count = 0;
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip    = mip_layout[level];
		const auto  active = MipExtent(level);
		if (linear_slice_stride == 0) {
			image_heights[level] = Common::AlignUp(active.height, tiling.texel_height);
		} else {
			const auto row_bytes = Multiply(mip.linear_pitch, tiling.block.bytes_per_element);
			EXIT_IF(row_bytes == 0);
			const auto rows = linear_slice_stride / row_bytes;
			if (linear_slice_stride % row_bytes == 0 &&
			    rows >=
			        (uint64_t {active.height} + tiling.texel_height - 1u) / tiling.texel_height &&
			    rows <= UINT32_MAX / tiling.texel_height) {
				image_heights[level] = static_cast<uint32_t>(rows) * tiling.texel_height;
			}
		}
		count += image_heights[level] != 0 ? 1u : active.depth;
	}
	copies.reserve(static_cast<size_t>(count));
	for (uint32_t level = 0; level < levels; ++level) {
		const auto&         mip    = mip_layout[level];
		const auto          active = MipExtent(level);
		vk::BufferImageCopy copy {};
		copy.bufferOffset      = Add(buffer_offset, mip.linear_offset);
		copy.bufferRowLength   = mip.linear_pitch * tiling.texel_width;
		copy.bufferImageHeight = image_heights[level] != 0 ? image_heights[level]
		                         : !IsTiled() ? mip.height * tiling.texel_height
		                                      : Common::AlignUp(active.height, tiling.texel_height);
		copy.imageSubresource  = {aspect, level, 0, 1};
		copy.imageExtent       = {active.width, active.height, 1};
		if (image_heights[level] == 0) {
			for (uint32_t slice = 0; slice < active.depth; ++slice) {
				copy.bufferOffset                    = Add(Add(buffer_offset, mip.linear_offset),
				                                           Multiply(slice, linear_slice_stride));
				copy.imageSubresource.baseArrayLayer = IsVolume() ? 0u : slice;
				copy.imageOffset.z                   = IsVolume() ? static_cast<int32_t>(slice) : 0;
				copies.push_back(copy);
			}
		} else {
			copy.imageSubresource.layerCount = IsVolume() ? 1u : active.depth;
			copy.imageExtent.depth           = IsVolume() ? active.depth : 1u;
			copies.push_back(copy);
		}
	}
	return copies;
}

} // namespace Libs::Graphics

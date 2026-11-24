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
	return info.IsDepth() && (info.tile_mode == Prospero::TileMode::kDepth ||
	                          info.tile_mode == Prospero::TileMode::kLinear);
}

GuestRange TiledMipRange(const ImageInfo& info, uint32_t level) {
	const auto& block = info.tiling.block;
	if (info.tile_mode != Prospero::TileMode::kStandard4KB ||
	    block.family != TileBlockFamily::Standard4KB || info.IsVolume() || info.samples != 1 ||
	    info.HasMetadata() || level >= info.resources.levels || level >= info.mip_layout.size() ||
	    info.tiling.texel_width == 0 || info.tiling.texel_height == 0) {
		return {};
	}
	const auto extent = info.MipExtent(level);
	const auto width  = (extent.width + info.tiling.texel_width - 1u) / info.tiling.texel_width;
	const auto height = (extent.height + info.tiling.texel_height - 1u) / info.tiling.texel_height;
	const auto& mip   = info.mip_layout[level];
	uint32_t last = 0, origin = 0;
	if (!std::has_single_bit(width) || !std::has_single_bit(height) ||
	    width > block.block_width || height > block.block_height ||
	    mip.tail_x > block.block_width - width || mip.tail_y > block.block_height - height ||
	    mip.tail_x % width != 0 || mip.tail_y % height != 0 ||
	    !TileGetBlockOffset(block, width - 1u, height - 1u, 0, last) ||
	    !TileGetBlockOffset(block, mip.tail_x, mip.tail_y, 0, origin)) {
		return {};
	}
	// Standard4KB interleaves monotonically ordered X/Y bits, so the last
	// corner bounds every active texel. Tiling preserves any holes in this span.
	const uint64_t size = uint64_t {last} + block.bytes_per_element;
	if ((origin & (std::bit_ceil(size) - 1u)) != 0 || size > block.block_size - origin ||
	    mip.offset > UINT64_MAX - origin) {
		return {};
	}
	return {mip.offset + origin, size};
}

uint32_t CompactMipSize(const ImageInfo& info) {
	if (info.resources.levels != 1 || info.resources.layers != 1) {
		return 0;
	}
	const auto range = TiledMipRange(info, 0);
	if (range.size == 0 || range.size % 4 != 0 || range.address != 0) {
		return 0;
	}
	const auto alignment = std::max<uint64_t>(256, std::bit_ceil(range.size));
	const auto in_block  = info.data.address & (info.tiling.block.block_size - 1u);
	return (info.data.address & (alignment - 1u)) == 0 &&
	               range.size <= info.tiling.block.block_size - in_block
	           ? static_cast<uint32_t>(range.size)
	           : 0;
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
	const bool auto_size = data.size == 0;
	TileTextureElementLayout element {};
	EXIT_NOT_IMPLEMENTED(!TileGetTextureElementLayout(guest_format, element));
	if (bytes_per_block == 0) {
		bytes_per_block = element.bytes;
	}
	EXIT_IF(bytes_per_block != element.bytes);
	tiling = {{.bytes_per_element = element.bytes}, element.texel_width, element.texel_height};
	tiled_slice_stride = 0;
	first_tail_level   = 16;

	if (samples > 1) {
		EXIT_NOT_IMPLEMENTED(IsVolume() || resources.levels != 1 ||
		                     (tile_mode != Prospero::TileMode::kDepth &&
		                      tile_mode != Prospero::TileMode::kRenderTarget));
		const auto fragments = std::countr_zero(samples);
		if (pitch == 0) {
			pitch = TileGetRenderTargetPitch(extent.width, bytes_per_block, fragments);
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
		tiled_slice_stride = IsTiled() ? slice : 0;
		mip_layout[0]      = {0, data.size, pitch, extent.height};
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
			mip_layout[level] = {offsets[level].offset,
			                     Multiply(offsets[level].size, MipExtent(level).depth),
			                     padded[level].width / element.texel_width,
			                     padded[level].height / element.texel_height};
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
		EXIT_NOT_IMPLEMENTED(!TileGetTiledTextureLayout(description, surface, mip_layout.data()));
		tiling             = surface.texture;
		first_tail_level   = surface.first_tail_level;
		tiled_slice_stride = surface.block_slice_size;
		if (data.size == 0) {
			data.size = surface.total_size;
		}
		if (!IsVolume() && resources.layers > 1 && data.size % resources.layers == 0) {
			tiled_slice_stride = std::max(tiled_slice_stride, data.size / resources.layers);
		}
		if (!IsVolume()) {
			for (uint32_t level = 0; level < resources.levels; ++level) {
				mip_layout[level].size = Multiply(mip_layout[level].size, resources.layers);
			}
		}
		const auto compact_size = CompactMipSize(*this);
		if (compact_size != 0 && (auto_size || data.size == compact_size)) {
			data.size = mip_layout[0].size = compact_size;
		}
	}
}

ImageLinearLayout ImageInfo::LinearLayout() const {
	ImageLinearLayout layout {};
	// Guest-linear and depth-plane slices hold every level at its guest offset and pitch.
	if (!IsTiled() || (IsDepthPlane(*this) && resources.levels == 1)) {
		for (uint32_t level = 0; level < resources.levels; ++level) {
			const auto  slices  = MipExtent(level).depth;
			const auto& mip     = mip_layout[level];
			const auto  size    = mip.size / slices;
			layout.mips[level]  = {mip.offset, size, mip.pitch};
			layout.slice_stride = std::max(layout.slice_stride, Add(mip.offset, size));
		}
		if (TransferLayers() > 1 && data.size % TransferLayers() == 0) {
			layout.slice_stride = std::max(layout.slice_stride, data.size / TransferLayers());
		}
		return layout;
	}
	const auto bytes     = tiling.block.bytes_per_element;
	const auto alignment = std::max(bytes, 4u);
	uint64_t   offset    = 0;
	uint32_t   mip_pitch = IsVolume() ? extent.width : pitch;
	for (uint32_t level = 0; level < resources.levels; ++level) {
		const auto active = MipExtent(level);
		const auto linear_pitch =
		    Common::AlignUp(mip_pitch, tiling.texel_width) / tiling.texel_width;
		const auto rows = Common::AlignUp(active.height, tiling.texel_height) / tiling.texel_height;
		const auto size = Multiply(Multiply(linear_pitch, rows), bytes);
		EXIT_IF(offset > UINT64_MAX - (alignment - 1u));
		offset             = Common::AlignUp<uint64_t>(offset, alignment);
		layout.mips[level] = {offset, size, linear_pitch};
		offset             = Add(offset, Multiply(size, active.depth));
		mip_pitch          = std::max(mip_pitch >> 1u, 1u);
	}
	return layout;
}

namespace {

bool MatchStoredMipChain(ImageInfo& info, uint32_t physical_levels, bool metadata,
                         uint32_t view_levels, uint32_t& base_level,
                         const vk::Extent3D* rebased_extent) {
	ImageInfo physical = info;
	if (rebased_extent != nullptr) {
		physical.extent = *rebased_extent;
	}
	physical.resources.levels = physical_levels;
	physical.UpdateSize();
	info.UpdateSize();
	const auto same_mip = [&](uint32_t target, uint32_t source) {
		return physical.mip_layout[target] == info.mip_layout[source] &&
		       physical.MipExtent(target) == info.MipExtent(source);
	};
	if (physical.first_tail_level == info.first_tail_level &&
	    physical.tiled_slice_stride == info.tiled_slice_stride &&
	    physical.data.size == info.data.size) {
		bool matches = true;
		for (uint32_t level = 0; level < physical_levels; ++level) {
			matches &= same_mip(level, level);
		}
		if (matches) {
			return true;
		}
	}
	if (metadata ||
	    (info.TransferLayers() > 1 && physical.tiled_slice_stride != info.tiled_slice_stride)) {
		return false;
	}
	// T# addresses the last mip. A view can select the same stored subresources
	// with different mip indices; inaccessible mips need no host representation.
	for (uint32_t base = 0; base + view_levels <= physical_levels; ++base) {
		bool matches = true;
		for (uint32_t i = 0; i < view_levels; ++i) {
			const auto source = base_level + i;
			const auto target = base + i;
			if (!same_mip(target, source) ||
			    (target >= physical.first_tail_level) != (source >= info.first_tail_level)) {
				matches = false;
				break;
			}
		}
		if (matches) {
			info       = std::move(physical);
			base_level = base;
			return true;
		}
	}
	return false;
}

} // namespace

bool ResolveTextureMipView(ImageInfo& info, uint32_t physical_levels, bool metadata,
                           uint32_t view_levels, uint32_t& base_level) {
	if (!info.IsTiled()) {
		return false;
	}
	// A view entirely past MAX_MIP may address its own chain starting at its base mip (PPSA02110
	// compresses mip 1 through a T# that declares one level). A render-target mip that is not
	// tiled as its own image still lies in the stored chain, which is matched instead.
	if (base_level >= physical_levels) {
		const ImageInfo saved = info;
		const auto      mip   = info.MipExtent(base_level);
		const vk::Extent3D extent {mip.width, mip.height, info.IsVolume() ? mip.depth : 1u};
		uint32_t           rebased_base = base_level;
		if (MatchStoredMipChain(info, view_levels, metadata, view_levels, rebased_base, &extent)) {
			base_level = rebased_base;
			return true;
		}
		info = saved;
	}
	return MatchStoredMipChain(info, physical_levels, metadata, view_levels, base_level, nullptr);
}

uint32_t ImageInfo::DataAlignment() const {
	if (!IsTiled()) {
		return 256;
	}
	const auto compact_size = CompactMipSize(*this);
	if (compact_size != 0 && data.size == compact_size && mip_layout[0].size == compact_size) {
		return std::max(256u, std::bit_ceil(compact_size));
	}
	return tiling.block.block_size;
}

ImageSubresource ImageInfo::SubresourceOf(const ImageInfo& container) const noexcept {
	if (!IsCompatible(container) || tile_mode != container.tile_mode || type != container.type ||
	    IsVolume() || resources.levels != 1 || resources.layers == 0 ||
	    container.resources.layers == 0 ||
	    container.resources.levels > container.mip_layout.size() || !data.Valid() ||
	    !container.data.Valid() || data.address < container.data.address ||
	    data.End() > container.data.End() ||
	    container.data.size % container.resources.layers != 0 ||
	    data.size % resources.layers != 0 || HasStencil() != container.HasStencil() ||
	    (HasStencil() && (stencil.address < container.stencil.address ||
	                      stencil.End() > container.stencil.End()))) {
		return {};
	}

	// PS5 block slices contain the entire reversed mip chain. A rebased tail
	// descriptor covers its addressed span within the shared tail block.
	const auto slice_stride  = container.data.size / container.resources.layers;
	const auto child_stride  = data.size / resources.layers;
	const auto address_delta = data.address - container.data.address;
	const auto layer         = address_delta / slice_stride;
	if ((resources.layers > 1 && child_stride != slice_stride) || layer > INT32_MAX) {
		return {};
	}
	const auto compact_size = CompactMipSize(*this);
	const bool compact =
	    compact_size != 0 && child_stride == compact_size && mip_layout[0].size == compact_size;
	// Tail levels share an offset, so the extent identifies the level.
	for (uint32_t level = 0; level < container.resources.levels; ++level) {
		const auto& layout = container.mip_layout[level];
		if (extent.width != std::max(container.extent.width >> level, 1u) ||
		    extent.height != std::max(container.extent.height >> level, 1u) ||
		    mip_layout[0].pitch != layout.pitch || mip_layout[0].height != layout.height ||
		    layout.size == 0 || layout.size % container.resources.layers != 0) {
			continue;
		}
		auto mip_offset = layout.offset;
		if (compact) {
			const auto range = TiledMipRange(container, level);
			if (range.size != compact_size) {
				continue;
			}
			mip_offset = range.address;
		} else if (mip_layout[0].tail_x != layout.tail_x || mip_layout[0].tail_y != layout.tail_y ||
		           child_stride != layout.size / container.resources.layers) {
			continue;
		}
		if (mip_offset != address_delta % slice_stride ||
		    child_stride > slice_stride - mip_offset) {
			continue;
		}
		return {static_cast<int32_t>(level), static_cast<int32_t>(layer)};
	}
	return {};
}

uint64_t ImageInfo::LinearSize(uint32_t levels) const {
	levels = levels == 0 ? resources.levels : levels;
	EXIT_IF(levels == 0 || levels > resources.levels || levels > mip_layout.size());
	const auto linear = LinearLayout();
	if (linear.slice_stride == 0) {
		const auto& mip = linear.mips[levels - 1u];
		return Add(mip.offset, Multiply(mip.size, MipExtent(levels - 1u).depth));
	}
	uint64_t size = 0;
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip    = linear.mips[level];
		const auto  slices = MipExtent(level).depth;
		const auto  last   = Add(mip.offset, Multiply(slices - 1u, linear.slice_stride));
		size               = std::max(size, Add(last, mip.size));
	}
	return size;
}

std::vector<vk::BufferImageCopy> ImageInfo::BufferCopies(uint64_t buffer_offset,
                                                         uint32_t levels) const {
	levels = levels == 0 ? resources.levels : levels;
	EXIT_IF(levels > resources.levels || levels > mip_layout.size() || tiling.texel_width == 0 ||
	        tiling.texel_height == 0);
	const auto                       aspect = DataAspect(pixel_format);
	const auto                       linear = LinearLayout();
	std::vector<vk::BufferImageCopy> copies;
	std::array<uint32_t, 16>         image_heights {};
	uint64_t                         count = 0;
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip    = linear.mips[level];
		const auto  active = MipExtent(level);
		if (linear.slice_stride == 0) {
			image_heights[level] = Common::AlignUp(active.height, tiling.texel_height);
		} else {
			const auto row_bytes = Multiply(mip.pitch, tiling.block.bytes_per_element);
			EXIT_IF(row_bytes == 0);
			const auto rows = linear.slice_stride / row_bytes;
			if (linear.slice_stride % row_bytes == 0 &&
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
		const auto&         mip    = linear.mips[level];
		const auto          active = MipExtent(level);
		vk::BufferImageCopy copy {};
		copy.bufferOffset      = Add(buffer_offset, mip.offset);
		copy.bufferRowLength   = mip.pitch * tiling.texel_width;
		copy.bufferImageHeight = image_heights[level];
		copy.imageSubresource  = {aspect, level, 0, 1};
		copy.imageExtent       = {active.width, active.height, 1};
		if (image_heights[level] == 0) {
			copy.bufferImageHeight = IsTiled() ? Common::AlignUp(active.height, tiling.texel_height)
			                                   : mip_layout[level].height * tiling.texel_height;
			for (uint32_t slice = 0; slice < active.depth; ++slice) {
				copy.bufferOffset =
				    Add(Add(buffer_offset, mip.offset), Multiply(slice, linear.slice_stride));
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

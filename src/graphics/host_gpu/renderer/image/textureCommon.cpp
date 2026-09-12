#include "graphics/host_gpu/renderer/image/textureCommon.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <numeric>

namespace Libs::Graphics {
namespace {

// Rows are channel order; columns are the number of physical components minus one. Unused
// selectors complete each entry to a permutation so logical write masks can be inverted.
constexpr Prospero::ColorComponentMapping kRenderTargetColorMappings[4][4] = {
    {Prospero::ColorMappingRgba, Prospero::ColorMappingRgba, Prospero::ColorMappingRgba,
     Prospero::ColorMappingRgba},
    {Prospero::ColorMappingGr, Prospero::ColorMappingRabg, Prospero::ColorMappingRgab,
     Prospero::ColorMappingBgra},
    {Prospero::ColorMappingBgra, Prospero::ColorMappingGr, Prospero::ColorMappingBgra,
     Prospero::ColorMappingAbgr},
    {Prospero::ColorMappingAgba, Prospero::ColorMappingArbg, Prospero::ColorMappingAgbr,
     Prospero::ColorMappingArgb},
};

struct HostFormatInfo {
	vk::Format                      format = vk::Format::eUndefined;
	Prospero::ColorComponentMapping host_to_storage;
};

HostFormatInfo ResolveHostFormat(Prospero::BufferFormat guest_format,
                                 Prospero::ChannelOrder order) {
	if (guest_format == Prospero::BufferFormat::k10_11_11Float) {
		return {vk::Format::eB10G11R11UfloatPack32, Prospero::ColorMappingBgra};
	}
	if (order == Prospero::ChannelOrder::kAlt) {
		switch (guest_format) {
			case Prospero::BufferFormat::k8_8_8_8UNorm:
				return {vk::Format::eB8G8R8A8Unorm, Prospero::ColorMappingBgra};
			case Prospero::BufferFormat::k8_8_8_8SNorm:
				return {vk::Format::eB8G8R8A8Snorm, Prospero::ColorMappingBgra};
			case Prospero::BufferFormat::k8_8_8_8Srgb:
				return {vk::Format::eB8G8R8A8Srgb, Prospero::ColorMappingBgra};
			case Prospero::BufferFormat::k10_10_10_2UNorm:
				return {vk::Format::eA2R10G10B10UnormPack32, Prospero::ColorMappingBgra};
			default: break;
		}
	}
	const auto format = VulkanFormat(guest_format);
	switch (guest_format) {
		case Prospero::BufferFormat::k5_5_5_1UNorm: return {format, Prospero::ColorMappingBgra};
		case Prospero::BufferFormat::k1_5_5_5UNorm:
		case Prospero::BufferFormat::k4_4_4_4UNorm: return {format, Prospero::ColorMappingAbgr};
		default: return {format, {}};
	}
}

} // namespace

RenderTargetFormatInfo TextureGetRenderTargetFormat(Prospero::ChannelLayout layout,
                                                    Prospero::ChannelType   type,
                                                    Prospero::ChannelOrder  order) {
	const auto encoding = Prospero::ResolveRenderTargetFormat(layout, type);
	if (encoding.IsValid() && encoding.SupportsOrder(order)) {
		const auto host_format = ResolveHostFormat(encoding.buffer_format, order);
		const auto bytes       = Prospero::RenderTargetBytesPerElement(encoding.buffer_format);
		if (host_format.format != vk::Format::eUndefined && bytes != 0) {
			const auto order_mapping =
			    kRenderTargetColorMappings[static_cast<size_t>(order)][encoding.components - 1u];
			return {host_format.format, bytes, host_format.host_to_storage.Then(order_mapping),
			        encoding.buffer_format};
		}
	}
	EXIT("unsupported render-target format combination: layout=%u type=%u order=%u\n",
	     static_cast<uint32_t>(layout), static_cast<uint32_t>(type), static_cast<uint32_t>(order));
}

vk::ComponentMapping TextureGetComponentMapping(uint32_t                        swizzle,
                                                Prospero::ColorComponentMapping host_to_storage) {
	constexpr std::array host_components {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG,
	                                      vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA};
	std::array<vk::ComponentSwizzle, 4> storage_components {};
	for (uint32_t host_component = 0; host_component < host_components.size(); ++host_component) {
		storage_components[host_to_storage.Map(host_component)] = host_components[host_component];
	}
	const auto resolve = [&](uint32_t component) {
		const auto selector = static_cast<Prospero::CompSwizzle>(GetDstSel(swizzle, component));
		switch (selector) {
			case Prospero::CompSwizzle::kZero: return vk::ComponentSwizzle::eZero;
			case Prospero::CompSwizzle::kOne: return vk::ComponentSwizzle::eOne;
			case Prospero::CompSwizzle::kRed: return storage_components[0];
			case Prospero::CompSwizzle::kGreen: return storage_components[1];
			case Prospero::CompSwizzle::kBlue: return storage_components[2];
			case Prospero::CompSwizzle::kAlpha: return storage_components[3];
			default: EXIT("unknown swizzle: %u\n", static_cast<uint32_t>(selector));
		}
	};
	return {resolve(0), resolve(1), resolve(2), resolve(3)};
}

SurfaceFormatInfo TextureGetSurfaceFormatInfo(Prospero::BufferFormat format) {
	const auto backing_format = Prospero::RemapTextureFormat(format);
	const auto host_format = ResolveHostFormat(backing_format, Prospero::ChannelOrder::kStandard);
	const auto conversion_format =
	    backing_format != format ? format : Prospero::BufferFormat::kInvalid;
	if (host_format.format != vk::Format::eUndefined) {
		return {host_format.format, conversion_format, host_format.host_to_storage};
	}
	EXIT("unknown format: fmt = %u\n", static_cast<uint32_t>(format));
}

bool TexturePlanDownloadChunks(std::span<const vk::BufferImageCopy> regions, uint64_t image_size,
                               const TextureDownloadBlock& block, uint64_t capacity,
                               uint64_t alignment, std::vector<TextureDownloadChunk>& out_chunks) {
	out_chunks.clear();
	if (regions.empty() || image_size == 0 || block.width == 0 || block.height == 0 ||
	    block.bytes == 0 || !std::has_single_bit(alignment)) {
		return false;
	}
	capacity = Common::AlignDown(capacity, alignment);
	if (capacity == 0) {
		return false;
	}

	struct Piece {
		vk::BufferImageCopy copy;
		uint64_t            size = 0;
	};
	std::vector<Piece> pieces;
	pieces.reserve(regions.size());
	for (const auto& region: regions) {
		const auto& extent    = region.imageExtent;
		const auto row_texels = region.bufferRowLength != 0 ? region.bufferRowLength : extent.width;
		if (extent.width == 0 || extent.height == 0 || extent.depth != 1 ||
		    row_texels < extent.width || region.imageOffset.y < 0 ||
		    static_cast<uint64_t>(region.imageOffset.y) + extent.height > INT32_MAX ||
		    (region.bufferImageHeight != 0 && region.bufferImageHeight < extent.height)) {
			return false;
		}
		const uint64_t row_bytes =
		    static_cast<uint64_t>((row_texels - 1u) / block.width + 1u) * block.bytes;
		const uint64_t rows = (static_cast<uint64_t>(extent.height) - 1u) / block.height + 1u;
		if (row_bytes > capacity || rows > image_size / row_bytes) {
			return false;
		}
		const uint64_t size = rows * row_bytes;
		if (region.bufferOffset > image_size - size || region.bufferOffset % alignment != 0 ||
		    size % alignment != 0) {
			return false;
		}
		const uint64_t rows_per_piece = capacity / row_bytes;
		if (rows <= rows_per_piece) {
			pieces.push_back({region, size});
			continue;
		}
		// Every piece boundary must stay on the chunk alignment.
		const uint64_t row_step   = alignment / std::gcd(row_bytes, alignment);
		const uint64_t piece_rows = Common::AlignDown(rows_per_piece, row_step);
		if (piece_rows == 0) {
			return false;
		}
		for (uint64_t row = 0; row < rows; row += piece_rows) {
			const uint64_t count = std::min(piece_rows, rows - row);
			const uint64_t top   = row * block.height;
			auto           piece = region;
			piece.bufferOffset += row * row_bytes;
			piece.imageOffset.y += static_cast<int32_t>(top);
			piece.imageExtent.height = static_cast<uint32_t>(
			    std::min<uint64_t>(count * block.height, extent.height - top));
			if (region.bufferImageHeight != 0) {
				piece.bufferImageHeight = static_cast<uint32_t>(count * block.height);
			}
			pieces.push_back({piece, count * row_bytes});
		}
	}

	std::ranges::stable_sort(pieces, {},
	                         [](const Piece& piece) { return piece.copy.bufferOffset; });
	for (size_t index = 1; index < pieces.size(); index++) {
		const auto& previous = pieces[index - 1];
		if (previous.copy.bufferOffset + previous.size > pieces[index].copy.bufferOffset) {
			return false;
		}
	}

	for (size_t first = 0; first < pieces.size();) {
		TextureDownloadChunk chunk;
		chunk.offset = pieces[first].copy.bufferOffset;
		for (; first < pieces.size(); first++) {
			const auto end = pieces[first].copy.bufferOffset + pieces[first].size;
			if (end - chunk.offset > capacity) {
				break;
			}
			auto copy = pieces[first].copy;
			copy.bufferOffset -= chunk.offset;
			chunk.regions.push_back(copy);
			chunk.size = end - chunk.offset;
		}
		out_chunks.push_back(std::move(chunk));
	}
	return true;
}

} // namespace Libs::Graphics

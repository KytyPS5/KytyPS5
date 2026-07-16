#include "graphics/host_gpu/objects/textureCommon.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/host_gpu/vma.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>

namespace Libs::Graphics {

RenderTargetFormatInfo TextureGetRenderTargetFormat(uint32_t raw_layout, uint32_t raw_type,
                                                    uint32_t raw_order) {
	const auto layout = static_cast<Prospero::ChannelLayout>(raw_layout);
	const auto type   = static_cast<Prospero::ChannelType>(raw_type);
	const auto order  = static_cast<Prospero::ChannelOrder>(raw_order);
	const auto is     = [=](Prospero::ChannelLayout l, Prospero::ChannelType t, Prospero::ChannelOrder o) {
		return layout == l && type == t && order == o;
	};

	if (layout == Prospero::ChannelLayout::k8 && type == Prospero::ChannelType::kUNorm &&
	    raw_order <= Prospero::GpuEnumValue(Prospero::ChannelOrder::kAltReversed)) {
		return {VK_FORMAT_R8_UNORM, 1};
	}
	if (is(Prospero::ChannelLayout::k8_8, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R8G8_UNORM, 2};
	}
	if (is(Prospero::ChannelLayout::k8_8_8_8, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R8G8B8A8_UNORM, 4};
	}
	if (is(Prospero::ChannelLayout::k8_8_8_8, Prospero::ChannelType::kSNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R8G8B8A8_SNORM, 4};
	}
	if (is(Prospero::ChannelLayout::k8_8_8_8, Prospero::ChannelType::kSrgb, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R8G8B8A8_SRGB, 4};
	}
	if (is(Prospero::ChannelLayout::k8_8_8_8, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kAlt)) {
		return {VK_FORMAT_B8G8R8A8_UNORM, 4};
	}
	if (is(Prospero::ChannelLayout::k8_8_8_8, Prospero::ChannelType::kSNorm, Prospero::ChannelOrder::kAlt)) {
		return {VK_FORMAT_B8G8R8A8_SNORM, 4};
	}
	if (is(Prospero::ChannelLayout::k8_8_8_8, Prospero::ChannelType::kSrgb, Prospero::ChannelOrder::kAlt)) {
		return {VK_FORMAT_B8G8R8A8_SRGB, 4};
	}
	if (is(Prospero::ChannelLayout::k5_5_5_1, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R5G5B5A1_UNORM_PACK16, 2};
	}
	if (is(Prospero::ChannelLayout::k4_4_4_4, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kReversed)) {
		return {VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2};
	}
	if (is(Prospero::ChannelLayout::k10_10_10_2, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4};
	}
	if (is(Prospero::ChannelLayout::k10_10_10_2, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kAlt)) {
		return {VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4};
	}
	if (is(Prospero::ChannelLayout::k11_11_10, Prospero::ChannelType::kFloat, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4};
	}
	if (is(Prospero::ChannelLayout::k16, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16_UNORM, 2};
	}
	if (is(Prospero::ChannelLayout::k16, Prospero::ChannelType::kUInt, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16_UINT, 2};
	}
	if (is(Prospero::ChannelLayout::k16, Prospero::ChannelType::kFloat, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16_SFLOAT, 2};
	}
	if (is(Prospero::ChannelLayout::k16_16, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16G16_UNORM, 4};
	}
	if (is(Prospero::ChannelLayout::k16_16, Prospero::ChannelType::kSNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16G16_SNORM, 4};
	}
	if (is(Prospero::ChannelLayout::k16_16, Prospero::ChannelType::kUInt, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16G16_UINT, 4};
	}
	if (is(Prospero::ChannelLayout::k16_16, Prospero::ChannelType::kFloat, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16G16_SFLOAT, 4};
	}
	if (is(Prospero::ChannelLayout::k16_16_16_16, Prospero::ChannelType::kUNorm, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16G16B16A16_UNORM, 8};
	}
	if (is(Prospero::ChannelLayout::k16_16_16_16, Prospero::ChannelType::kFloat, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
	}
	if (is(Prospero::ChannelLayout::k32, Prospero::ChannelType::kFloat, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R32_SFLOAT, 4};
	}
	if (is(Prospero::ChannelLayout::k32_32, Prospero::ChannelType::kUInt, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R32G32_UINT, 8};
	}
	if (is(Prospero::ChannelLayout::k32_32, Prospero::ChannelType::kFloat, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R32G32_SFLOAT, 8};
	}
	if (is(Prospero::ChannelLayout::k32_32_32_32, Prospero::ChannelType::kFloat, Prospero::ChannelOrder::kStandard)) {
		return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
	}
	EXIT("unsupported render-target format combination: layout=%u type=%u order=%u\n", raw_layout,
	     raw_type, raw_order);
}

static uint64_t TextureUploadLevelSrcOffset(const TileSizeOffset& level_size) {
	return (level_size.src_size != 0 ? level_size.src_offset : level_size.offset);
}

static uint64_t TextureUploadLevelSrcSize(const TileSizeOffset& level_size) {
	return (level_size.src_size != 0 ? level_size.src_size : level_size.size);
}

uint64_t TextureUploadSliceSourceOffset(const TextureUploadLayout& layout, uint32_t level,
                                        uint32_t                 slice,
                                        TextureUploadSliceLayout source_slice_layout) {
	if (level >= 16 || layout.level_sizes[level].size == 0) {
		EXIT("invalid texture upload slice source, level=%u slice=%u\n", level, slice);
	}
	const auto level_offset = TextureUploadLevelSrcOffset(layout.level_sizes[level]);
	const auto slice_stride =
	    source_slice_layout == TextureUploadSliceLayout::MipChainPerSlice
	        ? (layout.source_slice_stride != 0 ? layout.source_slice_stride : layout.slice_stride)
	        : TextureUploadLevelSrcSize(layout.level_sizes[level]);
	if (slice_stride != 0 && slice > (UINT64_MAX - level_offset) / slice_stride) {
		EXIT("texture upload slice source offset overflow, level=%u slice=%u\n", level, slice);
	}
	return level_offset + static_cast<uint64_t>(slice) * slice_stride;
}

uint64_t TextureCalcUploadSize(const TextureUploadLayout&          layout,
                               const std::vector<BufferImageCopy>& regions, uint64_t levels,
                               uint32_t depth, TextureUploadSliceLayout source_slice_layout) {
	uint64_t size = 0;

	for (const auto& r: regions) {
		size = std::max<uint64_t>(size, static_cast<uint64_t>(r.offset) +
		                                    layout.level_sizes[r.dst_level].size);
	}

	for (uint32_t level = 0; level < levels; level++) {
		const auto src_size = TextureUploadLevelSrcSize(layout.level_sizes[level]);
		for (uint32_t z = 0; z < depth; z++) {
			size = std::max<uint64_t>(
			    size,
			    TextureUploadSliceSourceOffset(layout, level, z, source_slice_layout) + src_size);
		}
	}

	return size;
}

void TextureCopyBufferBytes(GraphicContext* ctx, VulkanBuffer* src_buffer,
                            uint64_t src_buffer_offset, uint64_t copy_size,
                            UtilScratchBuffer* dst) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(src_buffer == nullptr);
	EXIT_IF(dst == nullptr);
	EXIT_IF(dst->Data() == nullptr);

	std::memset(dst->Data(), 0, copy_size);

	if (copy_size == 0 || src_buffer_offset >= src_buffer->buffer_size) {
		return;
	}

	const auto available =
	    std::min<uint64_t>(copy_size, src_buffer->buffer_size - src_buffer_offset);
	if (available == 0) {
		return;
	}

	if ((src_buffer->memory.property & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
		void* data = nullptr;
		VulkanMapMemory(ctx, &src_buffer->memory, &data);
		std::memcpy(dst->Data(), static_cast<const uint8_t*>(data) + src_buffer_offset, available);
		VulkanUnmapMemory(ctx, &src_buffer->memory);
		return;
	}

	VulkanBuffer readback {};
	readback.usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	readback.memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
	                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	VulkanCreateBuffer(ctx, (src_buffer_offset + available + 3u) & ~uint64_t {3}, &readback);
	UtilCopyBuffer(src_buffer, &readback, src_buffer_offset + available);

	void* data = nullptr;
	VulkanMapMemory(ctx, &readback.memory, &data);
	std::memcpy(dst->Data(), static_cast<const uint8_t*>(data) + src_buffer_offset, available);
	VulkanUnmapMemory(ctx, &readback.memory);

	VulkanDeleteBuffer(ctx, &readback);
}

VkComponentSwizzle TextureGetComponentSwizzle(uint8_t s) {
	switch (static_cast<Prospero::CompSwizzle>(s)) {
		case Prospero::CompSwizzle::kZero: return VK_COMPONENT_SWIZZLE_ZERO;
		case Prospero::CompSwizzle::kOne: return VK_COMPONENT_SWIZZLE_ONE;
		case Prospero::CompSwizzle::kRed: return VK_COMPONENT_SWIZZLE_R;
		case Prospero::CompSwizzle::kGreen: return VK_COMPONENT_SWIZZLE_G;
		case Prospero::CompSwizzle::kBlue: return VK_COMPONENT_SWIZZLE_B;
		case Prospero::CompSwizzle::kAlpha: return VK_COMPONENT_SWIZZLE_A;
		default: EXIT("unknown swizzle: %d\n", static_cast<int>(s));
	}
	return VK_COMPONENT_SWIZZLE_IDENTITY;
}

static uint32_t TextureGetDstSel(uint32_t swizzle, uint32_t channel) {
	return (swizzle >> (channel * 3u)) & 0x7u;
}

VkComponentMapping TextureGetComponentMapping(uint32_t swizzle) {
	VkComponentMapping components {};
	components.r = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 0)));
	components.g = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 1)));
	components.b = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 2)));
	components.a = TextureGetComponentSwizzle(static_cast<uint8_t>(TextureGetDstSel(swizzle, 3)));
	return components;
}

bool TextureCheckFormat(GraphicContext* ctx, VkImageCreateInfo* image_info) {
	VkImageFormatProperties props {};
	if (vkGetPhysicalDeviceImageFormatProperties(
	        ctx->physical_device, image_info->format, image_info->imageType, image_info->tiling,
	        image_info->usage, image_info->flags, &props) == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		auto apply_fallback = [&](VkFormat replacement, const char* message) {
			image_info->format = replacement;
			const bool result  = TextureCheckFormat(ctx, image_info);
			LOGF("%s [%s]\n", message, (!result ? "FAIL" : "SUCCESS"));
			return result;
		};

		if (image_info->format == VK_FORMAT_R8G8B8A8_SRGB) {
			// TODO() convert SRGB -> LINEAR in shader
			return apply_fallback(VK_FORMAT_R8G8B8A8_UNORM,
			                      "replace VK_FORMAT_R8G8B8A8_SRGB => VK_FORMAT_R8G8B8A8_UNORM");
		}
		if (image_info->format == VK_FORMAT_B8G8R8A8_SRGB) {
			// TODO() convert SRGB -> LINEAR in shader
			return apply_fallback(VK_FORMAT_B8G8R8A8_UNORM,
			                      "replace VK_FORMAT_B8G8R8A8_SRGB => VK_FORMAT_B8G8R8A8_UNORM");
		}
		return false;
	}
	return true;
}

static bool TextureCheckFormatExact(GraphicContext* ctx, const VkImageCreateInfo& image_info) {
	VkImageFormatProperties props {};
	return vkGetPhysicalDeviceImageFormatProperties(
	           ctx->physical_device, image_info.format, image_info.imageType, image_info.tiling,
	           image_info.usage, image_info.flags, &props) != VK_ERROR_FORMAT_NOT_SUPPORTED;
}

bool TextureCheckStorageSwizzle(VkImageCreateInfo* image_info, VkComponentMapping* components) {
	if ((image_info->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
		if (components->r == VK_COMPONENT_SWIZZLE_R && components->g == VK_COMPONENT_SWIZZLE_G &&
		    components->b == VK_COMPONENT_SWIZZLE_B && components->a == VK_COMPONENT_SWIZZLE_A) {
			return true;
		}

		if (components->r == VK_COMPONENT_SWIZZLE_B && components->g == VK_COMPONENT_SWIZZLE_G &&
		    components->b == VK_COMPONENT_SWIZZLE_R && components->a == VK_COMPONENT_SWIZZLE_A &&
		    image_info->format == VK_FORMAT_R8G8B8A8_SRGB) {
			LOGF("replace VK_FORMAT_R8G8B8A8_SRGB => VK_FORMAT_B8G8R8A8_SRGB\n");

			components->r      = VK_COMPONENT_SWIZZLE_R;
			components->g      = VK_COMPONENT_SWIZZLE_G;
			components->b      = VK_COMPONENT_SWIZZLE_B;
			components->a      = VK_COMPONENT_SWIZZLE_A;
			image_info->format = VK_FORMAT_B8G8R8A8_SRGB;
			return true;
		}

		// TODO() swizzle channels in shader

		return false;
	}
	return true;
}

VkImageUsageFlags TextureGetUsage(TextureFormatUsage usage) {
	VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (TextureHasFormatUsage(usage, TextureFormatUsage::Sampled)) {
		vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	if (TextureHasFormatUsage(usage, TextureFormatUsage::Storage)) {
		vk_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}
	return vk_usage;
}

VkImageUsageFlags TextureGetViewUsage(TextureFormatUsage usage) {
	VkImageUsageFlags vk_usage = 0;
	if (TextureHasFormatUsage(usage, TextureFormatUsage::Sampled)) {
		vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	if (TextureHasFormatUsage(usage, TextureFormatUsage::Storage)) {
		vk_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}
	return vk_usage;
}

VkFormat TextureGetFormat(uint32_t fmt, [[maybe_unused]] TextureFormatUsage usage) {
	const auto vk_format = Prospero::SurfaceFormat(fmt);
	if (vk_format != VK_FORMAT_UNDEFINED) {
		return vk_format;
	}
	EXIT("unknown format: fmt = %u\n", fmt);
	return VK_FORMAT_UNDEFINED;
}

static uint32_t AlignUpU32(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t ShiftCeilU32(uint32_t value, uint32_t shift) {
	return static_cast<uint32_t>((static_cast<uint64_t>(value) + (1ull << shift) - 1ull) >> shift);
}

struct Standard4KBVolumeMipLayout {
	uint32_t first_tail_level  = 0;
	uint32_t block_depth       = 1;
	uint64_t block_slice_size  = 0;
	uint64_t level_offsets[16] = {};
	uint64_t level_sizes[16]   = {};
};

static bool CalcStandard4kbVolumeMipLayout(uint32_t format, uint32_t pitch, uint32_t height,
                                           uint64_t levels, Standard4KBVolumeMipLayout* out) {
	EXIT_IF(out == nullptr);
	EXIT_NOT_IMPLEMENTED(levels == 0 || levels > 16);

	uint32_t bytes_per_element       = 0;
	uint32_t texels_per_element_wide = 0;
	uint32_t texels_per_element_tall = 0;
	uint32_t block_width_log2        = 0;
	uint32_t block_height_log2       = 0;
	uint32_t block_depth_log2        = 0;

	if (!TileGetStandard4KBVolumeLayout(format, &bytes_per_element, &texels_per_element_wide,
	                                    &texels_per_element_tall, &block_width_log2,
	                                    &block_height_log2, &block_depth_log2)) {
		return false;
	}

	const uint32_t row_elements0 =
	    std::max((pitch + texels_per_element_wide - 1u) / texels_per_element_wide, 1u);
	const uint32_t height_elements0 =
	    std::max((height + texels_per_element_tall - 1u) / texels_per_element_tall, 1u);
	const uint32_t     block_width       = 1u << block_width_log2;
	const uint32_t     block_height      = 1u << block_height_log2;
	const uint32_t     block_depth       = 1u << block_depth_log2;
	const uint32_t     tail_width_limit  = block_width;
	const uint32_t     tail_height_limit = block_height >> 1u;
	constexpr uint32_t max_tail_levels   = 5u;

	out->first_tail_level = static_cast<uint32_t>(levels);
	out->block_depth      = block_depth;
	out->block_slice_size = 0;

	for (uint32_t level = 0; level < levels; level++) {
		const uint32_t row_elements    = std::max(ShiftCeilU32(row_elements0, level), 1u);
		const uint32_t height_elements = std::max(ShiftCeilU32(height_elements0, level), 1u);

		out->level_offsets[level] = out->block_slice_size;
		if (row_elements <= tail_width_limit && height_elements <= tail_height_limit &&
		    levels - level <= max_tail_levels) {
			out->first_tail_level   = level;
			out->level_sizes[level] = 4096u;
			out->block_slice_size += 4096u;
			for (uint32_t tail_level = level + 1; tail_level < levels; tail_level++) {
				out->level_offsets[tail_level] = out->level_offsets[level];
				out->level_sizes[tail_level]   = 4096u;
			}
			break;
		}

		out->level_sizes[level] = static_cast<uint64_t>(block_depth) *
		                          AlignUpU32(row_elements, block_width) *
		                          AlignUpU32(height_elements, block_height) * bytes_per_element;
		out->block_slice_size += out->level_sizes[level];
	}

	return out->block_slice_size != 0;
}

uint32_t TextureGetAtlasSliceYStride(VkFormat format, uint32_t mip_height, uint32_t depth,
                                     uint64_t levels) {
	return (depth > 1 && levels > 1 && UtilIsBcFormat(format) ? AlignUpU32(mip_height, 4u)
	                                                          : mip_height);
}

uint32_t TextureCalcStackedImageHeight(VkFormat format, uint32_t height, uint32_t depth,
                                       uint64_t levels) {
	auto image_height = height * depth;
	if (depth <= 1 || levels <= 1 || !UtilIsBcFormat(format)) {
		return image_height;
	}

	uint32_t mip_height = height;
	for (uint32_t level = 0; level < levels; level++) {
		const auto mip_image_height =
		    TextureGetAtlasSliceYStride(format, mip_height, depth, levels) * depth;
		image_height = std::max<uint32_t>(image_height, mip_image_height << level);
		if (mip_height > 1) {
			mip_height /= 2;
		}
	}

	return image_height;
}

uint32_t TextureCalcMipmapAtlasImageHeight(VkFormat format, uint32_t width, uint32_t height,
                                           uint32_t depth, uint64_t levels) {
	auto image_height = height * depth;
	if (levels <= 1) {
		return image_height;
	}

	uint32_t mip_height = height;
	for (uint32_t level = 0; level < levels; level++) {
		const auto mipmap_offset = UtilCalcMipmapOffset(level, width, height);
		const auto mip_bottom =
		    static_cast<uint32_t>(mipmap_offset.second) +
		    TextureGetAtlasSliceYStride(format, mip_height, depth, levels) * depth;
		image_height = std::max<uint32_t>(image_height, mip_bottom);
		if (mip_height > 1) {
			mip_height /= 2;
		}
	}

	return image_height;
}

bool TextureIs3DTexture(uint64_t type) {
	return static_cast<Prospero::ImageType>(type) == Prospero::ImageType::kColor3D;
}

bool TextureIsCubeTexture(uint64_t type) {
	return static_cast<Prospero::ImageType>(type) == Prospero::ImageType::kCube;
}

bool TextureIsLayeredTexture(uint64_t type) {
	const auto image_type = static_cast<Prospero::ImageType>(type);
	return image_type == Prospero::ImageType::kCube || image_type == Prospero::ImageType::kColor1DArray ||
	       image_type == Prospero::ImageType::kColor2DArray ||
	       image_type == Prospero::ImageType::kColor2DMsaaArray;
}

bool TextureCanCreateCubeView(uint64_t type, uint32_t base_array, uint32_t layer_count) {
	return TextureIsCubeTexture(type) && base_array % 6u == 0 && layer_count >= 6 &&
	       layer_count % 6u == 0;
}

VkComponentMapping TextureCreateImage(GraphicContext* ctx, VulkanImage* vk_obj, VulkanMemory* mem,
                                      const TextureImageCreateParams& params) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(params.owner == nullptr);

	const bool array_texture  = TextureIsLayeredTexture(params.type);
	const bool volume_texture = TextureIs3DTexture(params.type);

	auto pixel_format = TextureGetFormat(params.fmt, params.format_usage);
	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(params.width == 0);
	EXIT_NOT_IMPLEMENTED(params.height == 0);
	EXIT_NOT_IMPLEMENTED(params.levels == 0 || params.levels > 16);

	uint32_t image_height = 0;
	uint32_t image_mips   = 0;
	if (params.image_layout == TextureUploadDestination::MipAtlas) {
		EXIT_NOT_IMPLEMENTED(params.base_level != 0);
		const auto atlas_depth = (array_texture || volume_texture ? 1u : params.depth);
		image_height           = TextureCalcMipmapAtlasImageHeight(
		    pixel_format, static_cast<uint32_t>(params.width), static_cast<uint32_t>(params.height),
		    atlas_depth, params.levels);
		image_mips = 1;
	} else {
		image_height =
		    (array_texture || volume_texture
		         ? static_cast<uint32_t>(params.height)
		         : TextureCalcStackedImageHeight(pixel_format, static_cast<uint32_t>(params.height),
		                                         params.depth, params.levels));
		image_mips = static_cast<uint32_t>(params.levels);
	}

	VkComponentMapping components =
	    TextureGetComponentMapping(static_cast<uint32_t>(params.swizzle));

	VkImageCreateInfo image_info {};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext = nullptr;
	image_info.flags =
	    (params.allow_cube_view && TextureCanCreateCubeView(params.type, 0, params.depth)
	         ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
	         : 0) |
	    (volume_texture ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0) |
	    (params.compatible_format_views ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
	image_info.imageType     = (volume_texture ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D);
	image_info.extent.width  = static_cast<uint32_t>(params.width);
	image_info.extent.height = image_height;
	image_info.extent.depth  = (volume_texture ? params.depth : 1);
	image_info.mipLevels     = image_mips;
	image_info.arrayLayers   = (array_texture ? params.depth : 1);
	image_info.format        = pixel_format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_info.usage         = TextureGetUsage(params.format_usage);
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	const bool storage_view = TextureHasFormatUsage(params.view_usage, TextureFormatUsage::Storage);
	if (storage_view && !TextureCheckStorageSwizzle(&image_info, &components)) {
		if (!params.storage_swizzle_fallback) {
			EXIT("swizzle is not supported");
		}

		static std::atomic_uint log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("\t %s swizzle 0x%08" PRIx64 " is not supported, using identity mapping\n",
			     params.owner, params.swizzle);
		}
		components.r = VK_COMPONENT_SWIZZLE_R;
		components.g = VK_COMPONENT_SWIZZLE_G;
		components.b = VK_COMPONENT_SWIZZLE_B;
		components.a = VK_COMPONENT_SWIZZLE_A;
	}

	const auto view_checked_format = image_info.format;
	const auto requested_usage     = params.format_usage;
	const auto required_usage      = params.required_format_usage;
	const bool has_optional_usage  = static_cast<uint32_t>(requested_usage & ~required_usage) != 0;
	if (!TextureCheckFormatExact(ctx, image_info) && has_optional_usage) {
		auto required_info   = image_info;
		required_info.usage  = TextureGetUsage(required_usage);
		required_info.format = view_checked_format;
		if (TextureCheckFormatExact(ctx, required_info)) {
			static std::atomic_uint log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("\t %s usage 0x%08x is not supported for format %d, using required "
				     "usage 0x%08x\n",
				     params.owner, static_cast<uint32_t>(TextureGetUsage(requested_usage)),
				     static_cast<int>(image_info.format),
				     static_cast<uint32_t>(TextureGetUsage(required_usage)));
			}
			image_info = required_info;
		}
	}

	if (!TextureCheckFormat(ctx, &image_info)) {
		if (has_optional_usage) {
			image_info.format = view_checked_format;
			image_info.usage  = TextureGetUsage(required_usage);
		}
		if (!has_optional_usage || !TextureCheckFormat(ctx, &image_info)) {
			EXIT("format is not supported");
		}
	}

	vk_obj->extent.width  = image_info.extent.width;
	vk_obj->extent.height = image_info.extent.height;
	vk_obj->layers        = image_info.arrayLayers;
	vk_obj->mip_levels    = image_info.mipLevels;
	vk_obj->format        = image_info.format;
	vk_obj->image         = nullptr;
	vk_obj->layout        = image_info.initialLayout;

	UtilResetImageViews(vk_obj);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool created = VulkanCreateImage(ctx, &image_info, vk_obj, mem);
	EXIT_NOT_IMPLEMENTED(!created);

	vk_obj->memory = *mem;

	return components;
}

void TextureCreateImageViews(GraphicContext* ctx, VulkanImage* vk_obj,
                             VkComponentMapping components, uint64_t type, uint32_t base_array,
                             uint32_t base_level, uint32_t level_count, uint32_t depth,
                             bool allow_cube_view, TextureFormatUsage view_usage) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(level_count == 0 || base_level + level_count > vk_obj->mip_levels);

	const bool layered_texture = TextureIsLayeredTexture(type);
	const bool volume_texture  = TextureIs3DTexture(type);
	const auto layer_count     = (layered_texture && base_array < depth ? depth - base_array : 1u);
	const auto volume_slices   = std::max(depth >> base_level, 1u);

	VkImageViewUsageCreateInfo usage_info {};
	usage_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	usage_info.pNext = nullptr;
	usage_info.usage = TextureGetViewUsage(view_usage);

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = (usage_info.usage != 0 ? &usage_info : nullptr);
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components                      = components;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = (layered_texture ? base_array : 0);
	create_info.subresourceRange.baseMipLevel   = base_level;
	create_info.subresourceRange.layerCount     = layer_count;
	create_info.subresourceRange.levelCount     = level_count;
	if (volume_texture) {
		create_info.viewType                        = VK_IMAGE_VIEW_TYPE_3D;
		create_info.subresourceRange.baseArrayLayer = 0;
		create_info.subresourceRange.layerCount     = 1;
	} else if (allow_cube_view && TextureCanCreateCubeView(type, base_array, layer_count)) {
		create_info.viewType =
		    (layer_count > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE);
	} else if (layered_texture) {
		create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	}

	vkCreateImageView(ctx->device, &create_info, nullptr,
	                  &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);
	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	if (!volume_texture || depth >= 1) {
		create_info.viewType                    = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		create_info.subresourceRange.layerCount = (volume_texture ? volume_slices : layer_count);
		vkCreateImageView(ctx->device, &create_info, nullptr,
		                  &vk_obj->image_view[VulkanImage::VIEW_DEFAULT_ARRAY]);
		EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT_ARRAY] == nullptr);
	}
}

static uint64_t CalcTextureSliceStride(const TileSizeOffset* level_sizes, uint64_t levels,
                                       uint64_t total_size, uint32_t depth) {
	uint64_t stride = 0;
	for (uint32_t i = 0; i < levels; i++) {
		stride =
		    std::max(stride, static_cast<uint64_t>(level_sizes[i].offset) + level_sizes[i].size);
	}

	if (depth > 1 && total_size != 0 && total_size % depth == 0) {
		const auto guest_stride = total_size / depth;
		if (guest_stride >= stride) {
			stride = guest_stride;
		}
	}

	return stride;
}

static uint64_t CalcLinearUploadLevelSize(uint32_t fmt, uint32_t pitch, uint32_t height) {
	if (const uint32_t bytes_per_element = Prospero::NumBytesPerElement(fmt); bytes_per_element != 0) {
		return static_cast<uint64_t>(pitch) * height * bytes_per_element;
	}

	if (const uint32_t bytes_per_block = Prospero::BlockCompressedBytesPerBlock(fmt); bytes_per_block != 0) {
		const uint32_t blocks_w = std::max((pitch + 3u) / 4u, 1u);
		const uint32_t blocks_h = std::max((height + 3u) / 4u, 1u);
		return static_cast<uint64_t>(blocks_w) * blocks_h * bytes_per_block;
	}

	return 0;
}

static uint64_t FillVolumeLinearUploadLevels(TileSizeOffset* level_sizes, uint32_t fmt,
                                             uint64_t height, uint64_t levels,
                                             uint32_t base_pitch) {
	uint64_t offset = 0;
	auto     pitch  = base_pitch;
	auto     h      = static_cast<uint32_t>(height);

	for (uint32_t i = 0; i < levels; i++) {
		const auto size = CalcLinearUploadLevelSize(fmt, pitch, h);
		EXIT_NOT_IMPLEMENTED(size > 0xffffffffull);
		EXIT_NOT_IMPLEMENTED(offset > 0xffffffffull);

		level_sizes[i].size       = static_cast<uint32_t>(size);
		level_sizes[i].offset     = static_cast<uint32_t>(offset);
		level_sizes[i].src_size   = 0;
		level_sizes[i].src_offset = 0;
		level_sizes[i].x          = 0;
		level_sizes[i].y          = 0;

		offset += size;
		if (pitch > 1) {
			pitch /= 2;
		}
		if (h > 1) {
			h /= 2;
		}
	}

	return offset;
}

TextureUploadLayout TextureCalcUploadLayout(uint32_t fmt, uint64_t width, uint64_t height,
                                            uint64_t levels, uint32_t depth, uint64_t pitch,
                                            uint64_t tile, uint64_t upload_size,
                                            bool allow_depth_tile,
                                            bool require_single_mip_small_tiles,
                                            bool volume_texture, const char* owner) {
	TextureUploadLayout layout {};
	layout.tile           = static_cast<uint32_t>(tile);
	layout.pitch          = static_cast<uint32_t>(pitch);
	layout.volume_texture = volume_texture;

	if (fmt != 0) {
		if (layout.tile != 0) {
			const auto tile_mode = static_cast<Prospero::TileMode>(layout.tile);
			layout.fmt_tiled_render_target =
			    (Prospero::RenderTargetBytesPerElement(static_cast<uint32_t>(fmt)) != 0 &&
			     tile_mode == Prospero::TileMode::kRenderTarget);
			layout.fmt_tiled_standard256b =
			    (TileIsStandard256BTextureSupported(static_cast<uint32_t>(fmt)) &&
			     tile_mode == Prospero::TileMode::kStandard256B &&
			     (!require_single_mip_small_tiles || levels == 1));
			layout.fmt_tiled_standard4kb =
			    (TileIsStandard4KBTextureSupported(static_cast<uint32_t>(fmt)) &&
			     tile_mode == Prospero::TileMode::kStandard4KB &&
			     (!require_single_mip_small_tiles || levels == 1));
			layout.fmt_tiled_standard64kb =
			    (TileIsStandard64KBTextureSupported(static_cast<uint32_t>(fmt)) &&
			     tile_mode == Prospero::TileMode::kStandard64KB);
			layout.fmt_tiled_depth =
			    (allow_depth_tile && Prospero::RenderTargetBytesPerElement(static_cast<uint32_t>(fmt)) != 0 &&
			     tile_mode == Prospero::TileMode::kDepth);
			if (!layout.fmt_tiled_render_target && !layout.fmt_tiled_standard256b &&
			    !layout.fmt_tiled_standard4kb && !layout.fmt_tiled_standard64kb &&
			    !layout.fmt_tiled_depth) {
				EXIT("%s: unsupported typed tiled upload, using linear fallback: fmt=%u tile=%u "
				     "size=%" PRIu64 " extent=%" PRIu64 "x%" PRIu64 " pitch=%" PRIu64
				     " levels=%" PRIu64 "\n",
				     owner, static_cast<uint32_t>(fmt), layout.tile, upload_size, width, height,
				     pitch, levels);
				layout.tile = 0;
			}
		}

		layout.pitch = TileGetTexturePitch(fmt, width, levels, layout.tile);

		TileGetTextureSize(fmt, width, height, layout.pitch, levels, layout.tile, nullptr,
		                   layout.level_sizes, layout.padded_sizes);

		if (layout.volume_texture) {
			TileSizeOffset source_levels[16] {};
			std::copy_n(layout.level_sizes, levels, source_levels);
			layout.slice_stride =
			    FillVolumeLinearUploadLevels(layout.level_sizes, fmt, height, levels, layout.pitch);
			if (layout.fmt_tiled_render_target) {
				layout.source_slice_stride =
				    CalcTextureSliceStride(source_levels, levels, upload_size, depth);
				for (uint32_t level = 0; level < levels; level++) {
					layout.level_sizes[level].src_offset = source_levels[level].offset;
					layout.level_sizes[level].src_size   = source_levels[level].size;
					layout.level_sizes[level].x          = source_levels[level].x;
					layout.level_sizes[level].y          = source_levels[level].y;
				}
			}
		}

		if (layout.fmt_tiled_depth) {
			for (uint32_t i = 0; i < levels; i++) {
				layout.level_sizes[i].x = layout.padded_sizes[i].width;
				layout.level_sizes[i].y = layout.padded_sizes[i].height;
			}
		}
	} else {
		EXIT("%s: legacy texture upload format unsupported: fmt=0 tile=%u size=%" PRIu64
		     " extent=%" PRIu64 "x%" PRIu64 " pitch=%" PRIu64 " levels=%" PRIu64 "\n",
		     owner, layout.tile, upload_size, width, height, pitch, levels);
	}

	if (!layout.volume_texture) {
		layout.slice_stride =
		    CalcTextureSliceStride(layout.level_sizes, levels, upload_size, depth);
	}
	return layout;
}

std::vector<BufferImageCopy>
TextureBuildUploadRegions(const TextureUploadLayout& layout, VkFormat image_format, uint32_t width,
                          uint32_t height, uint32_t depth, uint64_t levels, bool array_texture,
                          bool volume_texture, TextureUploadDestination destination,
                          TextureUploadSliceLayout slice_layout) {
	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = layout.pitch;

	std::vector<BufferImageCopy> regions(levels * depth);
	for (uint32_t i = 0; i < levels; i++) {
		EXIT_NOT_IMPLEMENTED(layout.level_sizes[i].size == 0);

		const auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

		for (uint32_t z = 0; z < depth; z++) {
			const auto region_index = i * depth + z;
			const auto slice_offset = (slice_layout == TextureUploadSliceLayout::MipChainPerSlice
			                               ? z * layout.slice_stride
			                               : z * static_cast<uint64_t>(layout.level_sizes[i].size));

			regions[region_index].offset =
			    static_cast<uint32_t>(layout.level_sizes[i].offset + slice_offset);
			regions[region_index].width  = mip_width;
			regions[region_index].height = mip_height;
			regions[region_index].copy_height =
			    (!array_texture && !volume_texture && depth > 1 && levels > 1 &&
			             UtilIsBcFormat(image_format)
			         ? TextureGetAtlasSliceYStride(image_format, mip_height, depth, levels)
			         : 0);
			regions[region_index].dst_layer = (array_texture ? z : 0);
			regions[region_index].dst_z     = (volume_texture ? static_cast<int>(z) : 0);
			if (layout.fmt_tiled_depth && layout.level_sizes[i].x != 0) {
				regions[region_index].pitch = layout.level_sizes[i].x;
			} else if (!layout.volume_texture &&
			           static_cast<Prospero::TileMode>(layout.tile) == Prospero::TileMode::kLinear &&
			           layout.padded_sizes[i].width != 0) {
				regions[region_index].pitch = layout.padded_sizes[i].width;
			} else {
				regions[region_index].pitch = mip_pitch;
			}

			if (destination == TextureUploadDestination::MipLevels) {
				regions[region_index].dst_level = i;
				regions[region_index].dst_x     = 0;
				regions[region_index].dst_y =
				    (array_texture || volume_texture
				         ? 0
				         : static_cast<int>(z * TextureGetAtlasSliceYStride(
				                                    image_format, mip_height, depth, levels)));
			} else {
				regions[region_index].dst_level = 0;
				regions[region_index].dst_x     = mipmap_offset.first;
				regions[region_index].dst_y =
				    (array_texture || volume_texture
				         ? mipmap_offset.second
				         : mipmap_offset.second +
				               static_cast<int>(z * TextureGetAtlasSliceYStride(
				                                        image_format, mip_height, depth, levels)));
			}
		}

		if (mip_width > 1) {
			mip_width /= 2;
		}
		if (mip_height > 1) {
			mip_height /= 2;
		}
		if (mip_pitch > 1) {
			mip_pitch /= 2;
		}
	}

	return regions;
}

static uint64_t GetLevelSrcOffset(const TileSizeOffset& level_size) {
	return (level_size.src_size != 0 ? level_size.src_offset : level_size.offset);
}

static uint64_t GetLevelSrcSize(const TileSizeOffset& level_size) {
	return (level_size.src_size != 0 ? level_size.src_size : level_size.size);
}

static uint64_t GetSliceSrcStride(const TextureUploadLayout& layout, uint32_t level,
                                  TextureUploadSliceLayout source_slice_layout) {
	return (
	    source_slice_layout == TextureUploadSliceLayout::MipChainPerSlice
	        ? (layout.source_slice_stride != 0 ? layout.source_slice_stride : layout.slice_stride)
	        : GetLevelSrcSize(layout.level_sizes[level]));
}

static uint64_t FmaskRegionCopySize(const BufferImageCopy& region) {
	const uint64_t row_length = (region.pitch != 0 ? region.pitch : region.width);
	if (region.height == 0 || region.width == 0) {
		return 0;
	}
	return ((static_cast<uint64_t>(region.height - 1u) * row_length) + region.width) *
	       sizeof(uint32_t);
}

static void UploadFmaskIdentity(GraphicContext* ctx, VulkanImage* vk_obj,
                                const std::vector<BufferImageCopy>& regions, uint64_t dst_layout,
                                const char* owner) {
	constexpr uint32_t kIdentityFmaskPattern = 0x76543210u;

	std::vector<BufferImageCopy> upload_regions = regions;
	uint64_t                     upload_size    = 0;
	for (auto& region: upload_regions) {
		upload_size = (upload_size + 255u) & ~uint64_t {255};
		EXIT_NOT_IMPLEMENTED(upload_size > UINT32_MAX);
		region.offset = static_cast<uint32_t>(upload_size);
		upload_size += FmaskRegionCopySize(region);
	}

	LOGF("%s: temporary: decoding PS5 FMASK8_S4_F4 as identity pattern 0x%08" PRIx32
	     ", upload_size=%" PRIu64 " regions=%zu\n",
	     owner, kIdentityFmaskPattern, upload_size, upload_regions.size());

	UtilScratchBuffer temp_buf(upload_size);
	auto*             words = static_cast<uint32_t*>(temp_buf.Data());
	for (uint64_t i = 0; i < upload_size / sizeof(uint32_t); i++) {
		words[i] = kIdentityFmaskPattern;
	}

	UtilFillImage(ctx, vk_obj, temp_buf.Data(), upload_size, upload_regions, dst_layout);
}

void TextureUploadGuestImage(GraphicContext* ctx, VulkanImage* vk_obj, const void* src_data,
                             uint64_t size, const std::vector<BufferImageCopy>& regions,
                             const TextureUploadLayout& layout, uint32_t fmt, uint64_t width,
                             uint64_t height, uint32_t depth, uint64_t levels,
                             TextureUploadSliceLayout source_slice_layout, const char* owner,
                             uint64_t dst_layout) {
	if (fmt == 0) {
		EXIT("%s: legacy texture upload format unsupported: fmt=0 tile=%u size=%" PRIu64
		     " extent=%" PRIu64 "x%" PRIu64 " depth=%u pitch=%u levels=%" PRIu64 "\n",
		     owner, layout.tile, size, width, height, depth, layout.pitch, levels);
	} else if (static_cast<Prospero::TileMode>(layout.tile) == Prospero::TileMode::kLinear) {
		UtilFillImage(ctx, vk_obj, src_data, size, regions, dst_layout);
	} else if (layout.fmt_tiled_render_target) {
		LOGF("%s: detiling typed render-target texture: fmt=%u tile=%u size=%" PRIu64
		     " extent=%" PRIu64 "x%" PRIu64 " depth=%" PRIu64 " pitch=%u levels=%" PRIu64 "\n",
		     owner, static_cast<uint32_t>(fmt), layout.tile, size, width, height,
		     static_cast<uint64_t>(depth), layout.pitch, levels);
		UtilScratchBuffer temp_buf(size);
		for (uint32_t i = 0; i < levels; i++) {
			for (uint32_t z = 0; z < depth; z++) {
				const auto region_index = i * depth + z;
				auto* dst = static_cast<uint8_t*>(temp_buf.Data()) + regions[region_index].offset;
				const auto* src = static_cast<const uint8_t*>(src_data) +
				                  TextureUploadSliceSourceOffset(layout, i, z, source_slice_layout);
				TileConvertTiledToLinearRenderTarget(
				    dst, src, regions[region_index].width, regions[region_index].height,
				    regions[region_index].pitch,
				    Prospero::RenderTargetBytesPerElement(static_cast<uint32_t>(fmt)),
				    layout.level_sizes[i].size, GetLevelSrcSize(layout.level_sizes[i]),
				    layout.level_sizes[i].x, layout.level_sizes[i].y);
			}
		}
		UtilFillImage(ctx, vk_obj, temp_buf.Data(), size, regions, dst_layout);
	} else if (layout.fmt_tiled_depth) {
		if (Prospero::IsFmaskTextureFormat(static_cast<uint32_t>(fmt))) {
			UploadFmaskIdentity(ctx, vk_obj, regions, dst_layout, owner);
			return;
		}
		const uint32_t bytes_per_element = Prospero::RenderTargetBytesPerElement(static_cast<uint32_t>(fmt));
		if (bytes_per_element == 1) {
			UtilFillImage(ctx, vk_obj, src_data, size, regions, dst_layout);
		} else {
			LOGF("%s: detiling typed depth texture: fmt=%u tile=%u size=%" PRIu64 " extent=%" PRIu64
			     "x%" PRIu64 " depth=%" PRIu64 " pitch=%u levels=%" PRIu64 "\n",
			     owner, static_cast<uint32_t>(fmt), layout.tile, size, width, height,
			     static_cast<uint64_t>(depth), layout.pitch, levels);
			UtilScratchBuffer temp_buf(size);
			for (uint32_t i = 0; i < levels; i++) {
				for (uint32_t z = 0; z < depth; z++) {
					const auto region_index = i * depth + z;
					auto*      dst =
					    static_cast<uint8_t*>(temp_buf.Data()) + regions[region_index].offset;
					const auto* src = static_cast<const uint8_t*>(src_data) +
					                  GetLevelSrcOffset(layout.level_sizes[i]) +
					                  z * GetSliceSrcStride(layout, i, source_slice_layout);
					TileConvertTiledToLinearDepth(
					    dst, src, static_cast<uint32_t>(fmt), regions[region_index].width,
					    regions[region_index].height, regions[region_index].pitch,
					    layout.level_sizes[i].size);
				}
			}
			UtilFillImage(ctx, vk_obj, temp_buf.Data(), size, regions, dst_layout);
		}
	} else if (layout.fmt_tiled_standard256b) {
		UtilScratchBuffer temp_buf(size);
		for (uint32_t i = 0; i < levels; i++) {
			for (uint32_t z = 0; z < depth; z++) {
				const auto region_index = i * depth + z;
				auto* dst = static_cast<uint8_t*>(temp_buf.Data()) + regions[region_index].offset;
				const auto* src = static_cast<const uint8_t*>(src_data) +
				                  layout.level_sizes[i].offset +
				                  z * GetSliceSrcStride(layout, i, source_slice_layout);
				TileConvertTiledToLinearStandard256B(
				    dst, src, static_cast<uint32_t>(fmt), regions[region_index].width,
				    regions[region_index].height, regions[region_index].pitch,
				    layout.level_sizes[i].size, layout.level_sizes[i].size);
			}
		}
		UtilFillImage(ctx, vk_obj, temp_buf.Data(), size, regions, dst_layout);
	} else if (layout.fmt_tiled_standard4kb && layout.volume_texture) {
		LOGF("%s: detiling typed Standard4KB 3D texture: fmt=%u tile=%u size=%" PRIu64
		     " extent=%" PRIu64 "x%" PRIu64 " depth=%" PRIu64 " pitch=%u levels=%" PRIu64 "\n",
		     owner, static_cast<uint32_t>(fmt), layout.tile, size, width, height,
		     static_cast<uint64_t>(depth), layout.pitch, levels);
		UtilScratchBuffer temp_buf(size);
		if (levels == 1) {
			TileConvertTiledToLinearStandard4KB3D(
			    temp_buf.Data(), src_data, static_cast<uint32_t>(fmt), static_cast<uint32_t>(width),
			    static_cast<uint32_t>(height), depth, layout.pitch, layout.slice_stride, size,
			    size);
		} else {
			std::memset(temp_buf.Data(), 0, static_cast<size_t>(size));

			Standard4KBVolumeMipLayout volume_layout {};
			EXIT_NOT_IMPLEMENTED(!CalcStandard4kbVolumeMipLayout(
			    static_cast<uint32_t>(fmt), layout.pitch, static_cast<uint32_t>(height), levels,
			    &volume_layout));

			auto*       dst_base   = static_cast<uint8_t*>(temp_buf.Data());
			const auto* src_base   = static_cast<const uint8_t*>(src_data);
			uint32_t    mip_width  = static_cast<uint32_t>(width);
			uint32_t    mip_height = static_cast<uint32_t>(height);
			uint32_t    mip_pitch  = layout.pitch;

			for (uint32_t level = 0; level < levels; level++) {
				if (level < volume_layout.first_tail_level) {
					for (uint32_t z = 0; z < depth; z += volume_layout.block_depth) {
						const uint32_t copy_depth = std::min(volume_layout.block_depth, depth - z);
						const auto     region_index = level * depth + z;
						auto*          dst          = dst_base + regions[region_index].offset;
						const auto*    src = src_base +
						                     (static_cast<uint64_t>(z / volume_layout.block_depth) *
						                      volume_layout.block_slice_size) +
						                     volume_layout.level_offsets[level];
						const uint64_t dst_size =
						    (static_cast<uint64_t>(copy_depth - 1u) * layout.slice_stride) +
						    layout.level_sizes[level].size;
						TileConvertTiledToLinearStandard4KB3D(
						    dst, src, static_cast<uint32_t>(fmt), mip_width, mip_height, copy_depth,
						    mip_pitch, layout.slice_stride, dst_size,
						    volume_layout.level_sizes[level], false);
					}
				}

				if (mip_width > 1) {
					mip_width /= 2;
				}
				if (mip_height > 1) {
					mip_height /= 2;
				}
				if (mip_pitch > 1) {
					mip_pitch /= 2;
				}
			}
		}
		UtilFillImage(ctx, vk_obj, temp_buf.Data(), size, regions, dst_layout);
	} else if (layout.fmt_tiled_standard4kb) {
		LOGF("%s: detiling typed Standard4KB texture: fmt=%u tile=%u size=%" PRIu64
		     " extent=%" PRIu64 "x%" PRIu64 " depth=%" PRIu64 " pitch=%u\n",
		     owner, static_cast<uint32_t>(fmt), layout.tile, size, width, height,
		     static_cast<uint64_t>(depth), layout.pitch);
		UtilScratchBuffer temp_buf(size);
		for (uint32_t i = 0; i < levels; i++) {
			for (uint32_t z = 0; z < depth; z++) {
				const auto region_index = i * depth + z;
				auto* dst = static_cast<uint8_t*>(temp_buf.Data()) + regions[region_index].offset;
				const auto* src = static_cast<const uint8_t*>(src_data) +
				                  GetLevelSrcOffset(layout.level_sizes[i]) +
				                  z * GetSliceSrcStride(layout, i, source_slice_layout);
				TileConvertTiledToLinearStandard4KB(
				    dst, src, static_cast<uint32_t>(fmt), regions[region_index].width,
				    regions[region_index].height, regions[region_index].pitch,
				    layout.level_sizes[i].size, GetLevelSrcSize(layout.level_sizes[i]),
				    layout.level_sizes[i].x, layout.level_sizes[i].y);
			}
		}
		UtilFillImage(ctx, vk_obj, temp_buf.Data(), size, regions, dst_layout);
	} else if (layout.fmt_tiled_standard64kb) {
		LOGF("%s: detiling typed Standard64KB texture: fmt=%u tile=%u size=%" PRIu64
		     " extent=%" PRIu64 "x%" PRIu64 " depth=%" PRIu64 " pitch=%u\n",
		     owner, static_cast<uint32_t>(fmt), layout.tile, size, width, height,
		     static_cast<uint64_t>(depth), layout.pitch);
		UtilScratchBuffer temp_buf(size);
		for (uint32_t i = 0; i < levels; i++) {
			for (uint32_t z = 0; z < depth; z++) {
				const auto region_index = i * depth + z;
				auto* dst = static_cast<uint8_t*>(temp_buf.Data()) + regions[region_index].offset;
				const auto* src = static_cast<const uint8_t*>(src_data) +
				                  GetLevelSrcOffset(layout.level_sizes[i]) +
				                  z * GetSliceSrcStride(layout, i, source_slice_layout);
				TileConvertTiledToLinearStandard64KB(
				    dst, src, static_cast<uint32_t>(fmt), regions[region_index].width,
				    regions[region_index].height, regions[region_index].pitch,
				    layout.level_sizes[i].size, GetLevelSrcSize(layout.level_sizes[i]),
				    layout.level_sizes[i].x, layout.level_sizes[i].y);
			}
		}
		UtilFillImage(ctx, vk_obj, temp_buf.Data(), size, regions, dst_layout);
	} else if (layout.tile != 0) {
		EXIT("%s: typed tiled upload still unsupported after sizing, using linear fallback: fmt=%u "
		     "tile=%u size=%" PRIu64 " extent=%" PRIu64 "x%" PRIu64 " pitch=%u levels=%" PRIu64
		     "\n",
		     owner, static_cast<uint32_t>(fmt), layout.tile, size, width, height, layout.pitch,
		     levels);
		UtilFillImage(ctx, vk_obj, src_data, size, regions, dst_layout);
	}
}

} // namespace Libs::Graphics

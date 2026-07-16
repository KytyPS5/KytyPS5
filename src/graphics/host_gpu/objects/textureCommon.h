#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_TEXTURECOMMON_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_TEXTURECOMMON_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/utils.h"

#include <vector>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

struct GraphicContext;
struct VulkanImage;
struct VulkanMemory;

enum class TextureFormatUsage : uint32_t {
	None    = 0,
	Sampled = 1u << 0u,
	Storage = 1u << 1u,
};

constexpr TextureFormatUsage operator|(TextureFormatUsage lhs, TextureFormatUsage rhs) {
	return static_cast<TextureFormatUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr TextureFormatUsage operator&(TextureFormatUsage lhs, TextureFormatUsage rhs) {
	return static_cast<TextureFormatUsage>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr TextureFormatUsage operator~(TextureFormatUsage usage) {
	return static_cast<TextureFormatUsage>(~static_cast<uint32_t>(usage));
}

constexpr TextureFormatUsage& operator|=(TextureFormatUsage& lhs, TextureFormatUsage rhs) {
	lhs = lhs | rhs;
	return lhs;
}

constexpr bool TextureHasFormatUsage(TextureFormatUsage usage, TextureFormatUsage flag) {
	return (static_cast<uint32_t>(usage & flag) == static_cast<uint32_t>(flag));
}

enum class TextureUploadDestination { MipLevels, MipAtlas };

enum class TextureUploadSliceLayout { MipChainPerSlice, MipLevelPerSlice };

struct RenderTargetFormatInfo {
	VkFormat format            = VK_FORMAT_UNDEFINED;
	uint32_t bytes_per_element = 0;
};

struct TextureUploadLayout {
	uint32_t       tile                    = 0;
	uint32_t       pitch                   = 0;
	uint64_t       slice_stride            = 0;
	uint64_t       source_slice_stride     = 0;
	bool           fmt_tiled_render_target = false;
	bool           fmt_tiled_standard256b  = false;
	bool           fmt_tiled_standard4kb   = false;
	bool           fmt_tiled_standard64kb  = false;
	bool           fmt_tiled_depth         = false;
	bool           volume_texture          = false;
	TileSizeOffset level_sizes[16]         = {};
	TilePaddedSize padded_sizes[16]        = {};
};

struct TextureImageCreateParams {
	uint32_t                 fmt                      = 0;
	uint64_t                 width                    = 0;
	uint64_t                 height                   = 0;
	uint32_t                 base_level               = 0;
	uint64_t                 levels                   = 1;
	uint32_t                 depth                    = 1;
	uint64_t                 type                     = 0;
	uint64_t                 swizzle                  = 0;
	TextureFormatUsage       format_usage             = TextureFormatUsage::Sampled;
	TextureFormatUsage       required_format_usage    = TextureFormatUsage::Sampled;
	TextureFormatUsage       view_usage               = TextureFormatUsage::Sampled;
	TextureUploadDestination image_layout             = TextureUploadDestination::MipLevels;
	bool                     allow_cube_view          = false;
	bool                     compatible_format_views  = false;
	bool                     storage_swizzle_fallback = false;
	const char*              owner                    = nullptr;
};

VkComponentSwizzle TextureGetComponentSwizzle(uint8_t s);
VkComponentMapping TextureGetComponentMapping(uint32_t swizzle);
bool               TextureCheckFormat(GraphicContext* ctx, VkImageCreateInfo* image_info);
bool TextureCheckStorageSwizzle(VkImageCreateInfo* image_info, VkComponentMapping* components);
VkImageUsageFlags      TextureGetUsage(TextureFormatUsage usage);
VkImageUsageFlags      TextureGetViewUsage(TextureFormatUsage usage);
VkFormat               TextureGetFormat(uint32_t fmt, TextureFormatUsage usage);
RenderTargetFormatInfo TextureGetRenderTargetFormat(uint32_t layout, uint32_t type, uint32_t order);
uint32_t TextureGetAtlasSliceYStride(VkFormat format, uint32_t mip_height, uint32_t depth,
                                     uint64_t levels);
uint32_t TextureCalcStackedImageHeight(VkFormat format, uint32_t height, uint32_t depth,
                                       uint64_t levels);
uint32_t TextureCalcMipmapAtlasImageHeight(VkFormat format, uint32_t width, uint32_t height,
                                           uint32_t depth, uint64_t levels);
bool     TextureIs3DTexture(uint64_t type);
bool     TextureIsCubeTexture(uint64_t type);
bool     TextureIsLayeredTexture(uint64_t type);
bool     TextureCanCreateCubeView(uint64_t type, uint32_t base_array, uint32_t layer_count);
VkComponentMapping TextureCreateImage(GraphicContext* ctx, VulkanImage* vk_obj, VulkanMemory* mem,
                                      const TextureImageCreateParams& params);
void TextureCreateImageViews(GraphicContext* ctx, VulkanImage* vk_obj,
                             VkComponentMapping components, uint64_t type, uint32_t base_array,
                             uint32_t base_level, uint32_t level_count, uint32_t depth,
                             bool allow_cube_view, TextureFormatUsage view_usage);
TextureUploadLayout TextureCalcUploadLayout(uint32_t fmt, uint64_t width, uint64_t height,
                                            uint64_t levels, uint32_t depth, uint64_t pitch,
                                            uint64_t tile, uint64_t upload_size,
                                            bool allow_depth_tile,
                                            bool require_single_mip_small_tiles,
                                            bool volume_texture, const char* owner);
uint64_t TextureUploadSliceSourceOffset(const TextureUploadLayout& layout, uint32_t level,
                                        uint32_t                 slice,
                                        TextureUploadSliceLayout source_slice_layout);
uint64_t TextureCalcUploadSize(const TextureUploadLayout&          layout,
                               const std::vector<BufferImageCopy>& regions, uint64_t levels,
                               uint32_t depth, TextureUploadSliceLayout source_slice_layout);
std::vector<BufferImageCopy>
TextureBuildUploadRegions(const TextureUploadLayout& layout, VkFormat image_format, uint32_t width,
                          uint32_t height, uint32_t depth, uint64_t levels, bool array_texture,
                          bool volume_texture, TextureUploadDestination destination,
                          TextureUploadSliceLayout slice_layout);
void TextureCopyBufferBytes(GraphicContext* ctx, VulkanBuffer* src_buffer,
                            uint64_t src_buffer_offset, uint64_t copy_size, UtilScratchBuffer* dst);
void TextureUploadGuestImage(GraphicContext* ctx, VulkanImage* vk_obj, const void* src_data,
                             uint64_t size, const std::vector<BufferImageCopy>& regions,
                             const TextureUploadLayout& layout, uint32_t fmt, uint64_t width,
                             uint64_t height, uint32_t depth, uint64_t levels,
                             TextureUploadSliceLayout source_slice_layout, const char* owner,
                             uint64_t dst_layout);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_TEXTURECOMMON_H_ */

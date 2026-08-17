#include "graphics/host_gpu/renderer/image/imageView.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <mutex>

namespace Libs::Graphics {

namespace {

[[nodiscard]] bool IsComponentSwizzle(vk::ComponentSwizzle swizzle) {
	switch (swizzle) {
		case vk::ComponentSwizzle::eIdentity:
		case vk::ComponentSwizzle::eZero:
		case vk::ComponentSwizzle::eOne:
		case vk::ComponentSwizzle::eR:
		case vk::ComponentSwizzle::eG:
		case vk::ComponentSwizzle::eB:
		case vk::ComponentSwizzle::eA: return true;
		default: return false;
	}
}

[[nodiscard]] bool IsCompatibleViewFormat(vk::Format image_format, vk::Format view_format) {
	return ImageViewOps::FormatsCompatible(image_format, view_format);
}

[[nodiscard]] bool IsStencilViewFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eS8Uint:
		case vk::Format::eR8Uint:
		case vk::Format::eR8Unorm: return true;
		default: return false;
	}
}

[[nodiscard]] bool IsValidViewType(const VulkanImage& image, const ImageViewInfo& info) {
	switch (image.image_type) {
		case vk::ImageType::e1D:
			if (info.type != vk::ImageViewType::e1D && info.type != vk::ImageViewType::e1DArray) {
				return false;
			}
			return info.type != vk::ImageViewType::e1D || info.layer_count == 1;
		case vk::ImageType::e2D:
			switch (info.type) {
				case vk::ImageViewType::e2D: return info.layer_count == 1;
				case vk::ImageViewType::e2DArray: return true;
				case vk::ImageViewType::eCube:
					return static_cast<bool>(image.flags &
					                         vk::ImageCreateFlagBits::eCubeCompatible) &&
					       info.base_layer % 6 == 0 && info.layer_count == 6;
				case vk::ImageViewType::eCubeArray:
					return static_cast<bool>(image.flags &
					                         vk::ImageCreateFlagBits::eCubeCompatible) &&
					       info.base_layer % 6 == 0 && info.layer_count % 6 == 0;
				default: return false;
			}
		case vk::ImageType::e3D:
			switch (info.type) {
				case vk::ImageViewType::e3D: return info.base_layer == 0 && info.layer_count == 1;
				case vk::ImageViewType::e2D:
					return static_cast<bool>(image.flags &
					                         vk::ImageCreateFlagBits::e2DArrayCompatible) &&
					       info.level_count == 1 && info.layer_count == 1;
				case vk::ImageViewType::e2DArray:
					return static_cast<bool>(image.flags &
					                         vk::ImageCreateFlagBits::e2DArrayCompatible) &&
					       info.level_count == 1;
				default: return false;
			}
		default: return false;
	}
}

[[nodiscard]] bool IsValidAspect(const VulkanImage& image, vk::ImageAspectFlags aspect) {
	const auto depth_format = DepthAspectTransferFormat(image.format);
	if (depth_format == vk::Format::eUndefined) {
		return aspect == vk::ImageAspectFlagBits::eColor;
	}
	const auto supported = ImageViewOps::DepthAspectMask(image.format);
	return static_cast<bool>(aspect) && !(aspect & ~supported);
}

} // namespace

namespace ImageViewOps {

namespace {

enum CompatibilityClass : uint32_t {
	None    = 0,
	Bit8    = 1u << 0,
	Bit16   = 1u << 1,
	Bit24   = 1u << 2,
	Bit32   = 1u << 3,
	Bit48   = 1u << 4,
	Bit64   = 1u << 5,
	Bit96   = 1u << 6,
	Bit128  = 1u << 7,
	Bit192  = 1u << 8,
	Bit256  = 1u << 9,
	Bc1Rgb  = 1u << 10,
	Bc1Rgba = 1u << 11,
	Bc2     = 1u << 12,
	Bc3     = 1u << 13,
	Bc4     = 1u << 14,
	Bc5     = 1u << 15,
	Bc6h    = 1u << 16,
	Bc7     = 1u << 17,
	D16     = 1u << 18,
	D16S8   = 1u << 19,
	D24     = 1u << 20,
	D24S8   = 1u << 21,
	D32     = 1u << 22,
	D32S8   = 1u << 23,
	S8      = 1u << 24,
};

[[nodiscard]] uint32_t FormatClass(vk::Format format) noexcept {
	switch (format) {
		case vk::Format::eR4G4UnormPack8:
		case vk::Format::eR8Sint:
		case vk::Format::eR8Snorm:
		case vk::Format::eR8Srgb:
		case vk::Format::eR8Sscaled:
		case vk::Format::eR8Uint:
		case vk::Format::eR8Unorm:
		case vk::Format::eR8Uscaled: return Bit8;

		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eA4B4G4R4UnormPack16:
		case vk::Format::eA4R4G4B4UnormPack16:
		case vk::Format::eB4G4R4A4UnormPack16:
		case vk::Format::eB5G5R5A1UnormPack16:
		case vk::Format::eB5G6R5UnormPack16:
		case vk::Format::eR10X6UnormPack16:
		case vk::Format::eR12X4UnormPack16:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16Sint:
		case vk::Format::eR16Snorm:
		case vk::Format::eR16Sscaled:
		case vk::Format::eR16Uint:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16Uscaled:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR5G5B5A1UnormPack16:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eR8G8Sint:
		case vk::Format::eR8G8Snorm:
		case vk::Format::eR8G8Srgb:
		case vk::Format::eR8G8Sscaled:
		case vk::Format::eR8G8Uint:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8Uscaled: return Bit16;

		case vk::Format::eB8G8R8Sint:
		case vk::Format::eB8G8R8Snorm:
		case vk::Format::eB8G8R8Srgb:
		case vk::Format::eB8G8R8Sscaled:
		case vk::Format::eB8G8R8Uint:
		case vk::Format::eB8G8R8Unorm:
		case vk::Format::eB8G8R8Uscaled:
		case vk::Format::eR8G8B8Sint:
		case vk::Format::eR8G8B8Snorm:
		case vk::Format::eR8G8B8Srgb:
		case vk::Format::eR8G8B8Sscaled:
		case vk::Format::eR8G8B8Uint:
		case vk::Format::eR8G8B8Unorm:
		case vk::Format::eR8G8B8Uscaled: return Bit24;

		case vk::Format::eA2B10G10R10SintPack32:
		case vk::Format::eA2B10G10R10SnormPack32:
		case vk::Format::eA2B10G10R10SscaledPack32:
		case vk::Format::eA2B10G10R10UintPack32:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2B10G10R10UscaledPack32:
		case vk::Format::eA2R10G10B10SintPack32:
		case vk::Format::eA2R10G10B10SnormPack32:
		case vk::Format::eA2R10G10B10SscaledPack32:
		case vk::Format::eA2R10G10B10UintPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA2R10G10B10UscaledPack32:
		case vk::Format::eA8B8G8R8SintPack32:
		case vk::Format::eA8B8G8R8SnormPack32:
		case vk::Format::eA8B8G8R8SrgbPack32:
		case vk::Format::eA8B8G8R8SscaledPack32:
		case vk::Format::eA8B8G8R8UintPack32:
		case vk::Format::eA8B8G8R8UnormPack32:
		case vk::Format::eA8B8G8R8UscaledPack32:
		case vk::Format::eB10G11R11UfloatPack32:
		case vk::Format::eB8G8R8A8Sint:
		case vk::Format::eB8G8R8A8Snorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eB8G8R8A8Sscaled:
		case vk::Format::eB8G8R8A8Uint:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Uscaled:
		case vk::Format::eE5B9G9R9UfloatPack32:
		case vk::Format::eR10X6G10X6Unorm2Pack16:
		case vk::Format::eR12X4G12X4Unorm2Pack16:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16Sint:
		case vk::Format::eR16G16Snorm:
		case vk::Format::eR16G16Sscaled:
		case vk::Format::eR16G16Uint:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16Uscaled:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32Sint:
		case vk::Format::eR32Uint:
		case vk::Format::eR8G8B8A8Sint:
		case vk::Format::eR8G8B8A8Snorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eR8G8B8A8Sscaled:
		case vk::Format::eR8G8B8A8Uint:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Uscaled: return Bit32;

		case vk::Format::eR16G16B16Sfloat:
		case vk::Format::eR16G16B16Sint:
		case vk::Format::eR16G16B16Snorm:
		case vk::Format::eR16G16B16Sscaled:
		case vk::Format::eR16G16B16Uint:
		case vk::Format::eR16G16B16Unorm:
		case vk::Format::eR16G16B16Uscaled: return Bit48;

		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR16G16B16A16Sint:
		case vk::Format::eR16G16B16A16Snorm:
		case vk::Format::eR16G16B16A16Sscaled:
		case vk::Format::eR16G16B16A16Uint:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16G16B16A16Uscaled:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32Sint:
		case vk::Format::eR32G32Uint:
		case vk::Format::eR64Sfloat:
		case vk::Format::eR64Sint:
		case vk::Format::eR64Uint: return Bit64;

		case vk::Format::eR32G32B32Sfloat:
		case vk::Format::eR32G32B32Sint:
		case vk::Format::eR32G32B32Uint: return Bit96;

		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eR32G32B32A32Sint:
		case vk::Format::eR32G32B32A32Uint:
		case vk::Format::eR64G64Sfloat:
		case vk::Format::eR64G64Sint:
		case vk::Format::eR64G64Uint: return Bit128;

		case vk::Format::eR64G64B64Sfloat:
		case vk::Format::eR64G64B64Sint:
		case vk::Format::eR64G64B64Uint: return Bit192;

		case vk::Format::eR64G64B64A64Sfloat:
		case vk::Format::eR64G64B64A64Sint:
		case vk::Format::eR64G64B64A64Uint: return Bit256;

		case vk::Format::eBc1RgbSrgbBlock:
		case vk::Format::eBc1RgbUnormBlock: return Bc1Rgb | Bit64;
		case vk::Format::eBc1RgbaSrgbBlock:
		case vk::Format::eBc1RgbaUnormBlock: return Bc1Rgba | Bit64;
		case vk::Format::eBc2SrgbBlock:
		case vk::Format::eBc2UnormBlock: return Bc2 | Bit128;
		case vk::Format::eBc3SrgbBlock:
		case vk::Format::eBc3UnormBlock: return Bc3 | Bit128;
		case vk::Format::eBc4SnormBlock:
		case vk::Format::eBc4UnormBlock: return Bc4 | Bit64;
		case vk::Format::eBc5SnormBlock:
		case vk::Format::eBc5UnormBlock: return Bc5 | Bit128;
		case vk::Format::eBc6HSfloatBlock:
		case vk::Format::eBc6HUfloatBlock: return Bc6h | Bit128;
		case vk::Format::eBc7SrgbBlock:
		case vk::Format::eBc7UnormBlock: return Bc7 | Bit128;

		case vk::Format::eD16Unorm: return D16;
		case vk::Format::eD16UnormS8Uint: return D16S8;
		case vk::Format::eX8D24UnormPack32: return D24;
		case vk::Format::eD24UnormS8Uint: return D24S8;
		case vk::Format::eD32Sfloat: return D32;
		case vk::Format::eD32SfloatS8Uint: return D32S8;
		case vk::Format::eS8Uint: return S8;
		default: return None;
	}
}

} // namespace

vk::ImageAspectFlags DepthAspectMask(vk::Format format) {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD32Sfloat: return vk::ImageAspectFlagBits::eDepth;
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint:
			return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
		default: EXIT("unsupported depth/stencil image format: %d\n", static_cast<int>(format));
	}
}

bool FormatsCompatible(vk::Format base, vk::Format view) noexcept {
	if (base == view) {
		return true;
	}
	const auto base_class = FormatClass(base);
	const auto view_class = FormatClass(view);
	return view_class != None && (base_class & view_class) == view_class;
}

} // namespace ImageViewOps

vk::ImageView Image::FindView(const ImageViewInfo& view_info) {
	const auto& image      = backing;
	auto        normalized = view_info;
	const bool  is_storage = static_cast<bool>(normalized.usage & vk::ImageUsageFlagBits::eStorage);
	const auto  image_aspect = FullAspectMask(image.format);
	if (image_aspect & vk::ImageAspectFlagBits::eDepth &&
	    ImageViewOps::IsFormatDepthCompatible(normalized.format)) {
		normalized.format = image.format;
		normalized.aspect = vk::ImageAspectFlagBits::eDepth;
	}
	if (image_aspect & vk::ImageAspectFlagBits::eStencil &&
	    IsStencilViewFormat(normalized.format)) {
		normalized.format = image.format;
		normalized.aspect = vk::ImageAspectFlagBits::eStencil;
	}
	normalized.usage = is_storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags {};
	const bool format_compatible = normalized.format != vk::Format::eUndefined &&
	                               IsCompatibleViewFormat(image.format, normalized.format);
	const bool slice_view =
	    image.image_type == vk::ImageType::e3D && (normalized.type == vk::ImageViewType::e2D ||
	                                               normalized.type == vk::ImageViewType::e2DArray);
	const bool levels_valid = normalized.level_count != 0 &&
	                          normalized.base_level < image.mip_levels &&
	                          normalized.level_count <= image.mip_levels - normalized.base_level;
	const auto view_layers  = slice_view && levels_valid
	                              ? std::max(image.extent.depth >> normalized.base_level, 1u)
	                              : image.layers;
	const bool ranges_valid = levels_valid && normalized.layer_count != 0 &&
	                          normalized.base_layer < view_layers &&
	                          normalized.layer_count <= view_layers - normalized.base_layer;
	const bool mapping_valid =
	    IsComponentSwizzle(normalized.mapping.r) && IsComponentSwizzle(normalized.mapping.g) &&
	    IsComponentSwizzle(normalized.mapping.b) && IsComponentSwizzle(normalized.mapping.a);
	if (image.image == nullptr || !format_compatible || !ranges_valid || !mapping_valid ||
	    !IsValidViewType(image, normalized) || !IsValidAspect(image, normalized.aspect)) {
		EXIT("invalid image view: image_format=%d view_format=%d type=%d aspect=0x%x "
		     "mip=%u+%u layer=%u+%u usage=0x%x image_levels=%u image_layers=%u\n",
		     static_cast<int>(image.format), static_cast<int>(normalized.format),
		     static_cast<int>(normalized.type),
		     static_cast<vk::ImageAspectFlags::MaskType>(normalized.aspect), normalized.base_level,
		     normalized.level_count, normalized.base_layer, normalized.layer_count,
		     static_cast<vk::ImageUsageFlags::MaskType>(normalized.usage), image.mip_levels,
		     image.layers);
	}

	std::lock_guard lock(views.mutex);
	for (const auto& cached: views.views) {
		if (cached.info == normalized) {
			return cached.view;
		}
	}

	vk::ImageViewUsageCreateInfo usage {};
	usage.sType = vk::StructureType::eImageViewUsageCreateInfo;
	usage.usage = image.usage;
	if (!is_storage) {
		usage.usage &= ~vk::ImageUsageFlagBits::eStorage;
	}
	vk::ImageViewCreateInfo create {};
	create.sType                           = vk::StructureType::eImageViewCreateInfo;
	create.pNext                           = &usage;
	create.image                           = image.image;
	create.viewType                        = normalized.type;
	create.format                          = normalized.format;
	create.components                      = normalized.mapping;
	create.subresourceRange.aspectMask     = normalized.aspect;
	create.subresourceRange.baseMipLevel   = normalized.base_level;
	create.subresourceRange.levelCount     = normalized.level_count;
	create.subresourceRange.baseArrayLayer = normalized.base_layer;
	create.subresourceRange.layerCount     = normalized.layer_count;

	vk::ImageView view   = nullptr;
	const auto    result = m_graphics->device.createImageView(&create, nullptr, &view);
	if (result != vk::Result::eSuccess || view == nullptr) {
		EXIT("failed to create image view: result=%d image_format=%d view_format=%d type=%d "
		     "aspect=0x%x mip=%u+%u layer=%u+%u usage=0x%x\n",
		     static_cast<int>(result), static_cast<int>(image.format),
		     static_cast<int>(view_info.format), static_cast<int>(view_info.type),
		     static_cast<vk::ImageAspectFlags::MaskType>(view_info.aspect), view_info.base_level,
		     view_info.level_count, view_info.base_layer, view_info.layer_count,
		     static_cast<vk::ImageUsageFlags::MaskType>(view_info.usage));
	}
	views.views.push_back({normalized, view});
	return view;
}

} // namespace Libs::Graphics

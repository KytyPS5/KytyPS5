#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEVIEW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEVIEW_H_

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"

namespace Libs::Graphics {

[[nodiscard]] inline bool IsSupportedStorageSwizzle(uint32_t format, uint32_t swizzle) noexcept {
	const bool single_channel_uint = format == Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt) ||
	                                 format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UInt) ||
	                                 format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt);
	return swizzle == DstSel(4, 5, 6, 7) ||
	       (single_channel_uint &&
	        (swizzle == DstSel(4, 0, 0, 0) || swizzle == DstSel(4, 0, 0, 1))) ||
	       (format == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm) &&
	        (swizzle == DstSel(4, 5, 6, 1) || swizzle == DstSel(6, 5, 4, 7))) ||
	       (format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float) &&
	        swizzle == DstSel(5, 6, 7, 4));
}

[[noreturn]] inline void UnsupportedColorView(const char* usage, VkFormat image_format,
                                              VkFormat view_format, uint32_t swizzle) noexcept {
	EXIT("unsupported %s color image view: image_format=%d view_format=%d swizzle=0x%03x\n", usage,
	     static_cast<int>(image_format), static_cast<int>(view_format), swizzle);
}

[[nodiscard]] inline VkFormat BgraToRgbaSampledViewFormat(VkFormat image_format) noexcept {
	switch (image_format) {
		case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
		default: return VK_FORMAT_UNDEFINED;
	}
}

[[nodiscard]] inline bool IsBgraToRgba8SampledView(VkFormat image_format,
                                                   VkFormat view_format) noexcept {
	const bool bgra8 =
	    image_format == VK_FORMAT_B8G8R8A8_UNORM || image_format == VK_FORMAT_B8G8R8A8_SRGB;
	const bool rgba8 =
	    view_format == VK_FORMAT_R8G8B8A8_UNORM || view_format == VK_FORMAT_R8G8B8A8_SRGB;
	return bgra8 && rgba8;
}

[[nodiscard]] inline VkFormat BgraSrgbStorageViewFormat(VkFormat image_format) noexcept {
	return image_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
}

[[nodiscard]] inline bool IsBgraSrgbStorageView(VkFormat image_format, VkFormat view_format,
                                                uint32_t swizzle) noexcept {
	return view_format == BgraSrgbStorageViewFormat(image_format) && swizzle == DstSel(6, 5, 4, 7);
}

[[nodiscard]] inline int SelectSampledColorView(VkFormat image_format, VkFormat view_format,
                                                uint32_t swizzle) noexcept {
	if (image_format == view_format) {
		switch (swizzle) {
			case DstSel(4, 5, 6, 7): return VulkanImage::VIEW_DEFAULT;
			case DstSel(4, 0, 0, 1): return VulkanImage::VIEW_R001;
			case DstSel(4, 5, 0, 1): return VulkanImage::VIEW_RG01;
			case DstSel(4, 5, 6, 1): return VulkanImage::VIEW_RGB1;
			default: break;
		}
	}
	if (IsBgraToRgba8SampledView(image_format, view_format) && swizzle == DstSel(6, 5, 4, 7)) {
		return VulkanImage::VIEW_RGBA8_BGRA;
	}
	UnsupportedColorView("sampled", image_format, view_format, swizzle);
}

[[nodiscard]] inline int SelectSampledDepthView(VkFormat image_format, VkFormat view_format,
                                                uint32_t swizzle) noexcept {
	const bool d16 = image_format == VK_FORMAT_D16_UNORM && view_format == VK_FORMAT_R16_UNORM;
	const bool d32 =
	    (image_format == VK_FORMAT_D32_SFLOAT || image_format == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
	    view_format == VK_FORMAT_R32_SFLOAT;
	if (d16 || d32) {
		switch (swizzle) {
			case DstSel(4, 4, 4, 4): return VulkanImage::VIEW_DEPTH_TEXTURE;
			case DstSel(4, 0, 0, 0): return VulkanImage::VIEW_R000;
			default: break;
		}
	}
	EXIT("unsupported sampled depth image view: image_format=%d view_format=%d swizzle=0x%03x\n",
	     static_cast<int>(image_format), static_cast<int>(view_format), swizzle);
}

[[nodiscard]] inline bool
IsSupportedSampledDepthResource(const ShaderRecompiler::IR::ImageResource& resource) noexcept {
	return resource.kind == ShaderRecompiler::IR::ResourceKind::Image &&
	       resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	       resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None && resource.read &&
	       !resource.written && !resource.atomic;
}

[[nodiscard]] inline int SelectStorageColorView(VkFormat image_format, VkFormat view_format,
                                                uint32_t swizzle) noexcept {
	const bool single_channel_uint = view_format == VK_FORMAT_R8_UINT ||
	                                 view_format == VK_FORMAT_R16_UINT ||
	                                 view_format == VK_FORMAT_R32_UINT;
	const bool swizzle_ok =
	    swizzle == DstSel(4, 5, 6, 7) ||
	    (single_channel_uint && (swizzle == DstSel(4, 0, 0, 0) || swizzle == DstSel(4, 0, 0, 1))) ||
	    (view_format == VK_FORMAT_R8G8B8A8_UNORM &&
	     (swizzle == DstSel(4, 5, 6, 1) || swizzle == DstSel(6, 5, 4, 7))) ||
	    (view_format == VK_FORMAT_R32G32B32A32_SFLOAT && swizzle == DstSel(5, 6, 7, 4));
	if ((image_format != view_format &&
	     !IsBgraSrgbStorageView(image_format, view_format, swizzle)) ||
	    !swizzle_ok) {
		UnsupportedColorView("storage", image_format, view_format, swizzle);
	}
	return VulkanImage::VIEW_STORAGE;
}

[[nodiscard]] inline bool
IsSupportedStorageImageResource(const ShaderRecompiler::IR::ImageResource& resource) noexcept {
	return (resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
	        resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint) &&
	       (resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray) &&
	       resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None && resource.written &&
	       !resource.atomic && !resource.depth_compare;
}

inline void
ValidateStorageImageResource(const ShaderRecompiler::IR::ImageResource& resource) noexcept {
	if (!IsSupportedStorageImageResource(resource)) {
		EXIT("unsupported storage color image resource: kind=%u dimension=%u mip=%u "
		     "read=%d written=%d atomic=%d depth_compare=%d\n",
		     static_cast<uint32_t>(resource.kind), static_cast<uint32_t>(resource.dimension),
		     static_cast<uint32_t>(resource.mip_mode), resource.read, resource.written,
		     resource.atomic, resource.depth_compare);
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEVIEW_H_

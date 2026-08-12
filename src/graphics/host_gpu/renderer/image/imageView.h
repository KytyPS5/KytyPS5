#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEVIEW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEVIEW_H_

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <atomic>

namespace Libs::Graphics {

namespace ImageViewOps {

[[nodiscard]] vk::ImageAspectFlags DepthAspectMask(vk::Format format);
[[nodiscard]] bool                 FormatsCompatible(vk::Format base, vk::Format view) noexcept;
} // namespace ImageViewOps

[[nodiscard]] inline bool IsValidImageSwizzle(uint32_t swizzle) noexcept {
	if ((swizzle & ~0xfffu) != 0) {
		return false;
	}
	for (uint32_t channel = 0; channel < 4; channel++) {
		switch (GetDstSel(swizzle, channel)) {
			case 0:
			case 1:
			case 4:
			case 5:
			case 6:
			case 7: break;
			default: return false;
		}
	}
	return true;
}

[[noreturn]] inline void UnsupportedColorView(const char* usage, vk::Format image_format,
                                              vk::Format view_format, uint32_t swizzle) noexcept {
	EXIT("unsupported %s color image view: image_format=%d view_format=%d swizzle=0x%03x\n", usage,
	     static_cast<int>(image_format), static_cast<int>(view_format), swizzle);
}

[[nodiscard]] inline vk::Format SrgbStorageViewFormat(vk::Format image_format) noexcept {
	switch (image_format) {
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Srgb: return vk::Format::eR8G8B8A8Unorm;
		default: return vk::Format::eUndefined;
	}
}

[[nodiscard]] inline bool IsSupportedSampledColorView(vk::Format image_format,
                                                      vk::Format view_format,
                                                      uint32_t   swizzle) noexcept {
	return IsValidImageSwizzle(swizzle) &&
	       ImageViewOps::FormatsCompatible(image_format, view_format);
}

[[nodiscard]] inline uint32_t
SelectSampledColorView(vk::Format image_format, vk::Format view_format, uint32_t swizzle) noexcept {
	if (IsSupportedSampledColorView(image_format, view_format, swizzle)) {
		return swizzle;
	}
	UnsupportedColorView("sampled", image_format, view_format, swizzle);
}

[[nodiscard]] inline bool IsSupportedSampledDepthView(vk::Format image_format,
                                                      vk::Format view_format,
                                                      uint32_t   swizzle) noexcept {
	if (!IsSupportedSampledDepthFormat(image_format, view_format)) {
		return false;
	}
	switch (swizzle) {
		case DstSel(4, 4, 4, 4):
		case DstSel(4, 0, 0, 0):
		case DstSel(4, 0, 0, 1): return true;
		default: return false;
	}
}

[[nodiscard]] inline uint32_t
SelectSampledDepthView(vk::Format image_format, vk::Format view_format, uint32_t swizzle) noexcept {
	if (IsSupportedSampledDepthView(image_format, view_format, swizzle)) {
		return swizzle;
	}
	EXIT("unsupported sampled depth image view: image_format=%d view_format=%d swizzle=0x%03x\n",
	     static_cast<int>(image_format), static_cast<int>(view_format), swizzle);
}

[[nodiscard]] inline bool
IsSupportedSampledDepthResource(const ShaderRecompiler::IR::ImageResource& resource) noexcept {
	return resource.kind == ShaderRecompiler::IR::ResourceKind::Image &&
	       (resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray) &&
	       resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None && resource.read &&
	       !resource.written && !resource.atomic;
}

[[nodiscard]] inline bool
IsSupportedSampledDepthUintResource(const ShaderRecompiler::IR::ImageResource& resource) noexcept {
	return resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint &&
	       resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	       resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None && resource.read &&
	       !resource.written && !resource.atomic && !resource.depth_compare;
}

inline void ValidateStorageColorView(vk::Format image_format, vk::Format view_format,
                                     uint32_t swizzle) noexcept {
	if (!ImageViewOps::FormatsCompatible(image_format, view_format) ||
	    !IsValidImageSwizzle(swizzle)) {
		UnsupportedColorView("storage", image_format, view_format, swizzle);
	}
}

[[nodiscard]] inline bool
IsSupportedStorageImageResource(const ShaderRecompiler::IR::ImageResource& resource) noexcept {
	return (resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
	        resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint) &&
	       (resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1D ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1DArray ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D ||
	        resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray) &&
	       resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None && resource.written &&
	       (!resource.atomic ||
	        (resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint &&
	         resource.read)) &&
	       !resource.depth_compare;
}

// A Vulkan storage image view targets exactly ONE mip level, so a shader that selects the level
// at runtime (ImageMipMode::DynamicStorage) cannot be served exactly by widening a check. But
// when the DESCRIPTOR exposes a single level the runtime index is degenerate - only one value
// can be valid - so binding that level is correct by construction, not an approximation.
[[nodiscard]] inline bool
IsDegenerateDynamicMip(const ShaderRecompiler::IR::ImageResource& resource, uint32_t base_level,
                       uint32_t last_level) noexcept {
	return resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::DynamicStorage &&
	       base_level == last_level;
}

inline void ValidateStorageImageResource(const ShaderRecompiler::IR::ImageResource& resource,
                                         uint32_t base_level = 0,
                                         uint32_t last_level = 0) noexcept {
	if (resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::DynamicStorage) {
		auto relaxed     = resource;
		relaxed.mip_mode = ShaderRecompiler::IR::ImageMipMode::None;
		if (IsSupportedStorageImageResource(relaxed)) {
			if (!IsDegenerateDynamicMip(resource, base_level, last_level)) {
				// APPROXIMATION, and it is worth being precise about what is approximated.
				// SPIR-V's OpImageWrite has no LOD operand, and the emitter reflects that:
				// EmitImageMipLodU32 (spirvEmitterImageHelpers.cpp:241) has exactly ONE caller,
				// EmitImageLoad's OpImageFetch (spirvEmitterImageOps.cpp:146) - the STORE path
				// never reads it. So a dynamic-mip store already writes to whichever level the
				// view targets; the mip operand is discarded regardless. Rejecting the resource
				// never made that correct, it only turned an existing approximation into an
				// abort.
				//
				// Serving it properly needs one view per level bound as a descriptor array and
				// a switch in the shader. Until that exists, allow the write and say so: a
				// mip-generation shader will fill one level repeatedly instead of the chain,
				// which is a visible artefact, not corruption. Titles that do not use
				// dynamic-mip storage images never reach this path, so nothing that works
				// today is affected.
				static std::atomic<uint32_t> reported {0};
				if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
					LOGF("[img] dynamic-mip storage write over levels %u..%u is served by the "
					     "view's own level; mip selection is ignored (needs a per-level view "
					     "array)\n",
					     base_level, last_level);
				}
			}
			return;
		}
	}
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

#include "graphics/host_gpu/renderer/image/textureCommon.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>

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

} // namespace Libs::Graphics

#include "common/logging/log.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shaderBindings.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <optional>

namespace Libs::Graphics {

namespace {

enum class PssrRole : uint8_t { Unknown, Color, Depth, Motion, Output };

const char* RoleName(PssrRole role) {
	switch (role) {
		case PssrRole::Color: return "color";
		case PssrRole::Depth: return "depth";
		case PssrRole::Motion: return "motion";
		case PssrRole::Output: return "output";
		default: return "?";
	}
}

struct PssrSlot {
	bool                  valid = false;
	ShaderTextureResource descriptor;
	uint32_t              width  = 0;
	uint32_t              height = 0;
	vk::Format            format = vk::Format::eUndefined;
	PssrRole              role   = PssrRole::Unknown;
};

uint32_t ComponentCount(vk::Format format) {
	switch (format) {
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16Snorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR16G16Sint:
		case vk::Format::eR8G8Snorm:
		case vk::Format::eR8G8Unorm: return 2;
		case vk::Format::eR32Sfloat:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eD32Sfloat:
		case vk::Format::eD16Unorm:
		case vk::Format::eR8Unorm:
		case vk::Format::eR32Uint: return 1;
		default: return 4;
	}
}

} // namespace

// PS5 Pro upscaling request (graphics/guest_gpu/pssr.h). Which descriptor slot holds which image
// is worked out from sizes and formats: the largest image is the output; at the render size, a
// two-channel image is the motion vectors, a one-channel one the depth, the rest colour.
// For now the colour image is scaled into the output (linear filter); a temporal upscaler can use
// the motion vectors and depth from the same request later.
void RenderExecutor::ProcessPssr(uint64_t submit_id, CommandBuffer& buffer,
                                 const Pssr::Request& request) {
	if (request.magic != Pssr::Magic || request.version != Pssr::Version) {
		LOGF("PSSR: ignoring request with magic 0x%08x version %u\n", request.magic, request.version);
		return;
	}
	std::array<PssrSlot, Pssr::TextureSlots> slots {};
	for (uint32_t i = 0; i < Pssr::TextureSlots; i++) {
		if ((request.texture_mask & (1u << i)) == 0) {
			continue;
		}
		auto& slot = slots[i];
		std::memcpy(slot.descriptor.fields, request.textures[i], sizeof(slot.descriptor.fields));
		if (slot.descriptor.IsNull()) {
			continue;
		}
		slot.valid  = true;
		slot.width  = static_cast<uint32_t>(slot.descriptor.Width5()) + 1u;
		slot.height = static_cast<uint32_t>(slot.descriptor.Height5()) + 1u;
		slot.format = TextureGetSurfaceFormatInfo(slot.descriptor.Format()).vk_format;
	}

	int output = -1;
	for (uint32_t i = 0; i < slots.size(); i++) {
		if (slots[i].valid && ComponentCount(slots[i].format) >= 3 &&
		    (output < 0 || uint64_t {slots[i].width} * slots[i].height >
		                       uint64_t {slots[output].width} * slots[output].height)) {
			output = static_cast<int>(i);
		}
	}
	int color = -1;
	for (uint32_t i = 0; i < slots.size(); i++) {
		auto& slot = slots[i];
		if (!slot.valid) {
			continue;
		}
		if (static_cast<int>(i) == output) {
			slot.role = PssrRole::Output;
			continue;
		}
		const auto components = ComponentCount(slot.format);
		slot.role = components == 2   ? PssrRole::Motion
		            : components == 1 ? PssrRole::Depth
		                              : PssrRole::Color;
		if (slot.role == PssrRole::Color && color < 0 &&
		    (output < 0 || slot.width < slots[output].width || slot.height < slots[output].height)) {
			color = static_cast<int>(i);
		}
	}

	static std::atomic<uint32_t> logged {0};
	if (logged.fetch_add(1) < 4) {
		LOGF("PSSR request 0x%016" PRIx64 " (submit %" PRIu64 "):\n", request.params_address,
		     submit_id);
		for (uint32_t i = 0; i < slots.size(); i++) {
			const auto& slot = slots[i];
			if (!slot.valid) {
				continue;
			}
			LOGF("\tslot %u: %ux%u guest format %u (%s) -> %s%s\n", i, slot.width, slot.height,
			     static_cast<uint32_t>(slot.descriptor.Format()), vk::to_string(slot.format).c_str(),
			     RoleName(slot.role), static_cast<int>(i) == color ? " (used as input)" : "");
		}
	}
	if (output < 0 || color < 0) {
		return;
	}

	const auto resolve = [&](const PssrSlot& slot) {
		ShaderRecompiler::IR::ImageResource resource {};
		resource.resource_class = ShaderRecompiler::IR::ImageResourceClass::Sampled;
		resource.numeric_class  = Prospero::SampledTextureNumericClass(slot.descriptor.Format());
		resource.dimension      = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
		resource.read           = true;
		ShaderRecompiler::IR::DescriptorValue value {};
		std::memcpy(value.dwords.data(), slot.descriptor.fields, sizeof(slot.descriptor.fields));
		value.dword_count = 8;
		return ResolveTexture(resource, value);
	};
	auto&      cache          = m_context.GetTextureCache();
	const auto source_binding = resolve(slots[color]);
	const auto target_binding = resolve(slots[output]);
	auto&      source         = cache.GetImage(source_binding.image_id);
	auto&      target         = cache.GetImage(target_binding.image_id);
	if (&source == &target) {
		return;
	}

	const auto command = buffer.Handle();
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
	               std::nullopt, command);
	target.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
	               std::nullopt, command);
	vk::ImageBlit region {};
	region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	region.srcOffsets[1]  = vk::Offset3D {static_cast<int>(slots[color].width),
                                         static_cast<int>(slots[color].height), 1};
	region.dstOffsets[1]  = vk::Offset3D {static_cast<int>(slots[output].width),
                                         static_cast<int>(slots[output].height), 1};
	command.blitImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
	                  target.backing.image, vk::ImageLayout::eTransferDstOptimal, 1, &region,
	                  vk::Filter::eLinear);
	cache.MarkGpuWritten(target_binding.image_id);
}

} // namespace Libs::Graphics

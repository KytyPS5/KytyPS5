#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_FORMAT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_FORMAT_H_

#include "graphics/guest_gpu/gpu_defs.h"

namespace Libs::Graphics::Prospero {

enum class ChannelOrderSupport : uint8_t {
	kNone,
	kStandardOnly,
	kAll,
};

struct RenderTargetFormatEncoding {
	BufferFormat        buffer_format = BufferFormat::kInvalid;
	uint8_t             components    = 0;
	ChannelOrderSupport order_support = ChannelOrderSupport::kNone;

	[[nodiscard]] constexpr bool IsValid() const {
		return buffer_format != BufferFormat::kInvalid && components >= 1u && components <= 4u &&
		       order_support != ChannelOrderSupport::kNone;
	}

	[[nodiscard]] constexpr bool SupportsOrder(uint32_t raw_order) const {
		switch (order_support) {
			case ChannelOrderSupport::kStandardOnly:
				return raw_order == GpuEnumValue(ChannelOrder::kStandard);
			case ChannelOrderSupport::kAll:
				return raw_order <= GpuEnumValue(ChannelOrder::kAltReversed);
			default: return false;
		}
	}
};

RenderTargetFormatEncoding ResolveRenderTargetFormat(uint32_t layout, uint32_t type);
uint32_t                   NumBytesPerElement(uint32_t format);
uint32_t                   BlockCompressedBytesPerBlock(uint32_t format);
uint32_t                   RenderTargetBytesPerElement(uint32_t format);
bool                       IsSupportedTextureFormat(uint32_t format);
bool                       IsUintTextureFormat(uint32_t format);
bool                       IsFmaskTextureFormat(uint32_t format);

} // namespace Libs::Graphics::Prospero

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_FORMAT_H_ */

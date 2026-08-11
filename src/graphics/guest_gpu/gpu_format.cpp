#include "graphics/guest_gpu/gpu_format.h"

#include "graphics/guest_gpu/gpu_defs.h"

#include <array>
#include <bit>

namespace Libs::Graphics::Prospero {
namespace {

struct FormatInfo {
	uint32_t format;
	uint32_t bytes_per_element;
	uint32_t block_compressed_bytes_per_block;
	uint32_t render_target_bytes_per_element;
	bool     supported_sampled_texture_format;
	bool     unsigned_integer_texture_format;
};

constexpr FormatInfo kFormatInfo[] = {
    {GpuEnumValue(BufferFormat::k8UNorm), 1, 0, 1, true, false},
    {GpuEnumValue(BufferFormat::k8SNorm), 0, 0, 1, false, false},
    {GpuEnumValue(BufferFormat::k8UInt), 1, 0, 1, true, true},
    {GpuEnumValue(BufferFormat::k16UNorm), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k16SNorm), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k16UInt), 2, 0, 2, true, true},
    {GpuEnumValue(BufferFormat::k16SInt), 2, 0, 2, false, false},
    {GpuEnumValue(BufferFormat::k16Float), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k8_8UNorm), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k8_8SNorm), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k8_8UInt), 2, 0, 2, true, true},
    {GpuEnumValue(BufferFormat::k8_8SInt), 2, 0, 2, false, false},
    {GpuEnumValue(BufferFormat::k32UInt), 4, 0, 4, true, true},
    {GpuEnumValue(BufferFormat::k32SInt), 4, 0, 4, false, false},
    {GpuEnumValue(BufferFormat::k32Float), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k16_16UNorm), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k16_16SNorm), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k16_16UInt), 4, 0, 4, true, true},
    {GpuEnumValue(BufferFormat::k16_16SInt), 4, 0, 4, false, false},
    {GpuEnumValue(BufferFormat::k16_16Float), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k11_11_10Float), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k10_10_10_2UNorm), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k10_10_10_2UInt), 4, 0, 4, true, true},
    {GpuEnumValue(BufferFormat::k8_8_8_8UNorm), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k8_8_8_8SNorm), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k8_8_8_8UInt), 4, 0, 4, true, true},
    {GpuEnumValue(BufferFormat::k8_8_8_8SInt), 4, 0, 4, false, false},
    {GpuEnumValue(BufferFormat::k32_32UInt), 8, 0, 8, true, true},
    {GpuEnumValue(BufferFormat::k32_32SInt), 8, 0, 8, false, false},
    {GpuEnumValue(BufferFormat::k32_32Float), 8, 0, 8, true, false},
    {GpuEnumValue(BufferFormat::k16_16_16_16UNorm), 8, 0, 8, true, false},
    {GpuEnumValue(BufferFormat::k16_16_16_16SNorm), 8, 0, 8, true, false},
    {GpuEnumValue(BufferFormat::k16_16_16_16UInt), 8, 0, 8, true, true},
    {GpuEnumValue(BufferFormat::k16_16_16_16SInt), 8, 0, 8, false, false},
    {GpuEnumValue(BufferFormat::k16_16_16_16Float), 8, 0, 8, true, false},
    {GpuEnumValue(BufferFormat::k32_32_32UInt), 12, 0, 12, true, true},
    {GpuEnumValue(BufferFormat::k32_32_32SInt), 12, 0, 12, false, false},
    {GpuEnumValue(BufferFormat::k32_32_32Float), 12, 0, 12, true, false},
    {GpuEnumValue(BufferFormat::k32_32_32_32UInt), 16, 0, 16, true, true},
    {GpuEnumValue(BufferFormat::k32_32_32_32SInt), 16, 0, 16, false, false},
    {GpuEnumValue(BufferFormat::k32_32_32_32Float), 16, 0, 16, true, false},
    {GpuEnumValue(BufferFormat::k8Srgb), 1, 0, 0, true, false},
    {GpuEnumValue(BufferFormat::k8_8Srgb), 2, 0, 0, true, false},
    {GpuEnumValue(BufferFormat::k8_8_8_8Srgb), 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k9_9_9_5Float), 4, 0, 0, true, false},
    {GpuEnumValue(BufferFormat::k5_6_5UNorm), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k5_5_5_1UNorm), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k1_5_5_5UNorm), 0, 0, 2, false, false},
    {GpuEnumValue(BufferFormat::k4_4_4_4UNorm), 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::kFmask8_S4_F4), 1, 0, 1, true, false},
    {GpuEnumValue(BufferFormat::kBc1UNorm), 0, 8, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc1Srgb), 0, 8, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc2UNorm), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc2Srgb), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc3UNorm), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc3Srgb), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc4UNorm), 0, 8, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc4SNorm), 0, 8, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc5UNorm), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc5SNorm), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc6UFloat), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc6SFloat), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc7UNorm), 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc7Srgb), 0, 16, 0, true, false},
};

constexpr uint8_t RenderTargetTypeBit(ChannelType type) {
	return static_cast<uint8_t>(1u << GpuEnumValue(type));
}

constexpr auto kUNormType = RenderTargetTypeBit(ChannelType::kUNorm);
constexpr auto kIntegerTypes =
    RenderTargetTypeBit(ChannelType::kUInt) | RenderTargetTypeBit(ChannelType::kSInt);
constexpr auto kNormalizedIntegerTypes =
    kUNormType | RenderTargetTypeBit(ChannelType::kSNorm) | kIntegerTypes;

struct RenderTargetLayoutInfo {
	ChannelLayout       layout;
	BufferFormat        first_linear_format;
	uint8_t             linear_type_mask;
	uint8_t             components;
	BufferFormat        extra_format  = BufferFormat::kInvalid;
	ChannelType         extra_type    = ChannelType::kUNorm;
	ChannelOrderSupport order_support = ChannelOrderSupport::kAll;
};

constexpr RenderTargetLayoutInfo kRenderTargetLayouts[] = {
    {ChannelLayout::k8, BufferFormat::k8UNorm, kNormalizedIntegerTypes, 1, BufferFormat::k8Srgb,
     ChannelType::kSrgb},
    {ChannelLayout::k16, BufferFormat::k16UNorm, kNormalizedIntegerTypes, 1, BufferFormat::k16Float,
     ChannelType::kFloat},
    {ChannelLayout::k8_8, BufferFormat::k8_8UNorm, kNormalizedIntegerTypes, 2,
     BufferFormat::k8_8Srgb, ChannelType::kSrgb},
    {ChannelLayout::k32, BufferFormat::k32UInt, kIntegerTypes, 1, BufferFormat::k32Float,
     ChannelType::kFloat},
    {ChannelLayout::k16_16, BufferFormat::k16_16UNorm, kNormalizedIntegerTypes, 2,
     BufferFormat::k16_16Float, ChannelType::kFloat},
    {ChannelLayout::k11_11_10, BufferFormat::k11_11_10UNorm, kNormalizedIntegerTypes, 3,
     BufferFormat::k11_11_10Float, ChannelType::kFloat},
    {ChannelLayout::k10_11_11, BufferFormat::k10_11_11UNorm, kNormalizedIntegerTypes, 3,
     BufferFormat::k10_11_11Float, ChannelType::kFloat},
    {ChannelLayout::k2_10_10_10, BufferFormat::k2_10_10_10UNorm, kNormalizedIntegerTypes, 4},
    {ChannelLayout::k10_10_10_2, BufferFormat::k10_10_10_2UNorm, kNormalizedIntegerTypes, 4},
    {ChannelLayout::k8_8_8_8, BufferFormat::k8_8_8_8UNorm, kNormalizedIntegerTypes, 4,
     BufferFormat::k8_8_8_8Srgb, ChannelType::kSrgb},
    {ChannelLayout::k32_32, BufferFormat::k32_32UInt, kIntegerTypes, 2, BufferFormat::k32_32Float,
     ChannelType::kFloat},
    {ChannelLayout::k16_16_16_16, BufferFormat::k16_16_16_16UNorm, kNormalizedIntegerTypes, 4,
     BufferFormat::k16_16_16_16Float, ChannelType::kFloat},
    {ChannelLayout::k32_32_32_32, BufferFormat::k32_32_32_32UInt, kIntegerTypes, 4,
     BufferFormat::k32_32_32_32Float, ChannelType::kFloat},
    {ChannelLayout::k5_6_5, BufferFormat::k5_6_5UNorm, kUNormType, 3},
    {ChannelLayout::k5_5_5_1, BufferFormat::k5_5_5_1UNorm, kUNormType, 4},
    {ChannelLayout::k1_5_5_5, BufferFormat::k1_5_5_5UNorm, kUNormType, 4},
    {ChannelLayout::k4_4_4_4, BufferFormat::k4_4_4_4UNorm, kUNormType, 4},
    {ChannelLayout::k10_10_10_2Float, BufferFormat::kInvalid, 0, 4, BufferFormat::k10_10_10_2Float,
     ChannelType::kFloat, ChannelOrderSupport::kStandardOnly},
};

const RenderTargetLayoutInfo* FindRenderTargetLayout(uint32_t layout) {
	for (const auto& info: kRenderTargetLayouts) {
		if (GpuEnumValue(info.layout) == layout) {
			return &info;
		}
	}
	return nullptr;
}

constexpr auto MakeFormatInfoLookup() {
	constexpr uint32_t                            kMaxFormat = GpuEnumValue(BufferFormat::kBc7Srgb);
	std::array<const FormatInfo*, kMaxFormat + 1> lookup {};
	for (const auto& info: kFormatInfo) {
		lookup[info.format] = &info;
	}
	return lookup;
}

constexpr auto kFormatInfoLookup = MakeFormatInfoLookup();

const FormatInfo* FindFormatInfo(uint32_t format) {
	return format < kFormatInfoLookup.size() ? kFormatInfoLookup[format] : nullptr;
}

} // namespace

RenderTargetFormatEncoding ResolveRenderTargetFormat(uint32_t layout, uint32_t type) {
	const auto* layout_info = FindRenderTargetLayout(layout);
	if (layout_info == nullptr || type >= 8u) {
		return {};
	}

	const auto channel_type = static_cast<ChannelType>(type);
	auto       format       = BufferFormat::kInvalid;
	if (layout_info->extra_format != BufferFormat::kInvalid &&
	    channel_type == layout_info->extra_type) {
		format = layout_info->extra_format;
	} else if ((layout_info->linear_type_mask & (1u << type)) != 0) {
		const auto first_type =
		    std::countr_zero(static_cast<uint32_t>(layout_info->linear_type_mask));
		format = static_cast<BufferFormat>(GpuEnumValue(layout_info->first_linear_format) + type -
		                                   first_type);
	}
	if (format == BufferFormat::kInvalid) {
		return {};
	}
	return {format, layout_info->components, layout_info->order_support};
}

uint32_t NumBytesPerElement(uint32_t format) {
	const auto* info = FindFormatInfo(format);
	return info != nullptr ? info->bytes_per_element : 0;
}

uint32_t BlockCompressedBytesPerBlock(uint32_t format) {
	const auto* info = FindFormatInfo(format);
	return info != nullptr ? info->block_compressed_bytes_per_block : 0;
}

uint32_t RenderTargetBytesPerElement(uint32_t format) {
	const auto* info = FindFormatInfo(format);
	return info != nullptr ? info->render_target_bytes_per_element : 0;
}

bool IsSupportedTextureFormat(uint32_t format) {
	const auto* info = FindFormatInfo(format);
	return info != nullptr && info->supported_sampled_texture_format;
}

bool IsUintTextureFormat(uint32_t format) {
	const auto* info = FindFormatInfo(format);
	return info != nullptr && info->unsigned_integer_texture_format;
}

bool IsFmaskTextureFormat(uint32_t format) {
	return format == GpuEnumValue(BufferFormat::kFmask8_S4_F4);
}

} // namespace Libs::Graphics::Prospero

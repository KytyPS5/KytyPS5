#include "graphics/guest_gpu/gpu_format.h"

#include "graphics/guest_gpu/gpu_defs.h"

#include <array>

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

bool IsShaderDecodedSrgbTextureFormat(uint32_t format) {
	// Deliberately an explicit two-case test rather than a column in the format table,
	// because this is not a property of the guest format on its own - it is a property of
	// what the host can express. k8Srgb and k8_8Srgb are created as unorm images (see the
	// comment on their entries in vulkanCommon.cpp) because R8_SRGB and R8G8_SRGB are
	// optional in Vulkan and plenty of devices expose no features for them at all, so their
	// sRGB decode is owed by the shader. Every other guest sRGB format has a real host
	// counterpart - k8_8_8_8Srgb maps to eR8G8B8A8Srgb and each BC*Srgb to a native sRGB
	// block format - and the sampler already decodes those. A shared table column would
	// sweep them in and decode them twice.
	return format == GpuEnumValue(BufferFormat::k8Srgb) ||
	       format == GpuEnumValue(BufferFormat::k8_8Srgb);
}

} // namespace Libs::Graphics::Prospero

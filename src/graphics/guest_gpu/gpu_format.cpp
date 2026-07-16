#include "graphics/guest_gpu/gpu_format.h"

#include "graphics/guest_gpu/gpu_defs.h"

#include <array>

namespace Libs::Graphics::Prospero {
namespace {

constexpr FormatInfo kFormatInfo[] = {
    {GpuEnumValue(BufferFormat::k8UNorm), VK_FORMAT_R8_UNORM, 1, 0, 1, true, false},
    {GpuEnumValue(BufferFormat::k8SNorm), VK_FORMAT_UNDEFINED, 0, 0, 1, false, false},
    {GpuEnumValue(BufferFormat::k8UInt), VK_FORMAT_R8_UINT, 1, 0, 1, true, true},
    {GpuEnumValue(BufferFormat::k16UNorm), VK_FORMAT_R16_UNORM, 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k16SNorm), VK_FORMAT_R16_SNORM, 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k16UInt), VK_FORMAT_R16_UINT, 2, 0, 2, true, true},
    {GpuEnumValue(BufferFormat::k16SInt), VK_FORMAT_R16_SINT, 2, 0, 2, false, false},
    {GpuEnumValue(BufferFormat::k16Float), VK_FORMAT_R16_SFLOAT, 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k8_8UNorm), VK_FORMAT_R8G8_UNORM, 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k8_8SNorm), VK_FORMAT_R8G8_SNORM, 2, 0, 2, true, false},
    {GpuEnumValue(BufferFormat::k8_8UInt), VK_FORMAT_R8G8_UINT, 2, 0, 2, true, true},
    {GpuEnumValue(BufferFormat::k8_8SInt), VK_FORMAT_R8G8_SINT, 2, 0, 2, false, false},
    {GpuEnumValue(BufferFormat::k32UInt), VK_FORMAT_R32_UINT, 4, 0, 4, true, true},
    {GpuEnumValue(BufferFormat::k32SInt), VK_FORMAT_R32_SINT, 4, 0, 4, false, false},
    {GpuEnumValue(BufferFormat::k32Float), VK_FORMAT_R32_SFLOAT, 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k16_16UNorm), VK_FORMAT_R16G16_UNORM, 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k16_16SNorm), VK_FORMAT_R16G16_SNORM, 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k16_16UInt), VK_FORMAT_R16G16_UINT, 4, 0, 4, true, true},
    {GpuEnumValue(BufferFormat::k16_16SInt), VK_FORMAT_R16G16_SINT, 4, 0, 4, false, false},
    {GpuEnumValue(BufferFormat::k16_16Float), VK_FORMAT_R16G16_SFLOAT, 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k11_11_10Float), VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 0, 4,
     true, false},
    {GpuEnumValue(BufferFormat::k10_10_10_2UNorm), VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 0, 4,
     true, false},
    {GpuEnumValue(BufferFormat::k8_8_8_8UNorm), VK_FORMAT_R8G8B8A8_UNORM, 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k8_8_8_8SNorm), VK_FORMAT_R8G8B8A8_SNORM, 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k8_8_8_8UInt), VK_FORMAT_R8G8B8A8_UINT, 4, 0, 4, true, true},
    {GpuEnumValue(BufferFormat::k8_8_8_8SInt), VK_FORMAT_R8G8B8A8_SINT, 4, 0, 4, false, false},
    {GpuEnumValue(BufferFormat::k32_32UInt), VK_FORMAT_R32G32_UINT, 8, 0, 8, true, true},
    {GpuEnumValue(BufferFormat::k32_32SInt), VK_FORMAT_R32G32_SINT, 8, 0, 8, false, false},
    {GpuEnumValue(BufferFormat::k32_32Float), VK_FORMAT_R32G32_SFLOAT, 8, 0, 8, true, false},
    {GpuEnumValue(BufferFormat::k16_16_16_16UNorm), VK_FORMAT_R16G16B16A16_UNORM, 8, 0, 8, true,
     false},
    {GpuEnumValue(BufferFormat::k16_16_16_16SNorm), VK_FORMAT_R16G16B16A16_SNORM, 8, 0, 8, true,
     false},
    {GpuEnumValue(BufferFormat::k16_16_16_16UInt), VK_FORMAT_R16G16B16A16_UINT, 8, 0, 8, true,
     true},
    {GpuEnumValue(BufferFormat::k16_16_16_16SInt), VK_FORMAT_R16G16B16A16_SINT, 8, 0, 8, false,
     false},
    {GpuEnumValue(BufferFormat::k16_16_16_16Float), VK_FORMAT_R16G16B16A16_SFLOAT, 8, 0, 8, true,
     false},
    {GpuEnumValue(BufferFormat::k32_32_32UInt), VK_FORMAT_R32G32B32_UINT, 12, 0, 12, true, true},
    {GpuEnumValue(BufferFormat::k32_32_32SInt), VK_FORMAT_R32G32B32_SINT, 12, 0, 12, false,
     false},
    {GpuEnumValue(BufferFormat::k32_32_32Float), VK_FORMAT_R32G32B32_SFLOAT, 12, 0, 12, true,
     false},
    {GpuEnumValue(BufferFormat::k32_32_32_32UInt), VK_FORMAT_R32G32B32A32_UINT, 16, 0, 16, true,
     true},
    {GpuEnumValue(BufferFormat::k32_32_32_32SInt), VK_FORMAT_R32G32B32A32_SINT, 16, 0, 16, false,
     false},
    {GpuEnumValue(BufferFormat::k32_32_32_32Float), VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0, 16,
     true, false},
    {GpuEnumValue(BufferFormat::k8_8_8_8Srgb), VK_FORMAT_R8G8B8A8_SRGB, 4, 0, 4, true, false},
    {GpuEnumValue(BufferFormat::k9_9_9_5Float), VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, 0, 0, true,
     false},
    {GpuEnumValue(BufferFormat::k5_6_5UNorm), VK_FORMAT_B5G6R5_UNORM_PACK16, 2, 0, 2, true,
     false},
    {GpuEnumValue(BufferFormat::k5_5_5_1UNorm), VK_FORMAT_R5G5B5A1_UNORM_PACK16, 2, 0, 2, true,
     false},
    {GpuEnumValue(BufferFormat::k4_4_4_4UNorm), VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, 0, 2, true,
     false},
    {GpuEnumValue(BufferFormat::kFmask8_S4_F4), VK_FORMAT_R32_SFLOAT, 1, 0, 1, true, false},
    {GpuEnumValue(BufferFormat::kBc1UNorm), VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0, 8, 0, true,
     false},
    {GpuEnumValue(BufferFormat::kBc1Srgb), VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 0, 8, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc2UNorm), VK_FORMAT_BC2_UNORM_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc2Srgb), VK_FORMAT_BC2_SRGB_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc3UNorm), VK_FORMAT_BC3_UNORM_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc3Srgb), VK_FORMAT_BC3_SRGB_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc4UNorm), VK_FORMAT_BC4_UNORM_BLOCK, 0, 8, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc4SNorm), VK_FORMAT_BC4_SNORM_BLOCK, 0, 8, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc5UNorm), VK_FORMAT_BC5_UNORM_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc5SNorm), VK_FORMAT_BC5_SNORM_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc6UFloat), VK_FORMAT_BC6H_UFLOAT_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc6SFloat), VK_FORMAT_BC6H_SFLOAT_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc7UNorm), VK_FORMAT_BC7_UNORM_BLOCK, 0, 16, 0, true, false},
    {GpuEnumValue(BufferFormat::kBc7Srgb), VK_FORMAT_BC7_SRGB_BLOCK, 0, 16, 0, true, false},
};

constexpr auto MakeFormatInfoLookup() {
	constexpr uint32_t kMaxFormat = GpuEnumValue(BufferFormat::kBc7Srgb);
	std::array<const FormatInfo*, kMaxFormat + 1> lookup {};
	for (const auto& info: kFormatInfo) {
		lookup[info.format] = &info;
	}
	return lookup;
}

constexpr auto kFormatInfoLookup = MakeFormatInfoLookup();

} // namespace

std::span<const FormatInfo> Formats() {
	return kFormatInfo;
}

const FormatInfo* FindFormatInfo(uint32_t format) {
	return format < kFormatInfoLookup.size() ? kFormatInfoLookup[format] : nullptr;
}

VkFormat SurfaceFormat(uint32_t format) {
	const auto* info = FindFormatInfo(format);
	return info != nullptr ? info->vk_format : VK_FORMAT_UNDEFINED;
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

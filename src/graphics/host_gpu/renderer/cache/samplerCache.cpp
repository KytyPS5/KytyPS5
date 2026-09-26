#include "graphics/host_gpu/renderer/cache/samplerCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/renderContext.h"

namespace Libs::Graphics {

SamplerCache::~SamplerCache() {
	for (const auto& [key, sampler]: m_samplers) {
		(void)key;
		m_graphics.device.destroySampler(sampler, nullptr);
	}
}

vk::Sampler SamplerCache::GetSampler(const ShaderSamplerResource& r, Variant variant) {
	Common::LockGuard lock(m_mutex);

	const uint32_t variant_bits =
	    (variant.depth_compare ? 1u : 0u) | (variant.integer_border ? 2u : 0u);
	const SamplerKey key {r.fields[0], r.fields[1], r.fields[2], r.fields[3], variant_bits};
	if (auto iter = m_samplers.find(key); iter != m_samplers.end()) {
		return iter->second;
	}

	float      aniso_ratio = 1.0f;
	const auto mag_filter  = r.XyMagFilter();
	const auto min_filter  = r.XyMinFilter();

	auto is_aniso_filter = [](uint8_t filter) {
		switch (static_cast<Prospero::SamplerFilter>(filter)) {
			case Prospero::SamplerFilter::kAnisoPoint:
			case Prospero::SamplerFilter::kAnisoLinear: return true;
			case Prospero::SamplerFilter::kPoint:
			case Prospero::SamplerFilter::kBilinear: return false;
			default: EXIT("unknown sampler filter: %u\n", filter);
		}
		return false;
	};

	auto to_vk_filter = [](uint8_t filter) {
		switch (static_cast<Prospero::SamplerFilter>(filter)) {
			case Prospero::SamplerFilter::kPoint:
			case Prospero::SamplerFilter::kAnisoPoint: return vk::Filter::eNearest;
			case Prospero::SamplerFilter::kBilinear:
			case Prospero::SamplerFilter::kAnisoLinear: return vk::Filter::eLinear;
			default: EXIT("unknown sampler filter: %u\n", filter);
		}
		return vk::Filter::eNearest;
	};

	const bool aniso = is_aniso_filter(mag_filter) || is_aniso_filter(min_filter);
	if (aniso) {
		switch (static_cast<Prospero::SamplerAnisoRatio>(r.MaxAnisoRatio())) {
			case Prospero::SamplerAnisoRatio::kOne: aniso_ratio = 1.0f; break;
			case Prospero::SamplerAnisoRatio::kTwo: aniso_ratio = 2.0f; break;
			case Prospero::SamplerAnisoRatio::kFour: aniso_ratio = 4.0f; break;
			case Prospero::SamplerAnisoRatio::kEight: aniso_ratio = 8.0f; break;
			case Prospero::SamplerAnisoRatio::kSixteen: aniso_ratio = 16.0f; break;
			default:
				EXIT("unknown ratio: %d dwords=%08x,%08x,%08x,%08x\n",
				     static_cast<int>(r.MaxAnisoRatio()), r.fields[0], r.fields[1], r.fields[2],
				     r.fields[3]);
		}
	}

	const auto mip_filter = r.MipFilter();
	float      min_lod    = 0.0f;
	float      max_lod    = 0.0f;
	if (static_cast<Prospero::SamplerMipFilter>(mip_filter) != Prospero::SamplerMipFilter::kNone) {
		min_lod = static_cast<float>(r.MinLod()) / 256.0f;
		max_lod = static_cast<float>(r.MaxLod()) / 256.0f;
	}

	vk::SamplerCreateInfo sampler_info {};

	auto to_vk_address_mode = [](uint8_t clamp) {
		switch (static_cast<Prospero::SamplerClampMode>(clamp)) {
			case Prospero::SamplerClampMode::kWrap: return vk::SamplerAddressMode::eRepeat;
			case Prospero::SamplerClampMode::kMirror:
				return vk::SamplerAddressMode::eMirroredRepeat;
			case Prospero::SamplerClampMode::kClampLastTexel:
				return vk::SamplerAddressMode::eClampToEdge;
			case Prospero::SamplerClampMode::kMirrorOnceLastTexel:
				return vk::SamplerAddressMode::eMirrorClampToEdge;
			case Prospero::SamplerClampMode::kClampHalfBorder:
				return vk::SamplerAddressMode::eClampToBorder;
			case Prospero::SamplerClampMode::kMirrorOnceHalfBorder:
				return vk::SamplerAddressMode::eMirrorClampToEdge;
			case Prospero::SamplerClampMode::kClampBorder:
				return vk::SamplerAddressMode::eClampToBorder;
			case Prospero::SamplerClampMode::kMirrorOnceBorder:
				return vk::SamplerAddressMode::eMirrorClampToEdge;
			default: EXIT("unknown clamp: %u\n", clamp);
		}
		return vk::SamplerAddressMode::eClampToBorder;
	};

	// The border colour variant must match the numeric type of the sampled view.
	const bool      integer = variant.integer_border;
	vk::BorderColor border =
	    integer ? vk::BorderColor::eIntTransparentBlack : vk::BorderColor::eFloatTransparentBlack;
	switch (static_cast<Prospero::SamplerBorderColor>(r.BorderColorType())) {
		case Prospero::SamplerBorderColor::kTransBlack:
			border = integer ? vk::BorderColor::eIntTransparentBlack
			                 : vk::BorderColor::eFloatTransparentBlack;
			break;
		case Prospero::SamplerBorderColor::kOpaqueBlack:
			border =
			    integer ? vk::BorderColor::eIntOpaqueBlack : vk::BorderColor::eFloatOpaqueBlack;
			break;
		case Prospero::SamplerBorderColor::kOpaqueWhite:
			border =
			    integer ? vk::BorderColor::eIntOpaqueWhite : vk::BorderColor::eFloatOpaqueWhite;
			break;
		case Prospero::SamplerBorderColor::kFromTable:
			LOGF(
			    "temporary: approximating table border color as transparent black, index = %" PRIu16
			    "\n",
			    r.BorderColorPtr());
			border = integer ? vk::BorderColor::eIntTransparentBlack
			                 : vk::BorderColor::eFloatTransparentBlack;
			break;
		default: EXIT("unknown border color: %d", static_cast<int>(r.BorderColorType()));
	}

	sampler_info.magFilter = to_vk_filter(mag_filter);
	sampler_info.minFilter = to_vk_filter(min_filter);
	sampler_info.mipmapMode =
	    (static_cast<Prospero::SamplerMipFilter>(mip_filter) == Prospero::SamplerMipFilter::kLinear
	         ? vk::SamplerMipmapMode::eLinear
	         : vk::SamplerMipmapMode::eNearest);
	sampler_info.addressModeU = to_vk_address_mode(r.ClampX());
	sampler_info.addressModeV = to_vk_address_mode(r.ClampY());
	sampler_info.addressModeW = to_vk_address_mode(r.ClampZ());
	sampler_info.mipLodBias =
	    static_cast<float>(static_cast<int16_t>((r.LodBias() ^ 0x2000u) - 0x2000u)) / 256.0f;
	sampler_info.anisotropyEnable        = (aniso ? VK_TRUE : VK_FALSE);
	sampler_info.maxAnisotropy           = aniso_ratio;
	// Comparison sampling follows the shader's use of the sampler: the descriptor's compare
	// function encodes NEVER as zero, so it cannot enable comparison on its own.
	sampler_info.compareEnable           = (variant.depth_compare ? VK_TRUE : VK_FALSE);
	sampler_info.compareOp               = variant.depth_compare
	                                           ? static_cast<vk::CompareOp>(r.DepthCompareFunc())
	                                           : vk::CompareOp::eNever;
	sampler_info.minLod                  = min_lod;
	sampler_info.maxLod                  = max_lod;
	sampler_info.borderColor             = border;
	sampler_info.unnormalizedCoordinates = (r.ForceUnormCoords() ? VK_TRUE : VK_FALSE);

	if (r.ForceUnormCoords()) {
		sampler_info.addressModeU     = vk::SamplerAddressMode::eClampToEdge;
		sampler_info.addressModeV     = vk::SamplerAddressMode::eClampToEdge;
		sampler_info.addressModeW     = vk::SamplerAddressMode::eClampToEdge;
		sampler_info.mipmapMode       = vk::SamplerMipmapMode::eNearest;
		sampler_info.minLod           = 0.0f;
		sampler_info.maxLod           = 0.0f;
		sampler_info.anisotropyEnable = VK_FALSE;
		sampler_info.maxAnisotropy    = 1.0f;
		sampler_info.compareEnable    = VK_FALSE;
		sampler_info.mipLodBias       = 0.0f;
	}

	vk::Sampler vk_sampler = nullptr;
	const auto  result     = m_graphics.device.createSampler(&sampler_info, nullptr, &vk_sampler);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || vk_sampler == nullptr);

	m_samplers.emplace(key, vk_sampler);
	return vk_sampler;
}

} // namespace Libs::Graphics

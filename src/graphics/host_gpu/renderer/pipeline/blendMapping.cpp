#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"

#include "graphics/guest_gpu/hardwareContext.h"

#include <initializer_list>

namespace Libs::Graphics {

bool BlendFactorIsDualSource(uint8_t factor) {
	return factor >= static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color) &&
	       factor <= static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
}

namespace {

bool BlendFactorIsConstantColor(uint8_t factor) {
	return factor == static_cast<uint8_t>(Prospero::BlendFactor::kConstantColor) ||
	       factor == static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusConstantColor);
}

} // namespace

BlendMappingSupport ClassifyBlendMapping(const HW::BlendControl&                blend,
                                         const Prospero::ColorComponentMapping& mapping) {
	// Color constants are not swizzled with the exports; scalar constant alpha is unaffected.
	if (!mapping.IsIdentity() && (BlendFactorIsConstantColor(blend.color_srcblend) ||
	                              BlendFactorIsConstantColor(blend.color_destblend))) {
		return BlendMappingSupport::Unsupported;
	}
	if (mapping.Map(3) == 3u) {
		return BlendMappingSupport::Direct;
	}
	// Moving alpha requires the same equation for all channels.
	// A mapping that sends all four components to four different slots is a plain permutation:
	// every component the shader produces still lands somewhere, and the one that landed in alpha
	// is a known component, so the caller can put the shader's alpha back before blending. With
	// that available a differing alpha equation stops being fatal -- what moved is the alpha slot,
	// not the blend becoming inexpressible.
	uint32_t seen  = 0;
	for (uint32_t component = 0; component < 4u; component++) {
		seen |= 1u << mapping.Map(component);
	}
	const bool permutation  = seen == 0xfu;
	const bool alpha_differs =
	    blend.separate_alpha_blend &&
	    (blend.alpha_srcblend != blend.color_srcblend ||
	     blend.alpha_destblend != blend.color_destblend || blend.alpha_comb_fcn != blend.color_comb_fcn);
	// Moving alpha requires the same equation for all channels unless the permutation lets the
	// caller restore the alpha slot.
	if (alpha_differs && !permutation) {
		return BlendMappingSupport::Unsupported;
	}
	auto support = BlendMappingSupport::Direct;
	for (const auto factor: {blend.color_srcblend, blend.color_destblend}) {
		if (BlendFactorIsDualSource(factor)) {
			return BlendMappingSupport::Unsupported;
		}
		switch (static_cast<Prospero::BlendFactor>(factor)) {
			case Prospero::BlendFactor::kSrcAlpha:
			case Prospero::BlendFactor::kOneMinusSrcAlpha:
				support = BlendMappingSupport::SourceAlpha;
				break;
			case Prospero::BlendFactor::kDstAlpha:
			case Prospero::BlendFactor::kOneMinusDstAlpha:
			case Prospero::BlendFactor::kSrcAlphaSaturate: return BlendMappingSupport::Unsupported;
			default: break;
		}
	}
	if (support == BlendMappingSupport::SourceAlpha && permutation) {
		return BlendMappingSupport::PermutedSourceAlpha;
	}
	return support;
}

} // namespace Libs::Graphics

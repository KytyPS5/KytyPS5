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
	const auto color_op = static_cast<Prospero::BlendOp>(blend.color_comb_fcn);
	if ((color_op == Prospero::BlendOp::kMin || color_op == Prospero::BlendOp::kMax) &&
	    (!blend.separate_alpha_blend || blend.alpha_comb_fcn == blend.color_comb_fcn)) {
		// MIN/MAX compare unscaled components, so neither channel factors nor
		// source/destination alpha relocation change this common equation.
		return BlendMappingSupport::Direct;
	}
	// Color constants are not swizzled with the exports; scalar constant alpha is unaffected.
	if (!mapping.IsIdentity() && (BlendFactorIsConstantColor(blend.color_srcblend) ||
	                              BlendFactorIsConstantColor(blend.color_destblend))) {
		return BlendMappingSupport::Unsupported;
	}
	if (mapping.Map(3) == 3u) {
		return BlendMappingSupport::Direct;
	}
	if (blend.separate_alpha_blend &&
	    blend.color_srcblend == static_cast<uint8_t>(Prospero::BlendFactor::kSrcAlpha) &&
	    blend.color_destblend == static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrcAlpha) &&
	    blend.alpha_destblend == blend.color_destblend &&
	    blend.color_comb_fcn == static_cast<uint8_t>(Prospero::BlendOp::kAdd) &&
	    blend.alpha_comb_fcn == blend.color_comb_fcn) {
		switch (static_cast<Prospero::BlendFactor>(blend.alpha_srcblend)) {
			case Prospero::BlendFactor::kZero: return BlendMappingSupport::SourceAlphaZero;
			case Prospero::BlendFactor::kOne: return BlendMappingSupport::SourceAlphaOne;
			default: break;
		}
	}
	// Moving alpha requires the same equation for all channels.
	if (blend.separate_alpha_blend && (blend.alpha_srcblend != blend.color_srcblend ||
	                                   blend.alpha_destblend != blend.color_destblend ||
	                                   blend.alpha_comb_fcn != blend.color_comb_fcn)) {
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
	return support;
}

} // namespace Libs::Graphics

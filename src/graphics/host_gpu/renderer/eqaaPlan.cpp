#include "graphics/host_gpu/renderer/eqaaPlan.h"

#include <algorithm>
#include <bit>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace {

// The Vulkan standard locations, which the D3D and AMD standard tables match for 2/4/8/16.
constexpr std::array<uint8_t, 2>  Standard2 {0xcc, 0x44};
constexpr std::array<uint8_t, 4>  Standard4 {0x26, 0x6e, 0xa2, 0xea};
constexpr std::array<uint8_t, 8>  Standard8 {0x59, 0xb7, 0x9d, 0x35, 0xd3, 0x71, 0xfb, 0x1f};
constexpr std::array<uint8_t, 16> Standard16 {0x99, 0x57, 0xa5, 0x7c, 0x63, 0xda, 0xbd, 0x3b,
                                              0xe6, 0x18, 0x24, 0xc2, 0x80, 0x4f, 0xfe, 0x01};

using QuadPositions = std::array<std::array<uint8_t, 16>, 4>;

bool IsStandardPattern(const std::array<uint8_t, 16>& pattern, uint32_t samples) {
	const auto equal = [&](const auto& table) {
		return std::equal(table.begin(), table.end(), pattern.begin());
	};
	switch (samples) {
		case 2: return equal(Standard2);
		case 4: return equal(Standard4);
		case 8: return equal(Standard8);
		case 16: return equal(Standard16);
		default: return false;
	}
}

bool LocationInRange(uint8_t xy, const EqaaHostCaps& caps) {
	const float x = static_cast<float>(xy & 0xfu) / 16.0f;
	const float y = static_cast<float>(xy >> 4u) / 16.0f;
	return x >= caps.coord_min && x <= caps.coord_max && y >= caps.coord_min && y <= caps.coord_max;
}

uint32_t QuadAaMask(const std::array<uint32_t, 2>& mask, uint32_t quad) {
	return (mask[quad >> 1u] >> (16u * (quad & 1u))) & 0xffffu;
}

EqaaPlan Fallback(EqaaPlan plan, EqaaFallbackReason reason) {
	plan.mode   = EqaaMode::Fallback;
	plan.reason = reason;
	return plan;
}

// The targets render their stored samples at the host's standard locations.
EqaaPlan FragmentApprox(EqaaPlan plan, EqaaFallbackReason reason) {
	plan.mode   = EqaaMode::FragmentApprox;
	plan.reason = reason;
	return plan;
}

bool HostSupportsCount(const EqaaHostCaps& caps, uint32_t samples) {
	return caps.sample_locations && caps.sub_pixel_bits >= 4 && (caps.sample_counts & samples) != 0;
}

EqaaPlan PlanNative(EqaaPlan plan, const QuadPositions& positions, uint32_t samples,
                    const EqaaInputs& inputs, const EqaaHostCaps& caps, EqaaMode failure) {
	const auto fail = [&](EqaaFallbackReason reason) {
		return failure == EqaaMode::Fallback ? Fallback(plan, reason)
		                                     : FragmentApprox(plan, reason);
	};
	const bool uniform = std::ranges::all_of(positions, [&](const auto& quad) {
		return std::equal(quad.begin(), quad.begin() + samples, positions[0].begin());
	});
	if (!uniform) {
		return fail(EqaaFallbackReason::NonUniformNative);
	}
	if (IsStandardPattern(positions[0], samples)) {
		return plan;
	}
	// Custom locations on a depth attachment need a sample-locations-compatible image.
	if (inputs.has_depth) {
		return fail(EqaaFallbackReason::NativeDepth);
	}
	if (!HostSupportsCount(caps, samples) ||
	    !std::all_of(positions[0].begin(), positions[0].begin() + samples,
	                 [&](uint8_t xy) { return LocationInRange(xy, caps); })) {
		return fail(EqaaFallbackReason::HostUnsupported);
	}
	plan.mode          = EqaaMode::Native;
	plan.host_samples  = static_cast<uint8_t>(samples);
	plan.pattern.count = static_cast<uint8_t>(samples);
	std::copy_n(positions[0].begin(), samples, plan.pattern.xy.begin());
	for (auto& quad: plan.host_of_guest) {
		for (uint32_t sample = 0; sample < samples; sample++) {
			quad[sample] = static_cast<uint8_t>(sample);
		}
	}
	const auto full = (1u << samples) - 1u;
	for (uint32_t quad = 0; quad < 4; quad++) {
		if ((QuadAaMask(inputs.aa_mask, quad) & full) != full) {
			plan.inexact |= AaMaskIgnored;
		}
	}
	plan.inexact |= inputs.ps_reads_coverage ? CoverageInputUnmapped : 0u;
	return plan;
}

// One stored sample: guest sample 0 is the anchor, and a host shadow holds every guest location.
EqaaPlan PlanAnchorShadow(EqaaPlan plan, const QuadPositions& positions, uint32_t guest_samples,
                          const EqaaInputs& inputs, const EqaaHostCaps& caps) {
	uint32_t max_samples = 0;
	for (uint32_t count = 16; count != 0; count >>= 1u) {
		if (HostSupportsCount(caps, count)) {
			max_samples = count;
			break;
		}
	}
	const auto unique = [](std::vector<uint8_t> values) {
		std::ranges::sort(values);
		values.erase(std::unique(values.begin(), values.end()), values.end());
		return values;
	};
	std::vector<uint8_t> all;
	std::vector<uint8_t> anchors;
	for (const auto& quad: positions) {
		all.insert(all.end(), quad.begin(), quad.begin() + guest_samples);
		anchors.push_back(quad[0]);
	}
	auto host = unique(all);
	if (host.size() > max_samples) {
		// Colour and depth stay exact; only the pixel-shader invocation set grows.
		host = unique(anchors);
		plan.inexact |= PsSuperset;
	}
	if (max_samples < 2 || host.size() > max_samples ||
	    !std::ranges::all_of(host, [&](uint8_t xy) { return LocationInRange(xy, caps); })) {
		return Fallback(plan, max_samples < 2 ? EqaaFallbackReason::HostUnsupported
		                                      : EqaaFallbackReason::PatternTooLarge);
	}
	uint32_t host_samples = std::max<uint32_t>(std::bit_ceil(host.size()), 2u);
	while (host_samples <= max_samples && !HostSupportsCount(caps, host_samples)) {
		host_samples <<= 1u;
	}
	if (host_samples > max_samples) {
		return Fallback(plan, EqaaFallbackReason::HostUnsupported);
	}
	plan.mode          = EqaaMode::AnchorShadow;
	plan.host_samples  = static_cast<uint8_t>(host_samples);
	plan.pattern.count = static_cast<uint8_t>(host_samples);
	for (uint32_t index = 0; index < host_samples; index++) {
		plan.pattern.xy[index] = index < host.size() ? host[index] : host[0];
	}
	for (uint32_t quad = 0; quad < 4; quad++) {
		bool same_set = true;
		for (uint32_t sample = 0; sample < guest_samples; sample++) {
			const auto found = std::ranges::find(host, positions[quad][sample]);
			same_set         = same_set && found != host.end();
			plan.host_of_guest[quad][sample] =
			    found != host.end() ? static_cast<uint8_t>(found - host.begin()) : uint8_t {0};
		}
		const auto quad_set =
		    unique({positions[quad].begin(), positions[quad].begin() + guest_samples});
		if (!same_set || quad_set != host) {
			plan.inexact |= PsSuperset;
		}
		plan.anchor_select |= static_cast<uint16_t>(plan.host_of_guest[quad][0] << (4u * quad));
		if ((QuadAaMask(inputs.aa_mask, quad) & 1u) == 0) {
			plan.inexact |= AaMaskIgnored;
		}
	}
	plan.inexact |= inputs.ps_reads_coverage ? CoverageInputUnmapped : 0u;
	plan.inexact |= inputs.alpha_to_mask ? AlphaToMaskHost : 0u;
	plan.inexact |= inputs.ps_sample_rate ? SampleRateShading : 0u;
	plan.inexact |= inputs.mask_export ? MaskExportUnmapped : 0u;
	plan.inexact |= inputs.ps_centroid ? CentroidHostDefined : 0u;
	return plan;
}

} // namespace

EqaaPlan PlanEqaa(const EqaaInputs& inputs, const EqaaHostCaps& caps) {
	EqaaPlan plan;
	if (inputs.msaa_num_samples == 0 || inputs.msaa_num_samples > 4) {
		return plan;
	}
	const uint32_t guest_samples = 1u << inputs.msaa_num_samples;
	if (!inputs.msaa_enable) {
		plan.inexact = MsaaDisabled;
		return plan;
	}
	if (inputs.color_count == 0 && !inputs.has_depth) {
		return plan;
	}
	plan.guest_samples = static_cast<uint8_t>(guest_samples);

	QuadPositions positions {};
	for (uint32_t quad = 0; quad < 4; quad++) {
		for (uint32_t sample = 0; sample < guest_samples; sample++) {
			positions[quad][sample] = GuestSampleLocation(inputs.locations, quad, sample);
		}
	}
	const auto colors = std::span {inputs.colors}.first(std::min<size_t>(inputs.color_count, 8));
	const auto all_colors = [&](auto&& predicate) {
		return std::ranges::all_of(colors, predicate);
	};

	const auto n = inputs.msaa_num_samples;
	if (all_colors(
	        [&](const EqaaTargetSamples& c) { return c.samples == n && c.fragments == n; }) &&
	    (!inputs.has_depth || inputs.depth_samples == n)) {
		return PlanNative(plan, positions, guest_samples, inputs, caps, EqaaMode::Fallback);
	}

	const uint8_t stored = colors.empty() ? inputs.depth_samples : colors.front().fragments;
	if (stored < n && all_colors([&](const EqaaTargetSamples& c) {
		    return c.fragments == stored && (c.samples == stored || c.samples == n);
	    }) &&
	    (!inputs.has_depth || inputs.depth_samples == stored)) {
		const auto fail = [&](EqaaFallbackReason reason) {
			return stored == 0 ? Fallback(plan, reason) : FragmentApprox(plan, reason);
		};
		// FMASK records which fragment covers each of the other samples; it is not emulated.
		const bool fmask = std::ranges::any_of(
		    colors, [](const EqaaTargetSamples& c) { return c.samples > c.fragments; });
		plan.inexact |= fmask ? FmaskUnmodelled : 0u;
		// The CB keeps every anchor's fragment and drops unanchored ones it has no room for.
		if (fmask && inputs.max_anchor_samples != stored) {
			return fail(EqaaFallbackReason::AnchorCount);
		}
		plan.color_rule =
		    inputs.overrasterization_amount == 0 && !inputs.enable_postz_overrasterization
		        ? EqaaColorRule::AnchorOnly
		        : EqaaColorRule::OrReduce;
		if (plan.color_rule == EqaaColorRule::OrReduce) {
			return fail(EqaaFallbackReason::OrReduce);
		}
		if (stored == 0) {
			return PlanAnchorShadow(plan, positions, guest_samples, inputs, caps);
		}
		// Pixels covered only at unstored samples launch the guest PS but not the host's.
		plan.inexact |= PsSubset;
		return PlanNative(plan, positions, 1u << stored, inputs, caps, EqaaMode::FragmentApprox);
	}

	if (std::ranges::any_of(colors,
	                        [](const EqaaTargetSamples& c) { return c.samples > c.fragments; })) {
		return FragmentApprox(plan, EqaaFallbackReason::MixedSampleCounts);
	}
	return Fallback(plan, EqaaFallbackReason::MixedSampleCounts);
}

} // namespace Libs::Graphics

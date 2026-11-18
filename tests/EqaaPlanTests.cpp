#include "graphics/host_gpu/renderer/eqaaPlan.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "EqaaPlanTests: failed: %s\n", message);
		std::abort();
	}
}

// An NVIDIA-like host: 1..16 samples with custom locations, 4 sub-pixel bits.
EqaaHostCaps NvidiaCaps() {
	return {.sample_locations = true,
	        .sample_counts    = 1u | 2u | 4u | 8u | 16u,
	        .coord_min        = 0.0f,
	        .coord_max        = 0.9375f,
	        .sub_pixel_bits   = 4};
}

// Per quad pixel q, register k = locations[q * 4 + k] holds samples 4k..4k+3.
uint32_t Pack(int x0, int y0, int x1 = 0, int y1 = 0, int x2 = 0, int y2 = 0, int x3 = 0,
              int y3 = 0) {
	const auto nibble = [](int v) { return static_cast<uint32_t>(v) & 0xfu; };
	return nibble(x0) | (nibble(y0) << 4u) | (nibble(x1) << 8u) | (nibble(y1) << 12u) |
	       (nibble(x2) << 16u) | (nibble(y2) << 20u) | (nibble(x3) << 24u) | (nibble(y3) << 28u);
}

// RE2's checkerboard: two raster samples mirrored at +-0.25 px, swapped between row pairs.
EqaaInputs CheckerboardInputs(bool odd_frame) {
	EqaaInputs inputs;
	inputs.msaa_num_samples     = 1;
	inputs.msaa_exposed_samples = 1;
	inputs.msaa_enable          = true;
	const auto even_rows        = odd_frame ? Pack(-4, 0, 4, 0) : Pack(4, 0, -4, 0);
	const auto odd_rows         = odd_frame ? Pack(4, 0, -4, 0) : Pack(-4, 0, 4, 0);
	inputs.locations[0] = inputs.locations[4] = even_rows;
	inputs.locations[8] = inputs.locations[12] = odd_rows;
	inputs.color_count                         = 1;
	inputs.colors[0]                           = {0, 0};
	inputs.has_depth                           = true;
	inputs.depth_samples                       = 0;
	return inputs;
}

void TestCheckerboard() {
	const auto frame_n = PlanEqaa(CheckerboardInputs(false), NvidiaCaps());
	Check(frame_n.mode == EqaaMode::AnchorShadow, "checkerboard is not an anchor shadow");
	Check(frame_n.color_rule == EqaaColorRule::AnchorOnly, "checkerboard colour rule");
	Check(frame_n.host_samples == 2 && frame_n.pattern.count == 2, "checkerboard host count");
	Check(frame_n.pattern.xy[0] == 0x84 && frame_n.pattern.xy[1] == 0x8c,
	      "checkerboard host pattern is not the sorted union {-0.25, +0.25}");
	Check(frame_n.anchor_select == 0x0011, "frame N anchor select");
	Check(frame_n.inexact == 0, "checkerboard reported an inexact part");
	Check(frame_n.host_of_guest[0][1] == 0 && frame_n.host_of_guest[2][1] == 1,
	      "the non-anchor sample maps to the other host sample");

	const auto frame_n1 = PlanEqaa(CheckerboardInputs(true), NvidiaCaps());
	Check(frame_n1.mode == EqaaMode::AnchorShadow && frame_n1.anchor_select == 0x1100,
	      "frame N+1 anchor select");
	Check(frame_n1.pattern == frame_n.pattern,
	      "the frame flip changed the host pattern, so the pipeline key would change");
}

void TestStandardPatternsAreOff() {
	struct Case {
		uint8_t                 log2;
		std::array<uint32_t, 4> words;
	};
	const std::array cases {
	    Case {1, {Pack(4, 4, -4, -4)}},
	    Case {2, {Pack(-2, -6, 6, -2, -6, 2, 2, 6)}},
	    Case {3, {Pack(1, -3, -1, 3, 5, 1, -3, -5), Pack(-5, 5, -7, -1, 3, 7, 7, -7)}},
	};
	for (const auto& test: cases) {
		EqaaInputs inputs;
		inputs.msaa_num_samples = test.log2;
		inputs.msaa_enable      = true;
		for (uint32_t quad = 0; quad < 4; quad++) {
			for (uint32_t k = 0; k < 4; k++) {
				inputs.locations[quad * 4 + k] = test.words[k];
			}
		}
		inputs.color_count = 1;
		inputs.colors[0]   = {test.log2, test.log2};
		Check(PlanEqaa(inputs, NvidiaCaps()).mode == EqaaMode::Off,
		      "a standard native MSAA pattern did not keep the default locations");
	}
}

void TestNativeCustom() {
	EqaaInputs inputs;
	inputs.msaa_num_samples = 2;
	inputs.msaa_enable      = true;
	for (uint32_t quad = 0; quad < 4; quad++) {
		inputs.locations[quad * 4] = Pack(-3, -3, 3, -3, -3, 3, 3, 3);
	}
	inputs.color_count = 1;
	inputs.colors[0]   = {2, 2};
	const auto plan    = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Native && plan.host_samples == 4 && plan.pattern.count == 4,
	      "uniform custom 4x locations are not native");
	Check(plan.pattern.xy[0] == 0x55 && plan.pattern.xy[3] == 0xbb,
	      "native pattern does not follow the guest sample order");
	inputs.has_depth     = true;
	inputs.depth_samples = 2;
	Check(PlanEqaa(inputs, NvidiaCaps()).mode == EqaaMode::Fallback,
	      "native custom locations with depth must fall back");
}

void TestOff() {
	auto inputs             = CheckerboardInputs(false);
	inputs.msaa_num_samples = 0;
	Check(PlanEqaa(inputs, NvidiaCaps()).mode == EqaaMode::Off, "1x with custom S0 is not off");
	inputs             = CheckerboardInputs(false);
	inputs.msaa_enable = false;
	const auto plan    = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Off && (plan.inexact & MsaaDisabled) != 0,
	      "MSAA_ENABLE=0 is not off");
}

void TestFragmentApprox() {
	EqaaInputs inputs;
	inputs.msaa_num_samples = 2;
	inputs.msaa_enable      = true;
	inputs.color_count      = 1;
	inputs.colors[0]        = {2, 1};
	Check(PlanEqaa(inputs, NvidiaCaps()).mode == EqaaMode::FragmentApprox,
	      "4 coverage / 2 fragments is not the fragment approximation");
}

void TestOverrasterization() {
	auto inputs                     = CheckerboardInputs(false);
	inputs.overrasterization_amount = 1;
	const auto plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Fallback && plan.color_rule == EqaaColorRule::OrReduce,
	      "overrasterization did not select the OR-reduce rule");
}

void TestPsSuperset() {
	auto inputs          = CheckerboardInputs(false);
	inputs.locations[12] = Pack(-4, 0, 2, 2);
	const auto plan      = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::AnchorShadow && (plan.inexact & PsSuperset) != 0 &&
	          plan.host_samples == 4 && plan.pattern.count == 4,
	      "differing quad sets did not report a PS superset");
	Check(plan.pattern.xy[3] == plan.pattern.xy[0], "three locations did not pad with H[0]");
}

void TestAnchorsOnly() {
	EqaaInputs inputs;
	inputs.msaa_num_samples = 4;
	inputs.msaa_enable      = true;
	for (uint32_t quad = 0; quad < 4; quad++) {
		for (uint32_t k = 0; k < 4; k++) {
			const int base = static_cast<int>(quad) - 8 + static_cast<int>(k);
			inputs.locations[quad * 4 + k] =
			    Pack(base + 4 * static_cast<int>(quad % 2), -8 + static_cast<int>(quad), base, 1,
			         base, 2, base, 3);
		}
	}
	inputs.color_count = 1;
	const auto plan    = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::AnchorShadow && (plan.inexact & PsSuperset) != 0 &&
	          plan.host_samples == 4,
	      "too many distinct locations did not fall back to the anchors");
	for (uint32_t quad = 0; quad < 4; quad++) {
		Check(plan.pattern.xy[(plan.anchor_select >> (4 * quad)) & 0xf] ==
		          GuestSampleLocation(inputs.locations, quad, 0),
		      "an anchor does not select its own location");
	}
}

void TestRange() {
	auto caps      = NvidiaCaps();
	caps.coord_min = 0.3f;
	Check(PlanEqaa(CheckerboardInputs(false), caps).mode == EqaaMode::Fallback,
	      "a location outside the host range did not fall back");
	caps                  = NvidiaCaps();
	caps.sample_locations = false;
	Check(PlanEqaa(CheckerboardInputs(false), caps).mode == EqaaMode::Fallback,
	      "a host without sample locations did not fall back");
}

// Standard 4x locations in every quad pixel.
void SetStandard4(EqaaInputs& inputs) {
	for (uint32_t quad = 0; quad < 4; quad++) {
		inputs.locations[quad * 4] = Pack(-2, -6, 6, -2, -6, 2, 2, 6);
	}
}

// Death Stranding 2: 4 coverage samples tracked in FMASK over 1 stored fragment, 1 depth sample.
EqaaInputs OneFragmentInputs() {
	EqaaInputs inputs;
	inputs.msaa_num_samples     = 2;
	inputs.msaa_exposed_samples = 2;
	inputs.msaa_enable          = true;
	SetStandard4(inputs);
	inputs.color_count   = 2;
	inputs.colors[0]     = {2, 0};
	inputs.colors[1]     = {2, 0};
	inputs.has_depth     = true;
	inputs.depth_samples = 0;
	return inputs;
}

void TestOneFragment() {
	const auto plan = PlanEqaa(OneFragmentInputs(), NvidiaCaps());
	Check(plan.mode == EqaaMode::AnchorShadow, "4 coverage / 1 fragment is not an anchor shadow");
	Check(plan.guest_samples == 4 && plan.host_samples == 4 && plan.pattern.count == 4,
	      "4 coverage / 1 fragment host count");
	Check(plan.pattern.xy[0] == 0x26 && plan.pattern.xy[1] == 0x6e && plan.pattern.xy[2] == 0xa2 &&
	          plan.pattern.xy[3] == 0xea,
	      "4 coverage / 1 fragment host pattern is not the sorted standard 4x set");
	Check(plan.anchor_select == 0x0000, "sample 0 is not the anchor in every quad pixel");
	Check(plan.inexact == FmaskUnmodelled, "4 coverage / 1 fragment inexact parts");

	auto no_depth      = OneFragmentInputs();
	no_depth.has_depth = false;
	Check(PlanEqaa(no_depth, NvidiaCaps()).mode == EqaaMode::AnchorShadow,
	      "4 coverage / 1 fragment without depth is not an anchor shadow");

	// Two raster samples over FMASK behave like the checkerboard with one stored fragment.
	auto checkerboard      = CheckerboardInputs(false);
	checkerboard.colors[0] = {1, 0};
	const auto two         = PlanEqaa(checkerboard, NvidiaCaps());
	Check(two.mode == EqaaMode::AnchorShadow && two.anchor_select == 0x0011 &&
	          two.inexact == FmaskUnmodelled,
	      "2 coverage / 1 fragment is not the checkerboard shadow");
}

void TestOneFragmentFallbacks() {
	auto inputs               = OneFragmentInputs();
	inputs.max_anchor_samples = 2;
	auto plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::AnchorCount,
	      "more anchors than stored fragments did not fall back");

	// Without FMASK the colour target sees one sample, so the anchor count does not matter.
	auto checkerboard               = CheckerboardInputs(false);
	checkerboard.max_anchor_samples = 1;
	Check(PlanEqaa(checkerboard, NvidiaCaps()).mode == EqaaMode::AnchorShadow,
	      "the anchor count changed a target without FMASK");

	inputs               = OneFragmentInputs();
	inputs.depth_samples = 1;
	plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::FragmentApprox &&
	          plan.reason == EqaaFallbackReason::MixedSampleCounts,
	      "1 colour fragment over 2 depth samples is not the fragment approximation");

	inputs           = OneFragmentInputs();
	inputs.colors[1] = {1, 0};
	Check(PlanEqaa(inputs, NvidiaCaps()).mode == EqaaMode::FragmentApprox,
	      "FMASK over part of the raster samples is not the fragment approximation");

	inputs                          = OneFragmentInputs();
	inputs.overrasterization_amount = 2;
	plan                            = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::OrReduce,
	      "overrasterization over FMASK did not fall back");
}

// 4 coverage samples over 2 stored fragments: the two anchors keep their own fragments.
void TestTwoFragments() {
	EqaaInputs inputs;
	inputs.msaa_num_samples = 2;
	inputs.msaa_enable      = true;
	SetStandard4(inputs);
	inputs.color_count        = 1;
	inputs.colors[0]          = {2, 1};
	inputs.max_anchor_samples = 1;
	auto plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Native && plan.guest_samples == 4 && plan.host_samples == 2 &&
	          plan.pattern.count == 2,
	      "4 coverage / 2 fragments does not place the two anchors");
	Check(plan.pattern.xy[0] == 0x26 && plan.pattern.xy[1] == 0x6e,
	      "the host samples are not guest samples 0 and 1");
	Check(plan.inexact == (FmaskUnmodelled | PsSubset), "4 coverage / 2 fragments inexact parts");

	inputs.has_depth     = true;
	inputs.depth_samples = 1;
	plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::FragmentApprox && plan.reason == EqaaFallbackReason::NativeDepth,
	      "anchors at custom locations with depth are not the fragment approximation");

	// Anchors at the standard 2x locations need no custom locations, even with depth.
	for (uint32_t quad = 0; quad < 4; quad++) {
		inputs.locations[quad * 4] = Pack(4, 4, -4, -4, -6, 2, 2, 6);
	}
	plan = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Off && plan.pattern.count == 0,
	      "anchors at the standard 2x locations do not keep the default locations");

	inputs.locations[12] = Pack(-4, -4, 4, 4, -6, 2, 2, 6);
	plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::FragmentApprox &&
	          plan.reason == EqaaFallbackReason::NonUniformNative,
	      "per-quad anchors are not the fragment approximation");

	inputs                  = {};
	inputs.msaa_num_samples = 2;
	inputs.msaa_enable      = true;
	SetStandard4(inputs);
	inputs.color_count = 1;
	inputs.colors[0]   = {1, 1};
	plan               = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Native && plan.host_samples == 2 && plan.inexact == PsSubset,
	      "2 stored samples without FMASK under 4 raster samples do not place the anchors");
}

// Death Stranding 2's water pre-pass: 4 raster samples on a regular grid, one stored fragment
// under FMASK, and a depth target that keeps all four samples.
EqaaInputs FullDepthInputs() {
	EqaaInputs inputs;
	inputs.msaa_num_samples     = 2;
	inputs.msaa_exposed_samples = 2;
	inputs.msaa_enable          = true;
	for (uint32_t quad = 0; quad < 4; quad++) {
		inputs.locations[quad * 4] = Pack(-4, -4, 4, -4, -4, 4, 4, 4);
	}
	inputs.max_anchor_samples = 2;
	inputs.color_count        = 1;
	inputs.colors[0]          = {2, 0};
	inputs.has_depth          = true;
	inputs.depth_samples      = 2;
	return inputs;
}

void TestFullDepth() {
	Check(FullDepthInputs().locations[0] == 0x444cc4ccu, "the pre-pass sample locations");
	auto plan = PlanEqaa(FullDepthInputs(), NvidiaCaps());
	Check(plan.mode == EqaaMode::AnchorShadow && plan.full_depth,
	      "1 fragment over full-rate depth is not a full-depth shadow");
	Check(plan.guest_samples == 4 && plan.host_samples == 4 && plan.pattern.count == 4,
	      "full-depth host count");
	Check(plan.pattern.xy[0] == 0x44 && plan.pattern.xy[1] == 0x4c && plan.pattern.xy[2] == 0xc4 &&
	          plan.pattern.xy[3] == 0xcc,
	      "the full-depth host pattern is not the guest's grid");
	Check(plan.anchor_select == 0x0000, "sample 0 is not the stored fragment's sample");
	for (uint32_t quad = 0; quad < 4; quad++) {
		for (uint32_t sample = 0; sample < 4; sample++) {
			Check(plan.host_of_guest[quad][sample] == sample,
			      "a full-depth host sample is not its guest sample");
		}
	}
	Check(plan.inexact == FmaskUnmodelled, "full-depth inexact parts");
	Check(!PlanEqaa(OneFragmentInputs(), NvidiaCaps()).full_depth,
	      "a one-sample depth target was planned as full-rate depth");

	// The host samples keep the guest's order where sorting the locations would not.
	auto reversed = FullDepthInputs();
	for (uint32_t quad = 0; quad < 4; quad++) {
		reversed.locations[quad * 4] = Pack(4, 4, -4, 4, 4, -4, -4, -4);
	}
	plan = PlanEqaa(reversed, NvidiaCaps());
	Check(plan.mode == EqaaMode::AnchorShadow && plan.full_depth && plan.pattern.xy[0] == 0xcc &&
	          plan.pattern.xy[1] == 0xc4 && plan.pattern.xy[2] == 0x4c &&
	          plan.pattern.xy[3] == 0x44 && plan.anchor_select == 0x0000,
	      "the full-depth host samples do not follow the guest sample order");

	// The standard locations still need the shadow that stores one fragment.
	auto standard = FullDepthInputs();
	SetStandard4(standard);
	plan = PlanEqaa(standard, NvidiaCaps());
	Check(plan.mode == EqaaMode::AnchorShadow && plan.full_depth && plan.pattern.count == 4 &&
	          plan.pattern.xy[0] == 0x26 && plan.pattern.xy[3] == 0xea,
	      "full-rate depth at the standard locations is not a full-depth shadow");

	// Without FMASK the colour target sees one sample, so the anchor count does not matter.
	auto plain               = FullDepthInputs();
	plain.colors[0]          = {0, 0};
	plain.max_anchor_samples = 0;
	plan                     = PlanEqaa(plain, NvidiaCaps());
	Check(plan.mode == EqaaMode::AnchorShadow && plan.full_depth && plan.inexact == 0,
	      "a one-sample colour target over full-rate depth is not a full-depth shadow");

	// Every sample's PA_SC_AA_MASK bit counts, not the anchor's alone.
	auto masked    = FullDepthInputs();
	masked.aa_mask = {0xfffffffdu, 0xffffffffu};
	Check((PlanEqaa(masked, NvidiaCaps()).inexact & AaMaskIgnored) != 0,
	      "a depth sample's bit cleared in PA_SC_AA_MASK was not reported");
}

void TestFullDepthFallbacks() {
	auto inputs               = FullDepthInputs();
	inputs.max_anchor_samples = 0;
	auto plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::AnchorCount,
	      "fewer anchors than depth samples did not fall back");

	inputs                          = FullDepthInputs();
	inputs.overrasterization_amount = 1;
	plan                            = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::OrReduce,
	      "overrasterization over full-rate depth did not fall back");

	// The depth target's samples cannot follow locations that differ between quad pixels.
	inputs               = FullDepthInputs();
	inputs.locations[12] = Pack(-4, -4, 4, -4, -4, 4, 2, 2);
	plan                 = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::NonUniformNative,
	      "per-quad locations over full-rate depth did not fall back");

	auto caps             = NvidiaCaps();
	caps.sample_locations = false;
	plan                  = PlanEqaa(FullDepthInputs(), caps);
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::HostUnsupported,
	      "a host without sample locations did not fall back");
	caps               = NvidiaCaps();
	caps.sample_counts = 1u | 2u;
	plan               = PlanEqaa(FullDepthInputs(), caps);
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::HostUnsupported,
	      "a host without custom 4x locations did not fall back");
	caps           = NvidiaCaps();
	caps.coord_min = 0.3f;
	plan           = PlanEqaa(FullDepthInputs(), caps);
	Check(plan.mode == EqaaMode::Fallback && plan.reason == EqaaFallbackReason::HostUnsupported,
	      "a location outside the host range did not fall back");

	// Two stored fragments over four depth samples have no plan.
	inputs           = FullDepthInputs();
	inputs.colors[0] = {2, 1};
	plan             = PlanEqaa(inputs, NvidiaCaps());
	Check(plan.mode == EqaaMode::FragmentApprox &&
	          plan.reason == EqaaFallbackReason::MixedSampleCounts && !plan.full_depth,
	      "2 colour fragments over 4 depth samples are not the fragment approximation");
}

void TestAaMask() {
	auto inputs    = CheckerboardInputs(false);
	inputs.aa_mask = {0xffff0002u, 0xffffffffu};
	Check((PlanEqaa(inputs, NvidiaCaps()).inexact & AaMaskIgnored) != 0,
	      "an anchor bit cleared in PA_SC_AA_MASK was not reported");
}

} // namespace

int main() {
	TestCheckerboard();
	TestStandardPatternsAreOff();
	TestNativeCustom();
	TestOff();
	TestFragmentApprox();
	TestOverrasterization();
	TestPsSuperset();
	TestAnchorsOnly();
	TestRange();
	TestAaMask();
	TestOneFragment();
	TestOneFragmentFallbacks();
	TestTwoFragments();
	TestFullDepth();
	TestFullDepthFallbacks();
	std::printf("EqaaPlanTests: all tests passed\n");
	return 0;
}

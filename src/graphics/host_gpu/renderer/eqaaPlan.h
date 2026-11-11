#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_EQAAPLAN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_EQAAPLAN_H_

#include <array>
#include <cstdint>

namespace Libs::Graphics {

#pragma pack(push, 1)

// Host sample locations on a 1x1 grid; zero samples keeps the Vulkan standard locations.
struct HostSamplePattern {
	uint8_t count = 0;
	// (x + 8) | ((y + 8) << 4): the guest's signed 1/16-pixel offset from the pixel centre.
	std::array<uint8_t, 16> xy {};

	bool operator==(const HostSamplePattern&) const = default;
};

#pragma pack(pop)

enum class EqaaMode : uint8_t { Off, Native, AnchorShadow, FragmentApprox, Fallback };
enum class EqaaColorRule : uint8_t { AnchorOnly, OrReduce };

enum class EqaaFallbackReason : uint8_t {
	None,
	OrReduce,
	MixedSampleCounts,
	NonUniformNative,
	NativeDepth,
	HostUnsupported,
	PatternTooLarge,
	ShadowFormat,
	TargetSampled,
	AnchorCount,
};

// Inexact parts of an otherwise exact plan, for log-once reporting.
enum EqaaInexact : uint32_t {
	PsSuperset            = 1u << 0u,
	CoverageInputUnmapped = 1u << 1u,
	AlphaToMaskHost       = 1u << 2u,
	AaMaskIgnored         = 1u << 3u,
	SampleRateShading     = 1u << 4u,
	MaskExportUnmapped    = 1u << 5u,
	CentroidHostDefined   = 1u << 6u,
	MsaaDisabled          = 1u << 7u,
	FmaskUnmodelled       = 1u << 8u,
	PsSubset              = 1u << 9u,
};

struct EqaaTargetSamples {
	// Log2 encodings: CB_COLORn_ATTRIB.NUM_SAMPLES / NUM_FRAGMENTS, DB_Z_INFO.NUM_SAMPLES.
	uint8_t samples   = 0;
	uint8_t fragments = 0;
};

struct EqaaInputs {
	uint8_t                          msaa_num_samples          = 0;
	uint8_t                          msaa_exposed_samples      = 0;
	uint8_t                          coverage_to_shader_select = 0;
	bool                             msaa_enable               = false;
	std::array<uint32_t, 16>         locations {};
	std::array<uint32_t, 2>          aa_mask {0xffffffffu, 0xffffffffu};
	uint8_t                          max_anchor_samples             = 0;
	uint8_t                          overrasterization_amount       = 0;
	bool                             enable_postz_overrasterization = false;
	bool                             ps_reads_coverage              = false;
	bool                             ps_sample_rate                 = false;
	bool                             ps_centroid                    = false;
	bool                             mask_export                    = false;
	bool                             alpha_to_mask                  = false;
	uint32_t                         color_count                    = 0;
	std::array<EqaaTargetSamples, 8> colors {};
	bool                             has_depth     = false;
	uint8_t                          depth_samples = 0;
};

struct EqaaHostCaps {
	bool sample_locations = false;
	// VkSampleCountFlags that accept custom sample locations.
	uint32_t sample_counts  = 0;
	float    coord_min      = 0.0f;
	float    coord_max      = 0.0f;
	uint32_t sub_pixel_bits = 0;
};

struct EqaaPlan {
	EqaaMode           mode          = EqaaMode::Off;
	EqaaColorRule      color_rule    = EqaaColorRule::AnchorOnly;
	EqaaFallbackReason reason        = EqaaFallbackReason::None;
	uint8_t            guest_samples = 1;
	uint8_t            host_samples  = 1;
	HostSamplePattern  pattern;
	// [quad pixel][guest sample] -> host sample; quad pixel = (y & 1) * 2 + (x & 1).
	std::array<std::array<uint8_t, 16>, 4> host_of_guest {};
	// Nibble q holds host_of_guest[q][0], the host sample that carries guest sample 0.
	uint16_t anchor_select = 0;
	uint32_t inexact       = 0;
};

// Signed 1/16-pixel offset of guest sample `sample` in quad pixel `quad`.
[[nodiscard]] constexpr uint8_t GuestSampleLocation(const std::array<uint32_t, 16>& locations,
                                                    uint32_t quad, uint32_t sample) {
	const auto word = locations[quad * 4u + sample / 4u] >> (8u * (sample % 4u));
	const auto x    = static_cast<int32_t>(word << 28u) >> 28;
	const auto y    = static_cast<int32_t>((word >> 4u) << 28u) >> 28;
	return static_cast<uint8_t>((x + 8) | ((y + 8) << 4u));
}

[[nodiscard]] EqaaPlan PlanEqaa(const EqaaInputs& inputs, const EqaaHostCaps& caps);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_EQAAPLAN_H_

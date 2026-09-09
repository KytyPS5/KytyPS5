#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class              = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension                  = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count                  = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		uint32_t                      indirect_sampler           = UINT32_MAX;
		bool                          cube                       = false;
		bool                          needs_manual_depth_compare = false;
		bool                          operator==(const Image&) const = default;
	};

	std::vector<BoundedSrtLayout> bounded_srt_reads;
	std::vector<BufferTableLayout> buffer_tables;
	// Dense native buffer -> original logical buffer metadata. Empty tables have no slot.
	std::vector<uint32_t> buffer_origins;
	std::vector<Buffer> buffers;
	std::vector<Image>  images;
	// Candidate sampler slots clone a base sampler's metadata. Descriptor payloads stay in
	// ResourceSnapshot; only the slot topology and sampled pairs affect shader permutations.
	std::vector<uint32_t> sampler_origins;
	std::vector<SampledResourcePair> sampled_pairs;
	std::vector<uint8_t> sampler_depth_compare_funcs; // depth_compare_func for each sampler

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Resolves and specializes the immutable resource plan in one transaction. On failure both
// destinations are unchanged.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization);

// Provides the current thread's precise failure reason after MaterializeResources returns false.
// The view remains valid until the next materialization attempt on the same thread.
[[nodiscard]] std::string_view LastResourceSpecializationError() noexcept;

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */

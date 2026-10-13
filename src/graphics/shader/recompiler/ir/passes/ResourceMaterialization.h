#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <span>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   zero_stride_oob                 = false;
		uint32_t               indirect_feedback_offset        = BufferResource::NoIndirectFeedback;
		uint32_t               indirect_feedback_keys          = 0;
		uint32_t               indirect_root                   = BufferResource::NoIndirectBuffer;
		uint32_t               indirect_mapping_offset         = 0;
		uint32_t               indirect_search_iterations      = 0;
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		uint32_t                      indirect_feedback_offset = ImageResource::NoIndirectFeedback;
		uint32_t                      indirect_feedback_keys   = 0;
		bool                          cube                     = false;
		bool                          fmask                    = false;
		// A null candidate no key names, there only so its table always has an arm of this shape.
		bool shape_padding                  = false;
		bool bindless                       = false;
		bool operator==(const Image&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;
	std::vector<bool>   unbound_buffers;
	std::vector<bool>   unbound_images;

	bool operator==(const ResourceSpecialization& other) const {
		return buffers == other.buffers && images == other.images;
	}
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Whether the materialization would refuse the indirect buffer table `table_source` against guest
// memory as it is now - too many distinct descriptors, a foreign stride, or too many probes. Run
// during tracking, before the table is bound: `sources` are the tracker's interned descriptor
// sources. Evaluates the heap descriptor with the same clean walk the materialization uses and
// enumerates through the same memoized MaterializeIndirectBuffer, so the answer, and an accepted
// enumeration, are reused by the materialization that follows. False whenever it cannot tell.
bool IndirectBufferTableRefused(const Program& program, std::span<const DescriptorSource> sources,
                                uint32_t table_source, const SrtRuntime& runtime);

// Refreshes cached resources and specialization in place. A failed refresh must not be used.
// Why the last MaterializeResources on this thread refused, for the report that follows it.
std::string_view LastMaterializeFailure();

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                          std::vector<uint32_t>* refused_tables = nullptr);

bool               RefreshDescriptorState(const ResourcePlan& program, ResourceSnapshot& snapshot,
                                          ResourceSpecialization& specialization);
[[nodiscard]] bool ImageServedBindless(const ResourcePlan& program, uint32_t image_index);
[[nodiscard]] bool ReadOverlapsWrittenBuffer(const ResourcePlan&     program,
                                             const ResourceSnapshot& snapshot, uint64_t address,
                                             uint64_t bytes);

// Applies an already-derived specialization to native IR before layout and emission.
// Names the first field two specializations differ in, or nullptr when they match. Reporting
// only, but it is what tells a redundant permutation apart from a genuinely new one.
[[nodiscard]] const char* FirstSpecializationDifference(const ResourceSpecialization& before,
                                                        const ResourceSpecialization& after);
[[nodiscard]] bool        SpecializationServes(const ResourceSpecialization& built,
                                               const ResourceSpecialization& wanted);
void InheritUnboundShapes(std::span<const ResourceSpecialization* const> donors,
                          ResourceSpecialization&                        wanted);
// False when the specialization does not describe this translation of the program.
[[nodiscard]] bool ApplyResourceSpecialization(Program&                      program,
                                               const ResourceSpecialization& specialization);

// How many times an indirect image table has been enumerated rather than reused. A reused table
// reads the same words as a re-derived one, so this is what separates the two.
uint64_t IndirectImageEnumerationCount();

// Keys a draw reported selecting out of a descriptor heap too large to enumerate. The host reads
// the bitmap the draw wrote and hands the set bits back here; the next materialization of that
// table resolves them, so the pass converges to the materials it actually uses.
void AddObservedIndirectKeys(uint64_t signature, std::span<const uint32_t> keys);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */

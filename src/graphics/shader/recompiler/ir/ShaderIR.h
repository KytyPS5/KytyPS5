#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.h"
#include "graphics/shader/shader.h"

#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class ResourceKind {
	None,
	ScalarBuffer,
	ScalarAddress,
	Buffer,
	Flat,
	Global,
	Scratch,
	Lds,
	Gds,
	Image,
	ImageUint,
	StorageImage,
	StorageImageUint,
	Sampler
};

[[nodiscard]] constexpr bool IsAddressResourceKind(ResourceKind kind) {
	return kind == ResourceKind::ScalarAddress || kind == ResourceKind::Flat ||
	       kind == ResourceKind::Global || kind == ResourceKind::Scratch;
}

[[nodiscard]] constexpr bool ImageResourceKindMatches(ResourceKind       kind,
                                                      ImageResourceClass resource_class) {
	switch (resource_class) {
		case ImageResourceClass::Sampled:
			return kind == ResourceKind::Image || kind == ResourceKind::ImageUint;
		case ImageResourceClass::Storage:
			return kind == ResourceKind::StorageImage || kind == ResourceKind::StorageImageUint;
		case ImageResourceClass::StorageUint: return kind == ResourceKind::StorageImageUint;
		case ImageResourceClass::None: return false;
	}
	return false;
}

struct MemoryInfo {
	ResourceKind            kind                     = ResourceKind::None;
	uint32_t                resource                 = 0;
	uint32_t                sampler                  = 0;
	uint32_t                offset                   = 0;
	uint32_t                secondary_offset         = 0;
	uint32_t                dmask                    = 0;
	uint32_t                data_dwords              = 1;
	uint32_t                data_bits                = 32;
	uint32_t                component_index          = 0;
	uint32_t                component_count          = 1;
	uint32_t                data_format              = 0;
	uint32_t                number_format            = 0;
	uint32_t                image_sample_flags       = 0;
	Decoder::ImageDimension image_dimension          = Decoder::ImageDimension::Unknown;
	uint32_t                image_address_components = 0;
	uint32_t                image_nsa_dwords         = 0;
	uint32_t                image_nsa_addr[Decoder::MaxImageNsaAddressComponents] = {};
	uint32_t                memory_segment                                        = 0;
	bool                    address_is_full                                       = false;
	bool                    data_signed                                           = false;
	bool                    typed                                                 = false;
	bool                    formatted                                             = false;
	bool                    image_has_mip                                         = false;
	bool                    image_cube                                            = false;
	bool                    image_r128                                            = false;
	bool                    glc                                                   = false;
	bool                    slc                                                   = false;
	bool                    idxen                                                 = false;
	bool                    offen                                                 = false;
	bool                    planning_only                                         = false;

	bool operator==(const MemoryInfo& other) const = default;
};

enum class ExportTargetKind { Unknown, Null, Position, Primitive, Parameter, Mrt, MrtZ };

struct ExportInfo {
	ExportTargetKind kind   = ExportTargetKind::Unknown;
	uint32_t         target = 0;
	uint32_t         index  = 0;
	uint32_t         en     = 0;
	bool             done   = false;
	bool             compr  = false;
	bool             vm     = false;

	bool operator==(const ExportInfo& other) const = default;
};

struct DescriptorValue {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	bool operator==(const DescriptorValue& other) const {
		return dword_count == other.dword_count && dwords == other.dwords;
	}
};

struct BufferResource {
	static constexpr uint32_t NoImageAlias = UINT32_MAX;

	uint32_t               source             = 0;
	uint32_t               first_use_pc       = 0;
	uint32_t               max_byte_extent    = 0;
	uint32_t               packed_stride      = 0;
	Prospero::BufferFormat descriptor_format  = Prospero::BufferFormat::kInvalid;
	uint32_t               descriptor_swizzle = DstSel(4, 5, 6, 7);
	uint32_t               image_alias        = NoImageAlias;
	bool                   read               = false;
	bool                   written            = false;
	bool                   atomic             = false;
	bool                   formatted          = false;
	bool                   scalar             = false;

	bool operator==(const BufferResource& other) const = default;
};

enum class ImageMipMode { None, DynamicStorage };

constexpr uint32_t ShaderImageIdentitySwizzle = 0x00000facu;

struct ImageResource {
	static constexpr uint32_t NoIndirectImage = UINT32_MAX;

	uint32_t                source                    = 0;
	uint32_t                first_use_pc              = 0;
	ResourceKind            kind                      = ResourceKind::None;
	Decoder::ImageDimension dimension                 = Decoder::ImageDimension::Unknown;
	ImageMipMode            mip_mode                  = ImageMipMode::None;
	uint32_t                mip_count                 = 1;
	Prospero::BufferFormat  conversion_format         = Prospero::BufferFormat::kInvalid;
	uint32_t                shader_swizzle            = ShaderImageIdentitySwizzle;
	bool                    read                      = false;
	bool                    written                   = false;
	bool                    atomic                    = false;
	bool                    depth_compare             = false;
	bool                    cube                      = false;
	bool                    r128                      = false;
	uint32_t                indirect_root             = NoIndirectImage;
	uint32_t                indirect_mapping_offset   = 0;
	uint32_t                indirect_mapping_capacity = 0;
	std::vector<uint32_t>   indirect_resources;

	bool operator==(const ImageResource& other) const = default;
};

struct SamplerResource {
	uint32_t source                = 0;
	uint32_t first_use_pc          = 0;
	bool     force_point_filtering = false;

	bool operator==(const SamplerResource& other) const = default;
};

struct SampledResourcePair {
	uint32_t image        = 0;
	uint32_t sampler      = 0;
	uint32_t first_use_pc = 0;

	bool operator==(const SampledResourcePair& other) const = default;
};

enum class StageInputKind {
	VertexIndex,
	InstanceIndex,
	FragCoord,
	FrontFacing,
	BaryCoordSmooth,
	BaryCoordNoPerspective,
	WorkgroupId,
	LocalInvocationId,
	LocalInvocationIndex,
	GlobalInvocationId,
	Parameter,
};

enum class StageOutputKind {
	Position,
	Parameter,
	Mrt,
	Depth,
	SampleMask,
	PointSize,
	ClipDistance,
	CullDistance,
	Layer
};

struct PositionExportComponent {
	uint32_t clip_distance = UINT32_MAX;
	uint32_t cull_distance = UINT32_MAX;
	bool     valid          = false;
	bool     point_size     = false;
	bool     layer          = false;
	bool     viewport       = false;
};

inline PositionExportComponent DecodePositionExportComponent(uint32_t control,
	                                                           uint32_t pos_index,
	                                                           uint32_t component) {
	PositionExportComponent result;
	if (pos_index == 0 || component >= 4) {
		return result;
	}

	uint32_t slot   = pos_index - 1;
	uint32_t vector = 3;
	for (uint32_t i = 0; i < 3; i++) {
		if ((control & (1u << (21u + i))) != 0) {
			if (slot == 0) {
				vector = i;
				break;
			}
			slot--;
		}
	}
	if (vector == 3) {
		return result;
	}

	result.valid = true;
	if (vector == 0) {
		result.point_size = component == 0 && (control & (1u << 16u)) != 0;
		result.layer      = component == 2 && (control & (1u << 18u)) != 0;
		result.viewport   = component == 2 && (control & (1u << 19u)) != 0;
		return result;
	}

	const auto scalar = (vector - 1) * 4 + component;
	const auto lower  = (1u << scalar) - 1u;
	const auto clip   = control & 0xffu;
	const auto cull   = (control >> 8u) & 0xffu;
	if ((clip & (1u << scalar)) != 0) {
		result.clip_distance = std::popcount(clip & lower);
	}
	if ((cull & (1u << scalar)) != 0) {
		result.cull_distance = std::popcount(cull & lower);
	}
	return result;
}

struct StageInput {
	StageInputKind kind            = StageInputKind::VertexIndex;
	uint32_t       location        = 0;
	uint32_t       component_count = 1;
	std::string    debug_name;
	bool           per_vertex = false;

	bool operator==(const StageInput& other) const = default;
};

struct StageOutput {
	StageOutputKind kind     = StageOutputKind::Parameter;
	uint32_t        index    = 0;
	uint32_t        location = 0;
	std::string     debug_name;

	bool operator==(const StageOutput& other) const = default;
};

enum class DescriptorBindingKind {
	Buffers,
	Sampled1D,
	Sampled1DArray,
	Sampled2D,
	Sampled2DArray,
	Sampled2DMsaa,
	Sampled2DMsaaArray,
	Sampled3D,
	SampledUint1D,
	SampledUint1DArray,
	SampledUint2D,
	SampledUint2DArray,
	SampledUint2DMsaa,
	SampledUint2DMsaaArray,
	SampledUint3D,
	Storage1D,
	Storage1DArray,
	Storage2D,
	Storage2DArray,
	Storage3D,
	StorageUint1D,
	StorageUint1DArray,
	StorageUint2D,
	StorageUint2DArray,
	StorageUint3D,
	StorageAtomic1D,
	StorageAtomic1DArray,
	StorageAtomic2D,
	StorageAtomic2DArray,
	StorageAtomic3D,
	Samplers,
	Gds,
	BdaPagetable,
	FaultBuffer,
	FlattenedSrt,
	UserData,
	Count,
};

constexpr uint32_t NativePushConstantSize = 128;

[[nodiscard]] constexpr uint32_t NativeBinding(ShaderType stage, DescriptorBindingKind kind) {
	return static_cast<uint32_t>(kind) +
	       (stage == ShaderType::Pixel ? static_cast<uint32_t>(DescriptorBindingKind::Count) : 0u);
}

[[nodiscard]] inline std::optional<DescriptorBindingKind>
DescriptorBindingForImage(const ImageResource& image) {
	using Dimension = Decoder::ImageDimension;
	using Kind      = DescriptorBindingKind;

	switch (image.kind) {
		case ResourceKind::Image:
			switch (image.dimension) {
				case Dimension::Dim1D: return Kind::Sampled1D;
				case Dimension::Dim1DArray: return Kind::Sampled1DArray;
				case Dimension::Dim2D: return Kind::Sampled2D;
				case Dimension::Dim2DArray: return Kind::Sampled2DArray;
				case Dimension::Dim2DMsaa: return Kind::Sampled2DMsaa;
				case Dimension::Dim2DMsaaArray: return Kind::Sampled2DMsaaArray;
				case Dimension::Dim3D: return Kind::Sampled3D;
				default: return std::nullopt;
			}
		case ResourceKind::ImageUint:
			switch (image.dimension) {
				case Dimension::Dim1D: return Kind::SampledUint1D;
				case Dimension::Dim1DArray: return Kind::SampledUint1DArray;
				case Dimension::Dim2D: return Kind::SampledUint2D;
				case Dimension::Dim2DArray: return Kind::SampledUint2DArray;
				case Dimension::Dim2DMsaa: return Kind::SampledUint2DMsaa;
				case Dimension::Dim2DMsaaArray: return Kind::SampledUint2DMsaaArray;
				case Dimension::Dim3D: return Kind::SampledUint3D;
				default: return std::nullopt;
			}
		case ResourceKind::StorageImage:
			if (image.atomic) {
				return std::nullopt;
			}
			switch (image.dimension) {
				case Dimension::Dim1D: return Kind::Storage1D;
				case Dimension::Dim1DArray: return Kind::Storage1DArray;
				case Dimension::Dim2D: return Kind::Storage2D;
				case Dimension::Dim2DArray: return Kind::Storage2DArray;
				case Dimension::Dim3D: return Kind::Storage3D;
				default: return std::nullopt;
			}
		case ResourceKind::StorageImageUint:
			switch (image.dimension) {
				case Dimension::Dim1D:
					if (image.atomic) {
						return Kind::StorageAtomic1D;
					}
					return Kind::StorageUint1D;
				case Dimension::Dim1DArray:
					if (image.atomic) {
						return Kind::StorageAtomic1DArray;
					}
					return Kind::StorageUint1DArray;
				case Dimension::Dim2D:
					if (image.atomic) {
						return Kind::StorageAtomic2D;
					}
					return Kind::StorageUint2D;
				case Dimension::Dim2DArray:
					if (image.atomic) {
						return Kind::StorageAtomic2DArray;
					}
					return Kind::StorageUint2DArray;
				case Dimension::Dim3D:
					if (image.atomic) {
						return Kind::StorageAtomic3D;
					}
					return Kind::StorageUint3D;
				default: return std::nullopt;
			}
		default: return std::nullopt;
	}
}

struct DescriptorBinding {
	DescriptorBindingKind kind = DescriptorBindingKind::Buffers;
	std::vector<uint32_t> resources;

	bool operator==(const DescriptorBinding& other) const = default;
};

struct BindingLayout {
	uint32_t                       push_constant_offset = 0;
	uint32_t                       push_constant_size  = 0;
	uint32_t                       memory_offset_dword = 0;
	uint32_t                       memory_offset_count = 0;
	std::vector<uint32_t>          user_data_registers;
	std::vector<DescriptorBinding> descriptors;

	[[nodiscard]] uint32_t ShaderDataDwords() const {
		return memory_offset_dword + (memory_offset_count + 3u) / 4u;
	}

	bool operator==(const BindingLayout& other) const = default;
};

struct ShaderInfo {
	static constexpr uint32_t MaxBuffers      = 32;
	static constexpr uint32_t MaxImages       = 32;
	static constexpr uint32_t MaxSamplers     = 32;
	static constexpr uint32_t MaxSampledPairs = 64;

	std::vector<BufferResource>      buffers;
	std::vector<ImageResource>       images;
	std::vector<SamplerResource>     samplers;
	std::vector<SampledResourcePair> sampled_pairs;
	std::vector<StageInput>          inputs;
	std::vector<StageOutput>         outputs;
	std::array<uint8_t, 32>          vertex_fetch_components {};
	int32_t                          vertex_offset_sgpr = -1;
	bool                             has_bitwise_xor    = false;
	bool                             uses_dma           = false;

	bool operator==(const ShaderInfo& other) const = default;
};

struct SpirvRequirements {
	bool subgroup_ballot              = false;
	bool subgroup_shuffle             = false;
	bool subgroup_local_invocation_id = false;
	bool compute_derivatives          = false;
	bool image_gather_extended        = false;
	bool function_lds                 = false;
	bool function_scratch             = false;
	bool pixel_valid_mask             = false;
};

struct BlockInfo {
	uint32_t        id       = 0;
	uint32_t        start_pc = 0;
	uint32_t        end_pc   = 0;
	CFG::Terminator terminator;
	Value           condition;
	Value           indirect_target;
};

struct DescriptorSource {
	struct IndirectImage {
		uint32_t material_source = 0;
		uint32_t heap_source     = 0;
		uint32_t selector_stride = 0;
		uint32_t selector_offset = 0;
		uint32_t key_arg         = 0;

		bool operator==(const IndirectImage& other) const = default;
	};

	std::array<Value, 8>         dwords {};
	uint32_t                     dword_count = 0;
	std::optional<IndirectImage> indirect_image;

	bool operator==(const DescriptorSource& other) const = default;
};

struct SrtRead {
	Value    value;
	uint32_t flat_offset = 0;
	uint32_t use_pc      = 0;

	bool operator==(const SrtRead& other) const = default;
};

struct Program {
	Program() = default;
	~Program();

	Program(const Program&)            = delete;
	Program& operator=(const Program&) = delete;
	Program(Program&&) noexcept         = default;
	Program& operator=(Program&& other) noexcept;

	ShaderType                    stage               = ShaderType::Unknown;
	uint64_t                      shader_hash         = 0;
	uint32_t                      wave_size           = 64;
	uint32_t                      user_data_base      = 0;
	uint32_t                      user_data_count     = 64;
	uint32_t                      scratch_dwords      = 0;
	bool                          dispatcher_fallback = false;
	CFG::FailureKind              cfg_failure_kind    = CFG::FailureKind::None;
	std::string                   fallback_reason;
	std::vector<std::unique_ptr<Block>> block_storage;
	BlockList                           blocks;
	std::vector<BlockInfo>              block_info;
	// Decoded MIMG/VMEM metadata carries details such as RDNA2 NSA address registers and
	// storage-image swizzles. Typed memory instructions carry a dense index into these shader-local
	// tables until those fields are consumed by emission.
	std::vector<MemoryInfo>       memory_info;
	std::vector<ExportInfo>       export_info;
	std::vector<DescriptorSource> descriptor_sources;
	std::vector<SrtRead>          srt_reads;
	std::vector<Value>            dynamic_reads;
	bool                          srt_plan_complete = false;
	ShaderInfo                    info;
	bool                          resource_tracking_complete = false;
	bool                          shader_info_complete       = false;
	BindingLayout                 bindings;
	bool                          binding_layout_complete = false;

	std::optional<SpirvRequirements> spirv_requirements;
};

std::string ProgramToString(const Program& program);

bool  ValidateProgram(const Program& program, bool require_ssa, std::string* error);
void  ResolveControlFlowIdentities(Program& program);
bool  EquivalentValue(const Program& program, Value left, Value right);
Value ResolveInvariantPhi(const Program& program, Value value);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_ */

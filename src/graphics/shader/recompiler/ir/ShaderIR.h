#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/Block.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.h"
#include "graphics/shader/shader.h"

#include <array>
#include <bit>
#include <bitset>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Xbyak {
class CodeGenerator;
}

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class ResourceKind {
	None,
	ScalarBuffer,
	ScalarAddress,
	Buffer,
	IndirectBuffer,
	Flat,
	FlatLocal,
	Global,
	Scratch,
	Lds,
	Gds,
	Image,
	Sampler
};

[[nodiscard]] constexpr bool IsAddressResourceKind(ResourceKind kind) {
	return kind == ResourceKind::ScalarAddress || kind == ResourceKind::Flat ||
	       kind == ResourceKind::FlatLocal || kind == ResourceKind::Global ||
	       kind == ResourceKind::Scratch;
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
	bool                    address_is_full          = false;
	bool                    data_signed              = false;
	bool                    typed                    = false;
	bool                    formatted                = false;
	bool                    image_has_mip            = false;
	bool                    image_r128               = false;
	// glc read as a cache hint; false on an atomic, where the bit names the return value.
	bool cache_bypass   = false;
	bool idxen          = false;
	bool offen          = false;
	bool coherent       = false;
	bool planning_only  = false;
	bool dynamic_buffer = false;
	bool glc            = false;

	[[nodiscard]] bool SupportsIndirectBufferLoad(ValueOpcode opcode) const {
		return !typed && data_bits == 32u &&
		       (formatted ? opcode == ValueOpcode::LoadBufferU32
		                  : opcode == ValueOpcode::ReadConstBuffer ||
		                        opcode == ValueOpcode::LoadBufferU32 ||
		                        opcode == ValueOpcode::LoadBufferU32x2 ||
		                        opcode == ValueOpcode::LoadBufferU32x3 ||
		                        opcode == ValueOpcode::LoadBufferU32x4);
	}

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

struct BufferResource {
	static constexpr uint32_t NoImageAlias       = UINT32_MAX;
	static constexpr uint32_t NoIndirectFeedback = UINT32_MAX;
	static constexpr uint32_t NoIndirectBuffer   = UINT32_MAX;

	uint32_t               source             = 0;
	uint32_t               descriptor_index   = UINT32_MAX;
	uint32_t               first_use_pc       = 0;
	uint32_t               max_byte_extent    = 0;
	uint32_t               packed_stride      = 0;
	Prospero::BufferFormat descriptor_format  = Prospero::BufferFormat::kInvalid;
	uint32_t               descriptor_swizzle = DstSel(4, 5, 6, 7);
	uint32_t               image_alias        = NoImageAlias;
	// Where in the flattened SRT this table records the heap keys it selects, and how many it can
	// name. A table too large to enumerate is served from what the GPU reports instead.
	uint32_t indirect_feedback_offset = NoIndirectFeedback;
	uint32_t indirect_feedback_keys   = 0;
	// A table the shader indexes at runtime binds every record it can select as its own dense
	// buffer, and the access picks one with a switch. The root names itself; a candidate names the
	// root it was expanded from. Mapping and iterations describe the key search in the flattened
	// SRT, laid out exactly as the image table's is.
	uint32_t              indirect_root              = NoIndirectBuffer;
	uint32_t              indirect_mapping_offset    = 0;
	uint32_t              indirect_search_iterations = 0;
	std::vector<uint32_t> indirect_resources;
	bool                  read      = false;
	bool                  written   = false;
	bool                  atomic    = false;
	bool                  formatted = false;
	bool                  scalar    = false;
	bool                  unindexed = false;

	bool operator==(const BufferResource& other) const = default;
};

enum class ImageMipMode { None, Dynamic };

constexpr uint32_t ShaderImageIdentitySwizzle = 0x00000facu;

struct ImageResource {
	static constexpr uint32_t NoIndirectImage    = UINT32_MAX;
	static constexpr uint32_t NoIndirectFeedback = UINT32_MAX;

	uint32_t                      source            = 0;
	uint32_t                      descriptor_index  = UINT32_MAX;
	uint32_t                      first_use_pc      = 0;
	ImageResourceClass            resource_class    = ImageResourceClass::None;
	Prospero::TextureNumericClass numeric_class     = Prospero::TextureNumericClass::Unsupported;
	Decoder::ImageDimension       dimension         = Decoder::ImageDimension::Unknown;
	ImageMipMode                  mip_mode          = ImageMipMode::None;
	uint32_t                      mip_count         = 1;
	Prospero::BufferFormat        conversion_format = Prospero::BufferFormat::kInvalid;
	uint32_t                      shader_swizzle    = ShaderImageIdentitySwizzle;
	bool                          read              = false;
	bool                          written           = false;
	bool                          atomic            = false;
	bool                          atomic64          = false;
	bool                          depth_compare     = false;
	bool                          cube              = false;
	bool                          r128              = false;
	uint32_t                      indirect_root     = NoIndirectImage;
	uint32_t                      indirect_mapping_offset    = 0;
	uint32_t                      indirect_search_iterations = 0;
	// Where in the flattened SRT this table records the keys it selects, and how many it can name.
	uint32_t              indirect_feedback_offset = NoIndirectFeedback;
	uint32_t              indirect_feedback_keys   = 0;
	std::vector<uint32_t> indirect_resources;
	bool                  bindless = false;

	bool operator==(const ImageResource& other) const = default;
};

struct SamplerResource {
	uint32_t source       = 0;
	uint32_t first_use_pc = 0;
	// Native filtering/border variants share the original sampler's runtime descriptor.
	uint32_t snapshot_index        = 0;
	std::vector<uint32_t> indirect_resources;
	bool     force_point_filtering = false;
	bool     depth_compare         = false;
	bool     integer_border        = false;
	bool     gather_lod            = false;

	bool operator==(const SamplerResource& other) const = default;
};

struct SampledResourcePair {
	uint32_t image        = 0;
	uint32_t sampler      = 0;
	uint32_t first_use_pc = 0;

	bool operator==(const SampledResourcePair& other) const = default;
};

enum class TessellationAttribute {
	LocalOutput,
	ControlInput,
	ControlOutput,
	EvaluationInput,
	PatchOutput,
	Factor
};

enum class StageInputKind {
	VertexIndex,
	InvocationId,
	PrimitiveId,
	TessCoord,
	InstanceIndex,
	FragCoord,
	FrontFacing,
	PackedAncillary,
	Layer,
	SampleId,
	BaryCoordSmooth,
	BaryCoordSmoothSample,
	BaryCoordSmoothCentroid,
	BaryCoordNoPerspective,
	WorkgroupId,
	NumWorkgroups,
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
	// The per-pixel stencil reference from MRTZ channel 1; reaches Vulkan as FragStencilRefEXT.
	Stencil,
	SampleMask,
	PointSize,
	ClipDistance,
	CullDistance,
	Layer,
	ViewportIndex
};

struct PositionExportComponent {
	uint32_t clip_distance = UINT32_MAX;
	uint32_t cull_distance = UINT32_MAX;
	bool     point_size    = false;
	bool     layer         = false;
	bool     viewport      = false;
};

inline PositionExportComponent DecodePositionExportComponent(uint32_t control, uint32_t pos_index,
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

inline constexpr uint32_t FirstImageBinding           = 1u;
inline constexpr uint32_t FirstComparisonImageBinding = 22u;
inline constexpr uint32_t FirstStorageImageBinding    = 29u;
inline constexpr uint32_t ImageBindingCount           = 48u;

// Fixed length, so neither module nor layout moves with the heap; ShaderInfo::MaxImages.
inline constexpr uint32_t IndexedImageBindingElements = 256u;
// An element of such a binding past the images it holds; the host binds its first element there.
inline constexpr uint32_t PaddingImageResource = UINT32_MAX;
// Bounds the looped search for any 32-bit key count; the key count never picks a permutation.
inline constexpr uint32_t IndirectSearchIterations = 33u;

// After the key/ordinal pairs: a count, then per ordinal shape << 16 | element.
[[nodiscard]] constexpr uint32_t IndirectImageShape(Decoder::ImageDimension dimension, bool cube) {
	return (static_cast<uint32_t>(dimension) << 1u) | (cube ? 1u : 0u);
}
[[nodiscard]] constexpr uint32_t IndirectImageSlot(uint32_t shape, uint32_t element) {
	return (shape << 16u) | element;
}

inline constexpr uint32_t BindlessDescriptorSet     = 2u;
inline constexpr uint32_t BindlessArenaBinding      = 0u;
inline constexpr uint32_t BindlessFirstImageBinding = 1u;
inline constexpr uint32_t BindlessImageSlots        = 16384u;

inline constexpr uint32_t BindlessIntegerDescriptorSet = 3u;

enum class BindlessShape : uint32_t {
	Image2D,
	Image2DArray,
	Image3D,
	Image1D,
	Uint2D,
	Uint2DArray,
	Uint3D,
	Uint1D,
	Count
};
inline constexpr uint32_t BindlessFloatShapes   = 4u;
inline constexpr uint32_t BindlessUintShapeCode = 1u << 4u;
static_assert(IndirectImageShape(Decoder::ImageDimension::Dim2DMsaaArray, true) <
              BindlessUintShapeCode);

// Cube samples go through the 2D-array element of their view; only the coordinates differ.
[[nodiscard]] constexpr std::optional<BindlessShape>
BindlessShapeFor(Decoder::ImageDimension       dimension,
                 Prospero::TextureNumericClass numeric = Prospero::TextureNumericClass::Float) {
	uint32_t shape = 0;
	switch (dimension) {
		case Decoder::ImageDimension::Dim2D: shape = 0u; break;
		case Decoder::ImageDimension::Dim2DArray: shape = 1u; break;
		case Decoder::ImageDimension::Dim3D: shape = 2u; break;
		case Decoder::ImageDimension::Dim1D: shape = 3u; break;
		default: return std::nullopt;
	}
	switch (numeric) {
		case Prospero::TextureNumericClass::Float: return static_cast<BindlessShape>(shape);
		case Prospero::TextureNumericClass::Uint:
			return static_cast<BindlessShape>(shape + BindlessFloatShapes);
		default: return std::nullopt;
	}
}
[[nodiscard]] constexpr Decoder::ImageDimension BindlessShapeDimension(BindlessShape shape) {
	switch (static_cast<uint32_t>(shape) % BindlessFloatShapes) {
		case 0u: return Decoder::ImageDimension::Dim2D;
		case 1u: return Decoder::ImageDimension::Dim2DArray;
		case 3u: return Decoder::ImageDimension::Dim1D;
		default: return Decoder::ImageDimension::Dim3D;
	}
}
[[nodiscard]] constexpr Prospero::TextureNumericClass
BindlessShapeNumericClass(BindlessShape shape) {
	return static_cast<uint32_t>(shape) >= BindlessFloatShapes
	           ? Prospero::TextureNumericClass::Uint
	           : Prospero::TextureNumericClass::Float;
}
[[nodiscard]] constexpr uint32_t BindlessShapeCode(Decoder::ImageDimension dimension, bool cube,
                                                   Prospero::TextureNumericClass numeric) {
	return IndirectImageShape(dimension, cube) |
	       (numeric == Prospero::TextureNumericClass::Uint ? BindlessUintShapeCode : 0u);
}
[[nodiscard]] constexpr uint32_t BindlessImageBinding(BindlessShape shape) {
	return static_cast<uint32_t>(shape) >= BindlessFloatShapes
	           ? static_cast<uint32_t>(shape) - BindlessFloatShapes
	           : BindlessFirstImageBinding + static_cast<uint32_t>(shape);
}
[[nodiscard]] constexpr uint32_t BindlessImageSet(BindlessShape shape) {
	return static_cast<uint32_t>(shape) >= BindlessFloatShapes ? BindlessIntegerDescriptorSet
	                                                           : BindlessDescriptorSet;
}
static_assert(BindlessImageSlots <= 0x10000u);

enum class DescriptorBindingKind : uint32_t {
	Buffers  = 0u,
	Samplers = FirstImageBinding + ImageBindingCount,
	Gds,
	BdaPagetable,
	FaultBuffer,
	FlattenedSrt,
	ShaderData,
	SharedMemory,
	Count,
};

static_assert(static_cast<uint32_t>(DescriptorBindingKind::Samplers) == 49u);
static_assert(static_cast<uint32_t>(DescriptorBindingKind::Count) == 56u);

struct PushData {
	static constexpr uint32_t        DwordCount              = 32;
	static constexpr uint32_t        MeshDrawDwordCount      = 6;
	static constexpr uint32_t        DispatchLimitDwordCount = 2;
	static constexpr uint32_t        NoStart                 = UINT32_MAX;
	std::array<uint32_t, DwordCount> dwords {};

	[[nodiscard]] static constexpr bool CanFit(uint32_t start, uint32_t size) {
		return size != 0 && start <= DwordCount && size <= DwordCount - start;
	}
	[[nodiscard]] static constexpr uint32_t StartFor(uint32_t cursor, uint32_t size) {
		return CanFit(cursor, size) ? cursor : NoStart;
	}
	[[nodiscard]] static constexpr bool StartServes(uint32_t start, uint32_t cursor) {
		return start == NoStart || start >= cursor;
	}
};

// A null V# binds one dword at this offset, so every index lies past it: reads zero, writes drop.
inline constexpr uint32_t NullBufferByteOffset = 252;
inline constexpr uint32_t NullBufferRange      = 4;

static_assert(sizeof(PushData) == 128);
constexpr uint32_t NativePushConstantSize = sizeof(PushData);

[[nodiscard]] constexpr uint32_t NativeBinding(ShaderType stage, DescriptorBindingKind kind) {
	const uint32_t group = stage == ShaderType::Pixel                    ? 1u
	                       : stage == ShaderType::TessellationControl    ? 2u
	                       : stage == ShaderType::TessellationEvaluation ? 3u
	                                                                     : 0u;
	return static_cast<uint32_t>(kind) +
	       group * static_cast<uint32_t>(DescriptorBindingKind::Count);
}

// Descriptor sets a graphics pipeline layout holds: the pixel stage has its own.
//
// Both stages used to share set 0, which made the whole layout a function of *both* shaders - and
// therefore made a fragment pipeline library, whose layout must be compatible with the final
// pipeline's, unusable with any vertex shader but the one it was built beside. Note 157 measured
// the cost of that: 41 corpus vertex shaders carry 19 distinct binding signatures, so reuse across
// vertex partners would have collapsed. With the pixel stage in its own set, the fragment half of
// the layout is a function of the pixel shader alone.
constexpr uint32_t NativeDescriptorSetCount = 2;

// Where a stage's resources live. Mesh, vertex and compute share set 0 - only one of them is ever
// present in a pipeline - and pixel has set 1.
[[nodiscard]] constexpr uint32_t NativeDescriptorSet(ShaderType stage) {
	return stage == ShaderType::Pixel ? 1u : 0u;
}

// The same answer from the other side, for code that holds a binding index rather than a stage.
//
// This is not a second implementation of the rule: `NativeBinding` already offsets the pixel stage
// by `DescriptorBindingKind::Count`, so the index *is* the stage, and both directions read the same
// constant. That is what keeps the SPIR-V's DescriptorSet decoration and the host's descriptor
// writes in step - they are not two intentions that must agree, they are one function asked twice.
// The static_assert below locks that down for every stage.
[[nodiscard]] constexpr uint32_t NativeDescriptorSetForBinding(uint32_t native_binding) {
	return native_binding / static_cast<uint32_t>(DescriptorBindingKind::Count) == 1u ? 1u : 0u;
}

[[nodiscard]] constexpr bool NativeDescriptorSetAgrees(ShaderType stage) {
	constexpr auto last = static_cast<DescriptorBindingKind>(
	    static_cast<uint32_t>(DescriptorBindingKind::Count) - 1u);
	return NativeDescriptorSetForBinding(NativeBinding(stage, DescriptorBindingKind {})) ==
	           NativeDescriptorSet(stage) &&
	       NativeDescriptorSetForBinding(NativeBinding(stage, last)) == NativeDescriptorSet(stage);
}
static_assert(NativeDescriptorSetAgrees(ShaderType::Vertex) &&
              NativeDescriptorSetAgrees(ShaderType::Pixel) &&
              NativeDescriptorSetAgrees(ShaderType::TessellationControl) &&
              NativeDescriptorSetAgrees(ShaderType::TessellationEvaluation));

[[nodiscard]] constexpr ImageResourceClass ImageBindingResourceClass(DescriptorBindingKind kind) {
	const auto value = static_cast<uint32_t>(kind);
	if (value >= FirstImageBinding && value < FirstStorageImageBinding) {
		return ImageResourceClass::Sampled;
	}
	if (value >= FirstStorageImageBinding &&
	    value < static_cast<uint32_t>(DescriptorBindingKind::Samplers)) {
		return ImageResourceClass::Storage;
	}
	return ImageResourceClass::None;
}

[[nodiscard]] constexpr uint32_t ImageBindingIndex(DescriptorBindingKind kind) {
	return static_cast<uint32_t>(kind) - FirstImageBinding;
}

[[nodiscard]] constexpr std::optional<DescriptorBindingKind>
DescriptorBindingForImage(const ImageResource& image) {
	constexpr uint32_t SampledFloatBinding = 1u;
	constexpr uint32_t SampledUintBinding  = 8u;
	constexpr uint32_t SampledSintBinding  = 15u;
	constexpr uint32_t StorageFloatBinding = FirstStorageImageBinding;
	constexpr uint32_t StorageUintBinding  = StorageFloatBinding + 5u;
	constexpr uint32_t AtomicUintBinding   = StorageUintBinding + 5u;

	uint32_t base    = 0;
	bool     sampled = false;
	if (image.resource_class == ImageResourceClass::Sampled) {
		if (image.atomic) {
			return std::nullopt;
		}
		sampled = true;
		switch (image.numeric_class) {
			case Prospero::TextureNumericClass::Float:
				base = image.depth_compare ? FirstComparisonImageBinding : SampledFloatBinding;
				break;
			case Prospero::TextureNumericClass::Uint: base = SampledUintBinding; break;
			case Prospero::TextureNumericClass::Sint: base = SampledSintBinding; break;
			case Prospero::TextureNumericClass::Unsupported: return std::nullopt;
			default: return std::nullopt;
		}
		if (image.depth_compare && image.numeric_class != Prospero::TextureNumericClass::Float) {
			return std::nullopt;
		}
	} else if (image.resource_class == ImageResourceClass::Storage) {
		if (image.atomic) {
			if (image.numeric_class != Prospero::TextureNumericClass::Uint) {
				return std::nullopt;
			}
			base = AtomicUintBinding + (image.atomic64 ? 5u : 0u);
		} else {
			switch (image.numeric_class) {
				case Prospero::TextureNumericClass::Float: base = StorageFloatBinding; break;
				case Prospero::TextureNumericClass::Uint: base = StorageUintBinding; break;
				case Prospero::TextureNumericClass::Sint:
				case Prospero::TextureNumericClass::Unsupported: return std::nullopt;
				default: return std::nullopt;
			}
		}
	} else {
		return std::nullopt;
	}

	uint32_t dimension = 0;
	switch (image.dimension) {
		case Decoder::ImageDimension::Dim1D: break;
		case Decoder::ImageDimension::Dim1DArray: dimension = 1u; break;
		case Decoder::ImageDimension::Dim2D: dimension = 2u; break;
		case Decoder::ImageDimension::Dim2DArray: dimension = 3u; break;
		case Decoder::ImageDimension::Dim2DMsaa:
			if (!sampled) {
				return std::nullopt;
			}
			dimension = 4u;
			break;
		case Decoder::ImageDimension::Dim2DMsaaArray:
			if (!sampled) {
				return std::nullopt;
			}
			dimension = 5u;
			break;
		case Decoder::ImageDimension::Dim3D: dimension = sampled ? 6u : 4u; break;
		case Decoder::ImageDimension::Unknown: return std::nullopt;
		default: return std::nullopt;
	}
	return static_cast<DescriptorBindingKind>(base + dimension);
}

struct Bindings {
	std::array<uint32_t, static_cast<size_t>(DescriptorBindingKind::Count)> descriptor_counts {};
	uint64_t                                                                descriptor_mask = 0;
	uint32_t push_data_start_dword = PushData::NoStart;
	uint32_t memory_offset_dword   = 0;
	// The shader samples images out of the global bindless heap sets, and whether it also needs
	// the integer heap set.
	bool uses_bindless    = false;
	bool bindless_integer = false;

	[[nodiscard]] uint32_t ShaderDataDwords() const {
		return memory_offset_dword +
		       (descriptor_counts[static_cast<size_t>(DescriptorBindingKind::Buffers)] + 3u) / 4u;
	}
	[[nodiscard]] bool UsesPushData() const { return push_data_start_dword != PushData::NoStart; }
	void               AdvancePushData(uint32_t& cursor) const {
		if (UsesPushData()) {
			cursor = push_data_start_dword + ShaderDataDwords();
		}
	}

	bool operator==(const Bindings& other) const = default;
};

struct DmaPointer {
	bool     user_data = false;
	bool     store     = false;
	uint32_t lo        = 0;
	uint32_t hi        = 0;
	uint32_t relative_offset = UINT32_MAX;
	uint32_t records_offset  = UINT32_MAX;
	uint32_t records         = 0;
	uint32_t high_mask       = 0;

	bool operator==(const DmaPointer& other) const = default;
};

struct ShaderInfo {
	static constexpr uint32_t MaxBuffers = 64;
	// Dense buffer slots a materialized shader may bind: the buffers it tracks plus the candidates
	// an indirect buffer table expands into. 64 was the largest count whose packed memory offsets
	// still fit the 32-dword push constant block beside 16 user-data registers - a threshold, not a
	// ceiling: past it a shader takes the shader-data storage buffer instead, which is one more
	// upload and one more descriptor write per dispatch. Unlike MaxImages below, this one really is
	// a push-constant number: the shader-data block packs only the buffers' memory offsets.
	// A table reserves its candidate count rounded up to a power of two, so this is really a bucket
	// plus the buffers a shader tracks: 128 candidate slots and the 11 buffers RE9's two indirect
	// tables track. 64 candidate slots would serve 63 keys, and both of those tables have already
	// been measured reporting more than that (67 and 75). The device limits that bound it are the
	// storage-buffer ones CreateDescriptorLayout now checks, and the emitted switch, which indexes
	// contiguous candidates of one shape and costs an arm per candidate per access site otherwise.
	static constexpr uint32_t MaxDenseBuffers = 138;
	// Dense image slots a materialized shader may bind: the images it tracks plus the candidates an
	// indirect image table expands into. Unlike MaxDenseBuffers above this is not a push-constant
	// threshold - the emitter packs only the buffers' memory offsets into the shader-data
	// block, so an image costs no push dword - and it is not a binding count either: images are
	// grouped into at most ImageBindingCount bindings whose descriptorCount carries the rest. What
	// an image does cost is one Vulkan descriptor per draw, so the real ceiling is the device's
	// maxPerStageDescriptorSampledImages / maxDescriptorSetSampledImages / maxPerStageResources -
	// CreateDescriptorLayout checks all three against the layout it is about to build - and the
	// emitted switch, at roughly 95 SPIR-V words per candidate per access site.
	// 128 was measured starving RE9's menu: its background pixel shader carries four tables and
	// they are served in order out of this one count, so the first two took 54 and 53 candidates
	// and left the last two 11 to 17 slots for the 31 keys they had observed - 21 of them past
	// the budget, sampling black on every draw, and the count moving frame to frame as the
	// earlier tables grew is what made the same surface black in one frame and lit in the next.
	// 256 covers the 145 candidates those four tables converge on with headroom; the ceiling no
	// budget can need is the heap's own 266 distinct descriptors, which a census of all 32768 of
	// its records measured.
	static constexpr uint32_t MaxImages       = 256;
	static constexpr uint32_t MaxSamplers     = 32;
	static constexpr uint32_t MaxSampledPairs = 64;

	std::vector<BufferResource>      buffers;
	std::vector<ImageResource>       images;
	std::vector<SamplerResource>     samplers;
	std::vector<SampledResourcePair> sampled_pairs;
	std::vector<StageInput>          inputs;
	std::vector<StageOutput>         outputs;
	std::vector<uint32_t>            user_data_registers;
	// Buffers the shader binds, tracked plus the candidates an indirect table expands into.
	std::bitset<MaxDenseBuffers>     live_buffers;
	std::array<uint8_t, 32>          vertex_fetch_components {};
	int32_t                          vertex_offset_sgpr   = -1;
	int32_t                          instance_offset_sgpr = -1;
	bool                             has_bitwise_xor              = false;
	bool                             uses_dma                     = false;
	bool                             writes_dma                   = false;
	bool                             uses_swizzle                 = false;
	bool                             uses_lds                     = false;
	bool                             uses_gds                     = false;
	bool                             uses_flattened_srt           = false;
	bool                             uses_dispatch_threads        = false;
	bool                             bvh                          = false;
	bool                             subgroup_ballot              = false;
	bool                             subgroup_barrier             = false;
	bool                             subgroup_shuffle             = false;
	bool                             subgroup_local_invocation_id = false;
	bool                             compute_derivatives          = false;
	bool                             image_gather_extended        = false;
	bool                             function_lds                 = false;
	bool                             function_scratch             = false;
	bool                             pixel_valid_mask             = false;
	bool                             buffer_int64_atomics         = false;
	bool                             buffer_u8                    = false;
	bool                             buffer_u16                   = false;
	bool                             shared_int64_atomics         = false;
	bool                             coherent_buffers             = false;
	bool                             float64                      = false;
	std::vector<DmaPointer>          dma_pointers;

	bool operator==(const ShaderInfo& other) const = default;
};

static_assert(IndexedImageBindingElements == ShaderInfo::MaxImages);

struct DescriptorSource {
	// How the shader turns its wave-uniform selector into a byte offset into the material table.
	// Stride names offset + k*stride; Mask names every value whose bits outside the mask are clear,
	// which is what an alignment mask on an already-scaled index reaches.
	enum class SelectorKind : uint32_t { Stride, Mask };

	struct IndirectDescriptor {
		// No material table at all: the key is computed in the shader and reaches the host only as
		// feedback. There is then nothing to enumerate - a probe would have to re-execute the key,
		// which is the one thing the host cannot do - so such a table is served from its observed
		// keys from the first draw on.
		static constexpr uint32_t NoMaterialTable = UINT32_MAX;

		uint32_t              material_source = UINT32_MAX;
		uint32_t              table_source    = 0;
		uint32_t              selector_stride = 0;
		uint32_t              selector_offset = 0;
		uint32_t              table_offset    = 0;
		uint32_t              workgroup_axis  = UINT32_MAX;
		uint32_t              selector_shift  = 0;
		Value                 key_count;
		Value                 selector_first;
		Value                 selector_mask;
		std::vector<uint32_t> sources;
		// The static immediate on the material read; the enumeration adds it to every probe.
		uint32_t material_offset = 0;
		// The heap record the key names. 32 bytes is one image descriptor, which is what a
		// table of plain T# holds, but an engine may stride its table differently.
		uint32_t heap_stride   = 32;
		uint32_t record_offset = 0;
		// A mask the shader applies to the material word before indexing the heap. It only ever
		// narrows the key, so the enumeration stays bounded. All ones when the shader applies
		// none.
		uint32_t key_mask = 0xffffffffu;
		// The key indexes the heap through the load's own index operand rather than through a byte
		// offset the shader computed, so hardware supplies the record step from the heap V#.
		// `heap_stride` is then only what the shader scaled the index by - usually 1 - and the
		// byte step is that times the V#'s stride, which is not known until the descriptor is read.
		bool indexed_heap = false;
		bool bindless     = false;
		// The material offset was matched as an affine chain, so its step need not be the
		// material V#'s record stride; every aligned word is probed.
		bool affine_selector = false;

		bool operator==(const IndirectDescriptor& other) const = default;
	};

	// A buffer V# the shader loads out of a descriptor table it indexes with a wave-uniform
	// selector. The table's own V# is CPU-derivable; only the record index is not, so the host
	// enumerates the records instead of re-executing the selector.
	struct IndirectBuffer {
		uint32_t heap_source     = 0;
		uint32_t selector_stride = 0;
		uint32_t selector_offset = 0;
		uint32_t record_offset   = 0;
		uint32_t key_arg         = 0;

		bool operator==(const IndirectBuffer& other) const = default;
	};

	std::array<Value, 8>              dwords {};
	uint32_t                          dword_count = 0;
	std::optional<IndirectDescriptor> indirect_descriptor;
	std::optional<IndirectBuffer>     indirect_buffer;

	bool operator==(const DescriptorSource& other) const = default;
};

struct SrtRead {
	Value    value;
	uint32_t flat_offset = 0;

	bool operator==(const SrtRead& other) const = default;
};

struct SrtReadWriteOrder {
	uint64_t buffers = 0;
	bool     unknown = true;

	bool operator==(const SrtReadWriteOrder& other) const = default;
};

struct ResourceBlock {
	// Conditional successors are ordered true, false; an empty condition follows every edge.
	Value                 condition;
	std::vector<uint32_t> successors;
	std::vector<uint32_t> sources;
	std::vector<uint32_t> srt_reads;
};

// Stable shader metadata consumed by the renderer after native IR has been discarded.
struct CompiledShaderInfo {
	ShaderType stage              = ShaderType::Unknown;
	uint64_t   shader_hash        = 0;
	uint32_t   wave_size          = 64;
	uint32_t   user_data_base     = 0;
	uint32_t   user_data_count    = 64;
	uint32_t   scratch_dwords     = 0;
	uint32_t   param_export_mask  = 0;
	bool       has_address_writes = false;
	ShaderInfo info;
	Bindings   bindings;
};

struct UniformFillPlan {
	UniformFill          fill;
	std::array<Value, 4> values;
};

// Resource analysis retained by the shader cache. It owns immutable descriptor/SRT,
// condition and fill values without translated blocks, plus reusable evaluation scratch.
struct ResourcePlan {
	struct EvaluationContext {
		struct Entry {
			uint64_t value          = 0;
			uint64_t generation     = 0;
			bool     lane_dependent = false;
		};

		std::vector<Entry> values;
		uint64_t           generation = 0;
	};

	ResourcePlan();
	~ResourcePlan();

	ResourcePlan(const ResourcePlan&)            = delete;
	ResourcePlan& operator=(const ResourcePlan&) = delete;
	ResourcePlan(ResourcePlan&&) noexcept;
	ResourcePlan& operator=(ResourcePlan&& other) noexcept;

	ShaderType stage           = ShaderType::Unknown;
	uint64_t   shader_hash     = 0;
	uint32_t   user_data_base  = 0;
	uint32_t   user_data_count = 64;
	// Kept with the plan, not only with the translated program: the host walk re-executes a
	// readfirstlane once per lane, so it needs the wave the shader was compiled for.
	uint32_t wave_size = 64;
	// With `wave_size` this decides whether the upper 32 lanes of a 64-bit guest mask exist at
	// all: a wave64 program on a 32-wide subgroup that is not emitted as two halves has no lane
	// 32..63 any ballot can set, so a mask bit up there is one nothing can clear.
	uint32_t host_subgroup_size = 64;
	bool     robust_buffer_access2 = false;
	// True when the guest wave is wider than the lanes the emitted module can activate; see
	// `Translator::ClampGhostLanes`.
	bool                          upper_lane_half_is_ghost = false;
	std::list<Inst>               value_storage;
	std::vector<MemoryInfo>       memory_info;
	std::vector<DescriptorSource> descriptor_sources;
	std::vector<ResourceBlock>    control_flow;
	// The control-flow description could not be built at all, so nothing is known about which
	// sources the shader reads. Distinct from an empty description built successfully: a shader
	// with no condition to prune on reads every source its accesses name, which is reachability
	// information, just trivial. Only the former may leave an unreadable source null.
	bool                           control_flow_unknown = false;
	std::vector<SrtRead>           srt_reads;
	std::vector<SrtReadWriteOrder> srt_read_order;
	std::vector<uint8_t>           clean_flat_slots;
	std::vector<uint8_t>           data_flat_slots;
	bool                           requires_specialization_memory = false;
	// Guest ranges read while compiling (linked SWAPPC leaf calls); a draw revalidates them.
	std::vector<std::pair<uint64_t, uint64_t>> source_reads;
	bool                           capture_specialization_reads   = false;
	bool                           srt_plan_complete              = false;
	bool                           resource_tracking_complete     = false;
	// Resources the materializer has *proven* the host cannot re-evaluate: it tried against real
	// guest memory, failed, and bound null. Named by `first_use_pc`, which is a property of the
	// code and so survives a re-translation, where a descriptor-source index would not.
	//
	// This is the one fact tracking cannot derive for itself. Whether a descriptor folds or
	// degrades is decided per draw, by whether a readfirstlane's lanes agree - two shaders that
	// are identical in every way this pass can see differ only there. So a recognizer that wants
	// to serve a degrading descriptor from a table has to be told, and being told is what makes
	// the relaxations it then applies sound rather than a guess.
	std::vector<uint32_t> unfoldable_pcs;
	ShaderInfo            info;
	UniformFillPlan       uniform_fill;
	BufferCopy            buffer_copy;
	// GPU-thread scratch for nested clean/EXEC memos, activity and material keys.
	mutable std::deque<EvaluationContext> evaluation_contexts;
	mutable uint32_t                      evaluation_value_count = 0;
	mutable uint32_t                      evaluation_depth       = 0;
	mutable std::vector<uint8_t>          active_sources;
	mutable std::vector<uint8_t>          visited_blocks;
	mutable std::vector<uint32_t>         pending_blocks;
	mutable std::vector<uint32_t>         material_keys;
	bool                                  frozen_values = false;
	struct ClosedBallot {
		enum State : uint8_t { Unknown, Open, Closed, Known };
		State    state = Unknown;
		uint64_t mask  = 0;
	};
	mutable std::vector<ClosedBallot> closed_ballots;
	// Pure functions of a frozen plan's instructions; a deque so a held web survives later inserts.
	struct PhiPlan {
		Value                    invariant;
		Value                    entry;
		std::vector<const Inst*> web;
	};
	mutable std::vector<uint32_t> phi_plan_index;
	mutable std::deque<PhiPlan>   phi_plans;
};

struct Program: ResourcePlan {
	Program() = default;
	~Program();

	Program(const Program&)            = delete;
	Program& operator=(const Program&) = delete;
	Program(Program&&) noexcept        = default;
	Program&           operator=(Program&& other) noexcept;
	CompiledShaderInfo TakeCompiledInfo() &&;

	std::vector<std::unique_ptr<Block>> block_storage;
	BlockList                           blocks;
	uint32_t                            scratch_dwords      = 0;
	bool                                dispatcher_fallback = false;
	// Set when a hardware ray-tracing intersect was lowered to a constant miss. Purely
	// diagnostic: the caller reports the shader once so the log shows which output is a lie.
	bool uses_bvh_intersect_stub = false;
	// Set when an S_SWAPPC_B64 call was replaced by an any-hit that accepts every hit.
	bool             uses_call_stub   = false;
	CFG::FailureKind cfg_failure_kind = CFG::FailureKind::None;
	std::string      fallback_reason;
	struct ScalarWrite {
		uint32_t  pc;
		ScalarReg reg;
	};
	std::vector<ScalarWrite> scalar_writes;
	// Typed memory and export instructions reference shader-local metadata by dense index.
	// Decoder-only details (such as NSA register numbers) have already become IR operands.
	std::vector<ExportInfo> export_info;
	// Set only for the duration of one translation, by a caller that can read guest memory: asks
	// whether the materialization would refuse the recognised indirect buffer table `source`
	// (an index into `sources`, the tracker's interned descriptor sources) against the memory as
	// it is now. A refused table is decoded in the shader from this translation on, exactly as the
	// unfoldable channel would have demoted it one translation later.
	std::function<bool(const Program& program, std::span<const DescriptorSource> sources,
	                   uint32_t source)>
	         refuse_indirect_buffer_table;
	bool     has_address_writes   = false;
	bool     shader_info_complete = false;
	Bindings bindings;
};

uint32_t StorageBufferElementBits(const Program& program, const MemoryInfo& memory);

std::string ProgramToString(const Program& program);
bool        HasShaderMemoryWrites(const Program& program);

void  ValidateProgram(const Program& program, bool require_ssa);
void  ResolveControlFlowIdentities(Program& program);
bool  EquivalentValue(const ResourcePlan& program, Value left, Value right);
Value ResolveInvariantPhi(const ResourcePlan& program, Value value);
// Why a loop-carried phi has no single entry value. Reported so the log can tell a self-contained
// cycle apart from a merge of two different entry values: the first needs a runtime descriptor,
// the second only needs the two operands proved equivalent.
enum class CyclicPhiReject {
	None,
	// Every operand of every phi in the web leads back into the web, so the loop is entered with
	// no value the host could stand in for.
	NoEntry,
	// Two operands enter the web from outside and are not equivalent, so this is a merge rather
	// than a loop and no single host binding stands for it.
	Merge,
};

// On Merge, the two operands that disagree, so a caller can name them instead of only the phi.
struct CyclicPhiFailure {
	CyclicPhiReject reason = CyclicPhiReject::None;
	Value           entry;
	Value           other;
};

// Valid only while nothing rewrites the program's instructions.
struct CyclicPhiEntryCache {
	struct Answer {
		Value                    entry;
		std::vector<const Inst*> web;
		CyclicPhiFailure         reject;
	};
	const ResourcePlan*                     program = nullptr;
	std::unordered_map<const Inst*, Answer> answers;
};

Value ResolveCyclicPhiEntry(const ResourcePlan& program, Value value,
                            std::vector<const Inst*>* web_out = nullptr,
                            CyclicPhiFailure*         reject  = nullptr,
                            CyclicPhiEntryCache*      cache   = nullptr);
Value ResolveActiveU32(Value value, Value active);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_ */

#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_INTERNAL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_INTERNAL_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <spirv/unified1/GLSL.std.450.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

struct InputBinding : IR::StageInput {
	uint32_t variable_id = 0;
};

struct OutputBinding : IR::StageOutput {
	uint32_t variable_id        = 0;
	uint32_t mesh_data_variable = 0;
};

using ImageDimension = Decoder::ImageDimension;

struct ImageDimensionInfo {
	ImageDimension dimension;
	uint32_t       spirv_dimension;
	uint32_t       coordinate_components;
	uint32_t       spatial_components;
	uint32_t       arrayed;
	uint32_t       multisampled;
};

constexpr std::array<ImageDimensionInfo, 7> ImageDimensions {{
    {ImageDimension::Dim1D, spv::Dim1D, 1, 1, 0, 0},
    {ImageDimension::Dim1DArray, spv::Dim1D, 2, 1, 1, 0},
    {ImageDimension::Dim2D, spv::Dim2D, 2, 2, 0, 0},
    {ImageDimension::Dim2DArray, spv::Dim2D, 3, 2, 1, 0},
    {ImageDimension::Dim3D, spv::Dim3D, 3, 3, 0, 0},
    {ImageDimension::Dim2DMsaa, spv::Dim2D, 2, 2, 0, 1},
    {ImageDimension::Dim2DMsaaArray, spv::Dim2D, 3, 2, 1, 1},
}};

const ImageDimensionInfo& ImageDimensionInfoFor(ImageDimension dimension);

struct SpirvRequirements {
	bool subgroup_ballot              = false;
	bool subgroup_shuffle             = false;
	bool subgroup_local_invocation_id = false;
	bool compute_derivatives          = false;
	bool image_gather_extended        = false;
	bool function_lds                 = false;
	bool function_scratch             = false;
	bool pixel_valid_mask             = false;
	bool buffer_int64_atomics         = false;
};

SpirvRequirements AnalyzeProgramRequirements(const IR::Program& program);

struct EmitterState {
	EmitterState(const IR::Program& program_, ShaderStageInputInfo input_info_)
	    : builder(program_.stage == ShaderType::Mesh ? 0x00010400u : 0x00010300u),
	      program(program_), input_info(input_info_),
	      requirements(AnalyzeProgramRequirements(program_)) {}

	Builder                                          builder;
	const IR::Program&                               program;
	ShaderStageInputInfo                             input_info;
	const SpirvRequirements                          requirements;
	uint32_t                                         lane_count              = 1;
	uint32_t                                         lane_half               = 0;
	uint32_t                                         storage_buffer_variable = 0;
	uint32_t                                         storage_buffer_u64_variable = 0;
	std::array<uint32_t, IR::ShaderInfo::MaxBuffers> memory_byte_offsets {};
	uint32_t                                         bda_pagetable_variable  = 0;
	uint32_t                                         fault_buffer_variable   = 0;
	uint32_t                                         bda_pointer_function    = 0;
	uint32_t                                         gds_variable            = 0;
	uint32_t                                         gds_length              = 0;
	uint32_t                                         push_constant_variable  = 0;
	uint32_t                                         shader_data_storage_variable = 0;
	uint32_t                                         flattened_srt_variable  = 0;
	uint32_t                                         lds_variable            = 0;
	std::array<uint32_t, 2>                          scratch_variable {};
	std::array<uint32_t, IR::ImageBindingCount>      image_variables {};
	uint32_t                   sampler_variable                      = 0;
	uint32_t                   main_func                             = 0;
	uint32_t                   mesh_guest_func                       = 0;
	uint32_t                   mesh_allocation                       = 0;
	uint32_t                   mesh_primitive_data                   = 0;
	uint32_t                   mesh_primitives                       = 0;
	uint32_t                   mesh_cull                             = 0;
	uint32_t                   entry_label                           = 0;
	uint32_t                   current_label                         = 0;
	const IR::Block*           current_block                         = nullptr;
	uint32_t                   pixel_valid_mask_variable             = 0;
	uint32_t                   subgroup_local_invocation_id_variable = 0;
	uint32_t                   per_vertex_variable                   = 0;
	uint32_t                   point_size_variable                   = 0;
	uint32_t                   clip_distance_variable                = 0;
	uint32_t                   cull_distance_variable                = 0;
	uint32_t                   layer_variable                        = 0;
	uint32_t                   viewport_index_variable               = 0;
	uint32_t                   depth_variable                        = 0;
	uint32_t                   sample_mask_variable                  = 0;
	std::vector<InputBinding>  inputs;
	std::vector<OutputBinding> outputs;
	std::vector<uint32_t>      interface_variables;
	std::unordered_map<const IR::Block*, uint32_t> labels;
};

uint32_t TypeVoid(EmitterState& state);
uint32_t TypeBool(EmitterState& state);
uint32_t TypeBoolVector(EmitterState& state, uint32_t components);
uint32_t TypeU32(EmitterState& state);
uint32_t TypeU64(EmitterState& state);
uint32_t TypeScalarU64(EmitterState& state);
uint32_t TypeU32Pair(EmitterState& state);
uint32_t TypeI32(EmitterState& state);
uint32_t TypeI32Pair(EmitterState& state);
uint32_t TypeF32(EmitterState& state);
uint32_t TypeU32Vector(EmitterState& state, uint32_t components);

uint32_t TypeU32Composite(EmitterState& state, uint32_t components);
uint32_t TypeI32Vector(EmitterState& state, uint32_t components);
uint32_t TypeF32Vector(EmitterState& state, uint32_t components);
uint32_t TypePointer(EmitterState& state, spv::StorageClass storage_class, uint32_t pointee);
uint32_t TypeFunction(EmitterState& state);
uint32_t TypeStorageBufferPointer(EmitterState& state);
uint32_t TypeStorageBufferElementPointer(EmitterState& state);
uint32_t TypeStorageBufferU64Pointer(EmitterState& state);
uint32_t TypeStorageBufferU64ElementPointer(EmitterState& state);
uint32_t TypePhysicalU32Pointer(EmitterState& state);
uint32_t TypePushConstantElementPointer(EmitterState& state);
uint32_t TypeU32ArrayPointer(EmitterState& state, spv::StorageClass storage_class, uint32_t dwords);
uint32_t TypeU32ElementPointer(EmitterState& state, spv::StorageClass storage_class);

inline void EmitLabel(EmitterState& state, uint32_t label) {
	state.current_label = label;
	state.builder.AddFunction({spv::OpLabel, label});
}

uint32_t TypeId(EmitterState& state, IR::Type type);

// Shared instruction construction; typed aliases add no forwarding functions.
template <spv::Op opcode, IR::Type type, typename... Args>
uint32_t EmitNative(EmitterState& state, Args... args) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({opcode, TypeId(state, type), result, args...});
	return result;
}

uint32_t GlslStd450(EmitterState& state);

template <GLSLstd450 opcode, IR::Type type, typename... Args>
uint32_t EmitGlsl(EmitterState& state, Args... args) {
	return EmitNative<spv::OpExtInst, type>(state, GlslStd450(state), opcode, args...);
}

inline uint32_t Unary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, result, value});
	return result;
}

inline uint32_t Binary(EmitterState& state, uint32_t opcode, uint32_t type, uint32_t lhs,
                       uint32_t rhs) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({opcode, type, result, lhs, rhs});
	return result;
}

inline uint32_t Select(EmitterState& state, uint32_t type, uint32_t condition, uint32_t true_value,
                       uint32_t false_value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpSelect, type, result, condition, true_value, false_value});
	return result;
}

struct ValueEmitContext {
	explicit ValueEmitContext(EmitterState& state_): state(state_) {}

	uint32_t              Def(IR::Value value);
	uint32_t              Arg(const IR::Inst& inst, size_t index);
	uint32_t              HalfArg(const IR::Inst& inst, size_t index, uint32_t half);
	uint32_t              Ballot(IR::Value predicate);
	uint32_t              FirstLane(uint32_t ballot);
	uint32_t              Shuffle(const IR::Inst& inst, size_t index, uint32_t lane);
	uint32_t              Result(const IR::Inst& inst);
	uint32_t              Define(const IR::Inst& inst, uint32_t value);
	uint32_t              ResourceIndex(IR::Value value, IR::ValueOpcode opcode);
	const IR::Inst*       ImageAddress(IR::Value value);
	const IR::MemoryInfo& Memory(const IR::Inst& inst) const;
	const IR::ExportInfo& Export(const IR::Inst& inst) const;
	uint32_t              Label(const IR::Block* block) const;
	[[noreturn]] void     Fail(const char* reason) const;
	[[noreturn]] void     Fail(const IR::Inst& inst, const char* reason) const;

	EmitterState&                                                      state;
	std::unordered_map<const IR::Inst*, uint32_t>                      definitions;
	const std::unordered_map<const IR::Inst*, uint32_t>*               dispatcher_spills = nullptr;
	std::unordered_map<const IR::Inst*, std::pair<uint32_t, uint32_t>> dispatcher_block_loads;
	uint32_t                                                           scratch_u32_variable = 0;
	ValueEmitContext*                                                  other_half = nullptr;
	uint32_t                                                           half       = 0;
};

enum class VertexInputScalarKind { Float, Sint, Uint };

constexpr uint32_t NoImageComponent = 0xffffffffu;

struct DppTargetLane {
	uint32_t lane  = 0;
	uint32_t valid = 0;
};

struct ImageSampleLayout {
	uint32_t offset = NoImageComponent;
	uint32_t dref   = NoImageComponent;
	uint32_t bias   = NoImageComponent;
	uint32_t coord  = 0;
	uint32_t lod    = NoImageComponent;
	uint32_t grad_x = NoImageComponent;
	uint32_t grad_y = NoImageComponent;
};

struct F32Class {
	uint32_t bits = 0;
	uint32_t nan  = 0;
	uint32_t zero = 0;
};

uint32_t PixelParameterLocation(const EmitterState& state, uint32_t attr);

bool PixelParameterIsFlat(const EmitterState& state, uint32_t attr);

bool PixelParameterIsCustom(const EmitterState& state, uint32_t attr);

VertexInputScalarKind VertexParameterScalarKind(const EmitterState& state, uint32_t location);

uint32_t VertexParameterComponentCount(const InputBinding& input);

uint32_t VertexParameterScalarType(EmitterState& state, VertexInputScalarKind kind);


uint32_t OutputVariableForExport(const EmitterState& state, const IR::ExportInfo& exp);

uint32_t ConstantU32(EmitterState& state, uint32_t value);

uint32_t EmitSubgroupLocalInvocationId(EmitterState& state);

[[noreturn]] void ExitDescriptorBindingFailure(const EmitterState&       state,
                                               IR::DescriptorBindingKind kind, uint32_t resource,
                                               const char* reason);

uint32_t ResourceForDescriptor(const EmitterState& state, IR::DescriptorBindingKind kind,
                               uint32_t resource);

uint32_t DescriptorElementPointer(EmitterState& state, uint32_t result_ptr_type,
                                  uint32_t variable_id, uint32_t array_index,
                                  IR::DescriptorBindingKind kind, uint32_t resource,
                                  const char* variable_name);

uint32_t ImageScalarType(EmitterState& state, Prospero::TextureNumericClass numeric_class);

uint32_t ImageVectorType(EmitterState& state, Prospero::TextureNumericClass numeric_class,
                         uint32_t components);

uint32_t ImageType(EmitterState& state, const IR::ImageResource& image);

uint32_t ImageViewSizeType(EmitterState& state, ImageDimension dimension);

uint32_t LoadSampledImageDescriptor(EmitterState& state, uint32_t resource);

uint32_t LoadSamplerDescriptor(EmitterState& state, uint32_t sampler);

uint32_t MakeSampledImage(EmitterState& state, uint32_t resource, uint32_t sampler);

uint32_t StorageImageDescriptorPointer(EmitterState& state, uint32_t resource);

void EmitStorageImageWrite(EmitterState& state, uint32_t resource, uint32_t mip_lod, uint32_t coord,
                           uint32_t texel);

spv::ExecutionModel ExecutionModelForStage(ShaderType stage);

uint32_t ConstantU32(EmitterState& state, uint32_t value);

uint32_t ConstantI32(EmitterState& state, int32_t value);

uint32_t ConstantF32(EmitterState& state, uint32_t bits);

uint32_t FloatBits(float value);

uint32_t ConstantF32Value(EmitterState& state, float value);

uint32_t ConstantBool(EmitterState& state, bool value);

uint32_t ConstantU64(EmitterState& state, uint64_t value);

uint32_t ConstantU32CompositeZero(EmitterState& state, uint32_t components);

void     DefineModule(EmitterState& state);
void     DefineMeshOutputs(EmitterState& state);
void     EmitMeshEntryPoint(EmitterState& state);
void     EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst);
uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index = 0);
uint32_t MeshPrimitivePointer(EmitterState& state);

DppTargetLane EmitDppQuadPermTargetLane(EmitterState& state, uint32_t subid, uint32_t control);

DppTargetLane EmitDppRowShiftTargetLane(EmitterState& state, uint32_t subid, uint32_t amount,
                                        bool left);

DppTargetLane EmitDppRowRotateRightTargetLane(EmitterState& state, uint32_t subid, uint32_t amount);

DppTargetLane EmitDppMirrorTargetLane(EmitterState& state, uint32_t subid, bool half_row);

DppTargetLane EmitDppTargetLane(EmitterState& state, uint32_t control);

uint32_t EmitSubgroupLocalInvocationId(EmitterState& state);

uint32_t InputVariableForKind(const EmitterState& state, IR::StageInputKind kind);

const InputBinding* InputBindingForParameter(const EmitterState& state, uint32_t location);

uint32_t EmitVertexParameterComponentU32(EmitterState& state, const InputBinding& input,
                                         uint32_t component);

uint32_t EmitInputComponentU32(EmitterState& state, IR::StageInputKind kind, uint32_t component);

uint32_t EmitLocalInvocationIndex(EmitterState& state);

uint32_t EmitBallotLaneActiveBool(EmitterState& state, uint32_t ballot, uint32_t lane);

uint32_t EmitSubgroupLaneActiveBool(EmitterState& state, uint32_t lane);

inline constexpr auto EmitAddU32 = EmitNative<spv::OpIAdd, IR::Type::U32, uint32_t, uint32_t>;

uint32_t EmitBinaryU32(EmitterState& state, uint32_t opcode, uint32_t lhs, uint32_t rhs);

uint32_t EmitShaderDataDwordLoad(EmitterState& state, uint32_t dword_index);

uint32_t StorageBufferPackedStride(const EmitterState& state, const IR::MemoryInfo& mem);

Prospero::BufferFormat StorageBufferFormat(const EmitterState& state, const IR::MemoryInfo& mem);

void EmitMemoryOffsets(EmitterState& state);

uint32_t LdsDwordCount(const EmitterState& state);

struct MemoryResourceAccess {
	IR::ResourceKind kind             = IR::ResourceKind::None;
	uint32_t         object_pointer   = 0;
	uint32_t         length           = 0;
	uint32_t         index_offset     = 0;
	uint32_t         byte_offset      = 0;
	bool             add_index_offset = false;
};

MemoryResourceAccess PrepareMemoryResourceAccess(EmitterState& state, const IR::MemoryInfo& mem);

MemoryResourceAccess PrepareStorageBufferResourceAccess(EmitterState&         state,
                                                        const IR::MemoryInfo& mem,
                                                        uint32_t variable, uint32_t pointer_type);

uint32_t EmitMemoryElementIndex(EmitterState& state, const MemoryResourceAccess& access,
                                uint32_t raw_index);

uint32_t EmitMemoryElementInBounds(EmitterState& state, const MemoryResourceAccess& access,
                                   uint32_t index);

uint32_t EmitMemoryElementPointer(EmitterState& state, const MemoryResourceAccess& access,
                                  uint32_t index);

uint32_t EmitStorageBufferElementPointer(EmitterState& state, const MemoryResourceAccess& access,
                                         uint32_t index, uint32_t pointer_type);

uint32_t EmitTBufferBitcastU32ToI32(EmitterState& state, uint32_t value);

inline constexpr auto EmitTBufferSelectF32 =
    EmitNative<spv::OpSelect, IR::Type::F32, uint32_t, uint32_t, uint32_t>;

bool IsSignedFormatComponent(Format::ComponentType type);

uint32_t EmitUFloatToF32Bits(EmitterState& state, uint32_t raw, uint32_t bits);

uint32_t NormalizeFormatComponent(EmitterState& state, const Format::BufferFormatInfo& info,
                                  uint32_t component, uint32_t raw);

void EmitDeviceAtomicMemoryBarrier(EmitterState& state);

uint32_t EmitFloatAtomicReplacement(EmitterState& state, uint32_t old, uint32_t source,
                                    bool max_value);

uint32_t EmitDsSwizzleTargetLane(EmitterState& state, uint32_t subid, uint32_t control);

inline constexpr auto EmitSelectValueU32 =
    EmitNative<spv::OpSelect, IR::Type::U32, uint32_t, uint32_t, uint32_t>;

uint32_t EmitAndConstant(EmitterState& state, uint32_t value, uint32_t mask);

uint32_t EmitShiftRightConstant(EmitterState& state, uint32_t value, uint32_t shift);

inline constexpr auto EmitOrU32 = EmitNative<spv::OpBitwiseOr, IR::Type::U32, uint32_t, uint32_t>;

uint32_t EmitCompareU32Constant(EmitterState& state, uint32_t opcode, uint32_t value,
                                uint32_t constant);

uint32_t EmitSubConstantMinusU32(EmitterState& state, uint32_t constant, uint32_t value);

uint32_t EmitF32ToF16RtzBits(EmitterState& state, uint32_t f32);

uint32_t EmitMinMaxU32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value);

uint32_t EmitMinMaxI32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value);

inline constexpr auto EmitBitcastF32ToU32 = EmitNative<spv::OpBitcast, IR::Type::U32, uint32_t>;

inline constexpr auto EmitBitcastU32ToF32 = EmitNative<spv::OpBitcast, IR::Type::F32, uint32_t>;

inline constexpr auto EmitAndU32 = EmitNative<spv::OpBitwiseAnd, IR::Type::U32, uint32_t, uint32_t>;

inline constexpr auto EmitLogicalAndBool =
    EmitNative<spv::OpLogicalAnd, IR::Type::U1, uint32_t, uint32_t>;

inline constexpr auto EmitLogicalOrBool =
    EmitNative<spv::OpLogicalOr, IR::Type::U1, uint32_t, uint32_t>;

inline constexpr auto EmitLogicalNotBool = EmitNative<spv::OpLogicalNot, IR::Type::U1, uint32_t>;

F32Class EmitClassifyF32Bits(EmitterState& state, uint32_t bits);

F32Class EmitClassifyF32(EmitterState& state, uint32_t value);

uint32_t EmitClassMaskBitMatch(EmitterState& state, uint32_t mask, uint32_t bit,
                               uint32_t class_match);

uint32_t EmitClassMaskF32(EmitterState& state, uint32_t value, uint32_t mask);

uint32_t EmitMinMaxF32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value);

inline constexpr auto EmitTruncF32Value = EmitGlsl<GLSLstd450Trunc, IR::Type::F32, uint32_t>;

uint32_t EmitFlushF32DenormToSignedZero(EmitterState& state, uint32_t value);

uint32_t EmitTrigCycleF32(EmitterState& state, uint32_t src, bool preserve_signed_zero);

inline constexpr auto EmitFNegateValue = EmitNative<spv::OpFNegate, IR::Type::F32, uint32_t>;

inline constexpr auto EmitFAbsValue = EmitGlsl<GLSLstd450FAbs, IR::Type::F32, uint32_t>;

uint32_t EmitF16BitsToF32(EmitterState& state, uint32_t bits);

void EmitProgram(EmitterState& state);

void DefineGetBdaPointer(EmitterState& state);

// These templates accept local lambdas from several emitter translation units.
template <typename Fn>
void EmitIfCondition(EmitterState& state, uint32_t condition, Fn&& fn) {
	const auto then_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone});
	state.builder.AddFunction({spv::OpBranchConditional, condition, then_label, merge_label});
	EmitLabel(state, then_label);
	fn();
	state.builder.AddFunction({spv::OpBranch, merge_label});
	EmitLabel(state, merge_label);
}

template <typename Fn>
uint32_t EmitValueOrDefaultIfCondition(EmitterState& state, uint32_t condition, uint32_t type,
                                       uint32_t default_value, Fn&& fn) {
	const auto then_label  = state.builder.AllocateId();
	const auto then_exit   = state.builder.AllocateId();
	const auto else_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone});
	state.builder.AddFunction({spv::OpBranchConditional, condition, then_label, else_label});
	EmitLabel(state, then_label);
	const auto then_value = fn();
	state.builder.AddFunction({spv::OpBranch, then_exit});
	EmitLabel(state, then_exit);
	state.builder.AddFunction({spv::OpBranch, merge_label});
	EmitLabel(state, else_label);
	state.builder.AddFunction({spv::OpBranch, merge_label});
	EmitLabel(state, merge_label);
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(
	    {spv::OpPhi, type, value, then_value, then_exit, default_value, else_label});
	return value;
}

template <typename Fn>
uint32_t EmitValueOrZeroIfCondition(EmitterState& state, uint32_t condition, Fn&& fn) {
	return EmitValueOrDefaultIfCondition(state, condition, TypeU32(state), ConstantU32(state, 0),
	                                     std::forward<Fn>(fn));
}

template <typename Fn>
uint32_t AtomicUpdate(EmitterState& state, uint32_t pointer, IR::ResourceKind kind, Fn&& desired) {
	const auto scope  = kind == IR::ResourceKind::Lds ? spv::ScopeWorkgroup : spv::ScopeDevice;
	const auto memory = [&] {
		switch (kind) {
			case IR::ResourceKind::Lds: return spv::MemorySemanticsWorkgroupMemoryMask;
			case IR::ResourceKind::Image: return spv::MemorySemanticsImageMemoryMask;
			default: return spv::MemorySemanticsUniformMemoryMask;
		}
	}();
	const auto preheader = state.builder.AllocateId();
	const auto header    = state.builder.AllocateId();
	const auto cont      = state.builder.AllocateId();
	const auto merge     = state.builder.AllocateId();
	const auto initial   = state.builder.AllocateId();
	const auto observed  = state.builder.AllocateId();
	const auto exchanged = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpBranch, preheader});
	EmitLabel(state, preheader);
	state.builder.AddFunction({spv::OpAtomicLoad, TypeU32(state), initial, pointer,
	                           ConstantU32(state, scope),
	                           ConstantU32(state, spv::MemorySemanticsMaskNone)});
	state.builder.AddFunction({spv::OpBranch, header});
	EmitLabel(state, header);
	state.builder.AddFunction(
	    {spv::OpPhi, TypeU32(state), observed, initial, preheader, exchanged, cont});
	const auto next = desired(observed);
	state.builder.AddFunction({spv::OpAtomicCompareExchange, TypeU32(state), exchanged, pointer,
	                           ConstantU32(state, scope),
	                           ConstantU32(state, spv::MemorySemanticsMaskNone),
	                           ConstantU32(state, spv::MemorySemanticsMaskNone), next, observed});
	const auto success = state.builder.AllocateId();
	state.builder.AddFunction({spv::OpIEqual, TypeBool(state), success, exchanged, observed});
	state.builder.AddFunction({spv::OpLoopMerge, merge, cont, spv::LoopControlMaskNone});
	state.builder.AddFunction({spv::OpBranchConditional, success, merge, cont});
	EmitLabel(state, cont);
	state.builder.AddFunction({spv::OpBranch, header});
	EmitLabel(state, merge);
	state.builder.AddFunction(
	    {spv::OpMemoryBarrier, ConstantU32(state, scope),
	     ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask | memory)});
	return observed;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_INTERNAL_H_ */

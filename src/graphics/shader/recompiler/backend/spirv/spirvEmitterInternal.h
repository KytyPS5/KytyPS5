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
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

enum : uint32_t {
	ExecutionModelVertex                     = 0,
	ExecutionModelFragment                   = 4,
	ExecutionModelGLCompute                  = 5,
	ExecutionModeOriginUpperLeft             = 7,
	ExecutionModeEarlyFragmentTests          = 9,
	ExecutionModeDepthReplacing              = 12,
	ExecutionModeLocalSize                   = 17,
	ExecutionModeSignedZeroInfNanPreserve    = 4461,
	ExecutionModeDerivativeGroupQuadsKHR     = 5289,
	AddressingModelLogical                   = 0,
	AddressingModelPhysicalStorageBuffer64   = 5348,
	MemoryModelGLSL450                       = 1,
	CapabilityShader                         = 1,
	CapabilityInt64                          = 11,
	CapabilityInt64Atomics                   = 12,
	CapabilityImageGatherExtended            = 25,
	CapabilityClipDistance                   = 32,
	CapabilityCullDistance                   = 33,
	CapabilitySampled1D                      = 43,
	CapabilityImage1D                        = 44,
	CapabilityImageQuery                     = 50,
	CapabilityStorageImageWriteWithoutFormat = 56,
	CapabilityGroupNonUniform                = 61,
	CapabilityGroupNonUniformBallot          = 64,
	CapabilityGroupNonUniformShuffle         = 65,
	CapabilitySignedZeroInfNanPreserve       = 4466,
	CapabilityShaderViewportIndexLayerEXT    = 5254,
	CapabilityFragmentBarycentricKHR         = 5284,
	CapabilityComputeDerivativeGroupQuadsKHR = 5288,
	CapabilityPhysicalStorageBufferAddresses = 5347,
	StorageClassUniformConstant              = 0,
	StorageClassInput                        = 1,
	StorageClassOutput                       = 3,
	StorageClassWorkgroup                    = 4,
	StorageClassFunction                     = 7,
	StorageClassPushConstant                 = 9,
	StorageClassImage                        = 11,
	StorageClassStorageBuffer                = 12,
	StorageClassPhysicalStorageBuffer        = 5349,
	FunctionControlNone                      = 0,
	SelectionControlNone                     = 0,
	LoopControlNone                          = 0,
};

enum : uint32_t {
	DecorationBlock         = 2,
	DecorationBuiltIn       = 11,
	DecorationNoPerspective = 13,
	DecorationFlat          = 14,
	DecorationAliased       = 20,
	DecorationLocation      = 30,
	DecorationArrayStride   = 6,
	DecorationBinding       = 33,
	DecorationDescriptorSet = 34,
	DecorationOffset        = 35,
	DecorationPerVertexKHR  = 5285,
};

enum : uint32_t {
	BuiltInPosition                  = 0,
	BuiltInPointSize                 = 1,
	BuiltInClipDistance              = 3,
	BuiltInCullDistance              = 4,
	BuiltInLayer                     = 9,
	BuiltInFragCoord                 = 15,
	BuiltInFrontFacing               = 17,
	BuiltInSampleMask                = 20,
	BuiltInFragDepth                 = 22,
	BuiltInWorkgroupId               = 26,
	BuiltInLocalInvocationId         = 27,
	BuiltInGlobalInvocationId        = 28,
	BuiltInLocalInvocationIndex      = 29,
	BuiltInSubgroupLocalInvocationId = 41,
	BuiltInVertexIndex               = 42,
	BuiltInInstanceIndex             = 43,
	BuiltInBaryCoordKHR              = 5286,
	BuiltInBaryCoordNoPerspKHR       = 5287,
};

enum : uint32_t {
	Dim1D              = 0,
	Dim3D              = 2,
	Dim2D              = 1,
	ImageFormatUnknown = 0,
	ImageFormatRgba32f = 1,
	ImageFormatR32ui   = 33,
};

enum : uint32_t {
	ImageOperandsBiasMask         = 0x00000001u,
	ImageOperandsLodMask          = 0x00000002u,
	ImageOperandsGradMask         = 0x00000004u,
	ImageOperandsOffsetMask       = 0x00000010u,
	ImageOperandsConstOffsetsMask = 0x00000020u,
	ImageOperandsSampleMask       = 0x00000040u,
};

enum : uint32_t {
	ScopeDevice                    = 1,
	ScopeWorkgroup                 = 2,
	ScopeSubgroup                  = 3,
	MemorySemanticsNone            = 0,
	MemorySemanticsAcquireRelease  = 0x00000008u,
	MemorySemanticsUniformMemory   = 0x00000040u,
	MemorySemanticsWorkgroupMemory = 0x00000100u,
	MemorySemanticsImageMemory     = 0x00000800u,
};

enum : uint32_t {
	OpExtInst                      = 12,
	OpTypeVoid                     = 19,
	OpTypeBool                     = 20,
	OpTypeInt                      = 21,
	OpTypeFloat                    = 22,
	OpTypeVector                   = 23,
	OpTypeImage                    = 25,
	OpTypeSampler                  = 26,
	OpTypeSampledImage             = 27,
	OpTypeArray                    = 28,
	OpTypeRuntimeArray             = 29,
	OpTypeStruct                   = 30,
	OpTypePointer                  = 32,
	OpTypeFunction                 = 33,
	OpConstantTrue                 = 41,
	OpConstantFalse                = 42,
	OpConstant                     = 43,
	OpConstantComposite            = 44,
	OpUndef                        = 1,
	OpFunction                     = 54,
	OpFunctionParameter            = 55,
	OpFunctionEnd                  = 56,
	OpFunctionCall                 = 57,
	OpVariable                     = 59,
	OpImageTexelPointer            = 60,
	OpLoad                         = 61,
	OpStore                        = 62,
	OpAccessChain                  = 65,
	OpArrayLength                  = 68,
	OpDecorate                     = 71,
	OpMemberDecorate               = 72,
	OpVectorShuffle                = 79,
	OpCompositeConstruct           = 80,
	OpCompositeExtract             = 81,
	OpCopyObject                   = 83,
	OpSampledImage                 = 86,
	OpImageSampleImplicitLod       = 87,
	OpImageSampleExplicitLod       = 88,
	OpImageSampleDrefImplicitLod   = 89,
	OpImageSampleDrefExplicitLod   = 90,
	OpImageFetch                   = 95,
	OpImageGather                  = 96,
	OpImageDrefGather              = 97,
	OpImageWrite                   = 99,
	OpImageQuerySizeLod            = 103,
	OpImageQueryLod                = 105,
	OpImageQuerySize               = 104,
	OpImageQueryLevels             = 106,
	OpConvertFToU                  = 109,
	OpConvertFToS                  = 110,
	OpConvertSToF                  = 111,
	OpConvertUToF                  = 112,
	OpUConvert                     = 113,
	OpConvertUToPtr                = 120,
	OpBitcast                      = 124,
	OpSNegate                      = 126,
	OpFNegate                      = 127,
	OpIAdd                         = 128,
	OpFAdd                         = 129,
	OpISub                         = 130,
	OpFSub                         = 131,
	OpIMul                         = 132,
	OpFMul                         = 133,
	OpUDiv                         = 134,
	OpFDiv                         = 136,
	OpIAddCarry                    = 149,
	OpUMulExtended                 = 151,
	OpSMulExtended                 = 152,
	OpAny                          = 154,
	OpAll                          = 155,
	OpLogicalNotEqual              = 165,
	OpLogicalOr                    = 166,
	OpLogicalAnd                   = 167,
	OpLogicalNot                   = 168,
	OpSelect                       = 169,
	OpIEqual                       = 170,
	OpINotEqual                    = 171,
	OpUGreaterThan                 = 172,
	OpSGreaterThan                 = 173,
	OpUGreaterThanEqual            = 174,
	OpSGreaterThanEqual            = 175,
	OpULessThan                    = 176,
	OpSLessThan                    = 177,
	OpULessThanEqual               = 178,
	OpSLessThanEqual               = 179,
	OpFOrdEqual                    = 180,
	OpFUnordEqual                  = 181,
	OpFOrdNotEqual                 = 182,
	OpFUnordNotEqual               = 183,
	OpFOrdLessThan                 = 184,
	OpFUnordLessThan               = 185,
	OpFOrdGreaterThan              = 186,
	OpFUnordGreaterThan            = 187,
	OpFOrdLessThanEqual            = 188,
	OpFUnordLessThanEqual          = 189,
	OpFOrdGreaterThanEqual         = 190,
	OpFUnordGreaterThanEqual       = 191,
	OpShiftRightLogical            = 194,
	OpShiftRightArithmetic         = 195,
	OpShiftLeftLogical             = 196,
	OpBitwiseOr                    = 197,
	OpBitwiseXor                   = 198,
	OpBitwiseAnd                   = 199,
	OpNot                          = 200,
	OpBitFieldInsert               = 201,
	OpBitFieldSExtract             = 202,
	OpBitFieldUExtract             = 203,
	OpBitReverse                   = 204,
	OpBitCount                     = 205,
	OpControlBarrier               = 224,
	OpMemoryBarrier                = 225,
	OpAtomicLoad                   = 227,
	OpAtomicExchange               = 229,
	OpAtomicCompareExchange        = 230,
	OpAtomicIAdd                   = 234,
	OpAtomicISub                   = 235,
	OpAtomicSMin                   = 236,
	OpAtomicUMin                   = 237,
	OpAtomicSMax                   = 238,
	OpAtomicUMax                   = 239,
	OpAtomicAnd                    = 240,
	OpAtomicOr                     = 241,
	OpAtomicXor                    = 242,
	OpPhi                          = 245,
	OpLoopMerge                    = 246,
	OpSelectionMerge               = 247,
	OpLabel                        = 248,
	OpBranch                       = 249,
	OpBranchConditional            = 250,
	OpSwitch                       = 251,
	OpKill                         = 252,
	OpReturn                       = 253,
	OpReturnValue                  = 254,
	OpGroupNonUniformBallot        = 339,
	OpGroupNonUniformBallotFindLSB = 343,
	OpGroupNonUniformShuffle       = 345,
};

enum : uint32_t {
	MemoryAccessAlignedMask = 0x2,
};

enum : uint32_t {
	GlslRoundEven       = 2,
	GlslTrunc           = 3,
	GlslFAbs            = 4,
	GlslFloor           = 8,
	GlslCeil            = 9,
	GlslFract           = 10,
	GlslSin             = 13,
	GlslCos             = 14,
	GlslExp2            = 29,
	GlslLog2            = 30,
	GlslSqrt            = 31,
	GlslInverseSqrt     = 32,
	GlslFMin            = 37,
	GlslFMax            = 40,
	GlslFClamp          = 43,
	GlslLdexp           = 53,
	GlslFma             = 50,
	GlslPackSnorm2x16   = 56,
	GlslPackUnorm2x16   = 57,
	GlslPackHalf2x16    = 58,
	GlslUnpackUnorm2x16 = 61,
	GlslUnpackHalf2x16  = 62,
	GlslFindILsb        = 73,
	GlslFindUMsb        = 75,
};

struct InputBinding {
	IR::StageInputKind kind            = IR::StageInputKind::VertexIndex;
	uint32_t           location        = 0;
	uint32_t           component_count = 1;
	uint32_t           variable_id     = 0;
	std::string        debug_name;
	bool               per_vertex = false;
};

struct OutputBinding {
	IR::StageOutputKind kind        = IR::StageOutputKind::Parameter;
	uint32_t            index       = 0;
	uint32_t            location    = 0;
	uint32_t            variable_id = 0;
	std::string         debug_name;
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
    {ImageDimension::Dim1D, Dim1D, 1, 1, 0, 0},
    {ImageDimension::Dim1DArray, Dim1D, 2, 1, 1, 0},
    {ImageDimension::Dim2D, Dim2D, 2, 2, 0, 0},
    {ImageDimension::Dim2DArray, Dim2D, 3, 2, 1, 0},
    {ImageDimension::Dim3D, Dim3D, 3, 3, 0, 0},
    {ImageDimension::Dim2DMsaa, Dim2D, 2, 2, 0, 1},
    {ImageDimension::Dim2DMsaaArray, Dim2D, 3, 2, 1, 1},
}};

const ImageDimensionInfo& ImageDimensionInfoFor(ImageDimension dimension);

struct EmitterState {
	EmitterState(const IR::Program& program_, ShaderStageInputInfo input_info_)
	    : program(program_), input_info(input_info_),
	      requirements(*program_.spirv_requirements) {}

	Builder                                          builder;
	const IR::Program&                               program;
	ShaderStageInputInfo                             input_info;
	const IR::SpirvRequirements&                     requirements;
	ShaderType                                       stage                   = ShaderType::Unknown;
	uint32_t                                         wave_size               = 64;
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
	uint32_t                                         scratch_variable        = 0;
	std::array<uint32_t, IR::ImageBindingCount>      image_variables {};
	uint32_t                   sampler_variable                      = 0;
	uint32_t                   main_func                             = 0;
	uint32_t                   entry_label                           = 0;
	uint32_t                   current_label                         = 0;
	uint32_t                   pixel_valid_mask_variable             = 0;
	uint32_t                   subgroup_local_invocation_id_variable = 0;
	uint32_t                   per_vertex_variable                   = 0;
	uint32_t                   point_size_variable                   = 0;
	uint32_t                   clip_distance_variable                = 0;
	uint32_t                   cull_distance_variable                = 0;
	uint32_t                   layer_variable                        = 0;
	uint32_t                   clip_distance_count                   = 0;
	uint32_t                   cull_distance_count                   = 0;
	uint32_t                   depth_variable                        = 0;
	uint32_t                   sample_mask_variable                  = 0;
	std::vector<InputBinding>  inputs;
	std::vector<OutputBinding> outputs;
	std::vector<uint32_t>      interface_variables;
};

uint32_t TypeVoid(EmitterState& state);
uint32_t TypeBool(EmitterState& state);
uint32_t TypeBoolVector(EmitterState& state, uint32_t components);
uint32_t TypeU32(EmitterState& state);
uint32_t TypeU64(EmitterState& state);
uint32_t TypeDeviceAddress(EmitterState& state);
uint32_t TypeScalarU64(EmitterState& state);
uint32_t TypeU32Pair(EmitterState& state);
uint32_t TypeI32(EmitterState& state);
uint32_t TypeI32Pair(EmitterState& state);
uint32_t TypeF32(EmitterState& state);
uint32_t TypeU32Vector(EmitterState& state, uint32_t components);

uint32_t TypeU32Composite(EmitterState& state, uint32_t components);
uint32_t TypeI32Vector(EmitterState& state, uint32_t components);
uint32_t TypeF32Vector(EmitterState& state, uint32_t components);
uint32_t TypePointer(EmitterState& state, uint32_t storage_class, uint32_t pointee);
uint32_t TypeFunction(EmitterState& state);
uint32_t TypeStorageBufferPointer(EmitterState& state);
uint32_t TypeStorageBufferElementPointer(EmitterState& state);
uint32_t TypeStorageBufferU64Pointer(EmitterState& state);
uint32_t TypeStorageBufferU64ElementPointer(EmitterState& state);
uint32_t TypeDeviceAddressStoragePointer(EmitterState& state);
uint32_t TypePhysicalU32Pointer(EmitterState& state);
uint32_t TypePushConstantElementPointer(EmitterState& state);
uint32_t TypeU32ArrayPointer(EmitterState& state, uint32_t storage_class, uint32_t dwords);
uint32_t TypeU32ElementPointer(EmitterState& state, uint32_t storage_class);

inline void EmitLabel(EmitterState& state, uint32_t label) {
	state.current_label = label;
	state.builder.AddFunction({OpLabel, label});
}

struct ValueEmitContext {
	ValueEmitContext(EmitterState& state_, const IR::Program& program_)
	    : state(state_), program(program_) {}

	uint32_t              Def(IR::Value value);
	uint32_t              Arg(const IR::Inst& inst, size_t index);
	uint32_t              Result(const IR::Inst& inst);
	uint32_t              TypeId(IR::Type type) const;
	uint32_t              Emit(const IR::Inst& inst, uint32_t opcode, IR::Type type,
	                           std::initializer_list<uint32_t> args);
	uint32_t              Define(const IR::Inst& inst, uint32_t value);
	uint32_t              ResourceIndex(IR::Value value, IR::ValueOpcode opcode);
	const IR::Inst*       ImageAddress(IR::Value value);
	const IR::MemoryInfo& Memory(const IR::Inst& inst) const;
	const IR::ExportInfo& Export(const IR::Inst& inst) const;
	uint32_t              Label(const IR::Block* block) const;
	[[noreturn]] void     Fail(const char* reason) const;
	[[noreturn]] void     Fail(const IR::Inst& inst, const char* reason) const;

	EmitterState&                                                      state;
	const IR::Program&                                                 program;
	std::unordered_map<const IR::Inst*, uint32_t>                      definitions;
	std::unordered_map<const IR::Block*, uint32_t>                     labels;
	const std::unordered_map<const IR::Inst*, uint32_t>*               dispatcher_spills = nullptr;
	std::unordered_map<const IR::Inst*, std::pair<uint32_t, uint32_t>> dispatcher_block_loads;
	const IR::Block*                                                   current_block = nullptr;
	uint32_t                                                           scratch_u32_variable = 0;
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

uint32_t PixelParameterMappedLocation(const EmitterState& state, uint32_t attr);

uint32_t PixelParameterLocation(const EmitterState& state, uint32_t attr);

bool PixelParameterIsFlat(const EmitterState& state, uint32_t attr);

bool PixelParameterIsCustom(const EmitterState& state, uint32_t attr);

VertexInputScalarKind VertexParameterScalarKind(const EmitterState& state, uint32_t location);

uint32_t VertexParameterComponentCount(const EmitterState& state, const InputBinding& input);

uint32_t VertexParameterScalarType(EmitterState& state, VertexInputScalarKind kind);

uint32_t VertexParameterScalarPointerType(EmitterState& state, VertexInputScalarKind kind);

uint32_t VertexParameterVectorOrScalarType(EmitterState& state, VertexInputScalarKind kind,
                                           uint32_t components);

uint32_t VertexParameterInputPointerType(EmitterState& state, VertexInputScalarKind kind,
                                         uint32_t components);

bool HasOutput(const std::vector<OutputBinding>& outputs, IR::StageOutputKind kind, uint32_t index);

void CopyProgramInputsAndOutputs(EmitterState& state, const IR::Program& program);

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

uint32_t ExecutionModelForStage(ShaderType stage);

uint32_t ConstantU32(EmitterState& state, uint32_t value);

uint32_t ConstantI32(EmitterState& state, int32_t value);

uint32_t ConstantF32(EmitterState& state, uint32_t bits);

uint32_t FloatBits(float value);

uint32_t ConstantF32Value(EmitterState& state, float value);

uint32_t ConstantBool(EmitterState& state, bool value);

uint32_t ConstantU64(EmitterState& state, uint64_t value);

uint32_t ConstantU32CompositeZero(EmitterState& state, uint32_t components);

uint32_t GlslStd450(EmitterState& state);

void AllocateInputVariables(EmitterState& state);

void AllocateOutputVariables(EmitterState& state);

uint32_t BuiltInForInput(IR::StageInputKind kind);

void AddInputAnnotationsAndNames(EmitterState& state);

void AddOutputAnnotationsAndNames(EmitterState& state);

void DecorateDescriptor(EmitterState& state, uint32_t variable, const char* name,
                        IR::DescriptorBindingKind kind);

void AddDescriptorAnnotationsAndNames(EmitterState& state);

void DefineModule(EmitterState& state);

uint32_t EmitTrueBool(EmitterState& state);

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

uint32_t EmitAddU32(EmitterState& state, uint32_t lhs, uint32_t rhs);

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

MemoryResourceAccess PrepareStorageBufferResourceAccess(EmitterState& state,
                                                         const IR::MemoryInfo& mem,
                                                         uint32_t variable,
                                                         uint32_t pointer_type);

uint32_t EmitMemoryElementIndex(EmitterState& state, const MemoryResourceAccess& access,
                                uint32_t raw_index);

uint32_t EmitMemoryElementInBounds(EmitterState& state, const MemoryResourceAccess& access,
                                   uint32_t index);

uint32_t EmitMemoryElementPointer(EmitterState& state, const MemoryResourceAccess& access,
                                  uint32_t index);

uint32_t EmitStorageBufferElementPointer(EmitterState& state,
                                         const MemoryResourceAccess& access, uint32_t index,
                                         uint32_t pointer_type);

uint32_t EmitTBufferBitcastF32ToU32(EmitterState& state, uint32_t value);

uint32_t EmitTBufferBitcastU32ToF32(EmitterState& state, uint32_t value);

uint32_t EmitTBufferBitcastU32ToI32(EmitterState& state, uint32_t value);

uint32_t EmitTBufferCompareU32Constant(EmitterState& state, uint32_t opcode, uint32_t value,
                                       uint32_t constant);

uint32_t EmitTBufferSelectF32(EmitterState& state, uint32_t condition, uint32_t true_value,
                              uint32_t false_value);

bool IsSignedFormatComponent(Format::ComponentType type);

uint32_t EmitHalfToF32Bits(EmitterState& state, uint32_t raw);

uint32_t EmitUFloatToF32Bits(EmitterState& state, uint32_t raw, uint32_t bits);

uint32_t NormalizeFormatComponent(EmitterState& state, const Format::BufferFormatInfo& info,
                                  uint32_t component, uint32_t raw);

void EmitDeviceAtomicMemoryBarrier(EmitterState& state);

uint32_t EmitFloatAtomicReplacement(EmitterState& state, uint32_t old, uint32_t source,
                                    bool max_value);

uint32_t EmitDsSwizzleTargetLane(EmitterState& state, uint32_t subid, uint32_t control);

uint32_t EmitSelectValueU32(EmitterState& state, uint32_t cond, uint32_t true_value,
                            uint32_t false_value);

void EmitShiftLeftLogicalU64Values(EmitterState& state, uint32_t low, uint32_t high, uint32_t shift,
                                   uint32_t& out_low, uint32_t& out_high);

void EmitShiftRightLogicalU64Values(EmitterState& state, uint32_t low, uint32_t high,
                                    uint32_t shift, uint32_t& out_low, uint32_t& out_high);

uint32_t EmitAndConstant(EmitterState& state, uint32_t value, uint32_t mask);

uint32_t EmitShiftRightConstant(EmitterState& state, uint32_t value, uint32_t shift);

uint32_t EmitOrU32(EmitterState& state, uint32_t lhs, uint32_t rhs);

uint32_t EmitCompareU32Constant(EmitterState& state, uint32_t opcode, uint32_t value,
                                uint32_t constant);

uint32_t EmitSubConstantMinusU32(EmitterState& state, uint32_t constant, uint32_t value);

uint32_t EmitF32ToF16RtzBits(EmitterState& state, uint32_t f32);

uint32_t EmitMinMaxU32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value);

uint32_t EmitMinMaxI32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value);

uint32_t EmitBitcastF32ToU32(EmitterState& state, uint32_t value);

uint32_t EmitBitcastU32ToF32(EmitterState& state, uint32_t value);

uint32_t EmitAndU32(EmitterState& state, uint32_t lhs, uint32_t rhs);

uint32_t EmitLogicalAndBool(EmitterState& state, uint32_t lhs, uint32_t rhs);

uint32_t EmitLogicalOrBool(EmitterState& state, uint32_t lhs, uint32_t rhs);

uint32_t EmitLogicalNotBool(EmitterState& state, uint32_t value);

F32Class EmitClassifyF32(EmitterState& state, uint32_t value);

uint32_t EmitClassMaskBitMatch(EmitterState& state, uint32_t mask, uint32_t bit,
                               uint32_t class_match);

uint32_t EmitClassMaskF32(EmitterState& state, uint32_t value, uint32_t mask);

uint32_t EmitMinMaxF32Value(EmitterState& state, uint32_t lhs, uint32_t rhs, bool max_value);

uint32_t EmitTruncF32Value(EmitterState& state, uint32_t value);

uint32_t EmitFlushF32DenormToSignedZero(EmitterState& state, uint32_t value);

uint32_t EmitTrigCycleF32(EmitterState& state, uint32_t src, bool preserve_signed_zero);

uint32_t EmitFNegateValue(EmitterState& state, uint32_t value);

uint32_t EmitFAbsValue(EmitterState& state, uint32_t value);

uint32_t EmitF16BitsToF32(EmitterState& state, uint32_t bits);

bool EmitValueAlu(ValueEmitContext& ctx, const IR::Inst& inst);

bool EmitValueFlow(ValueEmitContext& ctx, const IR::Inst& inst);

bool EmitValueMemory(ValueEmitContext& ctx, const IR::Inst& inst);

bool EmitValueImage(ValueEmitContext& ctx, const IR::Inst& inst);

void EmitProgram(EmitterState& state, const IR::Program& program);

void DefineGetBdaPointer(EmitterState& state);

// These templates accept local lambdas from several emitter translation units.
template <typename Fn>
void EmitIfCondition(EmitterState& state, uint32_t condition, Fn&& fn) {
	const auto then_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, condition, then_label, merge_label});
	EmitLabel(state, then_label);
	fn();
	state.builder.AddFunction({OpBranch, merge_label});
	EmitLabel(state, merge_label);
}

template <typename Fn>
uint32_t EmitValueOrDefaultIfCondition(EmitterState& state, uint32_t condition, uint32_t type,
                                       uint32_t default_value, Fn&& fn) {
	const auto then_label  = state.builder.AllocateId();
	const auto then_exit   = state.builder.AllocateId();
	const auto else_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, condition, then_label, else_label});
	EmitLabel(state, then_label);
	const auto then_value = fn();
	state.builder.AddFunction({OpBranch, then_exit});
	EmitLabel(state, then_exit);
	state.builder.AddFunction({OpBranch, merge_label});
	EmitLabel(state, else_label);
	state.builder.AddFunction({OpBranch, merge_label});
	EmitLabel(state, merge_label);
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpPhi, type, value, then_value, then_exit, default_value, else_label});
	return value;
}

template <typename Fn>
uint32_t EmitValueOrZeroIfCondition(EmitterState& state, uint32_t condition, Fn&& fn) {
	return EmitValueOrDefaultIfCondition(state, condition, TypeU32(state), ConstantU32(state, 0),
	                                     std::forward<Fn>(fn));
}

template <typename Fn>
uint32_t AtomicUpdate(EmitterState& state, uint32_t pointer, IR::ResourceKind kind, Fn&& desired) {
	const auto scope = kind == IR::ResourceKind::Lds ? ScopeWorkgroup : ScopeDevice;
	const auto memory = [&] {
		switch (kind) {
			case IR::ResourceKind::Lds: return MemorySemanticsWorkgroupMemory;
			case IR::ResourceKind::Image: return MemorySemanticsImageMemory;
			default: return MemorySemanticsUniformMemory;
		}
	}();
	const auto preheader = state.builder.AllocateId();
	const auto header    = state.builder.AllocateId();
	const auto cont      = state.builder.AllocateId();
	const auto merge     = state.builder.AllocateId();
	const auto initial   = state.builder.AllocateId();
	const auto observed  = state.builder.AllocateId();
	const auto exchanged = state.builder.AllocateId();
	state.builder.AddFunction({OpBranch, preheader});
	EmitLabel(state, preheader);
	state.builder.AddFunction({OpAtomicLoad, TypeU32(state), initial, pointer,
	                           ConstantU32(state, scope), ConstantU32(state, MemorySemanticsNone)});
	state.builder.AddFunction({OpBranch, header});
	EmitLabel(state, header);
	state.builder.AddFunction(
	    {OpPhi, TypeU32(state), observed, initial, preheader, exchanged, cont});
	const auto next = desired(observed);
	state.builder.AddFunction({OpAtomicCompareExchange, TypeU32(state), exchanged, pointer,
	                           ConstantU32(state, scope), ConstantU32(state, MemorySemanticsNone),
	                           ConstantU32(state, MemorySemanticsNone), next, observed});
	const auto success = state.builder.AllocateId();
	state.builder.AddFunction({OpIEqual, TypeBool(state), success, exchanged, observed});
	state.builder.AddFunction({OpLoopMerge, merge, cont, LoopControlNone});
	state.builder.AddFunction({OpBranchConditional, success, merge, cont});
	EmitLabel(state, cont);
	state.builder.AddFunction({OpBranch, header});
	EmitLabel(state, merge);
	state.builder.AddFunction({OpMemoryBarrier, ConstantU32(state, scope),
	                           ConstantU32(state, MemorySemanticsAcquireRelease | memory)});
	return observed;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_INTERNAL_H_ */

#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_INTERNAL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_INTERNAL_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/SpirvBuilder.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
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
	ExecutionModeDerivativeGroupQuadsKHR     = 5289,
	AddressingModelLogical                   = 0,
	MemoryModelGLSL450                       = 1,
	CapabilityShader                         = 1,
	CapabilityImageGatherExtended            = 25,
	CapabilityImageQuery                     = 50,
	CapabilityStorageImageReadWithoutFormat  = 55,
	CapabilityStorageImageWriteWithoutFormat = 56,
	CapabilityGroupNonUniform                = 61,
	CapabilityGroupNonUniformBallot          = 64,
	CapabilityGroupNonUniformShuffle         = 65,
	CapabilityComputeDerivativeGroupQuadsKHR = 5288,
	StorageClassUniformConstant              = 0,
	StorageClassInput                        = 1,
	StorageClassOutput                       = 3,
	StorageClassWorkgroup                    = 4,
	StorageClassFunction                     = 7,
	StorageClassPushConstant                 = 9,
	StorageClassImage                        = 11,
	StorageClassStorageBuffer                = 12,
	FunctionControlNone                      = 0,
	SelectionControlNone                     = 0,
	LoopControlNone                          = 0,
};

enum : uint32_t {
	DecorationBlock         = 2,
	DecorationBuiltIn       = 11,
	DecorationNoPerspective = 13,
	DecorationFlat          = 14,
	DecorationLocation      = 30,
	DecorationArrayStride   = 6,
	DecorationBinding       = 33,
	DecorationDescriptorSet = 34,
	DecorationOffset        = 35,
};

enum : uint32_t {
	BuiltInPosition                  = 0,
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
};

enum : uint32_t {
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
};

enum : uint32_t {
	ScopeDevice                    = 1,
	ScopeWorkgroup                 = 2,
	ScopeSubgroup                  = 3,
	MemorySemanticsNone            = 0,
	MemorySemanticsAcquireRelease  = 0x00000008u,
	MemorySemanticsUniformMemory   = 0x00000040u,
	MemorySemanticsWorkgroupMemory = 0x00000100u,
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
	OpConstant                     = 43,
	OpConstantComposite            = 44,
	OpFunction                     = 54,
	OpFunctionEnd                  = 56,
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
	OpSampledImage                 = 86,
	OpImageSampleImplicitLod       = 87,
	OpImageSampleExplicitLod       = 88,
	OpImageSampleDrefImplicitLod   = 89,
	OpImageSampleDrefExplicitLod   = 90,
	OpImageFetch                   = 95,
	OpImageGather                  = 96,
	OpImageDrefGather              = 97,
	OpImageWrite                   = 99,
	OpImage                        = 100,
	OpImageQuerySizeLod            = 103,
	OpImageQueryLod                = 105,
	OpImageQueryLevels             = 106,
	OpConvertFToU                  = 109,
	OpConvertFToS                  = 110,
	OpConvertSToF                  = 111,
	OpConvertUToF                  = 112,
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
	OpGroupNonUniformBallot        = 339,
	OpGroupNonUniformBallotFindLSB = 343,
	OpGroupNonUniformShuffle       = 345,
};

enum : uint32_t {
	GlslRoundEven      = 2,
	GlslTrunc          = 3,
	GlslFAbs           = 4,
	GlslFloor          = 8,
	GlslCeil           = 9,
	GlslFract          = 10,
	GlslSin            = 13,
	GlslCos            = 14,
	GlslExp2           = 29,
	GlslLog2           = 30,
	GlslSqrt           = 31,
	GlslInverseSqrt    = 32,
	GlslFMin           = 37,
	GlslFMax           = 40,
	GlslFClamp         = 43,
	GlslLdexp          = 53,
	GlslFma            = 50,
	GlslPackSnorm2x16  = 56,
	GlslPackUnorm2x16  = 57,
	GlslPackHalf2x16   = 58,
	GlslUnpackHalf2x16 = 62,
	GlslFindILsb       = 73,
	GlslFindUMsb       = 75,
};

struct RegisterBinding {
	IR::Register reg;
	uint32_t     pointer_id = 0;
};

struct InputBinding {
	IR::StageInputKind kind            = IR::StageInputKind::VertexIndex;
	uint32_t           location        = 0;
	uint32_t           component_count = 1;
	uint32_t           variable_id     = 0;
	std::string        debug_name;
};

struct OutputBinding {
	IR::StageOutputKind kind        = IR::StageOutputKind::Parameter;
	uint32_t            index       = 0;
	uint32_t            location    = 0;
	uint32_t            variable_id = 0;
	std::string         debug_name;
};

struct DescriptorResourceBinding {
	const IR::DescriptorBinding* descriptor  = nullptr;
	uint32_t                     array_index = 0;
};

struct SampledImageDescriptors {
	uint32_t image_type         = 0;
	uint32_t sampled_image_type = 0;
	uint32_t pointer_type       = 0;
	uint32_t array_type         = 0;
	uint32_t array_pointer_type = 0;
	uint32_t variable           = 0;
};

struct EmitterState {
	Builder                                builder;
	const IR::Program*                     program                        = nullptr;
	const IR::ResourceSnapshot*            resources                      = nullptr;
	const ShaderVertexInputInfo*           vertex_input_info              = nullptr;
	const ShaderPixelInputInfo*            pixel_input_info               = nullptr;
	const ShaderComputeInputInfo*          compute_input_info             = nullptr;
	ShaderType                             stage                          = ShaderType::Unknown;
	uint32_t                               wave_size                      = 64;
	bool                                   exact_subgroup_operations      = false;
	bool                                   per_invocation_masks           = false;
	uint32_t                               void_type                      = 0;
	uint32_t                               bool_type                      = 0;
	uint32_t                               uint_type                      = 0;
	uint32_t                               uint_pair_type                 = 0;
	uint32_t                               int_pair_type                  = 0;
	uint32_t                               int_type                       = 0;
	uint32_t                               float_type                     = 0;
	uint32_t                               vec2_uint_type                 = 0;
	uint32_t                               vec3_uint_type                 = 0;
	uint32_t                               vec4_uint_type                 = 0;
	uint32_t                               vec2_int_type                  = 0;
	uint32_t                               vec3_int_type                  = 0;
	uint32_t                               vec4_int_type                  = 0;
	uint32_t                               vec2_float_type                = 0;
	uint32_t                               vec3_float_type                = 0;
	uint32_t                               vec4_float_type                = 0;
	uint32_t                               ptr_func_uint                  = 0;
	uint32_t                               ptr_input_float                = 0;
	uint32_t                               ptr_input_bool                 = 0;
	uint32_t                               ptr_input_int                  = 0;
	uint32_t                               ptr_input_uint                 = 0;
	uint32_t                               ptr_input_vec2_float           = 0;
	uint32_t                               ptr_input_vec3_float           = 0;
	uint32_t                               ptr_input_vec2_int             = 0;
	uint32_t                               ptr_input_vec3_int             = 0;
	uint32_t                               ptr_input_vec4_int             = 0;
	uint32_t                               ptr_input_vec2_uint            = 0;
	uint32_t                               ptr_input_vec3_uint            = 0;
	uint32_t                               ptr_input_vec4_uint            = 0;
	uint32_t                               ptr_input_vec4_float           = 0;
	uint32_t                               sample_mask_array_type         = 0;
	uint32_t                               ptr_output_int                 = 0;
	uint32_t                               ptr_output_sample_mask_array   = 0;
	uint32_t                               ptr_output_float               = 0;
	uint32_t                               ptr_output_vec4_float          = 0;
	uint32_t                               per_vertex_type                = 0;
	uint32_t                               ptr_output_per_vertex          = 0;
	uint32_t                               storage_runtime_array_type     = 0;
	uint32_t                               storage_buffer_type            = 0;
	uint32_t                               ptr_storage_buffer             = 0;
	uint32_t                               ptr_storage_buffer_uint        = 0;
	uint32_t                               storage_buffer_array_type      = 0;
	uint32_t                               ptr_storage_buffer_array       = 0;
	uint32_t                               storage_buffer_variable        = 0;
	uint32_t                               address_memory_array_type      = 0;
	uint32_t                               ptr_address_memory_array       = 0;
	uint32_t                               address_memory_variable        = 0;
	uint32_t                               gds_variable                   = 0;
	uint32_t                               push_constant_u32x4_array_type = 0;
	uint32_t                               push_constant_rows_array_type  = 0;
	uint32_t                               push_constant_block_type       = 0;
	uint32_t                               ptr_push_constant_block        = 0;
	uint32_t                               ptr_push_constant_uint         = 0;
	uint32_t                               push_constant_variable         = 0;
	uint32_t                               vsharp_storage_variable        = 0;
	uint32_t                               flattened_srt_variable         = 0;
	uint32_t                               lds_array_type                 = 0;
	uint32_t                               ptr_workgroup_array            = 0;
	uint32_t                               ptr_workgroup_uint             = 0;
	uint32_t                               lds_variable                   = 0;
	std::array<SampledImageDescriptors, 6> sampled_images;
	uint32_t                               sampler_type                                  = 0;
	uint32_t                               sampler_array_type                            = 0;
	uint32_t                               ptr_uniform_sampler                           = 0;
	uint32_t                               ptr_uniform_sampler_array                     = 0;
	uint32_t                               sampler_variable                              = 0;
	uint32_t                               storage_image_type                            = 0;
	uint32_t                               ptr_uniform_storage_image                     = 0;
	uint32_t                               storage_image_array_type                      = 0;
	uint32_t                               ptr_uniform_storage_image_array               = 0;
	uint32_t                               storage_image_variable                        = 0;
	uint32_t                               storage_image_2d_array_type                   = 0;
	uint32_t                               ptr_uniform_storage_image_2d_array            = 0;
	uint32_t                               storage_image_2d_array_array_type             = 0;
	uint32_t                               ptr_uniform_storage_image_2d_array_array      = 0;
	uint32_t                               storage_image_2d_array_variable               = 0;
	uint32_t                               storage_image_3d_type                         = 0;
	uint32_t                               ptr_uniform_storage_image_3d                  = 0;
	uint32_t                               storage_image_3d_array_type                   = 0;
	uint32_t                               ptr_uniform_storage_image_3d_array            = 0;
	uint32_t                               storage_image_3d_variable                     = 0;
	uint32_t                               storage_image_uint_type                       = 0;
	uint32_t                               ptr_uniform_storage_image_uint                = 0;
	uint32_t                               storage_image_uint_array_type                 = 0;
	uint32_t                               ptr_uniform_storage_image_uint_array          = 0;
	uint32_t                               storage_image_uint_variable                   = 0;
	uint32_t                               storage_image_uint_2d_array_type              = 0;
	uint32_t                               ptr_uniform_storage_image_uint_2d_array       = 0;
	uint32_t                               storage_image_uint_2d_array_array_type        = 0;
	uint32_t                               ptr_uniform_storage_image_uint_2d_array_array = 0;
	uint32_t                               storage_image_uint_2d_array_variable          = 0;
	uint32_t                               storage_image_uint_3d_type                    = 0;
	uint32_t                               ptr_uniform_storage_image_uint_3d             = 0;
	uint32_t                               storage_image_uint_3d_array_type              = 0;
	uint32_t                               ptr_uniform_storage_image_uint_3d_array       = 0;
	uint32_t                               storage_image_uint_3d_variable                = 0;
	uint32_t                               ptr_image_uint                                = 0;
	uint32_t                               func_type                                     = 0;
	uint32_t                               main_func                                     = 0;
	uint32_t                               entry_label                                   = 0;
	uint32_t                               pixel_valid_mask_variable                     = 0;
	bool                                   dispatcher_fallback                           = false;
	uint32_t                               dispatch_pc_variable                          = 0;
	uint32_t                               dispatch_header_label                         = 0;
	uint32_t                               dispatch_select_label                         = 0;
	uint32_t                               dispatch_default_label                        = 0;
	uint32_t                               dispatch_after_switch_label                   = 0;
	uint32_t                               dispatch_continue_label                       = 0;
	uint32_t                               dispatch_merge_label                          = 0;
	uint32_t                               glsl_std450                                   = 0;
	uint32_t                               subgroup_local_invocation_id_variable         = 0;
	uint32_t                               per_vertex_variable                           = 0;
	uint32_t                               depth_variable                                = 0;
	uint32_t                               sample_mask_variable                          = 0;
	bool                                   needs_subgroup_ballot                         = false;
	bool                                   needs_subgroup_shuffle                        = false;
	bool                                   needs_subgroup_local_invocation_id            = false;
	bool                                   needs_compute_derivatives                     = false;
	bool                                   needs_image_gather_extended                   = false;
	bool                                   needs_function_lds                            = false;
	bool                                   needs_pixel_valid_mask                        = false;
	std::vector<RegisterBinding>           registers;
	std::vector<InputBinding>              inputs;
	std::vector<OutputBinding>             outputs;
	std::vector<uint32_t>                  interface_variables;
	std::vector<bool>                      reachable_blocks;
	std::map<uint32_t, uint32_t>           block_labels;
	std::map<uint32_t, uint32_t>           constants;
	std::map<uint32_t, uint32_t>           signed_constants;
	std::map<uint32_t, uint32_t>           float_constants;
};

constexpr uint32_t PsInputOffsetMask = 0x0000001fu;
constexpr uint32_t PsInputFlatShade  = 0x00000400u;

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

enum class ImageViewKind {
	Dim2D,
	Dim2DArray,
	Dim3D,
};

constexpr uint32_t SampledImageIndex(bool integer, ImageViewKind view) {
	return static_cast<uint32_t>(view) + (integer ? 3u : 0u);
}

constexpr IR::DescriptorBindingKind SampledBindingKind(bool integer, ImageViewKind view) {
	if (integer) {
		switch (view) {
			case ImageViewKind::Dim2DArray: return IR::DescriptorBindingKind::SampledUint2DArray;
			case ImageViewKind::Dim3D: return IR::DescriptorBindingKind::SampledUint3D;
			default: return IR::DescriptorBindingKind::SampledUint2D;
		}
	}
	switch (view) {
		case ImageViewKind::Dim2DArray: return IR::DescriptorBindingKind::Sampled2DArray;
		case ImageViewKind::Dim3D: return IR::DescriptorBindingKind::Sampled3D;
		default: return IR::DescriptorBindingKind::Sampled2D;
	}
}

struct AddCarryResult {
	uint32_t sum   = 0;
	uint32_t carry = 0;
};

struct F32Class {
	uint32_t bits       = 0;
	uint32_t nan        = 0;
	uint32_t snan       = 0;
	uint32_t zero       = 0;
	uint32_t quiet_bits = 0;
};

struct CubeF32Values {
	uint32_t x      = 0;
	uint32_t y      = 0;
	uint32_t z      = 0;
	uint32_t nx     = 0;
	uint32_t ny     = 0;
	uint32_t nz     = 0;
	uint32_t z_face = 0;
	uint32_t y_face = 0;
	uint32_t x_neg  = 0;
	uint32_t y_neg  = 0;
	uint32_t z_neg  = 0;
};

struct F16Class {
	uint32_t bits       = 0;
	uint32_t nan        = 0;
	uint32_t snan       = 0;
	uint32_t zero       = 0;
	uint32_t quiet_bits = 0;
};

uint32_t PixelParameterMappedLocation(const EmitterState& state, uint32_t attr);

uint32_t PixelParameterLocation(const EmitterState& state, uint32_t attr);

bool PixelParameterIsFlat(const EmitterState& state, uint32_t attr);

VertexInputScalarKind VertexParameterScalarKind(const EmitterState& state, uint32_t location);

uint32_t VertexParameterComponentCount(const EmitterState& state, const InputBinding& input);

uint32_t VertexParameterScalarType(const EmitterState& state, VertexInputScalarKind kind);

uint32_t VertexParameterScalarPointerType(const EmitterState& state, VertexInputScalarKind kind);

uint32_t VertexParameterVectorOrScalarType(const EmitterState& state, VertexInputScalarKind kind,
                                           uint32_t components);

uint32_t VertexParameterInputPointerType(const EmitterState& state, VertexInputScalarKind kind,
                                         uint32_t components);

void SetError(std::string* error, const char* message);

void CollectRegister(std::vector<RegisterBinding>* registers, IR::Register reg);

IR::Operand MakeRegisterOperand(IR::RegisterFile file, uint32_t index);

IR::Register SccRegister();

IR::Operand SccOperand();

bool IsInactiveWave32ExecHigh(const EmitterState& state, IR::Register reg);

bool IsMaskRegisterFile(IR::RegisterFile file);

bool IsSccOperand(const IR::Operand& operand);

bool IsCompareOpcode(IR::Opcode op);

void CollectMaskStateRegisters(std::vector<RegisterBinding>* registers);

void CollectSequentialRegisters(std::vector<RegisterBinding>* registers, const IR::Operand& base,
                                uint32_t count);

uint32_t MaxCollectedVectorRegisterEnd(const std::vector<RegisterBinding>& registers);

void CollectMoveRelSourceRegisters(const IR::Program&            program,
                                   std::vector<RegisterBinding>* registers);

bool IsPairDwordOpcode(IR::Opcode op);

uint32_t PairDwordSourceCount(IR::Opcode op, uint32_t src_count);

void CollectRegisters(const IR::Program& program, std::vector<RegisterBinding>* registers);

bool HasOutput(const std::vector<OutputBinding>& outputs, IR::StageOutputKind kind, uint32_t index);

void CopyProgramInputsAndOutputs(EmitterState* state, const IR::Program& program);

uint32_t OutputVariableForExport(const EmitterState& state, const IR::ExportInfo& exp);

bool ProgramNeedsComputeDerivatives(const IR::Program& program);

bool ProgramNeedsImageGatherExtended(const IR::Program& program);

bool IsLdsOpcode(IR::Opcode op);

bool ProgramNeedsFunctionLds(const IR::Program& program);

bool ProgramNeedsPixelValidMask(const IR::Program& program);

bool InstructionHasDppSource(const IR::Instruction& inst);

bool ProgramNeedsSubgroupBallot(const IR::Program& program);

bool ProgramNeedsSubgroupShuffle(const IR::Program& program);

bool IsCompareOpcode(IR::Opcode op);

bool ProgramNeedsSubgroupLocalInvocationId(const IR::Program& program);

uint32_t PointerForRegister(const EmitterState& state, IR::Register reg);

uint32_t ConstantU32(EmitterState* state, uint32_t value);

void EmitStoreU32(EmitterState* state, const IR::Operand& dst, uint32_t value);

uint32_t EmitSubgroupLocalInvocationId(EmitterState* state);

uint32_t EmitLaneIndexActiveBool(EmitterState* state, uint32_t lane);

[[noreturn]] void ExitDescriptorBindingFailure(const EmitterState&       state,
                                               IR::DescriptorBindingKind kind, uint32_t resource,
                                               const char* reason);

DescriptorResourceBinding ResourceForDescriptor(const EmitterState&       state,
                                                IR::DescriptorBindingKind kind, uint32_t resource);

uint32_t DescriptorElementPointer(EmitterState* state, uint32_t result_ptr_type,
                                  uint32_t variable_id, uint32_t array_index,
                                  IR::DescriptorBindingKind kind, uint32_t resource,
                                  const char* variable_name);

ImageViewKind SampledImageViewKind(const EmitterState& state, const IR::MemoryInfo& mem,
                                   uint32_t use_pc);

uint32_t ImageViewCoordinateComponents(ImageViewKind view);

uint32_t ImageViewImageType(const EmitterState& state, ImageViewKind view, bool integer);

uint32_t ImageViewSampledImageType(const EmitterState& state, ImageViewKind view, bool integer);

uint32_t ImageViewSizeType(const EmitterState& state, ImageViewKind view);

uint32_t LoadSampledImageDescriptor(EmitterState* state, const IR::MemoryInfo& mem, uint32_t use_pc,
                                    ImageViewKind view);

uint32_t LoadSamplerDescriptor(EmitterState* state, uint32_t sampler, uint32_t use_pc);

uint32_t MakeSampledImage(EmitterState* state, const IR::MemoryInfo& mem, uint32_t use_pc,
                          ImageViewKind view);

ImageViewKind StorageImageViewKind(const EmitterState& state, const IR::MemoryInfo& mem,
                                   bool uint_image, uint32_t use_pc);

uint32_t StorageImageDescriptorPointer(EmitterState* state, uint32_t resource, bool uint_image,
                                       uint32_t      use_pc = UINT32_MAX,
                                       ImageViewKind view   = ImageViewKind::Dim2D);

uint32_t LoadStorageImageDescriptor(EmitterState* state, uint32_t resource, bool uint_image,
                                    uint32_t use_pc, ImageViewKind view = ImageViewKind::Dim2D);

uint32_t ExecutionModelForStage(ShaderType stage);

uint32_t ConstantU32(EmitterState* state, uint32_t value);

uint32_t ConstantI32(EmitterState* state, int32_t value);

uint32_t ConstantF32(EmitterState* state, uint32_t bits);

uint32_t FloatBits(float value);

uint32_t ConstantF32Value(EmitterState* state, float value);

void AllocateInputVariables(EmitterState* state);

void AllocateOutputVariables(EmitterState* state);

uint32_t BuiltInForInput(IR::StageInputKind kind);

void AddInputAnnotationsAndNames(EmitterState* state);

void AddOutputAnnotationsAndNames(EmitterState* state);

void DecorateDescriptor(EmitterState* state, uint32_t variable, const char* name, uint32_t set,
                        uint32_t binding);

void AddDescriptorAnnotationsAndNames(EmitterState* state);

void EmitHeaderAndTypes(EmitterState* state);

void AllocateRegisterVariables(EmitterState* state);

void AllocateDescriptorVariables(EmitterState* state);

bool SdwaSelectorOffsetWidth(uint32_t sel, uint32_t& offset, uint32_t& width);

uint32_t EmitSdwaExtractU32(EmitterState* state, const IR::Operand& operand, uint32_t value);

uint32_t EmitTrueBool(EmitterState* state);

DppTargetLane EmitDppQuadPermTargetLane(EmitterState* state, uint32_t subid, uint32_t control);

DppTargetLane EmitDppRowShiftTargetLane(EmitterState* state, uint32_t subid, uint32_t amount,
                                        bool left);

DppTargetLane EmitDppRowRotateRightTargetLane(EmitterState* state, uint32_t subid, uint32_t amount);

DppTargetLane EmitDppMirrorTargetLane(EmitterState* state, uint32_t subid, bool half_row);

DppTargetLane EmitDppTargetLane(EmitterState* state, uint32_t control);

uint32_t EmitDppValueU32(EmitterState* state, const IR::Operand& operand, uint32_t value);

uint32_t EmitValueLoad(EmitterState* state, const IR::Operand& operand);

uint32_t EmitRegisterLoad(EmitterState* state, IR::Register reg);

uint32_t WaveMaskHighLoad(EmitterState* state, IR::RegisterFile file);

uint32_t EmitLaneMaskPartU32(EmitterState* state, uint32_t part);

uint32_t EmitThreadMaskBelowPartU32(EmitterState* state, uint32_t part);

uint32_t EmitMaskActiveBool(EmitterState* state, IR::RegisterFile file);

uint32_t EmitMaskZeroBool(EmitterState* state, IR::RegisterFile file, bool zero);

uint32_t EmitMaskSummaryZeroBool(EmitterState* state, IR::RegisterFile file, bool zero);

uint32_t EmitExecActiveBool(EmitterState* state);

uint32_t EmitConditionBool(EmitterState* state, const IR::Operand& operand);

uint32_t EmitSubgroupLocalInvocationId(EmitterState* state);

uint32_t InputVariableForKind(const EmitterState& state, IR::StageInputKind kind);

uint32_t InputVariableForParameter(const EmitterState& state, uint32_t location);

uint32_t EmitInputComponentU32(EmitterState* state, IR::StageInputKind kind, uint32_t component);

uint32_t EmitLocalInvocationIndex(EmitterState* state);

void EmitLoadInputF32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitSccBool(EmitterState* state, bool non_zero);

uint32_t EmitFloatLoad(EmitterState* state, const IR::Operand& operand);

uint32_t ApplyResultModifiersF32(EmitterState* state, uint32_t value, const IR::Operand& dst);

uint32_t EmitMixF32Load(EmitterState* state, const IR::Operand& operand);

IR::Operand OffsetRegisterOperand(const IR::Operand& operand, uint32_t offset);

uint32_t EmitLaneMaskOperandActiveBool(EmitterState* state, const IR::Operand& operand);

uint32_t EmitBallotLaneActiveBool(EmitterState* state, uint32_t ballot, uint32_t lane);

uint32_t EmitLaneIndexActiveBool(EmitterState* state, uint32_t lane);

uint32_t EmitSubgroupLaneActiveBool(EmitterState* state, uint32_t lane);

uint32_t EmitSequentialValueLoad(EmitterState* state, const IR::Operand& operand, uint32_t offset);

uint32_t EmitSequentialFloatLoad(EmitterState* state, const IR::Operand& operand, uint32_t offset);

uint32_t ImageAddressHalfComponent(const IR::Instruction& inst, uint32_t component);

IR::Operand ImageAddressOperand(const IR::Instruction& inst, const IR::Operand& base,
                                uint32_t component);

uint32_t EmitImageAddressValueLoad(EmitterState* state, const IR::Instruction& inst,
                                   const IR::Operand& base, uint32_t component);

uint32_t EmitImageAddressFloatLoad(EmitterState* state, const IR::Instruction& inst,
                                   const IR::Operand& base, uint32_t component);

uint32_t EmitZeroF32(EmitterState* state);

bool HasImageSampleFlag(const IR::Instruction& inst, uint32_t flag);

ImageSampleLayout MakeImageSampleLayout(const IR::Instruction& inst, ImageViewKind view);

uint32_t EmitImageCoordF32(EmitterState* state, const IR::Instruction& inst,
                           const ImageSampleLayout& layout, ImageViewKind view);

uint32_t EmitImageLodF32(EmitterState* state, const IR::Instruction& inst,
                         const ImageSampleLayout& layout);

uint32_t EmitImageDrefF32(EmitterState* state, const IR::Instruction& inst,
                          const ImageSampleLayout& layout);

uint32_t EmitImageBiasF32(EmitterState* state, const IR::Instruction& inst,
                          const ImageSampleLayout& layout);

uint32_t EmitImageGradientF32(EmitterState* state, const IR::Instruction& inst,
                              uint32_t first_component);

uint32_t EmitImagePackedOffset2I32(EmitterState* state, const IR::Instruction& inst,
                                   const ImageSampleLayout& layout);

uint32_t EmitImageOffsetCoordF32(EmitterState* state, const IR::Instruction& inst,
                                 const ImageSampleLayout& layout, uint32_t sampled_image,
                                 uint32_t coord, ImageViewKind view);

uint32_t EmitImageCoordU32(EmitterState* state, const IR::Instruction& inst,
                           ImageViewKind view = ImageViewKind::Dim2D);

uint32_t EmitImageLoadCoordU32(EmitterState* state, const IR::Instruction& inst,
                               ImageViewKind view);

uint32_t EmitImageMipLodU32(EmitterState* state, const IR::Instruction& inst,
                            const IR::Operand& address, ImageViewKind view);

uint32_t EmitImageQueryCoordF32(EmitterState* state, const IR::Instruction& inst,
                                ImageViewKind view);

uint32_t DmaskComponentIndex(uint32_t dmask, uint32_t component);

uint32_t EmitImageStoreComponentF32(EmitterState* state, const IR::Instruction& inst,
                                    uint32_t component);

uint32_t EmitImageStoreTexelF32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitImageStoreTexelU32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitDppWriteActiveBool(EmitterState* state, const IR::Operand& dst);

uint32_t EmitSdwaDestinationMerge(EmitterState* state, uint32_t pointer, const IR::Operand& dst,
                                  uint32_t value);

void EmitStoreU32(EmitterState* state, const IR::Operand& dst, uint32_t value);

uint32_t EmitAddU32(EmitterState* state, uint32_t lhs, uint32_t rhs);

uint32_t EmitBinaryU32(EmitterState* state, uint32_t opcode, uint32_t lhs, uint32_t rhs);

uint32_t EmitNotEqualZeroBool(EmitterState* state, uint32_t value);

uint32_t EmitSelectU32Value(EmitterState* state, uint32_t condition, uint32_t true_value,
                            uint32_t false_value);

uint32_t EmitByteAddress(EmitterState* state, const IR::Instruction& inst, uint32_t first_src,
                         uint32_t src_count);

uint32_t StorageBufferPackedStride(const EmitterState& state, const IR::MemoryInfo& mem,
                                   uint32_t use_pc);

uint32_t StorageBufferFormat(const EmitterState& state, const IR::MemoryInfo& mem, uint32_t use_pc);

bool ShouldApplyBufferAddTid(const IR::Instruction& inst);

uint32_t EmitBufferIndexWithAddTid(EmitterState* state, const IR::Instruction& inst, uint32_t index,
                                   uint32_t packed);

uint32_t EmitOptionalLogicalAndBool(EmitterState* state, uint32_t lhs, uint32_t rhs);

bool IsStorageBufferMemoryKind(IR::ResourceKind kind);

uint32_t EmitBufferAddressFromParts(EmitterState* state, const IR::Instruction& inst,
                                    uint32_t index, uint32_t offset, uint32_t soffset);

uint32_t EmitBufferByteAddress(EmitterState* state, const IR::Instruction& inst, uint32_t first_src,
                               uint32_t src_count);

uint32_t EmitMemoryByteAddress(EmitterState* state, const IR::Instruction& inst,
                               const IR::MemoryInfo& mem, uint32_t first_src, uint32_t src_count);

uint32_t EmitDwordIndex(EmitterState* state, const IR::Instruction& inst, uint32_t first_src,
                        uint32_t src_count);

uint32_t EmitMemoryDwordIndex(EmitterState* state, const IR::Instruction& inst,
                              const IR::MemoryInfo& mem, uint32_t first_src, uint32_t src_count);

DescriptorResourceBinding StorageBufferBindingForMemory(EmitterState*         state,
                                                        const IR::MemoryInfo& mem, uint32_t use_pc);

uint32_t EmitStorageBufferObjectPointer(EmitterState* state, const IR::MemoryInfo& mem,
                                        uint32_t use_pc);

uint32_t EmitStorageBufferElementInBounds(EmitterState* state, const IR::MemoryInfo& mem,
                                          uint32_t index, uint32_t use_pc);

uint32_t EmitStorageBufferElementPointer(EmitterState* state, const IR::MemoryInfo& mem,
                                         uint32_t index, uint32_t use_pc);

uint32_t EmitLdsElementPointer(EmitterState* state, uint32_t index);

uint32_t EmitGdsElementInBounds(EmitterState* state, uint32_t index);

uint32_t EmitGdsElementPointer(EmitterState* state, uint32_t index);

uint32_t EmitMemoryElementPointer(EmitterState* state, const IR::MemoryInfo& mem, uint32_t index,
                                  uint32_t use_pc);

uint32_t EmitMemoryLoadDwordValueU32(EmitterState* state, const IR::Instruction& inst,
                                     IR::ResourceKind kind, uint32_t first_src, uint32_t src_count);

void EmitMemoryLoadU32(EmitterState* state, const IR::Instruction& inst, IR::ResourceKind kind,
                       uint32_t first_src, uint32_t src_count);

uint32_t EmitMemoryLoadSubDwordValueU32(EmitterState* state, const IR::Instruction& inst,
                                        IR::ResourceKind kind, uint32_t first_src,
                                        uint32_t src_count, uint32_t data_bits, bool sign_extend);

void EmitMemoryLoadSubDwordU32(EmitterState* state, const IR::Instruction& inst,
                               IR::ResourceKind kind, uint32_t first_src, uint32_t src_count,
                               uint32_t data_bits, bool sign_extend);

void EmitMemoryStoreU32(EmitterState* state, const IR::Instruction& inst, IR::ResourceKind kind,
                        uint32_t first_src, uint32_t src_count);

void EmitMemoryStoreSubDwordU32(EmitterState* state, const IR::Instruction& inst,
                                IR::ResourceKind kind, uint32_t first_src, uint32_t src_count,
                                uint32_t data_bits);

uint32_t AddressSourceCount(const IR::Instruction& inst, uint32_t first_src);

bool IsFormattedBufferComponent(const IR::Instruction& inst);

Prospero::BufferFormat FormattedBufferFormat(const EmitterState&    state,
                                             const IR::Instruction& inst);

IR::Instruction WithFormatComponentByteOffset(const IR::Instruction& inst,
                                              Prospero::BufferFormat format);

uint32_t EmitTBufferBitcastF32ToU32(EmitterState* state, uint32_t value);

uint32_t EmitTBufferBitcastU32ToF32(EmitterState* state, uint32_t value);

uint32_t EmitTBufferBitcastU32ToI32(EmitterState* state, uint32_t value);

uint32_t EmitTBufferCompareU32Constant(EmitterState* state, uint32_t opcode, uint32_t value,
                                       uint32_t constant);

uint32_t EmitTBufferSelectF32(EmitterState* state, uint32_t condition, uint32_t true_value,
                              uint32_t false_value);

bool IsSignedFormatComponent(Format::ComponentType type);

uint32_t EmitExtractFormatFieldU32(EmitterState* state, uint32_t raw_word, uint32_t offset,
                                   uint32_t bits, bool sign_extend);

uint32_t EmitHalfToF32Bits(EmitterState* state, uint32_t raw);

uint32_t EmitUFloatToF32Bits(EmitterState* state, uint32_t raw, uint32_t bits);

uint32_t EmitFormatRawComponent(EmitterState* state, const IR::Instruction& inst,
                                const Format::BufferFormatInfo& info);

uint32_t NormalizeFormatComponent(EmitterState* state, const Format::BufferFormatInfo& info,
                                  uint32_t component, uint32_t raw);

uint32_t UnpackTBufferFormat(EmitterState* state, const IR::Instruction& inst,
                             const Format::BufferFormatInfo& info);

bool EmitTypedTBufferLoad(EmitterState* state, const IR::Instruction& inst,
                          const Format::BufferFormatInfo& info);

bool EmitFormattedBufferLoad(EmitterState* state, const IR::Instruction& inst);

uint32_t FormattedBufferDwordStoreComponentCount(Prospero::BufferFormat format,
                                                 uint32_t               opcode_components);

bool EmitBufferIntegerFormatStore(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitAtomicPointer(EmitterState* state, const IR::Instruction& inst);

void EmitDeviceAtomicMemoryBarrier(EmitterState* state);

void EmitAtomicU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitSLoadDword(EmitterState* state, const IR::Instruction& inst);

void EmitLoadSrtDword(EmitterState* state, const IR::Instruction& inst);

void EmitBufferLoadUbyte(EmitterState* state, const IR::Instruction& inst);

void EmitBufferLoadSbyte(EmitterState* state, const IR::Instruction& inst);

void EmitBufferLoadUshort(EmitterState* state, const IR::Instruction& inst);

void EmitBufferLoadSshort(EmitterState* state, const IR::Instruction& inst);

void EmitBufferLoadDword(EmitterState* state, const IR::Instruction& inst);

void EmitBufferStoreDword(EmitterState* state, const IR::Instruction& inst);

void EmitFlatLoadUbyte(EmitterState* state, const IR::Instruction& inst);

void EmitFlatLoadSbyte(EmitterState* state, const IR::Instruction& inst);

void EmitFlatLoadUshort(EmitterState* state, const IR::Instruction& inst);

void EmitFlatLoadSshort(EmitterState* state, const IR::Instruction& inst);

void EmitFlatLoadDword(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitDsAddtidDwordIndex(EmitterState* state, const IR::Instruction& inst,
                                uint32_t m0_src_index);

void EmitDsWriteAddtidB32(EmitterState* state, const IR::Instruction& inst);

void EmitDsReadAddtidB32(EmitterState* state, const IR::Instruction& inst);

void EmitDsAppendConsume(EmitterState* state, const IR::Instruction& inst, uint32_t atomic_opcode);

void EmitDsFloatMinMaxF32(EmitterState* state, const IR::Instruction& inst, bool max_value);

uint32_t EmitDsSwizzleTargetLane(EmitterState* state, uint32_t subid, uint32_t control);

void EmitDsSwizzleB32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitImageGetResinfoComponent(EmitterState* state, uint32_t image, uint32_t size,
                                      uint32_t component_index);

void EmitImageGetResinfo(EmitterState* state, const IR::Instruction& inst);

void EmitImageGetLod(EmitterState* state, const IR::Instruction& inst);

void EmitImageLoad(EmitterState* state, const IR::Instruction& inst);

void EmitImageStore(EmitterState* state, const IR::Instruction& inst);

void EmitImageSampleResult(EmitterState* state, const IR::Instruction& inst, uint32_t sample,
                           bool dref);

bool ImageSampleNeedsExplicitLod(const EmitterState& state, const IR::Instruction& inst);

void AddImageSampleOperands(EmitterState* state, const IR::Instruction& inst,
                            const ImageSampleLayout& layout, bool explicit_lod,
                            std::vector<uint32_t>* words);

uint32_t ImageSampleOpcode(const EmitterState& state, const IR::Instruction& inst);

void EmitImageSample(EmitterState* state, const IR::Instruction& inst);

uint32_t ImageGatherComponent(uint32_t dmask);

void EmitImageGather4(EmitterState* state, const IR::Instruction& inst);

void EmitMoveU32(EmitterState* state, const IR::Instruction& inst);

void EmitMoveF32Bits(EmitterState* state, const IR::Instruction& inst);

uint32_t MoveRelRegisterLimit(const EmitterState& state, IR::Register base);

void EmitMoveRelSourceU32(EmitterState* state, const IR::Instruction& inst);

void EmitMoveRelDestU32(EmitterState* state, const IR::Instruction& inst);

void EmitMoveU64(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitWqmLaneU32(EmitterState* state, uint32_t src);

void EmitWqmB64(EmitterState* state, const IR::Instruction& inst);

void EmitSaveexecB32(EmitterState* state, const IR::Instruction& inst);

void EmitSaveexecB64(EmitterState* state, const IR::Instruction& inst);

void EmitReadFirstLaneU32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitLaneIndex(EmitterState* state, const IR::Operand& operand);

void EmitReadLaneU32(EmitterState* state, const IR::Instruction& inst);

void EmitWriteLaneU32(EmitterState* state, const IR::Instruction& inst);

void EmitPermlaneB32(EmitterState* state, const IR::Instruction& inst, bool x16);

void EmitControlNop(EmitterState* state, const IR::Instruction& inst);

void EmitWaitcnt(EmitterState* state, const IR::Instruction& inst);

void EmitBarrier(EmitterState* state, const IR::Instruction& inst);

void EmitAbsI32(EmitterState* state, const IR::Instruction& inst);

void EmitUnaryU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitUnaryU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitFindLsbU32(EmitterState* state, const IR::Instruction& inst);

void EmitFindMsbFromHighU32(EmitterState* state, const IR::Instruction& inst);

void EmitFindMsbFromHighU64(EmitterState* state, const IR::Instruction& inst);

void EmitBitCountU64(EmitterState* state, const IR::Instruction& inst);

void EmitBitCountU32(EmitterState* state, const IR::Instruction& inst);

void EmitBitCountAddU32(EmitterState* state, const IR::Instruction& inst);

void EmitBinaryU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitBinaryNotRhsU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitBinaryThenNotU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitMaskedBitCountU32(EmitterState* state, const IR::Instruction& inst, uint32_t exec_index);

uint32_t EmitMask5U32(EmitterState* state, uint32_t value);

void EmitChainedBinaryU32(EmitterState* state, const IR::Instruction& inst, uint32_t first_op,
                          uint32_t second_op, bool mask_first_rhs = false,
                          bool mask_second_rhs = false);

void EmitBinaryU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitBinaryNotRhsU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitBinaryThenNotU64(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitSelectU64(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitSelectValueU32(EmitterState* state, uint32_t cond, uint32_t true_value,
                            uint32_t false_value);

void EmitStoreSccBool(EmitterState* state, uint32_t cond);

uint32_t EmitAndConstant(EmitterState* state, uint32_t value, uint32_t mask);

void EmitShiftLeftLogicalU64Values(EmitterState* state, uint32_t low, uint32_t high, uint32_t shift,
                                   uint32_t* out_low, uint32_t* out_high);

void EmitShiftLeftLogicalU64(EmitterState* state, const IR::Instruction& inst);

void EmitShiftRightLogicalU64Values(EmitterState* state, uint32_t low, uint32_t high,
                                    uint32_t shift, uint32_t* out_low, uint32_t* out_high);

void EmitShiftRightLogicalU64(EmitterState* state, const IR::Instruction& inst);

void EmitAdd3U32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitAndConstant(EmitterState* state, uint32_t value, uint32_t mask);

void EmitShiftU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

uint32_t EmitU16LaneBits(EmitterState* state, const IR::Operand& operand, bool high_lane,
                         bool sign_extend = false);

void EmitShiftU16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode,
                  bool arithmetic);

void EmitBinaryU16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitMinMaxU16(EmitterState* state, const IR::Instruction& inst, bool signed_value,
                   bool max_value);

uint32_t EmitXorConstant(EmitterState* state, uint32_t value, uint32_t mask);

AddCarryResult EmitAddCarryValues(EmitterState* state, uint32_t lhs, uint32_t rhs,
                                  uint32_t carry_in);

void EmitLaneMaskPairFromBool(EmitterState* state, const IR::Operand& dst, uint32_t value_bool);

void EmitPerInvocationMask(EmitterState* state, const IR::Operand& dst, uint32_t value_bool);

void EmitIAddCarryU32(EmitterState* state, const IR::Instruction& inst);

void EmitISubBorrowU32(EmitterState* state, const IR::Instruction& inst);

void EmitScalarAddCarryU32(EmitterState* state, const IR::Instruction& inst);

void EmitScalarSubBorrowU32(EmitterState* state, const IR::Instruction& inst);

void EmitScalarSubBorrowCarryU32(EmitterState* state, const IR::Instruction& inst);

void EmitBitClearU32(EmitterState* state, const IR::Instruction& inst);

void EmitBitSetU32(EmitterState* state, const IR::Instruction& inst);

void EmitUMadU64U32(EmitterState* state, const IR::Instruction& inst);

void EmitScalarSignedOverflowI32(EmitterState* state, const IR::Instruction& inst, bool subtract);

void EmitScalarShiftLeftAddCarryU32(EmitterState* state, const IR::Instruction& inst);

void EmitMulU24U32(EmitterState* state, const IR::Instruction& inst);

void EmitMulI24U32(EmitterState* state, const IR::Instruction& inst);

void EmitMulHighU32(EmitterState* state, const IR::Instruction& inst);

void EmitMulHighI32(EmitterState* state, const IR::Instruction& inst);

void EmitMadU32U24(EmitterState* state, const IR::Instruction& inst);

void EmitMadI32I24(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitShiftLeftConstant(EmitterState* state, uint32_t value, uint32_t shift);

uint32_t EmitShiftRightConstant(EmitterState* state, uint32_t value, uint32_t shift);

uint32_t EmitOrU32(EmitterState* state, uint32_t lhs, uint32_t rhs);

uint32_t EmitBitReplicateHalfU32(EmitterState* state, uint32_t value);

void EmitBitReplicateB64B32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitBitFieldExtractConstant(EmitterState* state, uint32_t value, uint32_t offset,
                                     uint32_t count);

uint32_t EmitRightAlignedMaskU32(EmitterState* state, uint32_t count);

void EmitBitFieldMaskU32(EmitterState* state, const IR::Instruction& inst);

void EmitRightAlignedMaskU64(EmitterState* state, uint32_t count, uint32_t* out_low,
                             uint32_t* out_high);

void EmitBitFieldMaskU64(EmitterState* state, const IR::Instruction& inst);

void EmitBitFieldExtractU64(EmitterState* state, const IR::Instruction& inst);

void EmitBitFieldExtractU32(EmitterState* state, const IR::Instruction& inst);

void EmitBitFieldExtract3U32(EmitterState* state, const IR::Instruction& inst, bool signed_value);

void EmitBitFieldInsertSelectU32(EmitterState* state, const IR::Instruction& inst);

void EmitAlignBitU32(EmitterState* state, const IR::Instruction& inst);

void EmitSelectU32(EmitterState* state, const IR::Instruction& inst);

void EmitSelectMaskU32(EmitterState* state, const IR::Instruction& inst);

void EmitSelectF32Bits(EmitterState* state, const IR::Instruction& inst);

void EmitSelectMaskF32Bits(EmitterState* state, const IR::Instruction& inst);

void EmitPackU16(EmitterState* state, const IR::Instruction& inst, bool high_src0, bool high_src1);

void EmitPackU8F32(EmitterState* state, const IR::Instruction& inst);

void EmitBinaryF32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitLdexpF32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitCompareU32Constant(EmitterState* state, uint32_t opcode, uint32_t value,
                                uint32_t constant);

uint32_t EmitSubConstantMinusU32(EmitterState* state, uint32_t constant, uint32_t value);

uint32_t EmitF32ToF16RtzBits(EmitterState* state, uint32_t f32);

void EmitPackF32ToF16Rtz(EmitterState* state, const IR::Instruction& inst);

void EmitPackNormalizedF32(EmitterState* state, const IR::Instruction& inst, uint32_t ext_inst);

uint32_t EmitMinMaxU32Value(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value);

void EmitMinMaxU32(EmitterState* state, const IR::Instruction& inst, bool max_value);

void EmitSadU32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitMinMaxI32Value(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value);

void EmitMinMaxI32(EmitterState* state, const IR::Instruction& inst, bool max_value);

void EmitMinMax3U32(EmitterState* state, const IR::Instruction& inst, bool signed_value,
                    bool max_value);

void EmitMed3U32(EmitterState* state, const IR::Instruction& inst, bool signed_value);

uint32_t EmitBitcastF32ToU32(EmitterState* state, uint32_t value);

uint32_t EmitBitcastU32ToF32(EmitterState* state, uint32_t value);

uint32_t EmitAndU32(EmitterState* state, uint32_t lhs, uint32_t rhs);

uint32_t EmitLogicalAndBool(EmitterState* state, uint32_t lhs, uint32_t rhs);

uint32_t EmitLogicalOrBool(EmitterState* state, uint32_t lhs, uint32_t rhs);

uint32_t EmitLogicalNotBool(EmitterState* state, uint32_t value);

F32Class EmitClassifyF32(EmitterState* state, uint32_t value);

uint32_t EmitClassMaskBitMatch(EmitterState* state, uint32_t mask, uint32_t bit,
                               uint32_t class_match);

uint32_t EmitClassMaskF32(EmitterState* state, uint32_t value, uint32_t mask);

uint32_t EmitMinMaxF32Value(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value);

void EmitMinMaxF32(EmitterState* state, const IR::Instruction& inst, bool max_value);

void EmitCompareResult(EmitterState* state, const IR::Operand& dst, uint32_t cond);

void EmitCompareU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitCompareU16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitCompareI16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitBitCompareB32(EmitterState* state, const IR::Instruction& inst, bool bit_set);

void EmitCompareNeU64(EmitterState* state, const IR::Instruction& inst);

void EmitCompareConstant(EmitterState* state, const IR::Instruction& inst, bool value);

void EmitCompareMaskU32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitCompareF32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitCompareOrderedF32(EmitterState* state, const IR::Instruction& inst, bool ordered);

void EmitCompareClassF32(EmitterState* state, const IR::Instruction& inst);

void EmitCompareMaskF32(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitConvertU32ToF32(EmitterState* state, const IR::Instruction& inst);

void EmitConvertI32ToF32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitTruncF32Value(EmitterState* state, uint32_t value);

void EmitConvertF32ToU32(EmitterState* state, const IR::Instruction& inst);

void EmitConvertF32ToI32(EmitterState* state, const IR::Instruction& inst);

void EmitConvertF32ToF16(EmitterState* state, const IR::Instruction& inst);

void EmitConvertF16ToF32(EmitterState* state, const IR::Instruction& inst);

void EmitConvertU16ToF16(EmitterState* state, const IR::Instruction& inst);

void EmitConvertF16ToU16(EmitterState* state, const IR::Instruction& inst);

void EmitConvertI16ToF16(EmitterState* state, const IR::Instruction& inst);

void EmitConvertF16ToI16(EmitterState* state, const IR::Instruction& inst);

void EmitConvertRoundPlusInfF32ToI32(EmitterState* state, const IR::Instruction& inst);

void EmitConvertFloorF32ToI32(EmitterState* state, const IR::Instruction& inst);

void EmitConvertI4ToOffsetF32(EmitterState* state, const IR::Instruction& inst);

void EmitConvertByteU32ToF32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitFlushF32DenormToSignedZero(EmitterState* state, uint32_t value);

void EmitRcpF32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitTrigCycleF32(EmitterState* state, uint32_t src, bool preserve_signed_zero);

void EmitFloatExtInst(EmitterState* state, const IR::Instruction& inst, uint32_t ext_inst,
                      bool scale_by_two_pi = false, bool flush_denorm = false);

void EmitMadF32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitFNegateValue(EmitterState* state, uint32_t value);

uint32_t EmitFAbsValue(EmitterState* state, uint32_t value);

uint32_t EmitFCompareValue(EmitterState* state, uint32_t opcode, uint32_t lhs, uint32_t rhs);

uint32_t EmitLogicalAndValue(EmitterState* state, uint32_t lhs, uint32_t rhs);

uint32_t EmitFSelectValue(EmitterState* state, uint32_t cond, uint32_t true_value,
                          uint32_t false_value);

uint32_t EmitFMulConstant(EmitterState* state, uint32_t value, uint32_t bits);

CubeF32Values EmitCubeF32Values(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitCubeIdF32(EmitterState* state, const CubeF32Values& cube);

uint32_t EmitCubeScF32(EmitterState* state, const CubeF32Values& cube);

uint32_t EmitCubeTcF32(EmitterState* state, const CubeF32Values& cube);

uint32_t EmitCubeMaF32(EmitterState* state, const CubeF32Values& cube);

void EmitCubeF32(EmitterState* state, const IR::Instruction& inst);

void EmitDot2AccF32F16(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitPackedF16Lane(EmitterState* state, const IR::Operand& operand, bool high_lane);

void EmitCompareF16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

uint32_t EmitPackedF16LaneBits(EmitterState* state, const IR::Operand& operand, bool high_lane);

uint32_t EmitF16BitsToF32(EmitterState* state, uint32_t bits);

uint32_t EmitPackLowF32ToF16Bits(EmitterState* state, uint32_t value);

void EmitPackB32F16(EmitterState* state, const IR::Instruction& inst);

F16Class EmitClassifyF16Bits(EmitterState* state, uint32_t value);

uint32_t EmitNegativeNumericF16Bool(EmitterState* state, const F16Class& cls);

uint32_t EmitRcpF16Bits(EmitterState* state, uint32_t bits);

uint32_t EmitSqrtF16Bits(EmitterState* state, uint32_t bits);

uint32_t EmitRsqF16Bits(EmitterState* state, uint32_t bits);

uint32_t EmitLog2F16Bits(EmitterState* state, uint32_t bits);

uint32_t EmitExp2F16Bits(EmitterState* state, uint32_t bits);

void EmitSpecialF16(EmitterState* state, const IR::Instruction& inst);

void EmitBinaryF16(EmitterState* state, const IR::Instruction& inst, uint32_t opcode);

void EmitFmaF16(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitMinMaxF16Bits(EmitterState* state, uint32_t lhs, uint32_t rhs, bool max_value);

void EmitMinMaxF16(EmitterState* state, const IR::Instruction& inst, bool max_value);

uint32_t EmitMinMax3F16Bits(EmitterState* state, uint32_t src0, uint32_t src1, uint32_t src2,
                            bool max_value);

void EmitMinMax3F16(EmitterState* state, const IR::Instruction& inst, bool max_value);

uint32_t EmitF16BitsEqualBool(EmitterState* state, uint32_t lhs, uint32_t rhs);

uint32_t EmitAnyNanF16Bool(EmitterState* state, uint32_t src0, uint32_t src1, uint32_t src2);

void EmitMed3F16(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitPackedMinMaxF16(EmitterState* state, const IR::Instruction& inst, bool max_value);

uint32_t EmitPackedF16BinaryLane(EmitterState* state, const IR::Instruction& inst, bool high_lane,
                                 uint32_t opcode);

uint32_t EmitPackedF16FmaLane(EmitterState* state, const IR::Instruction& inst, bool high_lane);

void EmitPackedF16(EmitterState* state, const IR::Instruction& inst);

void EmitPackedInteger16(EmitterState* state, const IR::Instruction& inst);

void EmitMadMixF16(EmitterState* state, const IR::Instruction& inst);

void EmitMinMax3F32(EmitterState* state, const IR::Instruction& inst, bool max_value);

void EmitMed3F32(EmitterState* state, const IR::Instruction& inst);

uint32_t EmitExportComponentF32(EmitterState* state, const IR::Instruction& inst,
                                uint32_t component);

uint32_t EmitExportVec4F32(EmitterState* state, const IR::Instruction& inst);

bool ExportWritesData(const IR::Instruction& inst);

void EmitExport(EmitterState* state, const IR::Instruction& inst);

bool ExportUsesPixelValidMask(const EmitterState& state, const IR::Instruction& inst);

void EmitKillIfBoolFalse(EmitterState* state, uint32_t active);

void EmitUpdatePixelValidMask(EmitterState* state);

void EmitKillIfPixelValidMaskInactive(EmitterState* state);

void ComputeReachableBlocks(EmitterState* state, const IR::Program& program);

void AllocateBlockLabels(EmitterState* state, const IR::Program& program);

void AllocateDispatcherState(EmitterState* state, const IR::Program& program);

uint32_t BlockLabel(const EmitterState& state, uint32_t block_id);

uint32_t EmitBranchCondition(EmitterState* state, CFG::BranchCondition condition);

void EmitStructuredPrefix(EmitterState* state, const CFG::Terminator& term);

void EmitReturn(EmitterState* state);

void EmitTerminator(EmitterState* state, const CFG::Terminator& term);

uint32_t InitialRegisterValue(const EmitterState& state, IR::Register reg);

void EmitRegisterVariables(EmitterState* state);

void EmitComputeInputRegisters(EmitterState* state);

void EmitPixelInputRegisters(EmitterState* state);

void EmitInstruction(EmitterState* state, const IR::Instruction& inst);

uint32_t DispatcherTargetValue(EmitterState* state, uint32_t block_id);

void EmitDispatcherStoreTarget(EmitterState* state, uint32_t block_id);

void EmitDispatcherExit(EmitterState* state);

void EmitDispatcherTerminator(EmitterState* state, const CFG::Terminator& term);

void EmitDispatcherSwitch(EmitterState* state, const IR::Program& program);

void EmitDispatcherBlocks(EmitterState* state, const IR::Program& program);

void EmitDispatcherLoopTail(EmitterState* state);

void EmitDispatcherFunction(EmitterState* state, const IR::Program& program);

void EmitFunction(EmitterState* state, const IR::Program& program);

// These templates accept local lambdas from several emitter translation units.
template <typename Fn>
void EmitIfCondition(EmitterState* state, uint32_t condition, Fn&& fn) {
	if (condition == 0) {
		fn();
		return;
	}

	const auto then_label  = state->builder.AllocateId();
	const auto merge_label = state->builder.AllocateId();
	state->builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state->builder.AddFunction({OpBranchConditional, condition, then_label, merge_label});
	state->builder.AddFunction({OpLabel, then_label});
	fn();
	state->builder.AddFunction({OpBranch, merge_label});
	state->builder.AddFunction({OpLabel, merge_label});
}

template <typename Fn>
uint32_t EmitValueOrZeroIfCondition(EmitterState* state, uint32_t condition, Fn&& fn) {
	if (condition == 0) {
		return fn();
	}

	const auto then_label  = state->builder.AllocateId();
	const auto else_label  = state->builder.AllocateId();
	const auto merge_label = state->builder.AllocateId();
	state->builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state->builder.AddFunction({OpBranchConditional, condition, then_label, else_label});
	state->builder.AddFunction({OpLabel, then_label});
	const auto then_value = fn();
	state->builder.AddFunction({OpBranch, merge_label});
	state->builder.AddFunction({OpLabel, else_label});
	state->builder.AddFunction({OpBranch, merge_label});
	state->builder.AddFunction({OpLabel, merge_label});
	const auto value = state->builder.AllocateId();
	state->builder.AddFunction({OpPhi, state->uint_type, value, then_value, then_label,
	                            ConstantU32(state, 0), else_label});
	return value;
}

template <typename Fn>
void EmitGuardedByExec(EmitterState* state, Fn&& fn) {
	const auto then_label  = state->builder.AllocateId();
	const auto merge_label = state->builder.AllocateId();
	const auto condition   = EmitExecActiveBool(state);
	state->builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state->builder.AddFunction({OpBranchConditional, condition, then_label, merge_label});
	state->builder.AddFunction({OpLabel, then_label});
	fn();
	state->builder.AddFunction({OpBranch, merge_label});
	state->builder.AddFunction({OpLabel, merge_label});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_INTERNAL_H_ */

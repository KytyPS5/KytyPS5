#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/shader.h"

#include <array>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

enum class Opcode {
	ControlNop,
	Waitcnt,
	Barrier,
	Sendmsg,
	TtraceData,
	InstPrefetch,
	MoveU32,
	MoveF32Bits,
	MoveRelDestU32,
	MoveRelSourceU32,
	MoveU64,
	WqmB64,
	SaveexecB32,
	SaveexecB64,
	ReadFirstLaneU32,
	ReadLaneU32,
	WriteLaneU32,
	Permlane16B32,
	Permlanex16B32,
	AbsI32,
	IAddU32,
	IAddCarryU32,
	ISubBorrowU32,
	ScalarAddCarryU32,
	ScalarSubBorrowU32,
	ScalarSubBorrowCarryU32,
	ScalarSignedAddOverflowI32,
	ScalarSignedSubOverflowI32,
	ScalarShiftLeftAddCarryU32,
	ISubU32,
	IMulU32,
	UMulHighU32,
	SMulHighI32,
	IMadI24U32,
	UMadU24U32,
	UMadU64U32,
	SadU32,
	IAdd3U32,
	IMulI24U32,
	UMulU24U32,
	IMinI32,
	IMaxI32,
	IMin3I32,
	IMax3I32,
	IMed3I32,
	UMinU32,
	UMaxU32,
	UMin3U32,
	UMax3U32,
	UMed3U32,
	BitwiseAndU32,
	BitwiseAndU64,
	BitwiseAndNotU32,
	BitwiseAndNotU64,
	BitwiseOrU32,
	BitwiseOrU64,
	BitwiseOrNotU32,
	BitwiseOrNotU64,
	BitwiseAndOrU32,
	BitwiseOr3U32,
	BitwiseXorU32,
	BitwiseXorU64,
	BitwiseXor3U32,
	BitwiseNandU32,
	BitwiseNandU64,
	BitwiseNorU32,
	BitwiseNorU64,
	BitwiseXnorU32,
	BitwiseXnorU64,
	BitwiseNotU32,
	BitwiseNotU64,
	BitClearU32,
	BitSetU32,
	BitReverseU32,
	BitCountU32,
	BitCountU64,
	BitReplicateB64B32,
	BitCountAddU32,
	MaskedBitCountLowU32,
	MaskedBitCountHighU32,
	FindLsbU32,
	FindMsbFromHighU32,
	FindMsbFromHighU64,
	BitFieldMaskU32,
	BitFieldMaskU64,
	BitFieldExtractU32,
	BitFieldExtractU64,
	BitFieldExtract3U32,
	BitFieldExtract3I32,
	BitFieldInsertSelectU32,
	BitCompare0B32,
	BitCompare1B32,
	AlignBitU32,
	ShiftLeftAddU32,
	AddShiftLeftU32,
	XorAddU32,
	ShiftLeftOrU32,
	ShiftLeftLogicalU32,
	ShiftLeftLogicalU64,
	ShiftLeftLogicalU16,
	ShiftRightLogicalU32,
	ShiftRightLogicalU64,
	ShiftRightLogicalU16,
	ShiftRightArithmeticI32,
	ShiftRightArithmeticI16,
	SelectU32,
	SelectMaskU32,
	SelectF32Bits,
	SelectMaskF32Bits,
	SelectU64,
	PackLowLowU16,
	PackLowHighU16,
	PackHighHighU16,
	CompareFalse,
	CompareTrue,
	CompareEqU32,
	CompareNeU32,
	CompareGtU32,
	CompareGeU32,
	CompareLtU32,
	CompareLeU32,
	CompareNeU64,
	CompareMaskEqU32,
	CompareMaskNeU32,
	CompareMaskGtU32,
	CompareMaskGeU32,
	CompareMaskLtU32,
	CompareMaskLeU32,
	CompareEqI32,
	CompareNeI32,
	CompareGtI32,
	CompareGeI32,
	CompareLtI32,
	CompareLeI32,
	CompareEqI16,
	CompareNeI16,
	CompareGtI16,
	CompareGeI16,
	CompareLtI16,
	CompareLeI16,
	CompareMaskEqI32,
	CompareMaskNeI32,
	CompareMaskGtI32,
	CompareMaskGeI32,
	CompareMaskLtI32,
	CompareMaskLeI32,
	CompareEqU16,
	CompareNeU16,
	CompareGtU16,
	CompareGeU16,
	CompareLtU16,
	CompareLeU16,
	CompareEqF32,
	CompareNeF32,
	CompareGtF32,
	CompareGeF32,
	CompareLtF32,
	CompareLeF32,
	CompareOrderedF32,
	CompareUnorderedF32,
	CompareUnordEqF32,
	CompareUnordNeF32,
	CompareUnordGtF32,
	CompareUnordGeF32,
	CompareUnordLtF32,
	CompareUnordLeF32,
	CompareClassF32,
	CompareEqF16,
	CompareNeF16,
	CompareGtF16,
	CompareGeF16,
	CompareLtF16,
	CompareLeF16,
	CompareUnordNeF16,
	CompareMaskEqF16,
	CompareMaskNeF16,
	CompareMaskGtF16,
	CompareMaskGeF16,
	CompareMaskLtF16,
	CompareMaskLeF16,
	CompareMaskUnordNeF16,
	CompareMaskUnordGeF16,
	CompareMaskEqF32,
	CompareMaskNeF32,
	CompareMaskGtF32,
	CompareMaskGeF32,
	CompareMaskLtF32,
	CompareMaskLeF32,
	CompareMaskUnordEqF32,
	CompareMaskUnordNeF32,
	CompareMaskUnordGtF32,
	CompareMaskUnordGeF32,
	CompareMaskUnordLtF32,
	CompareMaskUnordLeF32,
	ConvertByteU32ToF32,
	ConvertU32ToF32,
	ConvertI32ToF32,
	ConvertF32ToU32,
	ConvertF32ToI32,
	ConvertF32ToF16,
	ConvertF16ToF32,
	ConvertU16ToF16,
	ConvertF16ToU16,
	ConvertI16ToF16,
	ConvertF16ToI16,
	ConvertRoundPlusInfF32ToI32,
	ConvertFloorF32ToI32,
	ConvertI4ToOffsetF32,
	LdexpF32,
	PackF32ToF16Rtz,
	PackSnorm2x16F32,
	PackUnorm2x16F32,
	PackU16U32,
	PackU8F32,
	PackB32F16,
	PackedMadI16,
	PackedMulLoU16,
	PackedAddI16,
	PackedSubI16,
	PackedLshlrevB16,
	PackedLshrrevB16,
	PackedAshrrevI16,
	PackedMaxI16,
	PackedMinI16,
	PackedMadU16,
	PackedAddU16,
	PackedSubU16,
	PackedMaxU16,
	PackedMinU16,
	PackedAddF16,
	PackedMulF16,
	PackedMinF16,
	PackedMaxF16,
	PackedFmaF16,
	AddF16,
	SubF16,
	MulF16,
	MinF16,
	MaxF16,
	FmaF16,
	MadMixF16,
	IAddU16,
	ISubI16,
	IMinI16,
	IMaxI16,
	UMinU16,
	UMaxU16,
	RcpF32,
	FractF32,
	TruncF32,
	CeilF32,
	RoundEvenF32,
	FloorF32,
	Exp2F32,
	Log2F32,
	InverseSqrtF32,
	SqrtF32,
	RcpF16,
	SqrtF16,
	InverseSqrtF16,
	Log2F16,
	Exp2F16,
	FloorF16,
	CeilF16,
	TruncF16,
	RoundEvenF16,
	SinF32,
	CosF32,
	CubeIdF32,
	CubeScF32,
	CubeTcF32,
	CubeMaF32,
	FAddF32,
	FSubF32,
	FMulF32,
	FMinF32,
	FMaxF32,
	FMadF32,
	Dot2AccF32F16,
	FMin3F32,
	FMax3F32,
	FMed3F32,
	Min3F16,
	Max3F16,
	Med3F16,
	LoadSrtDword,
	SLoadDword,
	SBufferLoadDword,
	BufferLoadUbyte,
	BufferLoadSbyte,
	BufferLoadUshort,
	BufferLoadSshort,
	BufferLoadDword,
	BufferStoreByte,
	BufferStoreShort,
	BufferStoreDword,
	AtomicSwapU32,
	AtomicAddU32,
	AtomicSubU32,
	AtomicSMinI32,
	AtomicUMinU32,
	AtomicSMaxI32,
	AtomicUMaxU32,
	AtomicAndU32,
	AtomicOrU32,
	AtomicXorU32,
	FlatLoadUbyte,
	FlatLoadSbyte,
	FlatLoadUshort,
	FlatLoadSshort,
	FlatLoadDword,
	FlatStoreByte,
	FlatStoreShort,
	FlatStoreDword,
	DsReadUbyte,
	DsReadSbyte,
	DsReadUshort,
	DsReadSshort,
	DsReadB32,
	DsWriteByte,
	DsWriteShort,
	DsWriteB32,
	DsMinF32,
	DsMaxF32,
	DsSwizzleB32,
	DsConsume,
	DsAppend,
	DsWriteAddtidB32,
	DsReadAddtidB32,
	ImageGetResinfo,
	ImageGetLod,
	ImageLoad,
	ImageStore,
	ImageSample,
	ImageGather4,
	LoadInputF32,
	Export,
};

enum class RegisterFile { Scalar, Vector, Vcc, Exec, Scc, M0 };

struct Register {
	RegisterFile file  = RegisterFile::Scalar;
	uint32_t     index = 0;

	bool operator==(const Register& other) const {
		return file == other.file && index == other.index;
	}
};

enum class OperandKind { Register, ImmediateU32, PcRelativeU32, Null };

struct Operand {
	OperandKind kind = OperandKind::Null;
	Register    reg;
	uint32_t    imm                = 0;
	bool        sext_64            = false;
	uint32_t    sdwa_sel           = 6;
	uint32_t    omod               = 0;
	bool        sdwa_sext          = false;
	bool        op_sel             = false;
	bool        op_sel_hi          = false;
	bool        negate             = false;
	bool        negate_hi          = false;
	bool        absolute           = false;
	bool        clamp              = false;
	uint32_t    dpp_ctrl           = 0;
	uint32_t    dpp_row_mask       = 0xf;
	uint32_t    dpp_bank_mask      = 0xf;
	bool        dpp_fetch_inactive = false;
	bool        dpp_bound_ctrl     = false;
	bool        dpp                = false;

	bool operator==(const Operand& other) const = default;
};

enum class ResourceKind {
	None,
	ScalarBuffer,
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
	// Dense resource indices are assigned by resource tracking. These source IDs refer to the
	// exact scalar definitions reaching this instruction before that patching step.
	uint32_t resource_source = 0;
	uint32_t sampler_source  = 0;
	bool     data_signed     = false;
	bool     typed           = false;
	bool     formatted       = false;
	bool     image_has_mip   = false;
	bool     glc             = false;
	bool     slc             = false;
	bool     idxen           = false;
	bool     offen           = false;

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

struct InputInfo {
	uint32_t attr = 0;
	uint32_t chan = 0;

	bool operator==(const InputInfo& other) const = default;
};

enum class SaveexecMode { And, Orn2, Andn1 };

struct Instruction {
	uint32_t     pc = 0;
	Opcode       op = Opcode::MoveU32;
	Operand      dst;
	Operand      dst2;
	Operand      src[4];
	uint32_t     src_count         = 0;
	uint32_t     scalar_sources[4] = {};
	uint32_t     scalar_value      = 0;
	MemoryInfo   memory;
	ExportInfo   export_info;
	InputInfo    input_info;
	SaveexecMode saveexec_mode = SaveexecMode::And;

	bool operator==(const Instruction& other) const = default;
};

struct BasicBlock {
	uint32_t                 id         = 0;
	uint32_t                 start_pc   = 0;
	uint32_t                 end_pc     = 0;
	uint32_t                 inst_begin = 0;
	uint32_t                 inst_end   = 0;
	std::vector<uint32_t>    predecessors;
	std::vector<uint32_t>    successors;
	CFG::Terminator          terminator;
	std::vector<Instruction> instructions;
};

enum class ScalarValueOp {
	Undefined,
	Unknown,
	UserData,
	Constant,
	PcRelativeLow,
	PcRelativeHigh,
	Add,
	AddCarry,
	Carry,
	Sub,
	SubBorrow,
	Borrow,
	Mul,
	And,
	AndNot,
	Or,
	OrNot,
	Xor,
	Not,
	ShiftLeft,
	ShiftRight,
	ShiftRightArithmetic,
	BitFieldMaskU32,
	BitFieldMaskU64Low,
	BitFieldMaskU64High,
	Add3,
	ShiftLeftAdd,
	ShiftLeftAddCarry,
	AddShiftLeft,
	XorAdd,
	ShiftLeftOr,
	ReadConst,
	ReadConstBuffer,
	Phi,
};

struct ScalarValue {
	ScalarValueOp           op   = ScalarValueOp::Undefined;
	uint32_t                pc   = 0;
	uint32_t                imm  = 0;
	std::array<uint32_t, 6> args = {};
	std::vector<uint32_t>   phi_args;

	bool operator==(const ScalarValue& other) const = default;
};

struct DescriptorValue {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	bool operator==(const DescriptorValue& other) const {
		return dword_count == other.dword_count && dwords == other.dwords;
	}
};

struct ScalarProvenance {
	static constexpr uint32_t Undefined = 0;
	static constexpr uint32_t Unknown   = 1;

	std::vector<ScalarValue>     values;
	std::vector<DescriptorValue> descriptors;

	bool operator==(const ScalarProvenance& other) const = default;
};

struct SrtRead {
	uint32_t value       = 0;
	uint32_t flat_offset = 0;
	uint32_t use_pc      = 0;

	bool operator==(const SrtRead& other) const = default;
};

struct SrtPlan {
	std::vector<SrtRead>  reads;
	std::vector<uint32_t> dynamic_reads;
	std::vector<uint32_t> dynamic_sources;

	bool operator==(const SrtPlan& other) const = default;
};

struct BufferResource {
	static constexpr uint32_t NoImageAlias = UINT32_MAX;

	uint32_t source            = 0;
	uint32_t first_use_pc      = 0;
	uint32_t max_byte_extent   = 0;
	uint32_t packed_stride     = 0;
	uint32_t descriptor_format = 0;
	uint32_t image_alias       = NoImageAlias;
	bool     read              = false;
	bool     written           = false;
	bool     atomic            = false;
	bool     formatted         = false;
	bool     scalar            = false;

	bool operator==(const BufferResource& other) const = default;
};

enum class ImageMipMode { None, DynamicStorage };

constexpr uint32_t StorageImageIdentitySwizzle = 0x00000facu;

struct ImageResource {
	uint32_t                source          = 0;
	uint32_t                first_use_pc    = 0;
	ResourceKind            kind            = ResourceKind::None;
	Decoder::ImageDimension dimension       = Decoder::ImageDimension::Unknown;
	ImageMipMode            mip_mode        = ImageMipMode::None;
	uint32_t                storage_swizzle = StorageImageIdentitySwizzle;
	bool                    read            = false;
	bool                    written         = false;
	bool                    atomic          = false;
	bool                    depth_compare   = false;

	bool operator==(const ImageResource& other) const = default;
};

struct SamplerResource {
	uint32_t source       = 0;
	uint32_t first_use_pc = 0;

	bool operator==(const SamplerResource& other) const = default;
};

struct SampledResourcePair {
	uint32_t image        = 0;
	uint32_t sampler      = 0;
	uint32_t first_use_pc = 0;

	bool operator==(const SampledResourcePair& other) const = default;
};

struct AddressResource {
	uint32_t     source           = 0;
	uint32_t     first_use_pc     = 0;
	ResourceKind kind             = ResourceKind::Flat;
	int32_t      min_offset       = 0;
	uint64_t     specialized_base = 0;
	bool         read             = false;
	bool         written          = false;
	bool         atomic           = false;

	bool operator==(const AddressResource& other) const = default;
};

enum class StageInputKind {
	VertexIndex,
	InstanceIndex,
	FragCoord,
	FrontFacing,
	WorkgroupId,
	LocalInvocationId,
	LocalInvocationIndex,
	GlobalInvocationId,
	Parameter,
};

enum class StageOutputKind { Position, Parameter, Mrt, Depth, SampleMask };

struct StageInput {
	StageInputKind kind            = StageInputKind::VertexIndex;
	uint32_t       location        = 0;
	uint32_t       component_count = 1;
	std::string    debug_name;

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
	Sampled2D,
	Sampled2DArray,
	Sampled3D,
	SampledUint2D,
	SampledUint2DArray,
	SampledUint3D,
	Storage2D,
	Storage2DArray,
	Storage3D,
	StorageUint2D,
	StorageUint2DArray,
	StorageUint3D,
	Samplers,
	Gds,
	AddressMemory,
	FlattenedSrt,
	UserData,
	Count,
};

struct DescriptorBinding {
	DescriptorBindingKind kind    = DescriptorBindingKind::Buffers;
	uint32_t              binding = 0;
	std::vector<uint32_t> resources;

	bool operator==(const DescriptorBinding& other) const = default;
};

struct BindingLayout {
	uint32_t                       descriptor_set       = 0;
	uint32_t                       push_constant_offset = 0;
	uint32_t                       push_constant_size   = 0;
	std::vector<uint32_t>          user_data_registers;
	std::vector<DescriptorBinding> descriptors;

	bool operator==(const BindingLayout& other) const = default;
};

struct ShaderInfo {
	static constexpr uint32_t MaxBuffers      = 32;
	static constexpr uint32_t MaxAddresses    = 32;
	static constexpr uint32_t MaxImages       = 32;
	static constexpr uint32_t MaxSamplers     = 32;
	static constexpr uint32_t MaxSampledPairs = 64;

	std::vector<BufferResource>      buffers;
	std::vector<AddressResource>     addresses;
	std::vector<ImageResource>       images;
	std::vector<SamplerResource>     samplers;
	std::vector<SampledResourcePair> sampled_pairs;
	std::vector<StageInput>          inputs;
	std::vector<StageOutput>         outputs;
	int32_t                          vertex_offset_sgpr = -1;

	bool operator==(const ShaderInfo& other) const = default;
};

struct Program {
	ShaderType              stage               = ShaderType::Unknown;
	ShaderLaneMaskMode      lane_mask_mode      = ShaderLaneMaskMode::NativeWave;
	uint64_t                shader_hash         = 0;
	uint32_t                wave_size           = 64;
	uint32_t                user_data_base      = 0;
	uint32_t                user_data_count     = 64;
	bool                    dispatcher_fallback = false;
	CFG::FailureKind        cfg_failure_kind    = CFG::FailureKind::None;
	std::string             fallback_reason;
	std::vector<BasicBlock> blocks;
	ScalarProvenance        provenance;
	SrtPlan                 srt;
	bool                    srt_plan_complete     = false;
	bool                    srt_patching_complete = false;
	ShaderInfo              info;
	bool                    resource_tracking_complete = false;
	bool                    shader_info_complete       = false;
	BindingLayout           bindings;
	bool                    binding_layout_complete = false;
};

bool LowerProgram(const Decoder::Program& decoded, const CFG::Graph& cfg, ShaderType stage,
                  uint32_t wave_size, Program* program, std::string* error);

std::string RegisterToString(Register reg);
std::string OperandToString(const Operand& operand);
std::string ExportTargetKindToString(ExportTargetKind kind);
std::string InstructionToString(const Instruction& inst);
std::string ProgramToString(const Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_ */

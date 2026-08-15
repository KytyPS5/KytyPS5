#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/opcodes/Opcodes.h"
#include "graphics/shader/shader.h"

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

struct ValueProgram;

enum class RegisterFile { Scalar, Vector, Vcc, Exec, Scc, M0 };

struct Register {
	RegisterFile file  = RegisterFile::Scalar;
	uint32_t     index = 0;

	bool operator==(const Register& other) const {
		return file == other.file && index == other.index;
	}
};

enum class OperandKind { Register, ImmediateU32, PcRelativeU32, PcRelativeHighU32, Null };

struct Operand {
	OperandKind kind = OperandKind::Null;
	Register    reg;
	uint32_t    imm                = 0;
	bool        sext_64            = false;
	uint32_t    sdwa_sel           = 6;
	uint32_t    sdwa_dst_unused    = 2;
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
	bool                    address_is_full                                       = false;
	bool                    data_signed                                           = false;
	bool                    typed                                                 = false;
	bool                    formatted                                             = false;
	bool                    image_has_mip                                         = false;
	bool                    image_cube                                            = false;
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
	uint32_t     src_count = 0;
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

struct DescriptorValue {
	std::array<uint32_t, 8> dwords      = {};
	uint32_t                dword_count = 0;

	bool operator==(const DescriptorValue& other) const {
		return dword_count == other.dword_count && dwords == other.dwords;
	}
};

struct BufferResource {
	static constexpr uint32_t NoImageAlias = UINT32_MAX;

	uint32_t               source            = 0;
	uint32_t               first_use_pc      = 0;
	uint32_t               max_byte_extent   = 0;
	uint32_t               packed_stride     = 0;
	Prospero::BufferFormat descriptor_format = Prospero::BufferFormat::kInvalid;
	uint32_t               image_alias       = NoImageAlias;
	bool                   read              = false;
	bool                   written           = false;
	bool                   atomic            = false;
	bool                   formatted         = false;
	bool                   scalar            = false;

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
	bool                    cube            = false;

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
	uint32_t     source           = UINT32_MAX;
	uint32_t     first_use_pc     = 0;
	ResourceKind kind             = ResourceKind::Flat;
	int32_t      min_offset       = 0;
	uint64_t     specialized_base = 0;
	bool         unbased          = true;
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
	uint32_t                       buffer_offset_dword  = 0;
	uint32_t                       buffer_offset_count  = 0;
	std::vector<uint32_t>          user_data_registers;
	std::vector<DescriptorBinding> descriptors;

	[[nodiscard]] uint32_t ShaderDataDwords() const {
		return buffer_offset_dword + (buffer_offset_count + 3u) / 4u;
	}

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
	bool                             has_bitwise_xor    = false;

	bool operator==(const ShaderInfo& other) const = default;
};

struct Program {
	ShaderType                    stage               = ShaderType::Unknown;
	ShaderLaneMaskMode            lane_mask_mode      = ShaderLaneMaskMode::NativeWave;
	uint64_t                      shader_hash         = 0;
	uint32_t                      wave_size           = 64;
	uint32_t                      user_data_base      = 0;
	uint32_t                      user_data_count     = 64;
	bool                          dispatcher_fallback = false;
	CFG::FailureKind              cfg_failure_kind    = CFG::FailureKind::None;
	std::string                   fallback_reason;
	std::vector<BasicBlock>       blocks;
	std::shared_ptr<ValueProgram> values;
	bool                          srt_plan_complete = false;
	ShaderInfo                    info;
	bool                          resource_tracking_complete = false;
	bool                          shader_info_complete       = false;
	BindingLayout                 bindings;
	bool                          binding_layout_complete = false;
};

bool LowerProgram(const Decoder::Program& decoded, const CFG::Graph& cfg, ShaderType stage,
                  uint32_t wave_size, Program& program, std::string* error);

std::string RegisterToString(Register reg);
std::string OperandToString(const Operand& operand);
std::string ExportTargetKindToString(ExportTargetKind kind);
std::string InstructionToString(const Instruction& inst);
std::string ProgramToString(const Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERIR_H_ */

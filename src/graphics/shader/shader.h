#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/shaderBindings.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {

namespace HW {
struct VertexShaderInfo;
struct PixelShaderInfo;
struct ComputeShaderInfo;
struct ShaderRegisters;
} // namespace HW

enum class ShaderType { Unknown, Vertex, Pixel, Fetch, Compute };

enum class ShaderLaneMaskMode { NativeWave, PerInvocation };

namespace ShaderRecompiler::IR {
struct Program;
struct ResourceSnapshot;
} // namespace ShaderRecompiler::IR

struct ShaderStageRuntime {
	std::shared_ptr<const ShaderRecompiler::IR::Program>          program;
	std::shared_ptr<const ShaderRecompiler::IR::ResourceSnapshot> resources;

	[[nodiscard]] explicit operator bool() const {
		return program != nullptr && resources != nullptr;
	}
};

// Resolves an immutable native shader plan against current user data. The prior stage is preserved
// if any ReadConst, snapshot, or specialization check fails.
bool ShaderMaterializeStageRuntime(std::shared_ptr<const ShaderRecompiler::IR::Program> program,
                                   std::span<const uint32_t> user_data, uint64_t shader_base,
                                   ShaderStageRuntime* stage, std::string* error);

struct ShaderId {
	uint32_t              hash0 = 0;
	uint32_t              crc32 = 0;
	std::vector<uint32_t> ids;

	bool operator==(const ShaderId& other) const {
		return hash0 == other.hash0 && crc32 == other.crc32 && ids == other.ids;
	}
	bool operator!=(const ShaderId& other) const { return !(*this == other); }
};

constexpr uint32_t DstSel(uint32_t x, uint32_t y = 0, uint32_t z = 0, uint32_t w = 0) {
	return x | (y << 3u) | (z << 6u) | (w << 9u);
}

inline uint8_t GetDstSel(uint32_t swizzle, uint32_t channel) {
	return (swizzle >> (channel * 3u)) & 0x7u;
}

struct ShaderVertexInputInfo {
	static constexpr int RES_MAX = 32;

	ShaderBufferResource    resources[RES_MAX];
	ShaderVertexDestination resources_dst[RES_MAX];
	int                     resource_fetch_components[RES_MAX] = {};
	ShaderVertexInputBuffer buffers[RES_MAX];
	ShaderStageRuntime      stage;
	int                     resources_num     = 0;
	int                     fetch_shader_reg  = 0;
	int                     fetch_attrib_reg  = 0;
	int                     fetch_buffer_reg  = 0;
	int                     buffers_num       = 0;
	int                     export_count      = 0;
	uint32_t                param_export_mask = 0;
	bool                    fetch_external    = false;
	bool                    fetch_embedded    = false;
};

struct ShaderComputeInputInfo {
	uint32_t           threads_num[3]             = {0, 0, 0};
	uint32_t           dispatch_threads_num[3]    = {0, 0, 0};
	bool               group_id[3]                = {false, false, false};
	bool               dispatch_thread_dimensions = false;
	uint32_t           wave_size                  = 64;
	int                thread_ids_num             = 0;
	int                workgroup_register         = 0;
	bool               tg_size_en                 = false;
	ShaderStageRuntime stage;
};

struct ShaderPixelInputInfo {
	uint32_t                                       interpolator_settings[32]    = {0};
	uint32_t                                       input_num                    = 0;
	uint32_t                                       ps_system_input_base         = 0;
	uint8_t                                        target_output_mode[8]        = {};
	std::array<Prospero::ColorComponentMapping, 8> target_export_mapping        = {};
	uint32_t                                       mrt_output_mask              = 0;
	uint32_t                                       descriptor_set               = 0;
	bool                                           ps_pos_x                     = false;
	bool                                           ps_pos_y                     = false;
	bool                                           ps_pos_xy                    = false;
	bool                                           ps_pos_z                     = false;
	bool                                           ps_pos_w                     = false;
	bool                                           ps_front_face                = false;
	bool                                           ps_no_perspective            = false;
	bool                                           ps_pixel_kill_enable         = false;
	bool                                           ps_depth_export_enable       = false;
	bool                                           ps_sample_mask_export_enable = false;
	bool                                           ps_early_z                   = false;
	bool                                           ps_execute_on_noop           = false;
	ShaderStageRuntime                             stage;

	bool HasPositionInput() const { return ps_pos_x || ps_pos_y || ps_pos_z || ps_pos_w; }
};

struct ShaderSharp {
	uint16_t offset_dw : 15;
	uint16_t size      : 1;
};

struct ShaderUserData {
	uint16_t*    direct_resource_offset;
	ShaderSharp* sharp_resource_offset[4];
	uint16_t     eud_size_dw;
	uint16_t     srt_size_dw;
	uint16_t     direct_resource_count;
	uint16_t     sharp_resource_count[4];
};

struct ShaderRegisterRange {
	uint16_t start;
	uint16_t end;
};

struct ShaderDrawModifier {
	uint32_t enbl_start_vertex_offset   : 1;
	uint32_t enbl_start_index_offset    : 1;
	uint32_t enbl_start_instance_offset : 1;
	uint32_t enbl_draw_index            : 1;
	uint32_t enbl_user_vgprs            : 1;
	uint32_t render_target_slice_offset : 3;
	uint32_t fuse_draws                 : 1;
	uint32_t compiler_flags             : 23;
	uint32_t is_default                 : 1;
	uint32_t reserved                   : 31;
};

struct ShaderRegister {
	uint32_t offset;
	uint32_t value;
};

struct ShaderSpecialRegs {
	ShaderRegister      ge_cntl;
	ShaderRegister      vgt_shader_stages_en;
	uint32_t            dispatch_modifier;
	ShaderRegisterRange user_data_range;
	ShaderDrawModifier  draw_modifier;
	ShaderRegister      vgt_gs_out_prim_type;
	ShaderRegister      ge_user_vgpr_en;
};

struct ShaderSemantic {
	uint32_t semantic         : 8;
	uint32_t hardware_mapping : 8;
	uint32_t size_in_elements : 4;
	uint32_t is_f16           : 2;
	uint32_t is_flat_shaded   : 1;
	uint32_t is_linear        : 1;
	uint32_t is_custom        : 1;
	uint32_t static_vb_index  : 1;
	uint32_t static_attribute : 1;
	uint32_t reserved         : 1;
	uint32_t default_value    : 2;
	uint32_t default_value_hi : 2;
};

struct Shader {
	uint32_t             file_header;
	uint32_t             version;
	ShaderUserData*      user_data;
	const volatile void* code;
	ShaderRegister*      cx_registers;
	ShaderRegister*      sh_registers;
	ShaderSpecialRegs*   specials;
	ShaderSemantic*      input_semantics;
	ShaderSemantic*      output_semantics;
	uint32_t             header_size;
	uint32_t             shader_size;
	uint32_t             embedded_constant_buffer_size_dqw;
	uint32_t             target;
	uint32_t             num_input_semantics;
	uint16_t             scratch_size_dw_per_thread;
	uint16_t             num_output_semantics;
	uint16_t             special_sizes_bytes;
	uint8_t              type;
	uint8_t              num_cx_registers;
	uint8_t              num_sh_registers;
};

struct ShaderMappedData {
	ShaderUserData* user_data           = nullptr;
	ShaderSemantic* input_semantics     = nullptr;
	uint32_t        num_input_semantics = 0;
	uint32_t        code_size_bytes     = 0;
};

void ShaderInit();
void ShaderMapUserData(uint64_t addr, const ShaderMappedData& data);

void     ShaderDbgDumpInputInfo(const ShaderVertexInputInfo* info);
void     ShaderDbgDumpInputInfo(const ShaderPixelInputInfo* info);
void     ShaderDbgDumpInputInfo(const ShaderComputeInputInfo* info);
ShaderId ShaderGetIdVS(const HW::VertexShaderInfo* regs, const ShaderVertexInputInfo* input_info,
                       bool include_bind_specialization);
ShaderId ShaderGetIdPS(const HW::PixelShaderInfo* regs, const ShaderPixelInputInfo* input_info,
                       bool include_bind_specialization);
ShaderId ShaderGetIdCS(const HW::ComputeShaderInfo* regs, const ShaderComputeInputInfo* input_info,
                       bool include_bind_specialization);
// Returned SPIR-V spans are read-only views backed by the shader program cache.
bool ShaderCompileInfoVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh,
                         ShaderLaneMaskMode lane_mask_mode, ShaderVertexInputInfo* input_info,
                         std::span<const uint32_t>* spirv);
bool ShaderCompileInfoPS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh,
                         ShaderLaneMaskMode lane_mask_mode, const ShaderVertexInputInfo* vs_info,
                         std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
                         ShaderPixelInputInfo* input_info, std::span<const uint32_t>* spirv);
bool ShaderCompileInfoCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh,
                         ShaderComputeInputInfo* input_info, std::span<const uint32_t>* spirv);
bool ShaderCompileSpirvVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh,
                          ShaderLaneMaskMode lane_mask_mode, ShaderVertexInputInfo* input_info,
                          std::vector<uint32_t>* spirv);
bool ShaderCompileSpirvPS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh,
                          ShaderLaneMaskMode lane_mask_mode, ShaderPixelInputInfo* input_info,
                          std::vector<uint32_t>* spirv);
bool ShaderCompileSpirvCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh,
                          ShaderComputeInputInfo* input_info, std::vector<uint32_t>* spirv);
bool ShaderAddressValid(uint64_t addr);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_ */

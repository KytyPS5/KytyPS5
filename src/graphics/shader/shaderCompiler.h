#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct ShaderParams {
	std::span<const uint32_t> code;
	std::span<const uint32_t> user_data;
	uint64_t                  hash = 0;

	[[nodiscard]] uint64_t Base() const {
		return reinterpret_cast<uint64_t>(code.data());
	}
};

void BuildStageStaticKey(const ShaderVertexInputInfo& input_info, std::vector<uint32_t>& key);
void BuildStageStaticKey(const ShaderPixelInputInfo& input_info, std::vector<uint32_t>& key);
void BuildStageStaticKey(const ShaderComputeInputInfo& input_info, std::vector<uint32_t>& key);

ShaderParams PrepareProgram(const HW::VertexShaderInfo& regs, const HW::ShaderRegisters& sh,
                            ShaderVertexInputInfo& input_info);
ShaderParams PrepareProgram(
    const HW::PixelShaderInfo& regs, const HW::ShaderRegisters& sh,
    const ShaderVertexInputInfo&                        vertex_info,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    ShaderPixelInputInfo&                               input_info);
ShaderParams PrepareProgram(const HW::ComputeShaderInfo& regs, const HW::ShaderRegisters& sh,
                            ShaderComputeInputInfo& input_info);

bool MaterializeProgram(const std::shared_ptr<const ShaderRecompiler::IR::Program>& program,
                        const ShaderParams& params, ShaderVertexInputInfo& input_info);
bool MaterializeProgram(const std::shared_ptr<const ShaderRecompiler::IR::Program>& program,
                        const ShaderParams& params, ShaderPixelInputInfo& input_info);
bool MaterializeProgram(const std::shared_ptr<const ShaderRecompiler::IR::Program>& program,
                        const ShaderParams& params, ShaderComputeInputInfo& input_info);

vk::ShaderModule CompileProgram(vk::Device device, const ShaderParams& params,
                                ShaderVertexInputInfo& input_info);
vk::ShaderModule CompileProgram(vk::Device device, const ShaderParams& params,
                                ShaderPixelInputInfo& input_info);
vk::ShaderModule CompileProgram(vk::Device device, const ShaderParams& params,
                                ShaderComputeInputInfo& input_info);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_ */

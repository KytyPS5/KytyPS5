#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_

#include "graphics/shader/shader.h"

#include <array>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
} // namespace HW

struct ShaderParams {
	std::span<const uint32_t> code;
	std::array<uint32_t, 40>  user_data {}; // 32 user SGPRs plus the merged-stage s0:s7 prefix.
	uint32_t                  user_data_count = 0;
	uint64_t                  hash = 0;
	std::span<const uint32_t> back_code;

	[[nodiscard]] uint64_t Base() const {
		return reinterpret_cast<uint64_t>(code.data());
	}
};

void BuildStageStaticKey(const ShaderVertexInputInfo& input_info, std::vector<uint32_t>& key);
void BuildStageStaticKey(const ShaderPixelInputInfo& input_info, std::vector<uint32_t>& key);
void BuildStageStaticKey(const ShaderComputeInputInfo& input_info, std::vector<uint32_t>& key);

// The preparers below fill an input info the caller value-initializes.
ShaderParams PrepareProgram(const HW::VertexShaderInfo& regs, const HW::Context& context,
                            const HW::UserConfig& user_config, ShaderVertexInputInfo& input_info);
// Each stage's tess holds the VGT fields; the caller adds the layout reflected from the programs.
std::array<ShaderParams, 3>
PrepareTessellationPrograms(const HW::VertexShaderInfo& regs, const HW::Context& context,
                            std::array<ShaderVertexInputInfo, 3>& input_info);
ShaderParams PrepareProgram(
    const HW::PixelShaderInfo& regs, const HW::ShaderRegisters& sh,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    ShaderPixelInputInfo&                               input_info);
ShaderParams PrepareProgram(const HW::ComputeShaderInfo& regs, ShaderComputeInputInfo& input_info);

// Reads the guest memory PrepareProgram needs besides registers and shader code (vertex fetch
// tables); false when the range cannot be read.
using ShaderGuestReader = bool (*)(void* userdata, uint64_t address, void* data, uint64_t size);

// Makes PrepareProgram on the calling thread read guest memory through `reader` instead of plain
// loads while the scope lives (a null reader keeps plain loads).
class ShaderGuestReaderScope {
public:
	ShaderGuestReaderScope(ShaderGuestReader reader, void* userdata);
	~ShaderGuestReaderScope();
	ShaderGuestReaderScope(const ShaderGuestReaderScope&)            = delete;
	ShaderGuestReaderScope& operator=(const ShaderGuestReaderScope&) = delete;

private:
	ShaderGuestReader m_previous_reader;
	void*             m_previous_userdata;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_SHADERCOMPILER_H_ */

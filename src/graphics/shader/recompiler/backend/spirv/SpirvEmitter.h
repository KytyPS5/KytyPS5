#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Host device capabilities and user options that change the emitted SPIR-V. Built once per device.
struct Profile {
	bool maintenance8         = false; // Dynamic texel offsets on image samples.
	bool native_float_min_max = false; // NMin/NMax give the guest's NaN and signed-zero results.
	// OpReadClockKHR Device tick in picoseconds; 0 without shaderDeviceClock.
	uint32_t device_clock_period_ps = 0;
	// Storage buffer elements past the descriptor range, to the byte, read zero and drop writes.
	bool robust_buffer_access = false;
	// 8 and 8_8 sRGB textures sample through sRGB views; without one the shader decodes them.
	bool r8_srgb_views   = false;
	bool r8g8_srgb_views = false;
	// Native float ops: NaN, signed-zero and denormal results may differ from the guest.
	bool relaxed_float = false;
	// Ray queries: recognized traversal loops trace the dispatch's host TLAS.
	bool ray_query = false;
	// Per-vertex interpolation computes BaryCoordKHR.x as 1 - y - z instead of reading it. NVIDIA's
	// compiler stops on an internal breakpoint when a sample-shaded MSAA pipeline reads .x.
	bool derive_barycentric_x = false;
};

inline constexpr Profile DefaultProfile {};

// Locations of the pixel parameter inputs read only per-vertex or flat. Both stages declare these
// varyings as integers so their 32-bit words cross the stage interface unchanged.
[[nodiscard]] uint32_t PixelRawParameterMask(const ShaderPixelInputInfo&     info,
                                             std::span<const IR::StageInput> inputs);

// Resets and writes backend IDs and descriptor indices; do not emit the same program concurrently.
std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t       push_data_start_dword = 0,
                                  const Profile& profile               = DefaultProfile,
                                  bool           emit_debug_info       = false,
                                  uint32_t       push_data_ceiling     = IR::PushData::DwordCount);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */

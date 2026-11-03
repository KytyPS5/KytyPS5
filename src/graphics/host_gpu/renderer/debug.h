#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_

#include "common/emulatorConfig.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandBuffer;

namespace HW {
class UserConfig;
struct ScanModeControl;
struct ScreenViewport;
} // namespace HW

struct ScissorRect {
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;
};

inline bool graphics_debug_dump_enabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::LogDirection::Silent;
}

inline uint32_t render_target_mask_slot(uint32_t mask, uint32_t slot) {
	return (mask >> (slot * 4u)) & 0x0fu;
}

void        uc_print(const char* func, const HW::UserConfig& uc);
void        uc_check(const HW::UserConfig& uc);
void        hw_print(const CommandBuffer& buffer);
void        hw_check(const CommandBuffer& buffer);
ScissorRect calc_final_scissor(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc,
                                 vk::Extent2D extent, uint32_t viewport_index);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_

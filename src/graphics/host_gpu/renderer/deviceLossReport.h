#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

struct GraphicContext;

// KYTY_GPU_CHECKPOINTS=1 enables VK_NV_device_diagnostic_checkpoints: every command buffer debug
// op records a checkpoint, so a device loss report can name the operations the GPU was running.
[[nodiscard]] bool DeviceLossCheckpointsRequested();

void DeviceLossRecordCheckpoint(vk::CommandBuffer buffer, uint32_t op, uint64_t submit_id,
                                uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                uint64_t arg4, uint64_t pixel_shader);

// Names the compiled shader of the most recent checkpoint once the dispatch has resolved it.
void DeviceLossAnnotateShader(uint64_t shader_hash);

// Prints what VK_EXT_device_fault and the checkpoints know about a lost device. Only the first
// call for ErrorDeviceLost reports; other results and later calls do nothing.
void DeviceLossReport(GraphicContext& graphics, vk::Result result);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEVICELOSSREPORT_H_

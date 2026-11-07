#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUBREADCRUMBS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUBREADCRUMBS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <string>

// Which GPU command was running when the device was lost (KYTY_GPU_BREADCRUMBS, with
// VK_AMD_buffer_marker). Every recorded draw and dispatch gets an id. A marker writes it to host
// memory when the command starts (top of pipe) and another when it ends (bottom of pipe). The
// memory stays readable after the device is lost: a command started and never ended is the one
// the GPU hung in, and the description kept for its id says what it was.

namespace Libs::Graphics {

struct GraphicContext;

namespace GpuBreadcrumbs {

[[nodiscard]] bool Enabled(const GraphicContext& graphics);
// Around one command: Begin before it, End after it, in the same command buffer.
// `args_buffer` at `args_offset`: the indirect arguments the command reads. A copy taken just
// before it shows what the GPU read, whatever guest memory holds later.
[[nodiscard]] uint32_t Begin(GraphicContext& graphics, vk::CommandBuffer command,
                             std::string description, uint64_t indirect_args = 0,
                             vk::Buffer args_buffer = nullptr, uint64_t args_offset = 0);
void End(vk::CommandBuffer command, uint32_t id);
// What was in flight, printed when the device is lost.
void Report();
// Host-visible memory to print with the report (the GDS).
void Watch(const char* name, const uint32_t* data, uint32_t dwords);

} // namespace GpuBreadcrumbs
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUBREADCRUMBS_H_

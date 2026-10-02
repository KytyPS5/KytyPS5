#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_HOSTMEMORYIMPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_HOSTMEMORYIMPORT_H_

namespace Libs::Graphics {

struct GraphicContext;

// Zero-copy buffer cache groundwork: with KYTY_ZERO_COPY_PROBE=1, imports chunks of the guest
// backing view through VK_EXT_external_memory_host and logs what the driver accepts (memory
// types, per-import size limits, device addresses). Read-only: nothing is written to guest
// memory, and every import is released before returning.
void ProbeHostMemoryImport(GraphicContext& ctx);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_HOSTMEMORYIMPORT_H_

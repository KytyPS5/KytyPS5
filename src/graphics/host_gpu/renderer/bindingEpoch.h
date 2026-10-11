#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BINDINGEPOCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BINDINGEPOCH_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

inline std::atomic<uint64_t> g_graphics_binding_epoch {0};

inline void DisturbGraphicsBindings() {
	g_graphics_binding_epoch.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t GraphicsBindingEpoch() {
	return g_graphics_binding_epoch.load(std::memory_order_relaxed);
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BINDINGEPOCH_H_

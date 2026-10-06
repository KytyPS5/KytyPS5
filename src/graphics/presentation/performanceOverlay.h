#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PERFORMANCEOVERLAY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PERFORMANCEOVERLAY_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

struct GraphicContext;

// Called for every guest flip that reaches the screen, with the guest's video-out size.
void PerformanceOverlayRecordFlip(uint32_t guest_width, uint32_t guest_height) noexcept;

[[nodiscard]] bool PerformanceOverlayEnabled() noexcept;
void               TogglePerformanceOverlay() noexcept;
// Switches between the compact and the detailed panel, showing the panel if it was hidden.
void TogglePerformanceOverlayDetails() noexcept;

// Changes when the panel needs a repaint without a guest flip (toggled, or the guest stalled).
[[nodiscard]] uint64_t PerformanceOverlayRevision() noexcept;

// Must run between ImGui::NewFrame() and ImGui::Render(), on the present thread while the
// graphics subsystem is alive. Draws in the top-right corner and returns the panel's bottom
// edge, or 0 when hidden.
float DrawPerformanceOverlay(const GraphicContext& graphics, vk::Extent2D extent);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PERFORMANCEOVERLAY_H_

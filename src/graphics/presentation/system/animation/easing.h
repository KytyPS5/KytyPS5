#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_EASING_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_EASING_H_

#include "graphics/presentation/system/core/theme.h"

namespace Libs::Graphics::SystemUi {

/// The theme's easing curve `ease` at progress `t`, where 0 is the start and 1 the end. Progress
/// outside 0..1 is clamped and NaN counts as 0, so a curve always starts at 0 and ends at 1.
/// Emphasis overshoots past 1 on the way, every other curve stays within 0..1.
[[nodiscard]] float ApplyEase(Ease ease, float t);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ANIMATION_EASING_H_

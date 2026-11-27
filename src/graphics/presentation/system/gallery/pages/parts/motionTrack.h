#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_MOTION_TRACK_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_MOTION_TRACK_H_

#include "graphics/presentation/system/animation.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

#include <cstdint>
#include <string_view>

// The moving dot the motion pages show a duration or an ease with. Each needs an open FrameScope.
namespace Libs::Graphics::SystemUi::Gallery {

/// How long a looping specimen rests at each end before it moves back, in milliseconds.
inline constexpr float MOTION_HOLD_MS = 600.0f;

/// Whether a specimen that loops over `duration_ms` with a rest at each end is on its way out, or
/// resting after it, rather than on its way back, by the current clock. It flips at the moments a
/// move starts, so a fade or slide keyed on it plays once each way.
[[nodiscard]] bool LoopIsOut(float duration_ms);

/// How many times a specimen looping over `duration_ms` has flipped between out and back by the
/// current clock, so a slide keyed on it plays its entrance again at every flip.
[[nodiscard]] uint64_t LoopFlips(float duration_ms);

/// How far along its track a dot moving over `duration_ms` with `ease` is now, from 0 to 1. It
/// moves out, rests, moves back and rests again. With reduced motion it jumps between the ends at
/// the moments it would have started moving.
[[nodiscard]] float DotPosition(float duration_ms, Ease ease);

/// A labelled track with a dot `position` of the way along it. The dot travels between rooms kept
/// at both ends, so an ease that overshoots past either end still lands on the track.
[[nodiscard]] AnyElement TrackRow(const ThemeContext& theme, std::string_view label,
                                  float position);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_MOTION_TRACK_H_

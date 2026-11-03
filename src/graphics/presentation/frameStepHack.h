#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_FRAMESTEPHACK_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_FRAMESTEPHACK_H_

#include <cstdint>
#include <span>

namespace Libs::VideoOut::FrameStepHack {

// [HACK] ASTRO BOT (PPSA21559, PPSA21564, PPSA21567, PPSA21568) advances its simulation by one
// frame of its frame rate for each frame, whatever the time between frames. Below that rate, down
// to a third of it, the game's frame time follows the presented frames instead, and the physics code
// that is not stable with longer steps is changed (down to half of the rate where that code is not
// found). No effect on other titles, or on executables where the patched code and data are not
// found.

// Offsets from the load address of an executable segment.
struct CodeRange {
	uint64_t begin = 0;
	uint64_t end   = 0;
};

// Called once with the main executable, after relocation (and the cheat file, if any) and before the
// game runs. `mapped_size` is the size of its mapping, past which the physics fixes get their code.
void PatchMainProgram(uint64_t base_vaddr, uint64_t size, uint64_t mapped_size,
                      std::span<const CodeRange> code, bool cheat_file) noexcept;

// Called on the presentation thread for each presented main-bus guest flip.
void OnPresentedFrame() noexcept;

} // namespace Libs::VideoOut::FrameStepHack

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_FRAMESTEPHACK_H_

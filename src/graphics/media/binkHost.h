#ifndef EMULATOR_SRC_GRAPHICS_MEDIA_BINKHOST_H_
#define EMULATOR_SRC_GRAPHICS_MEDIA_BINKHOST_H_

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::BinkHost {

// Host-side playback for movies the guest decodes inside its own executable.
//
// Demon's Souls' logo and title screen are 4K Bink 2 (.bk2) files. The game never calls
// libSceVideodec or sceAvPlayer for them - it runs its own decoder on its own threads - so there
// is no HLE export to intercept and no texture-cache fix that reaches them; what the guest
// produces is garbage and we faithfully present it. Instead the kernel's file opens tell us which
// movie is playing and we decode it on the host, exactly as SharpEmu does.
//
// Enabled by KYTY_BINK_HOST=1. ffmpeg is bound at RUNTIME from KYTY_BINK_FFMPEG_DIR because our
// bundled avcodec is built without the binkvideo2 decoder; loading a build that has it needs no
// import library and no CMake change, and is safe here only because both are avcodec 61.19.101,
// so our headers describe those binaries' structures exactly.

// Called for every guest file open, with the resolved host path.
void NotifyFileOpen(const std::string& host_path);

// True once a .bk2 has been opened and ffmpeg is usable.
[[nodiscard]] bool Active();

// Next frame of the active movie, tightly packed at the requested size and converted to
// `av_pixel_format` (an AVPixelFormat). The caller passes whatever matches the swapchain image so
// the result copies in with no conversion - Demon's Souls presents 10-bit A2B10G10R10, not RGBA,
// and writing 8-bit pixels into it misaligns every channel.
// Returns false when playback is not running, the size is invalid, or the movie has ended.
[[nodiscard]] bool NextFrame(uint32_t width, uint32_t height, int av_pixel_format,
                             uint32_t bytes_per_pixel, std::vector<uint8_t>& out);

} // namespace Libs::Graphics::BinkHost

#endif // EMULATOR_SRC_GRAPHICS_MEDIA_BINKHOST_H_

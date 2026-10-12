#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_VIDEOOUTRESOLUTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_VIDEOOUTRESOLUTION_H_

#include "common/emulatorConfig.h"

#include <cstdint>

namespace Libs::Graphics {

// The output resolution the emulated console reports to a game (VideoOutGetOutputStatus).
//
// Games pick their render resolution from it: told 4K, a game renders its passes at 3840x2160
// whatever the window size, and the window shows a downsampled picture.
//   Title:  4K, unless the title may detect the output (param attribute3 bit 2) and the output is
//           below 4K. This was the only behaviour before the option.
//   FullHd: 1080p to every title.
//   Uhd:    4K to every title.
// FullHd and Uhd are emulator settings. Whether a console whose output is set to 1080p reports
// 1080p to a title without the detection bit is not verified.
struct VideoOutResolutionState {
	Config::VideoOutResolution setting = Config::VideoOutResolution::Title;
	// The title's param attribute3.
	int32_t  attribute3 = 0;
	uint32_t width      = 0;
	uint32_t height     = 0;
};

// The SceVideoOutOutputStatus resolution codes.
inline constexpr uint32_t VIDEO_OUT_RESOLUTION_FULL_HD = 1;
inline constexpr uint32_t VIDEO_OUT_RESOLUTION_UHD     = 2;

[[nodiscard]] constexpr uint32_t ReportedVideoOutResolution(const VideoOutResolutionState& s) {
	switch (s.setting) {
		case Config::VideoOutResolution::FullHd: return VIDEO_OUT_RESOLUTION_FULL_HD;
		case Config::VideoOutResolution::Uhd: return VIDEO_OUT_RESOLUTION_UHD;
		case Config::VideoOutResolution::Title: break;
	}
	return (s.attribute3 & 4) != 0 && s.width < 3840 && s.height < 2160
	           ? VIDEO_OUT_RESOLUTION_FULL_HD
	           : VIDEO_OUT_RESOLUTION_UHD;
}

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_VIDEOOUTRESOLUTION_H_ */

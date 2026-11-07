// Tests for the output resolution the emulated console reports to a game.

#include "graphics/presentation/videoOutResolution.h"

#include <iostream>

namespace {

using Libs::Graphics::ReportedVideoOutResolution;
using Libs::Graphics::VIDEO_OUT_RESOLUTION_FULL_HD;
using Libs::Graphics::VIDEO_OUT_RESOLUTION_UHD;
using Libs::Graphics::VideoOutResolutionState;
using Setting = Config::VideoOutResolution;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

void TestTitleKeepsTheEarlierRule() {
	Check(ReportedVideoOutResolution({.attribute3 = 0, .width = 1920, .height = 1080}) ==
	          VIDEO_OUT_RESOLUTION_UHD,
	      "a title that may not detect the output is told 4K");
	Check(ReportedVideoOutResolution({.attribute3 = 4, .width = 1920, .height = 1080}) ==
	          VIDEO_OUT_RESOLUTION_FULL_HD,
	      "a title that may detect it is told the output below 4K");
	Check(ReportedVideoOutResolution({.attribute3 = 4, .width = 3840, .height = 2160}) ==
	          VIDEO_OUT_RESOLUTION_UHD,
	      "a 4K output is 4K");
}

void TestTheConsoleSettingDecides() {
	for (const int32_t attribute3: {0, 4}) {
		Check(ReportedVideoOutResolution({.setting    = Setting::FullHd,
		                                  .attribute3 = attribute3,
		                                  .width      = 3840,
		                                  .height     = 2160}) == VIDEO_OUT_RESOLUTION_FULL_HD,
		      "a console set to 1080p reports 1080p to every title");
		Check(ReportedVideoOutResolution({.setting    = Setting::Uhd,
		                                  .attribute3 = attribute3,
		                                  .width      = 1920,
		                                  .height     = 1080}) == VIDEO_OUT_RESOLUTION_UHD,
		      "a console set to 4K reports 4K to every title");
	}
}

} // namespace

int main() {
	TestTitleKeepsTheEarlierRule();
	TestTheConsoleSettingDecides();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "video out resolution tests passed\n";
	return 0;
}

#include "graphics/regression/frameRegression.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Libs::Graphics::Regression::FrameView;
using Libs::Graphics::Regression::Mode;
using Libs::Graphics::Regression::Options;
using Libs::Graphics::Regression::PixelFormat;
using Libs::Graphics::Regression::Session;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FrameRegressionTests failed: %s\n", message);
		std::abort();
	}
}

FrameView View(uint64_t ordinal, const std::vector<uint8_t>& pixels) {
	return {.ordinal = ordinal,
	        .width = 2,
	        .height = 2,
	        .row_pitch = 8,
	        .format = PixelFormat::Rgba8Unorm,
	        .bytes = pixels};
}

std::unique_ptr<Session> Create(Mode mode, const std::filesystem::path& baseline,
	                            const std::filesystem::path& report, std::string& error) {
	return Session::Create(Options {.mode = mode,
	                                .baseline = baseline,
	                                .report = report,
	                                .frame_ordinals = {1, 3},
	                                .save_raw_frames = true},
	                       error);
}

} // namespace

int main() {
	const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::temp_directory_path() /
	                  ("kyty_frame_regression_tests_" + std::to_string(unique));
	std::filesystem::create_directories(root);
	const auto baseline = root / "baseline.json";
	const auto report   = root / "report.json";
	const std::vector<uint8_t> first {0, 1, 2, 255, 3, 4, 5, 255,
	                                  6, 7, 8, 255, 9, 10, 11, 255};
	const std::vector<uint8_t> second {20, 21, 22, 255, 23, 24, 25, 255,
	                                   26, 27, 28, 255, 29, 30, 31, 255};

	std::string error;
	auto record = Create(Mode::Record, baseline, {}, error);
	Check(record != nullptr, error.c_str());
	Check(!record->WantsFrame(0) && record->WantsFrame(1), "frame selection");
	Check(record->Observe(View(1, first), error), error.c_str());
	Check(record->Observe(View(3, second), error), error.c_str());
	Check(record->Complete() && record->Finalize(error) == 0, "baseline completion");
	Check(std::filesystem::is_regular_file(baseline), "baseline manifest");
	Check(std::filesystem::is_regular_file(root / "baseline_frames/frame_00000001.ppm"),
	      "baseline preview");

	error.clear();
	Check(Create(Mode::Record, baseline, {}, error) == nullptr &&
	          error.find("overwrite") != std::string::npos,
	      "baseline overwrite protection");

	Check(Create(Mode::Compare, baseline, baseline, error) == nullptr &&
	          error.find("collide") != std::string::npos,
	      "baseline manifest collision protection");
	Check(Create(Mode::Compare, baseline, root / "baseline.txt", error) == nullptr &&
	          error.find("collide") != std::string::npos,
	      "baseline artifact collision protection");
	Check(Create(Mode::Compare, baseline,
	             root / "baseline_frames/frame_00000001.raw", error) == nullptr &&
	          error.find("collide") != std::string::npos,
	      "baseline referenced-artifact collision protection");

	const auto oversized_baseline = root / "oversized.json";
	{
		std::ofstream stream(oversized_baseline, std::ios::binary | std::ios::trunc);
		const std::string oversized(4 * 1024 * 1024 + 1, ' ');
		stream.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
	}
	Check(Create(Mode::Compare, oversized_baseline, root / "oversized_report.json", error) ==
	          nullptr &&
	          error.find("read") != std::string::npos,
	      "oversized baseline protection");

	auto compare = Create(Mode::Compare, baseline, report, error);
	Check(compare != nullptr, error.c_str());
	Check(compare->Observe(View(1, first), error), error.c_str());
	Check(compare->Observe(View(3, second), error), error.c_str());
	Check(compare->Finalize(error) == 0, "exact comparison");

	auto changed = second;
	changed[5] ^= 0x40;
	const auto mismatch_report = root / "mismatch.json";
	auto mismatch = Create(Mode::Compare, baseline, mismatch_report, error);
	Check(mismatch != nullptr, error.c_str());
	Check(mismatch->Observe(View(1, first), error), error.c_str());
	Check(mismatch->Observe(View(3, changed), error), error.c_str());
	Check(mismatch->Finalize(error) == 2, "mismatch exit code");
	Check(std::filesystem::is_regular_file(
	          root / "mismatch_frames/diff_frame_00000003.ppm"),
	      "mismatch diff image");

	const auto incomplete_report = root / "incomplete.json";
	auto incomplete = Create(Mode::Compare, baseline, incomplete_report, error);
	Check(incomplete != nullptr, error.c_str());
	Check(incomplete->Observe(View(1, first), error), error.c_str());
	Check(incomplete->Finalize(error) == 2, "incomplete exit code");

	std::filesystem::remove_all(root);
	return 0;
}

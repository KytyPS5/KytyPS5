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
using Libs::Graphics::Regression::Provenance;
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

Provenance TestProvenance() {
	return {.test_id = "test-game/revision/checkpoint",
	        .build_id = "test-build",
	        .gpu_name = "test-gpu",
	        .configuration = "test-configuration",
	        .gpu_vendor_id = 1,
	        .gpu_device_id = 2,
	        .gpu_driver = 3,
	        .vulkan_api = 4};
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string ReadText(const std::filesystem::path& path) {
	std::ifstream stream(path);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::unique_ptr<Session> Create(Mode mode, const std::filesystem::path& baseline,
	                            const std::filesystem::path& report, std::string& error) {
	return Session::Create(Options {.mode = mode,
	                                .baseline = baseline,
	                                .report = report,
	                                .capture = {},
	                                .frame_ordinals = {1, 3},
	                                .provenance = TestProvenance(),
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
	Check(Create(Mode::Record, root / "reserved.kyty-lock", {}, error) == nullptr &&
	          error.find("reserved") != std::string::npos,
	      "reservation suffix protection");
	Check(Create(Mode::Record, baseline, {}, error) == nullptr &&
	          error.find("use") != std::string::npos,
	      "concurrent baseline writer protection");
	error.clear();
	Check(!record->WantsFrame(0) && record->WantsFrame(1), "frame selection");
	Check(record->Observe(View(1, first), error), error.c_str());
	Check(record->Observe(View(3, second), error), error.c_str());
	Check(record->Complete() && record->Finalize(error) == 0, "baseline completion");
	Check(std::filesystem::is_regular_file(baseline), "baseline manifest");
	Check(std::filesystem::is_regular_file(root / "baseline.json_frames/frame_00000001.ppm"),
	      "baseline preview");

	const auto parallel_json = root / "parallel.json";
	const auto parallel_text = root / "parallel.txt";
	auto parallel_first = Create(Mode::Record, parallel_json, {}, error);
	auto parallel_second = Create(Mode::Record, parallel_text, {}, error);
	Check(parallel_first != nullptr && parallel_second != nullptr,
	      "same-stem outputs must have independent reservations");
	Check(parallel_first->Observe(View(1, first), error) &&
	          parallel_first->Observe(View(3, second), error) &&
	          parallel_first->Finalize(error) == 0,
	      "first parallel baseline");
	Check(parallel_second->Observe(View(1, first), error) &&
	          parallel_second->Observe(View(3, second), error) &&
	          parallel_second->Finalize(error) == 0,
	      "second parallel baseline");
	Check(std::filesystem::is_directory(root / "parallel.json_frames") &&
	          std::filesystem::is_directory(root / "parallel.txt_frames"),
	      "same-stem artifact isolation");

	error.clear();
	Check(Create(Mode::Record, baseline, {}, error) == nullptr &&
	          error.find("overwrite") != std::string::npos,
	      "baseline overwrite protection");
	const auto directory_report = root / "directory-report.json";
	std::filesystem::create_directory(directory_report);
	Check(Create(Mode::Compare, baseline, directory_report, error) == nullptr &&
	          error.find("directory") != std::string::npos,
	      "report directory protection");

	Check(Create(Mode::Compare, baseline, baseline, error) == nullptr &&
	          error.find("collide") != std::string::npos,
	      "baseline manifest collision protection");
	Check(Create(Mode::Compare, baseline, root / "baseline.json_frames/report.json", error) ==
	          nullptr &&
	          error.find("collide") != std::string::npos,
	      "baseline artifact collision protection");
	Check(Create(Mode::Compare, baseline,
	             root / "baseline.json_frames/frame_00000001.raw", error) == nullptr &&
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
	Check(!compare->Observe(View(1, first), error), "finalized session must reject frames");

	auto incompatible_options = Options {.mode = Mode::Compare,
	                                     .baseline = baseline,
	                                     .report = root / "incompatible.json",
	                                     .capture = {},
	                                     .frame_ordinals = {1, 3},
	                                     .provenance = TestProvenance(),
	                                     .save_raw_frames = false};
	incompatible_options.provenance.gpu_driver++;
	error.clear();
	Check(Session::Create(incompatible_options, error) == nullptr &&
	          error.find("provenance") != std::string::npos,
	      "environment mismatch protection");
	incompatible_options.allow_environment_mismatch = true;
	Check(Session::Create(incompatible_options, error) != nullptr,
	      "explicit environment mismatch override");

	auto changed = second;
	changed[5] ^= 0x40;
	const auto mismatch_report = root / "mismatch.json";
	auto mismatch = Create(Mode::Compare, baseline, mismatch_report, error);
	Check(mismatch != nullptr, error.c_str());
	Check(mismatch->Observe(View(1, first), error), error.c_str());
	Check(mismatch->Observe(View(3, changed), error), error.c_str());
	Check(mismatch->Finalize(error) == 2, "mismatch exit code");
	Check(std::filesystem::is_regular_file(
	          root / "mismatch.json_frames/diff_frame_00000003.ppm"),
	      "mismatch diff image");
	error.clear();
	Check(Create(Mode::Compare, mismatch_report, root / "invalid-baseline-report.json", error) ==
	          nullptr &&
	          error.find("baseline") != std::string::npos,
	      "failed comparison report cannot become a baseline");

	auto alpha_changed = first;
	alpha_changed[3] = 0;
	const auto alpha_report = root / "alpha.json";
	auto alpha_mismatch = Create(Mode::Compare, baseline, alpha_report, error);
	Check(alpha_mismatch != nullptr, error.c_str());
	Check(alpha_mismatch->Observe(View(1, alpha_changed), error), error.c_str());
	Check(alpha_mismatch->Observe(View(3, second), error), error.c_str());
	Check(alpha_mismatch->Finalize(error) == 2, "alpha mismatch exit code");
	const auto alpha_diff = root / "alpha.json_frames/diff_frame_00000001.ppm";
	const auto alpha_bytes = ReadBytes(alpha_diff);
	Check(alpha_bytes.size() >= 12 &&
	          std::any_of(alpha_bytes.end() - 12, alpha_bytes.end(),
	                      [](uint8_t byte) { return byte != 0; }),
	      "alpha mismatch must be visible in difference image");

	{
		std::ofstream corrupt(root / "baseline.json_frames/frame_00000003.raw",
		                      std::ios::binary | std::ios::trunc);
		corrupt << "corrupt";
	}
	const auto corrupt_raw_report = root / "corrupt_raw.json";
	auto corrupt_raw = Create(Mode::Compare, baseline, corrupt_raw_report, error);
	Check(corrupt_raw != nullptr, error.c_str());
	Check(corrupt_raw->Observe(View(1, first), error), error.c_str());
	Check(corrupt_raw->Observe(View(3, changed), error), error.c_str());
	Check(corrupt_raw->Finalize(error) == 2, "corrupt reference raw exit code");
	const auto corrupt_report = ReadText(corrupt_raw_report);
	Check(corrupt_report.find("mismatch_reference_unavailable") != std::string::npos,
	      "corrupt reference raw diagnostic");

	const auto incomplete_report = root / "incomplete.json";
	auto incomplete = Create(Mode::Compare, baseline, incomplete_report, error);
	Check(incomplete != nullptr, error.c_str());
	Check(incomplete->Observe(View(1, first), error), error.c_str());
	Check(incomplete->Finalize(error) == 2, "incomplete exit code");

	const auto reverse_baseline = root / "reverse.json_frames/frame_00000001.raw";
	auto reverse_record = Create(Mode::Record, reverse_baseline, {}, error);
	Check(reverse_record != nullptr, error.c_str());
	Check(reverse_record->Observe(View(1, first), error), error.c_str());
	Check(reverse_record->Observe(View(3, second), error), error.c_str());
	Check(reverse_record->Finalize(error) == 0, "reverse baseline creation");
	error.clear();
	Check(Create(Mode::Compare, reverse_baseline, root / "reverse.json", error) == nullptr &&
	          error.find("overwrite") != std::string::npos,
	      "report artifact cannot overwrite baseline manifest");

	std::error_code cleanup_error;
	std::filesystem::remove_all(root, cleanup_error);
	Check(!cleanup_error, "temporary output cleanup");
	return 0;
}

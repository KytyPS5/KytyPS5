#include "graphics/capture/gpuTrace.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Libs::Graphics::Capture::LoadTrace;
using Libs::Graphics::Capture::Trace;
using Libs::Graphics::Capture::TraceEventType;
using Libs::Graphics::Capture::TraceReadLimits;
using Libs::Graphics::Capture::TraceWriter;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "GpuTraceTests failed: %s\n", message);
		std::abort();
	}
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(reinterpret_cast<const char*>(bytes.data()),
	             static_cast<std::streamsize>(bytes.size()));
	Check(stream.good(), "could not write test fixture");
}

} // namespace

int main() {
	const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::temp_directory_path() /
	                  ("kyty_gpu_trace_tests_" + std::to_string(unique));
	std::filesystem::create_directories(root);
	const auto capture = root / "commands.kycap";

	std::string error;
	Check(TraceWriter::Create(root / "reserved.kyty-lock", error) == nullptr &&
	          error.find("reserved") != std::string::npos,
	      "reservation suffix protection");
	auto        writer = TraceWriter::Create(capture, error);
	Check(writer != nullptr, error.c_str());
	std::filesystem::remove(capture);
	Check(TraceWriter::Create(capture, error) == nullptr && error.find("use") != std::string::npos,
	      "concurrent capture writer protection");
	writer.reset();
	writer = TraceWriter::Create(capture, error);
	Check(writer != nullptr, error.c_str());
	error.clear();
	const std::array<uint32_t, 3> draw {0xc0011000, 0x11223344, 0x55667788};
	const std::array<uint32_t, 2> constants {0xc0007600, 0xaabbccdd};
	const std::array<uint32_t, 2> compute {0xc0001500, 0x12345678};
	Check(writer->RecordGraphics(draw, constants, true, error), error.c_str());
	Check(writer->RecordCompute(0x27, compute, false, error), error.c_str());
	Check(writer->RecordMarker(TraceEventType::FlipPreparation, error), error.c_str());
	Check(writer->RecordMarker(TraceEventType::SuspendRequest, error), error.c_str());
	Check(writer->Finalize(error), error.c_str());
	Check(writer->Finalize(error), "capture finalization must be idempotent");

	Trace trace;
	Check(LoadTrace(capture, trace, error), error.c_str());
	Check(trace.events.size() == 4, "event count");
	Check(trace.events[0].sequence == 1 && trace.events[0].primary ==
	                                             std::vector<uint32_t>(draw.begin(), draw.end()),
	      "graphics event");
	Check(trace.events[0].secondary ==
	          std::vector<uint32_t>(constants.begin(), constants.end()) &&
	          trace.events[0].trigger_interrupt_on_done,
	      "graphics constants and flags");
	Check(trace.events[1].queue == 0x27 && !trace.events[1].trigger_interrupt_on_done,
	      "compute event");
	Check(trace.events[2].type == TraceEventType::FlipPreparation &&
	          trace.events[3].type == TraceEventType::SuspendRequest,
	      "marker events");

	auto corrupted = ReadBytes(capture);
	Check(corrupted.size() > 64, "capture size");
	corrupted[corrupted.size() - 32 - 1] ^= 0x80;
	const auto corrupt_path = root / "corrupt.kycap";
	WriteBytes(corrupt_path, corrupted);
	error.clear();
	Check(!LoadTrace(corrupt_path, trace, error) && error.find("checksum") != std::string::npos,
	      "checksum validation");

	auto metadata_corrupted = ReadBytes(capture);
	constexpr size_t second_record_queue_offset = 24 + 40 + 5 * sizeof(uint32_t) + 16;
	Check(metadata_corrupted.size() > second_record_queue_offset, "capture metadata size");
	metadata_corrupted[second_record_queue_offset] ^= 1;
	const auto metadata_path = root / "metadata_corrupt.kycap";
	WriteBytes(metadata_path, metadata_corrupted);
	error.clear();
	Check(!LoadTrace(metadata_path, trace, error) && error.find("checksum") != std::string::npos,
	      "metadata checksum validation");

	auto truncated = ReadBytes(capture);
	truncated.resize(truncated.size() - 3);
	const auto truncated_path = root / "truncated.kycap";
	WriteBytes(truncated_path, truncated);
	error.clear();
	Check(!LoadTrace(truncated_path, trace, error), "truncation validation");

	auto missing_footer = ReadBytes(capture);
	missing_footer.resize(missing_footer.size() - 32);
	const auto missing_footer_path = root / "missing_footer.kycap";
	WriteBytes(missing_footer_path, missing_footer);
	error.clear();
	Check(!LoadTrace(missing_footer_path, trace, error) &&
	          error.find("finalized") != std::string::npos,
	      "whole-footer truncation validation");

	auto missing_record = ReadBytes(capture);
	constexpr size_t last_marker_size = 40;
	missing_record.erase(missing_record.end() - 32 - last_marker_size,
	                     missing_record.end() - 32);
	const auto missing_record_path = root / "missing_record.kycap";
	WriteBytes(missing_record_path, missing_record);
	error.clear();
	Check(!LoadTrace(missing_record_path, trace, error) && error.find("footer") != std::string::npos,
	      "whole-record truncation validation");

	const auto header_only_path = root / "header_only.kycap";
	auto header_only = ReadBytes(capture);
	header_only.resize(24);
	WriteBytes(header_only_path, header_only);
	error.clear();
	Check(!LoadTrace(header_only_path, trace, error), "header-only capture validation");

	error.clear();
	Check(!LoadTrace(capture, trace, error,
	                 TraceReadLimits {.max_events = 2, .max_command_dwords = 100}),
	      "event limit");

	std::filesystem::remove_all(root);
	return 0;
}

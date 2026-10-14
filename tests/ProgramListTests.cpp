// A program list on disk holds what each host shader program was translated from. These tests
// cover the file format: what was written is read back, and a damaged tail loses only itself.

#include "graphics/host_gpu/renderer/pipeline/programList.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

using Libs::Graphics::AppendPipelineRecord;
using Libs::Graphics::LatestByIdentity;
using Libs::Graphics::ListedPipelineScan;
using Libs::Graphics::RewriteList;
using Libs::Graphics::AppendProgramRecord;
using Libs::Graphics::PipelineRecord;
using Libs::Graphics::ReadPipelineRecords;
using Libs::Graphics::ProgramRecord;
using Libs::Graphics::ReadProgramRecords;
using Libs::Graphics::RecordLastOffset;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ProgramListTests: failed: %s\n", message);
		std::abort();
	}
}

ProgramRecord Vertex() {
	ProgramRecord record;
	record.stage           = Libs::Graphics::ShaderType::Vertex;
	record.hash            = 0x1122334455667788ull;
	record.user_data_count = 24;
	record.push_data_start = 3;
	record.code            = {0xbf8c007fu, 0x7e000280u, 0xbf810000u};
	record.input_info      = {1, 2, 3, 4, 5, 6, 7};
	record.specialization.buffers.push_back(
	    {.packed_stride = 16, .descriptor_swizzle = 0x688, .zero_stride_oob = true});
	record.specialization.images.push_back({.mip_count = 5, .indirect_root = 2, .cube = true});
	record.specialization.images.push_back({.shader_swizzle = 0x123, .fmask = true});
	return record;
}

ProgramRecord Geometry() {
	ProgramRecord record;
	record.stage     = Libs::Graphics::ShaderType::Mesh;
	record.hash      = 7;
	record.code      = {1, 2};
	record.back_code = {3, 4, 5};
	return record;
}

PipelineRecord Pipeline() {
	PipelineRecord record;
	record.programs      = {0x11, 0, 0, 0x22};
	record.fixed_state   = {1, 2, 3, 4, 5};
	record.stage_count   = 1;
	record.stages[0]     = {9, 8, 7};
	record.pixel_present = true;
	record.pixel         = {6, 5};
	return record;
}

void TestPipelineRoundTrip() {
	std::vector<uint8_t> bytes;
	AppendPipelineRecord(bytes, Pipeline());
	auto second          = Pipeline();
	second.pixel_present = false;
	second.pixel.clear();
	AppendPipelineRecord(bytes, second);
	const auto records = ReadPipelineRecords(bytes);
	Check(records.size() == 2 && records[0] == Pipeline() && records[1] == second,
	      "pipeline records were not read back as written");
	bytes.pop_back();
	Check(ReadPipelineRecords(bytes).size() == 1, "a cut-off pipeline record was read");
}

void TestRoundTrip() {
	std::vector<uint8_t> bytes;
	AppendProgramRecord(bytes, Vertex());
	AppendProgramRecord(bytes, Geometry());
	const auto records = ReadProgramRecords(bytes);
	Check(records.size() == 2 && records[0] == Vertex() && records[1] == Geometry(),
	      "records were not read back as written");
	Check(ReadProgramRecords({}).empty(), "an empty file gave a record");
}

void TestEqualRecordsGiveEqualBytes() {
	std::vector<uint8_t> first;
	std::vector<uint8_t> second;
	AppendProgramRecord(first, Vertex());
	AppendProgramRecord(second, Vertex());
	Check(first == second, "the same record was written as different bytes");
}

void TestCutOffTail() {
	std::vector<uint8_t> bytes;
	AppendProgramRecord(bytes, Vertex());
	const auto whole = bytes.size();
	AppendProgramRecord(bytes, Geometry());
	for (auto size = whole; size < bytes.size(); size++) {
		const auto records = ReadProgramRecords(std::span(bytes).first(size));
		Check(records.size() == 1 && records[0] == Vertex(),
		      "a cut-off record took the record before it with it, or was read");
	}
}

void TestDamagedRecord() {
	std::vector<uint8_t> bytes;
	AppendProgramRecord(bytes, Vertex());
	const auto whole = bytes.size();
	AppendProgramRecord(bytes, Geometry());
	bytes[whole + 9] ^= 0x40;
	const auto records = ReadProgramRecords(bytes);
	Check(records.size() == 1 && records[0] == Vertex(), "a damaged record was read");
	bytes[5] ^= 0x01;
	Check(ReadProgramRecords(bytes).empty(), "records after a damaged one were read");
}

} // namespace

// A field added into padding keeps the size of a struct. The list's signature must still change:
// records from the old layout put each later field's byte in the wrong place.
struct LayoutBefore {
	uint32_t value = 0;
	bool     first = false;
	bool     last  = false;
};
struct LayoutAfter {
	uint32_t value = 0;
	bool     first = false;
	bool     added = false;
	bool     last  = false;
};

void TestLayoutChangeInPadding() {
	static_assert(sizeof(LayoutBefore) == sizeof(LayoutAfter));
	Check(RecordLastOffset(&LayoutBefore::last) != RecordLastOffset(&LayoutAfter::last),
	      "a field added into padding left the layout unchanged");
	Check(RecordLastOffset(&LayoutBefore::last) == 5 && RecordLastOffset(&LayoutBefore::value) == 0,
	      "a member offset is wrong");
}

// A program recorded again has the later record, in the place of the first one.
void TestTheLaterRecordWins() {
	struct Record {
		uint64_t    identity;
		std::string text;
	};
	std::vector<Record> records {{1, "first"}, {2, "two"}, {1, "again"}, {3, "dropped"}};
	const auto kept = LatestByIdentity(std::move(records), [](const Record& record) {
		return record.identity == 3 ? std::nullopt : std::optional<uint64_t> {record.identity};
	});
	Check(kept.size() == 2 && kept[0].first == 1 && kept[0].second.text == "again" &&
	          kept[1].first == 2 && kept[1].second.text == "two",
	      "a program recorded again did not keep its later record in its first place");
}

std::string ReadFile(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// The list is written into a temporary file that then takes its place, and stays open to append.
void TestAListIsRewrittenThroughATemporaryFile() {
	const auto folder = std::filesystem::temp_directory_path() / "kyty_program_list_tests";
	std::filesystem::remove_all(folder);
	const auto path = folder / "title.programs";
	std::filesystem::create_directories(folder);
	std::ofstream(path, std::ios::binary) << "old list";
	const std::string bytes = "new list";
	std::FILE* file = RewriteList(path, std::span(reinterpret_cast<const uint8_t*>(bytes.data()),
	                                              bytes.size()));
	Check(file != nullptr, "a list could not be rewritten");
	std::fputs("+appended", file);
	std::fclose(file);
	Check(ReadFile(path) == "new list+appended", "the rewritten list did not replace the old one");
	auto temp_path = path;
	temp_path += ".tmp";
	Check(!std::filesystem::exists(temp_path), "the temporary file was left behind");

	// A folder that cannot be made: nothing is written and the caller gets no file.
	std::ofstream(folder / "blocker") << "a file, not a folder";
	Check(RewriteList(folder / "blocker" / "title.programs", {}) == nullptr,
	      "a list under a file was taken as written");
	std::filesystem::remove_all(folder);
}

void TestListedPipelinesAreScannedOnlyAfterAChange() {
	ListedPipelineScan scan;
	Check(scan.Due(0, false), "the first draw did not scan the listed pipelines");
	Check(!scan.Due(0, false), "a draw scanned again with no new program");
	Check(scan.Due(1, false), "a new program did not start a scan");
	Check(!scan.Due(1, false), "a draw scanned again after the scan for the new program");
	Check(scan.Due(1, true), "the end of the program list did not start a scan");
	Check(!scan.Due(1, true), "a draw scanned again after the list was done");
}

int main() {
	TestRoundTrip();
	TestEqualRecordsGiveEqualBytes();
	TestCutOffTail();
	TestDamagedRecord();
	TestPipelineRoundTrip();
	TestLayoutChangeInPadding();
	TestListedPipelinesAreScannedOnlyAfterAChange();
	TestTheLaterRecordWins();
	TestAListIsRewrittenThroughATemporaryFile();
	std::puts("ProgramListTests: all cases passed");
	return 0;
}

#include "common/file.h"
#include "loader/elf.h"
#include "loader/elfValidator.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace Loader;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ElfValidatorTests: failed: %s\n", text);
		std::abort();
	}
}

template <typename T>
void Store(std::vector<uint8_t>& image, size_t offset, const T& value) {
	if (image.size() < offset + sizeof(T)) {
		image.resize(offset + sizeof(T));
	}
	std::memcpy(image.data() + offset, &value, sizeof(T));
}

Elf64_Ehdr Header(uint16_t program_count = 1) {
	Elf64_Ehdr header {};
	header.e_ident[EI_MAG0]       = 0x7f;
	header.e_ident[EI_MAG1]       = 'E';
	header.e_ident[EI_MAG2]       = 'L';
	header.e_ident[EI_MAG3]       = 'F';
	header.e_ident[EI_CLASS]      = ELFCLASS64;
	header.e_ident[EI_DATA]       = ELFDATA2LSB;
	header.e_ident[EI_VERSION]    = EV_CURRENT;
	header.e_ident[EI_OSABI]      = ELFOSABI_FREEBSD;
	header.e_ident[EI_ABIVERSION] = 2;
	header.e_type                 = ET_DYNEXEC;
	header.e_machine              = EM_X86_64;
	header.e_version              = EV_CURRENT;
	header.e_phoff                = sizeof(Elf64_Ehdr);
	header.e_ehsize               = sizeof(Elf64_Ehdr);
	header.e_phentsize            = sizeof(Elf64_Phdr);
	header.e_phnum                = program_count;
	return header;
}

SelfHeader Self() {
	SelfHeader self {};
	const uint8_t ident[] = {0x4f, 0x15, 0x3d, 0x1d, 0x00, 0x01,
	                         0x01, 0x12, 0x01, 0x01, 0x00, 0x00};
	std::memcpy(self.ident, ident, sizeof(ident));
	self.unknown = 0x22;
	return self;
}

std::vector<uint8_t> RawElf() {
	std::vector<uint8_t> image(0x180);
	const auto           header = Header();
	Store(image, 0, header);
	Elf64_Phdr load {};
	load.p_type   = PT_LOAD;
	load.p_offset = 0x100;
	load.p_filesz = 0x20;
	load.p_memsz  = 0x20;
	Store(image, header.e_phoff, load);
	return image;
}

std::vector<uint8_t> SelfImage() {
	std::vector<uint8_t> image(0x180);
	auto                 self = Self();
	self.segments_num           = 1;
	Store(image, 0, self);
	SelfSegment segment {};
	segment.type              = 0x800u;
	segment.offset            = 0x140;
	segment.compressed_size   = 0x20;
	segment.decompressed_size = 0x20;
	Store(image, sizeof(SelfHeader), segment);

	const uint64_t elf_offset = sizeof(SelfHeader) + sizeof(SelfSegment);
	const auto     header     = Header();
	Store(image, elf_offset, header);
	Elf64_Phdr load {};
	load.p_type   = PT_LOAD;
	load.p_offset = 0x2000;
	load.p_filesz = 0x20;
	load.p_memsz  = 0x20;
	Store(image, elf_offset + header.e_phoff, load);
	return image;
}

void TestRawElfAndFile() {
	const auto image = RawElf();
	ElfValidationReport report;
	std::string         error;
	Check(ValidateElfImage(image, &report, &error), error.c_str());
	Check(!report.self && report.elf_offset == 0, "raw ELF report");

	const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto path = std::filesystem::current_path() /
	                  ("kyty_elf_validator_" + std::to_string(unique) + ".bin");
	Common::File output(path);
	Check(!output.IsInvalid(), "create temporary ELF file");
	uint32_t written = 0;
	output.Write(image.data(), static_cast<uint32_t>(image.size()), &written);
	Check(written == image.size() && output.Flush(), "write temporary ELF file");
	output.Close();
	Check(ValidateElfFile(path, &report, &error), error.c_str());
	Check(Common::File::DeleteFile(path), "remove temporary ELF file");
}

void TestProgramAndSegmentBounds() {
	std::string error;
	auto        truncated = RawElf();
	truncated.resize(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) - 1u);
	Check(!ValidateElfImage(truncated, nullptr, &error) &&
	          error.find("program header table") != std::string::npos,
	      "truncated program table");

	auto overflow = RawElf();
	auto header   = Header();
	header.e_phoff = std::numeric_limits<uint64_t>::max() - 8u;
	Store(overflow, 0, header);
	Check(!ValidateElfImage(overflow, nullptr, &error) &&
	          error.find("program header table") != std::string::npos,
	      "overflowing program-table offset");

	auto outside = RawElf();
	Elf64_Phdr load {};
	load.p_type   = PT_LOAD;
	load.p_offset = 0x170;
	load.p_filesz = 0x20;
	Store(outside, Header().e_phoff, load);
	Check(!ValidateElfImage(outside, nullptr, &error) &&
	          error.find("segment payload") != std::string::npos,
	      "segment outside file");

	load.p_offset = std::numeric_limits<uint64_t>::max();
	load.p_filesz = 0;
	Store(outside, Header().e_phoff, load);
	Check(ValidateElfImage(outside, nullptr, &error),
	      "zero-length segment with unused offset");

	auto no_programs      = RawElf();
	auto empty_header     = Header(0);
	empty_header.e_phoff = std::numeric_limits<uint64_t>::max();
	Store(no_programs, 0, empty_header);
	Check(ValidateElfImage(no_programs, nullptr, &error),
	      "empty program table with unused offset");
}

void TestSectionBounds() {
	std::string error;
	auto        image  = RawElf();
	auto        header = Header();
	header.e_shoff     = 0x120;
	header.e_shentsize = sizeof(Elf64_Shdr);
	header.e_shnum     = 1;
	Store(image, 0, header);
	Elf64_Shdr section {};
	section.sh_type   = 1;
	section.sh_offset = 0x170;
	section.sh_size   = 0x20;
	Store(image, header.e_shoff, section);
	Check(!ValidateElfImage(image, nullptr, &error) &&
	          error.find("section payload") != std::string::npos,
	      "section outside file");

	section.sh_type   = 8; // SHT_NOBITS has no file payload.
	section.sh_offset = std::numeric_limits<uint64_t>::max();
	Store(image, header.e_shoff, section);
	Check(ValidateElfImage(image, nullptr, &error), "NOBITS section without file payload");
}

void TestSelfContainer() {
	auto image = SelfImage();
	ElfValidationReport report;
	std::string         error;
	Check(ValidateElfImage(image, &report, &error), error.c_str());
	Check(report.self && report.elf_offset == sizeof(SelfHeader) + sizeof(SelfSegment),
	      "SELF embedded ELF offset");

	auto unsupported = image;
	unsupported[4]   = 0xff;
	Check(!ValidateElfImage(unsupported, nullptr, &error) &&
	          error.find("unsupported SELF") != std::string::npos,
	      "unsupported SELF variant");

	auto aligned_declared_size = image;
	aligned_declared_size.resize(image.size() - 8u);
	auto self         = Self();
	self.segments_num = 1;
	self.file_size    = image.size();
	Store(aligned_declared_size, 0, self);
	Check(ValidateElfImage(aligned_declared_size, nullptr, &error),
	      "SELF omitted final alignment padding");

	auto invalid_declared_size = image;
	self.file_size             = invalid_declared_size.size() + 1u;
	Store(invalid_declared_size, 0, self);
	Check(!ValidateElfImage(invalid_declared_size, nullptr, &error) &&
	          error.find("declared file size") != std::string::npos,
	      "SELF declared file size");

	auto invalid_reference = image;
	SelfSegment segment {};
	std::memcpy(&segment, invalid_reference.data() + sizeof(SelfHeader), sizeof(segment));
	segment.type = 0x800u | (1ull << 20u);
	Store(invalid_reference, sizeof(SelfHeader), segment);
	Check(!ValidateElfImage(invalid_reference, nullptr, &error) &&
	          error.find("invalid ELF program header") != std::string::npos,
	      "SELF program-header reference");

	auto compressed = image;
	std::memcpy(&segment, compressed.data() + sizeof(SelfHeader), sizeof(segment));
	segment.compressed_size = 0x10;
	Store(compressed, sizeof(SelfHeader), segment);
	Check(!ValidateElfImage(compressed, nullptr, &error) &&
	          error.find("compressed SELF") != std::string::npos,
	      "unsupported SELF compression");

	auto overflowing = image;
	std::memcpy(&segment, overflowing.data() + sizeof(SelfHeader), sizeof(segment));
	segment.offset          = std::numeric_limits<uint64_t>::max() - 3u;
	segment.compressed_size = 8;
	Store(overflowing, sizeof(SelfHeader), segment);
	Check(!ValidateElfImage(overflowing, nullptr, &error) &&
	          error.find("SELF segment payload") != std::string::npos,
	      "overflowing SELF payload");

	auto program_overflow = image;
	Elf64_Phdr program {};
	program.p_type   = PT_LOAD;
	program.p_offset = std::numeric_limits<uint64_t>::max() - 3u;
	program.p_filesz = 8;
	const auto program_offset = sizeof(SelfHeader) + sizeof(SelfSegment) + sizeof(Elf64_Ehdr);
	Store(program_overflow, program_offset, program);
	Check(!ValidateElfImage(program_overflow, nullptr, &error) &&
	          error.find("segment range overflows") != std::string::npos,
	      "overflowing SELF ELF segment range");
}

} // namespace

int main() {
	TestRawElfAndFile();
	TestProgramAndSegmentBounds();
	TestSectionBounds();
	TestSelfContainer();
	return 0;
}

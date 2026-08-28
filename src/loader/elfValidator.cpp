#include "loader/elfValidator.h"

#include "common/file.h"
#include "loader/elf.h"

#include <cstring>
#include <limits>

namespace Loader {
namespace {

constexpr Elf64_Word SHT_NOBITS = 8;

bool Fail(std::string* error, const char* message) {
	if (error != nullptr) {
		*error = message;
	}
	return false;
}

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t* result) {
	if (result == nullptr || right > std::numeric_limits<uint64_t>::max() - left) {
		return false;
	}
	*result = left + right;
	return true;
}

bool RangeFits(uint64_t offset, uint64_t size, uint64_t total) {
	// ELF permits unused zero-length entries to carry an otherwise irrelevant offset.
	return size == 0 || (offset <= total && size <= total - offset);
}

bool TableFits(uint64_t base, uint64_t offset, uint64_t count, uint64_t element_size,
	           uint64_t total, uint64_t* table_offset = nullptr) {
	if (count == 0) {
		if (base > total) {
			return false;
		}
		if (table_offset != nullptr) {
			*table_offset = base;
		}
		return true;
	}
	uint64_t absolute = 0;
	if (!CheckedAdd(base, offset, &absolute) || absolute > total || element_size == 0 ||
	    count > (total - absolute) / element_size) {
		return false;
	}
	if (table_offset != nullptr) {
		*table_offset = absolute;
	}
	return true;
}

template <typename T>
bool ReadStruct(std::span<const uint8_t> image, uint64_t offset, T* value) {
	if (value == nullptr || offset > image.size() || sizeof(T) > image.size() - offset) {
		return false;
	}
	std::memcpy(value, image.data() + offset, sizeof(T));
	return true;
}

bool HasSelfMagic(std::span<const uint8_t> image) {
	SelfHeader header {};
	if (image.size() < 4) {
		return false;
	}
	std::memcpy(header.ident, image.data(), 4);
	return HasSupportedSelfMagic(header);
}

bool ValidateHeader(const Elf64_Ehdr& header, std::string* error) {
	if (header.e_ident[EI_MAG0] != 0x7f || header.e_ident[EI_MAG1] != 'E' ||
	    header.e_ident[EI_MAG2] != 'L' || header.e_ident[EI_MAG3] != 'F') {
		return Fail(error, "missing ELF magic");
	}
	if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
	    header.e_ident[EI_VERSION] != EV_CURRENT) {
		return Fail(error, "unsupported ELF class, byte order, or identification version");
	}
	if (header.e_ident[EI_OSABI] != ELFOSABI_FREEBSD ||
	    (header.e_ident[EI_ABIVERSION] != 0 && header.e_ident[EI_ABIVERSION] != 2)) {
		return Fail(error, "unsupported ELF ABI");
	}
	if ((header.e_type != ET_DYNEXEC && header.e_type != ET_DYNAMIC) ||
	    header.e_machine != EM_X86_64 || header.e_version != EV_CURRENT) {
		return Fail(error, "unsupported ELF type, machine, or version");
	}
	if (header.e_ehsize != sizeof(Elf64_Ehdr) || header.e_phentsize != sizeof(Elf64_Phdr) ||
	    (header.e_shnum != 0 && header.e_shentsize != sizeof(Elf64_Shdr))) {
		return Fail(error, "invalid ELF header entry sizes");
	}
	return true;
}

} // namespace

bool ValidateElfImage(std::span<const uint8_t> image, ElfValidationReport* report,
	                  std::string* error) {
	ElfValidationReport local;
	if (image.empty()) {
		return Fail(error, "empty ELF/SELF image");
	}

	SelfHeader self_header {};
	if (HasSelfMagic(image)) {
		local.self = true;
		if (!ReadStruct(image, 0, &self_header)) {
			return Fail(error, "truncated SELF header");
		}
		if (!IsSupportedSelfHeader(self_header)) {
			return Fail(error, "unsupported SELF header variant");
		}
		if (self_header.file_size > image.size()) {
			return Fail(error, "SELF declared file size is outside the file");
		}
		if (!TableFits(sizeof(SelfHeader), 0, self_header.segments_num, sizeof(SelfSegment),
		               image.size(), &local.elf_offset)) {
			return Fail(error, "truncated SELF segment table");
		}
		local.elf_offset +=
		    static_cast<uint64_t>(self_header.segments_num) * sizeof(SelfSegment);
		for (uint16_t index = 0; index < self_header.segments_num; index++) {
			SelfSegment segment {};
			const auto offset = sizeof(SelfHeader) +
			                    static_cast<uint64_t>(index) * sizeof(SelfSegment);
			if (!ReadStruct(image, offset, &segment) ||
			    !RangeFits(segment.offset, segment.compressed_size, image.size())) {
				return Fail(error, "SELF segment payload is outside the file");
			}
		}
	}

	Elf64_Ehdr header {};
	if (!ReadStruct(image, local.elf_offset, &header)) {
		return Fail(error, "truncated ELF header");
	}
	if (!ValidateHeader(header, error)) {
		return false;
	}

	uint64_t program_table = 0;
	if (!TableFits(local.elf_offset, header.e_phoff, header.e_phnum, sizeof(Elf64_Phdr),
	               image.size(), &program_table)) {
		return Fail(error, "ELF program header table is outside the file");
	}
	uint64_t section_table = 0;
	if (!local.self && header.e_shnum != 0 &&
	    !TableFits(local.elf_offset, header.e_shoff, header.e_shnum, sizeof(Elf64_Shdr),
	               image.size(), &section_table)) {
		return Fail(error, "ELF section header table is outside the file");
	}

	uint32_t process_param_count = 0;
	for (Elf64_Half index = 0; index < header.e_phnum; index++) {
		Elf64_Phdr program {};
		const auto offset = program_table + static_cast<uint64_t>(index) * sizeof(Elf64_Phdr);
		if (!ReadStruct(image, offset, &program)) {
			return Fail(error, "truncated ELF program header");
		}
		if (!local.self && !RangeFits(program.p_offset, program.p_filesz, image.size())) {
			return Fail(error, "ELF segment payload is outside the file");
		}
		if (local.self &&
		    program.p_filesz > std::numeric_limits<uint64_t>::max() - program.p_offset) {
			return Fail(error, "SELF ELF segment range overflows");
		}
		if (program.p_type == PT_OS_PROCPARAM) {
			process_param_count++;
			if (process_param_count != 1) {
				return Fail(error, "ELF contains multiple process-parameter segments");
			}
			if (program.p_filesz < 0x18u) {
				return Fail(error, "ELF process-parameter segment is truncated");
			}
		}
	}

	if (local.self) {
		for (uint16_t index = 0; index < self_header.segments_num; index++) {
			SelfSegment segment {};
			if (!ReadStruct(image,
			                sizeof(SelfHeader) + static_cast<uint64_t>(index) * sizeof(SelfSegment),
			                &segment)) {
				return Fail(error, "truncated SELF segment table");
			}
			if ((segment.type & 0x800u) == 0u) {
				continue;
			}

			const auto program_index = (segment.type >> 20u) & 0xfffu;
			if (program_index >= header.e_phnum) {
				return Fail(error, "SELF segment references an invalid ELF program header");
			}
			Elf64_Phdr program {};
			if (!ReadStruct(image, program_table + program_index * sizeof(Elf64_Phdr), &program)) {
				return Fail(error, "truncated ELF program header");
			}
			if (segment.compressed_size != segment.decompressed_size) {
				return Fail(error, "compressed SELF segments are not supported");
			}
			if (segment.decompressed_size != program.p_filesz) {
				return Fail(error, "SELF segment size does not match its ELF program header");
			}
		}
	} else if (header.e_shnum != 0) {
		for (Elf64_Half index = 0; index < header.e_shnum; index++) {
			Elf64_Shdr section {};
			const auto offset = section_table + static_cast<uint64_t>(index) * sizeof(Elf64_Shdr);
			if (!ReadStruct(image, offset, &section)) {
				return Fail(error, "truncated ELF section header");
			}
			if (section.sh_type != SHT_NOBITS &&
			    !RangeFits(section.sh_offset, section.sh_size, image.size())) {
				return Fail(error, "ELF section payload is outside the file");
			}
		}
	}

	if (report != nullptr) {
		*report = local;
	}
	if (error != nullptr) {
		error->clear();
	}
	return true;
}

bool ValidateElfFile(const std::filesystem::path& file_name, ElfValidationReport* report,
	                 std::string* error) {
	Common::File file(file_name, Common::File::Mode::Read);
	if (file.IsInvalid()) {
		return Fail(error, "cannot open ELF/SELF image");
	}
	if (file.Size() > std::numeric_limits<uint32_t>::max()) {
		file.Close();
		return Fail(error, "ELF/SELF image exceeds the supported validator size");
	}
	auto image = file.ReadWholeBuffer();
	file.Close();
	return ValidateElfImage(
	    {reinterpret_cast<const uint8_t*>(image.GetDataConst()), image.Size()}, report, error);
}

} // namespace Loader

#include "loader/bootFailure.h"

namespace Loader {

std::string_view BootFailureToString(BootFailure kind) {
	switch (kind) {
		case BootFailure::None: return "None";
		case BootFailure::FileNotFound: return "FileNotFound";
		case BootFailure::HeaderUnreadable: return "HeaderUnreadable";
		case BootFailure::NotAnElf: return "NotAnElf";
		case BootFailure::NotElf64: return "NotElf64";
		case BootFailure::NotLittleEndian: return "NotLittleEndian";
		case BootFailure::UnsupportedIdentVersion: return "UnsupportedIdentVersion";
		case BootFailure::UnsupportedOsAbi: return "UnsupportedOsAbi";
		case BootFailure::UnsupportedAbiVersion: return "UnsupportedAbiVersion";
		case BootFailure::UnsupportedObjectType: return "UnsupportedObjectType";
		case BootFailure::UnsupportedMachine: return "UnsupportedMachine";
		case BootFailure::UnsupportedElfVersion: return "UnsupportedElfVersion";
		case BootFailure::BadProgramHeaderSize: return "BadProgramHeaderSize";
		case BootFailure::BadSectionHeaderSize: return "BadSectionHeaderSize";
	}
	return "Unknown";
}

std::string_view BootFailureExplain(BootFailure kind) {
	switch (kind) {
		case BootFailure::None: return "the executable header is valid";
		case BootFailure::FileNotFound:
			return "the executable could not be opened - check the path and that the file is "
			       "readable";
		case BootFailure::HeaderUnreadable:
			return "the file is too short or truncated to contain an ELF header - the dump is "
			       "probably incomplete";
		case BootFailure::NotAnElf:
			return "this is not an ELF file - if it is still encrypted or is a .pkg, it has to be "
			       "extracted first";
		case BootFailure::NotElf64: return "this is a 32-bit ELF; the PS5 is 64-bit only";
		case BootFailure::NotLittleEndian: return "this ELF is big-endian; the PS5 is little-endian";
		case BootFailure::UnsupportedIdentVersion:
			return "the ELF identification version is not the one this format uses - the file is "
			       "likely corrupt";
		case BootFailure::UnsupportedOsAbi:
			return "this executable is not built for the FreeBSD ABI, so it is not a PS4/PS5 "
			       "binary - a PC or Linux executable will land here";
		case BootFailure::UnsupportedAbiVersion:
			return "the ABI version is one this loader does not handle yet - please report the "
			       "title";
		case BootFailure::UnsupportedObjectType:
			return "this ELF is neither a PS5 executable nor a PS5 shared library, so there is "
			       "nothing to boot";
		case BootFailure::UnsupportedMachine:
			return "this executable is not built for x86-64, so it cannot run here";
		case BootFailure::UnsupportedElfVersion:
			return "the ELF version field is not the current one - the file is likely corrupt";
		case BootFailure::BadProgramHeaderSize:
			return "the program header table entry size is wrong - the file is likely corrupt or "
			       "was extracted incorrectly";
		case BootFailure::BadSectionHeaderSize:
			return "the section header table entry size is wrong - the file is likely corrupt or "
			       "was extracted incorrectly";
	}
	return "the executable was rejected for a reason this build does not have a name for";
}

BootFailure ClassifyElfHeader(const Elf64_Ehdr& ehdr) {
	// Order matters and deliberately matches the original ladder in
	// Elf64::IsValid(): the earlier a check is, the less it assumes. Reporting
	// "not x86-64" for a file that is not an ELF at all would be misleading.
	if (ehdr.e_ident[EI_MAG0] != '\x7f' || ehdr.e_ident[EI_MAG1] != 'E' ||
	    ehdr.e_ident[EI_MAG2] != 'L' || ehdr.e_ident[EI_MAG3] != 'F') {
		return BootFailure::NotAnElf;
	}
	if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
		return BootFailure::NotElf64;
	}
	if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
		return BootFailure::NotLittleEndian;
	}
	if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) {
		return BootFailure::UnsupportedIdentVersion;
	}
	if (ehdr.e_ident[EI_OSABI] != ELFOSABI_FREEBSD) {
		return BootFailure::UnsupportedOsAbi;
	}
	if (ehdr.e_ident[EI_ABIVERSION] != 0 && ehdr.e_ident[EI_ABIVERSION] != 2) {
		return BootFailure::UnsupportedAbiVersion;
	}
	if (ehdr.e_type != ET_DYNEXEC && ehdr.e_type != ET_DYNAMIC) {
		return BootFailure::UnsupportedObjectType;
	}
	if (ehdr.e_machine != EM_X86_64) {
		return BootFailure::UnsupportedMachine;
	}
	if (ehdr.e_version != EV_CURRENT) {
		return BootFailure::UnsupportedElfVersion;
	}
	if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
		return BootFailure::BadProgramHeaderSize;
	}
	if (ehdr.e_shentsize > 0 && ehdr.e_shentsize != sizeof(Elf64_Shdr)) {
		return BootFailure::BadSectionHeaderSize;
	}
	return BootFailure::None;
}

} // namespace Loader

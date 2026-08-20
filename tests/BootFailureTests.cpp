#include "loader/bootFailure.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Loader::BootFailure;
using Loader::ClassifyElfHeader;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "BootFailureTests: failed: %s\n", text);
		std::abort();
	}
}

// A header that ClassifyElfHeader() accepts. Every negative case below starts
// from this and breaks exactly one field, so a test that fails names the field.
Loader::Elf64_Ehdr GoodHeader() {
	Loader::Elf64_Ehdr ehdr {};
	std::memset(&ehdr, 0, sizeof(ehdr));
	ehdr.e_ident[Loader::EI_MAG0]       = '\x7f';
	ehdr.e_ident[Loader::EI_MAG1]       = 'E';
	ehdr.e_ident[Loader::EI_MAG2]       = 'L';
	ehdr.e_ident[Loader::EI_MAG3]       = 'F';
	ehdr.e_ident[Loader::EI_CLASS]      = Loader::ELFCLASS64;
	ehdr.e_ident[Loader::EI_DATA]       = Loader::ELFDATA2LSB;
	ehdr.e_ident[Loader::EI_VERSION]    = Loader::EV_CURRENT;
	ehdr.e_ident[Loader::EI_OSABI]      = Loader::ELFOSABI_FREEBSD;
	ehdr.e_ident[Loader::EI_ABIVERSION] = 0;
	ehdr.e_type                         = Loader::ET_DYNEXEC;
	ehdr.e_machine                      = Loader::EM_X86_64;
	ehdr.e_version                      = Loader::EV_CURRENT;
	ehdr.e_phentsize                    = sizeof(Loader::Elf64_Phdr);
	ehdr.e_shentsize                    = sizeof(Loader::Elf64_Shdr);
	return ehdr;
}

void ExpectFailure(void (*damage)(Loader::Elf64_Ehdr&), BootFailure expected, const char* what) {
	auto ehdr = GoodHeader();
	damage(ehdr);
	const auto actual = ClassifyElfHeader(ehdr);
	if (actual != expected) {
		std::fprintf(stderr, "BootFailureTests: %s: expected %s, got %s\n", what,
		             Loader::BootFailureToString(expected).data(),
		             Loader::BootFailureToString(actual).data());
		std::abort();
	}
	// A classification nobody can read is not an improvement on a log line.
	Check(!Loader::BootFailureExplain(actual).empty(), "explanation is not empty");
}

void TestAcceptsValidHeaders() {
	Check(ClassifyElfHeader(GoodHeader()) == BootFailure::None, "a good executable is accepted");

	// Shared libraries and ABI version 2 are equally valid PS5 objects.
	auto shared   = GoodHeader();
	shared.e_type = Loader::ET_DYNAMIC;
	Check(ClassifyElfHeader(shared) == BootFailure::None, "a shared object is accepted");

	auto abi2                          = GoodHeader();
	abi2.e_ident[Loader::EI_ABIVERSION] = 2;
	Check(ClassifyElfHeader(abi2) == BootFailure::None, "ABI version 2 is accepted");

	// A file with no section table at all is legal; only a wrong entry size is not.
	auto no_sections       = GoodHeader();
	no_sections.e_shentsize = 0;
	Check(ClassifyElfHeader(no_sections) == BootFailure::None, "an absent section table is fine");
}

void TestClassifiesEachRejection() {
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_ident[Loader::EI_MAG1] = 'X'; },
	              BootFailure::NotAnElf, "bad magic");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_ident[Loader::EI_CLASS] = 1; },
	              BootFailure::NotElf64, "32-bit class");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_ident[Loader::EI_DATA] = 2; },
	              BootFailure::NotLittleEndian, "big endian");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_ident[Loader::EI_VERSION] = 7; },
	              BootFailure::UnsupportedIdentVersion, "ident version");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_ident[Loader::EI_OSABI] = 0; },
	              BootFailure::UnsupportedOsAbi, "System V ABI");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_ident[Loader::EI_ABIVERSION] = 3; },
	              BootFailure::UnsupportedAbiVersion, "ABI version 3");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_type = 2; }, BootFailure::UnsupportedObjectType,
	              "plain ET_EXEC");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_machine = 40; },
	              BootFailure::UnsupportedMachine, "ARM");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_version = 0; },
	              BootFailure::UnsupportedElfVersion, "ELF version");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_phentsize = 1; },
	              BootFailure::BadProgramHeaderSize, "program header entry size");
	ExpectFailure([](Loader::Elf64_Ehdr& e) { e.e_shentsize = 1; },
	              BootFailure::BadSectionHeaderSize, "section header entry size");
}

// The order of the checks is load-bearing: reporting "not x86-64" for a file
// that is not an ELF at all would send a reporter chasing the wrong thing.
void TestReportsTheEarliestCause() {
	auto ehdr = GoodHeader();
	ehdr.e_ident[Loader::EI_MAG0] = 0;
	ehdr.e_ident[Loader::EI_CLASS] = 1;
	ehdr.e_machine                 = 40;
	Check(ClassifyElfHeader(ehdr) == BootFailure::NotAnElf,
	      "the first failing check is the one reported");

	auto zeroed = Loader::Elf64_Ehdr {};
	std::memset(&zeroed, 0, sizeof(zeroed));
	Check(ClassifyElfHeader(zeroed) == BootFailure::NotAnElf, "an all-zero header is not an ELF");
}

void TestEveryKindHasNames() {
	const BootFailure all[] = {
	    BootFailure::None,
	    BootFailure::FileNotFound,
	    BootFailure::HeaderUnreadable,
	    BootFailure::NotAnElf,
	    BootFailure::NotElf64,
	    BootFailure::NotLittleEndian,
	    BootFailure::UnsupportedIdentVersion,
	    BootFailure::UnsupportedOsAbi,
	    BootFailure::UnsupportedAbiVersion,
	    BootFailure::UnsupportedObjectType,
	    BootFailure::UnsupportedMachine,
	    BootFailure::UnsupportedElfVersion,
	    BootFailure::BadProgramHeaderSize,
	    BootFailure::BadSectionHeaderSize,
	};

	for (auto kind: all) {
		const auto name = Loader::BootFailureToString(kind);
		Check(!name.empty() && name != "Unknown", "every kind has a stable name");
		Check(!Loader::BootFailureExplain(kind).empty(), "every kind has an explanation");
	}
}

} // namespace

int main() {
	TestAcceptsValidHeaders();
	TestClassifiesEachRejection();
	TestReportsTheEarliestCause();
	TestEveryKindHasNames();
	std::printf("BootFailureTests: all passed\n");
	return 0;
}

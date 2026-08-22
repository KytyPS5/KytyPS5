#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_BOOTFAILURE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_BOOTFAILURE_H_

#include "loader/elf.h"

#include <string_view>

namespace Loader {

// Why a title failed to boot, as a value rather than a log line.
//
// The shader recompiler already does this with CFG::FailureKind: it classifies
// the reason, keeps it on the object, and turns it into a name at the point of
// reporting. Game boot had no equivalent - every rejection in Elf64::IsValid()
// logged one register-level sentence and returned false, and the loader then
// died with the string "elf is not valid", which tells a reporter nothing about
// which of the eleven checks actually fired.
enum class BootFailure {
	None,
	FileNotFound,
	HeaderUnreadable,
	NotAnElf,
	NotElf64,
	NotLittleEndian,
	UnsupportedIdentVersion,
	UnsupportedOsAbi,
	UnsupportedAbiVersion,
	UnsupportedObjectType,
	UnsupportedMachine,
	UnsupportedElfVersion,
	BadProgramHeaderSize,
	BadSectionHeaderSize,
};

// Short stable name, for logs and for grepping. Mirrors FailureKindToString().
[[nodiscard]] std::string_view BootFailureToString(BootFailure kind);

// One sentence a person can act on, with no register values in it. This is what
// gets shown when boot fails, because the compatibility list is this project's
// front door and "elf is not valid" is not a bug report.
[[nodiscard]] std::string_view BootFailureExplain(BootFailure kind);

// Classifies an ELF header. Pure: no files, no loader state, no emulator - which
// is what makes every branch of it testable.
[[nodiscard]] BootFailure ClassifyElfHeader(const Elf64_Ehdr& ehdr);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_BOOTFAILURE_H_ */

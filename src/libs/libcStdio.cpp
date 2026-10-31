#include "libs/libcStdio.h"

#include "common/assert.h"
#include "common/logging/log.h"

#include <cinttypes>
#include <string>

namespace Libs::LibcStdio {

// NIDs of the FILE objects and fflush that the game's own libc.prx exports.
constexpr char NID_GUEST_STDIN[]  = "1TDo-ImqkJc"; // _Stdin
constexpr char NID_GUEST_STDOUT[] = "2sWzhYqFH4E"; // _Stdout
constexpr char NID_GUEST_STDERR[] = "H8AprKeZtNg"; // _Stderr
constexpr char NID_GUEST_FFLUSH[] = "MUjC4lbHrK4";

// NIDs of the slot variables, which a libc.prx may define itself.
constexpr char NID_STDINP[]  = "bgAcsbcEznc"; // __stdinp
constexpr char NID_STDOUTP[] = "zqJhBxAKfsc"; // __stdoutp
constexpr char NID_STDERRP[] = "as8Od-tH1BI"; // __stderrp

static GuestStdioSlots g_slots;
static GuestFflushFunc g_guest_fflush = nullptr;
static const void*     g_owner        = nullptr;

GuestStdioSlots& Slots() {
	return g_slots;
}

/// Looks up an export by NID and returns its absolute address, or 0 when it is missing.
/// Export records already include the module base address, so no rebasing is needed.
static uint64_t FindExport(const Loader::SymbolDatabase* exports, const char* nid,
                           Loader::SymbolType type) {
	const auto* record = exports != nullptr ? exports->FindByNid(nid, type) : nullptr;
	return record != nullptr ? record->vaddr : 0;
}

/// Reads the value of a slot variable that libc.prx defines itself. Returns 0 when libc.prx
/// does not define it, and sets defined to say which case it was. The value is a snapshot,
/// so a later reassignment inside libc is not mirrored. It is only meaningful once
/// relocations have run.
static uint64_t ReadDefinedSlot(const Loader::SymbolDatabase* exports, const char* slot_nid,
                                bool* defined) {
	const auto variable = FindExport(exports, slot_nid, Loader::SymbolType::Object);
	*defined            = variable != 0;
	return variable != 0 ? *reinterpret_cast<const uint64_t*>(variable) : 0;
}

/// Picks the value for one slot. A slot variable defined by libc.prx wins because the game
/// libc may read it too, and its value is copied as a snapshot. Without that definition, or
/// while it is still null, the slot points at the libc.prx FILE object.
static uint64_t ResolveSlot(const Loader::SymbolDatabase* exports, const char* slot_nid,
                            const char* file_nid, const char* label) {
	bool       defined = false;
	const auto value   = ReadDefinedSlot(exports, slot_nid, &defined);
	if (value != 0) {
		LOGF("LibcStdio: guest libc defines %s, using its value %016" PRIx64 "\n", label, value);
		return value;
	}
	if (defined) {
		LOGF("LibcStdio: guest libc defines %s but it is null at bind time, falling back to "
		     "the FILE object\n",
		     label);
	}

	const auto file = FindExport(exports, file_nid, Loader::SymbolType::Object);
	if (file == 0) {
		LOGF("LibcStdio: guest libc has no FILE object for %s [%s], leaving it null\n", label,
		     file_nid);
	}
	return file;
}

/// Re-reads one slot variable and overrides the slot when libc.prx has set it since the bind.
static void RefreshSlot(const Loader::SymbolDatabase* exports, const char* slot_nid,
                        const char* label, uint64_t* slot) {
	bool       defined = false;
	const auto value   = ReadDefinedSlot(exports, slot_nid, &defined);
	if (value != 0 && value != *slot) {
		LOGF("LibcStdio: guest libc set %s during init, using its value %016" PRIx64 "\n", label,
		     value);
		*slot = value;
	}
}

void BindGuestLibc(const Loader::SymbolDatabase* libc_exports, const void* owner) {
	g_slots.stdin_file  = ResolveSlot(libc_exports, NID_STDINP, NID_GUEST_STDIN, "__stdinp");
	g_slots.stdout_file = ResolveSlot(libc_exports, NID_STDOUTP, NID_GUEST_STDOUT, "__stdoutp");
	g_slots.stderr_file = ResolveSlot(libc_exports, NID_STDERRP, NID_GUEST_STDERR, "__stderrp");

	g_guest_fflush = reinterpret_cast<GuestFflushFunc>(
	    FindExport(libc_exports, NID_GUEST_FFLUSH, Loader::SymbolType::Func));
	if (g_guest_fflush == nullptr) {
		LOGF("LibcStdio: guest libc has no fflush [%s], leaving it null\n", NID_GUEST_FFLUSH);
	}

	g_owner = libc_exports != nullptr ? owner : nullptr;
}

void RefreshGuestLibc(const Loader::SymbolDatabase* libc_exports, const void* owner) {
	if (libc_exports == nullptr || owner != g_owner) {
		return;
	}
	RefreshSlot(libc_exports, NID_STDINP, "__stdinp", &g_slots.stdin_file);
	RefreshSlot(libc_exports, NID_STDOUTP, "__stdoutp", &g_slots.stdout_file);
	RefreshSlot(libc_exports, NID_STDERRP, "__stderrp", &g_slots.stderr_file);
}

void ResetGuestLibc(const void* owner) {
	if (owner == nullptr || owner == g_owner) {
		g_slots        = {};
		g_guest_fflush = nullptr;
		g_owner        = nullptr;
	}
}

bool HasGuestFflush() {
	return g_guest_fflush != nullptr;
}

int Fflush(FILE* stream, FILE* host_stdout) {
	if (stream != nullptr && stream == host_stdout) {
		return ::fflush(stream);
	}

	// Streams other than the host stdout are guest FILE objects that the host CRT cannot flush.
	EXIT_NOT_IMPLEMENTED(g_guest_fflush == nullptr);

	return g_guest_fflush(stream);
}

} // namespace Libs::LibcStdio

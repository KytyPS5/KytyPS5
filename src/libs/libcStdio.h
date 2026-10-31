#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBCSTDIO_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBCSTDIO_H_

#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstdio>

namespace Libs::LibcStdio {

/// Guest FILE* values behind the libc data imports __stdinp, __stdoutp and __stderrp.
/// The guest loads a slot to get its FILE*, so each slot must stay at a stable address.
struct GuestStdioSlots {
	uint64_t stdin_file  = 0;
	uint64_t stdout_file = 0;
	uint64_t stderr_file = 0;
};

using GuestFflushFunc = KYTY_SYSV_ABI int (*)(FILE*);

/// Returns the slots that the HLE libc exports as __stdinp, __stdoutp and __stderrp.
GuestStdioSlots& Slots();

/// Fills the stdio slots and the guest fflush from the export table of the game's libc.prx.
/// If libc.prx defines a slot variable itself and it is non-zero, its value is copied so that
/// definition is honoured. The copy is a snapshot and does not follow a later reassignment
/// inside libc. Otherwise the slot points at the libc.prx FILE object. Pass nullptr exports when
/// no libc.prx is loaded. Anything missing is left null and logged. The owner identifies the
/// bound module so that ResetGuestLibc can tell when that module goes away.
void BindGuestLibc(const Loader::SymbolDatabase* libc_exports, const void* owner);

/// Takes the snapshot again after libc.prx initializers have run. Any non-zero slot variable
/// value that differs from the slot replaces it. Does nothing unless
/// the owner is the bound module.
void RefreshGuestLibc(const Loader::SymbolDatabase* libc_exports, const void* owner);

/// Clears the binding when the owner is the bound module. A null owner clears it
/// unconditionally.
void ResetGuestLibc(const void* owner);

/// Reports whether a guest fflush is currently bound.
bool HasGuestFflush();

/// Flushes a stream. The host stdout is flushed on the host. Every other stream, including
/// nullptr, belongs to the guest libc and goes to its own fflush.
int Fflush(FILE* stream, FILE* host_stdout);

} // namespace Libs::LibcStdio

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBCSTDIO_H_ */

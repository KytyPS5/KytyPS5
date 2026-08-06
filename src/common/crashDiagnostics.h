#ifndef KYTY_COMMON_CRASH_DIAGNOSTICS_H_
#define KYTY_COMMON_CRASH_DIAGNOSTICS_H_

#include <cstdint>

namespace Common {

// If argv is `--kyty-exit-watch <parentPid> <outPath>`, wait for parent and write exit code.
// Returns true when this process WAS the watcher (caller must return from main immediately).
bool RunExitWatcherIfRequested(int argc, char** argv);

// Installs runtime hooks that log /GS security failures and fatal failfast before process death.
void InstallCrashDiagnostics();

// Optional keep-alive while soft-idling a guest ExitProcess (e.g. WindowArmIgnoreQuit).
using SoftIdleKeepAliveFn = void (*)(uint32_t seconds);
void SetSoftIdleKeepAlive(SoftIdleKeepAliveFn fn);

// When enabled, ACCESS_VIOLATION in guest VA ranges is logged to the fatal file (diag only).
void EnableGuestAccessViolationLogging(bool enable);

// Lightweight ring of recent HLE entries (always on). Flushed on fatal failfast /gs.
void NoteHleCall(const char* library, const char* module, const char* func);
void FlushHleRingToFatal(const char* reason);

// Last halt breadcrumb before ExitProcess(321) / FailFast / DbgExit (printed on soft-idle).
// kind examples: nested | assert | poison | guest_abort_trap | DbgExit | FailFast | ExitProcess
void NoteHaltReason(const char* kind, const char* detail = nullptr);
const char* GetLastHaltReason();

// Set when CFG-bitmap soft-continue runs; used by GPU queue_empty stall dump.
// host_tid / unique_id identify the faulting guest thread for post-CFG RIP snapshot.
void NoteCfgSoftContinue(uint32_t host_tid = 0, int unique_id = -1);
[[nodiscard]] bool CfgSoftContinueSeen();
[[nodiscard]] uint32_t CfgSoftContinueFaultHostTid();
[[nodiscard]] int      CfgSoftContinueFaultUniqueId();

} // namespace Common

#endif /* KYTY_COMMON_CRASH_DIAGNOSTICS_H_ */

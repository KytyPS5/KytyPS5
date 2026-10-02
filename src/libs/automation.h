#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_AUTOMATION_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_AUTOMATION_H_

#include "common/common.h"

#include <string>
#include <vector>

// Hooks for driving the emulator from a script (tools/autoplay). Everything here is off unless the
// emulator was started with --automation-dir <dir>; with it off, every Note* call below is a single
// relaxed atomic load and no thread or file exists.
//
// Files in <dir>:
//   commands.txt   append-only, one command per line (the emulator tails it)
//   events.jsonl   one JSON object per line: acks, shots, input resets, quit
//   status.json    heartbeat rewritten every 500 ms (tmp file + rename)
//   latest.png     most recent screenshot
//   shots/<name>.png  screenshots requested with `shot <name>`
//
// Commands (buttons: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad up down left
// right; join several with '+'):
//   press <btn>[+btn] [ms]        press for ms (default 120), then release
//   hold <btn>[+btn]              press until released
//   release <btn>[+btn]|all       release
//   stick <l|r> <x> <y> [ms]      x,y in -1..1 (y=+1 is up/forward); recentered after ms; omit ms to hold
//   trigger <l|r> <0..1> [ms]     analog trigger, released after ms (omit to hold)
//   reset                         release everything and recenter both sticks
//   shot [name]                   save a screenshot of the next presented frame
//   status                        rewrite status.json now
//   trace <on|off>                log every GPU submit
//   stall_present <seconds>       block the next present, to exercise hang detection
//   quit                          flush the log and exit immediately
namespace Libs::Graphics {
class MasterSemaphore;
}

namespace Libs::Automation {

enum class ShotFormat { Rgba8, Bgra8, A2B10G10R10, A2R10G10B10 };

// Starts the command thread. Does nothing unless Config::AutomationEnabled(). Idempotent.
void Start();

[[nodiscard]] bool Enabled();

// ---- status counters ----------------------------------------------------------------------
void NoteGuestFlip();
void NoteHostPresent();
void NotePadRead();
void NoteShaderCompile(uint64_t hash, const char* stage);
void NoteGpuSubmit(uint64_t tick, uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                   uint32_t arg2, uint32_t arg3, uint64_t arg4);
// The system overlay clears pad state when it takes over input.
void NoteInputReset(const char* reason);

// True while `trace on` is active. Callers log each GPU submit when it is.
[[nodiscard]] bool SubmitTraceEnabled();

// Command schedulers register their timeline so the heartbeat can show gpu tick vs current tick.
void RegisterGpuTicks(Graphics::MasterSemaphore* master);
void UnregisterGpuTicks(Graphics::MasterSemaphore* master);

// ---- presenter hooks ----------------------------------------------------------------------
// Names of screenshots waiting for the next presented frame. Empty when none are pending.
[[nodiscard]] std::vector<std::string> TakeScreenshotRequests();
[[nodiscard]] bool                     ScreenshotPending();
void DeliverScreenshot(std::vector<std::string> names, uint32_t width, uint32_t height,
                       ShotFormat format, std::vector<uint8_t> pixels);
void ScreenshotFailed(const std::vector<std::string>& names, const std::string& reason);
// Seconds the presenter should block, taken once. Set by `stall_present`.
[[nodiscard]] uint32_t TakePresentStallSeconds();

} // namespace Libs::Automation

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_AUTOMATION_H_ */

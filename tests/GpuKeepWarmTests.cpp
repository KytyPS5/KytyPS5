#include "graphics/host_gpu/renderer/gpuKeepWarm.h"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics::GpuKeepWarm;

constexpr uint64_t kMs = 1000ull * 1000;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "GpuKeepWarmTests: failed: %s\n", message);
		std::exit(1);
	}
}

void TestParseGroups() {
	Check(ParseGroups("0") == 0 && ParseGroups("") == 0 && ParseGroups("bogus") == 0 && ParseGroups("-1") == 0 && ParseGroups("1x") == 0,
	      "0 / empty / unknown = OFF");
	Check(ParseGroups("1") == 1 && ParseGroups("8") == 8 && ParseGroups("16") == 16, "K");
	Check(ParseGroups("17") == kMaxGroups && ParseGroups("999999") == kMaxGroups && ParseGroups("9999999") == 0, "above the maximum clamps");
}

void TestParseDispatchUs() {
	Check(ParseDispatchUs("") == kDefaultUs && ParseDispatchUs("abc") == kDefaultUs && ParseDispatchUs(".") == kDefaultUs, "default");
	Check(ParseDispatchUs("1.5") == 1500 && ParseDispatchUs("1") == 1000 && ParseDispatchUs("0.25") == 250, "ms to us");
	Check(ParseDispatchUs("2") == 2000 && ParseDispatchUs("10") == kMaxUs && ParseDispatchUs("2.5") == kMaxUs, "watchdog: at most 2 ms");
	Check(ParseDispatchUs("0.01") == kMinUs && ParseDispatchUs("0") == kMinUs, "at least 0.1 ms");
}

// ---- guard: Apple silicon host, and the same GPU as the Vulkan device ----

Luid LuidBigEndian(uint64_t id) {
	Luid luid {};
	for (size_t i = 0; i < 8; i++) luid[i] = static_cast<uint8_t>(id >> (8 * (7 - i)));
	return luid;
}

Luid LuidLittleEndian(uint64_t id) {
	Luid luid {};
	for (size_t i = 0; i < 8; i++) luid[i] = static_cast<uint8_t>(id >> (8 * i));
	return luid;
}

// The LUID that MoltenVK wrote for registry id 0x1000004d3 (Apple M3 Max, observed): big-endian. Little-endian is accepted too.
void TestLuid() {
	const Luid observed {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x04, 0xd3};
	Check(LuidIsRegistryId(observed, 0x1000004d3ull), "the observed MoltenVK LUID is the registry id");
	Check(LuidIsRegistryId(LuidBigEndian(0x1000004d3ull), 0x1000004d3ull), "big-endian");
	Check(LuidIsRegistryId(LuidLittleEndian(0x1000004d3ull), 0x1000004d3ull), "little-endian");
	Check(!LuidIsRegistryId(observed, 0x1000004d4ull) && !LuidIsRegistryId(Luid {}, 0x1000004d3ull), "another id / empty LUID");
}

void TestEligibility() {
	const DeviceInfo m3 {0x1000000aeull, "Apple M3 Max"};
	const DeviceInfo other {0x1000000bfull, "AMD Radeon Pro"};
	const DeviceInfo twin {0x1000000c0ull, "Apple M3 Max"};

	WantedDevice by_id;
	by_id.has_luid = true;
	by_id.luid     = LuidBigEndian(m3.registry_id);
	by_id.name     = m3.name;

	// Host that is not Apple silicon (Intel Mac): refused whatever the devices are.
	Check(!CheckEligibility(false, {m3}, by_id).ok && CheckEligibility(false, {m3}, by_id).reason != nullptr, "Intel host is refused");
	Check(CheckEligibility(true, {m3}, by_id).ok, "Apple silicon, the same device");
	Check(!CheckEligibility(true, {}, by_id).ok, "no Metal device");

	// Registry id picks the exact device, also when it is not the first one.
	const Eligibility second = CheckEligibility(true, {other, m3}, by_id);
	Check(second.ok && second.index == 1, "registry id selects the second device");
	// A Vulkan device that is not among the Metal devices: refused, not "the first one".
	WantedDevice unknown = by_id;
	unknown.luid         = LuidBigEndian(0x42);
	Check(!CheckEligibility(true, {m3}, unknown).ok && !CheckEligibility(true, {other, m3}, unknown).ok, "other registry id is refused");
	// Two devices with the same name: the id decides.
	const Eligibility twin_pick = CheckEligibility(true, {m3, twin}, WantedDevice {true, LuidBigEndian(twin.registry_id), twin.name});
	Check(twin_pick.ok && twin_pick.index == 1, "same name, different id");

	// No registry id from Vulkan: the name when unique, else the only device, else refused.
	WantedDevice by_name;
	by_name.name = m3.name;
	const Eligibility named = CheckEligibility(true, {other, m3}, by_name);
	Check(named.ok && named.index == 1, "unique name selects the device");
	Check(!CheckEligibility(true, {m3, twin}, by_name).ok, "ambiguous name with several GPUs is refused");
	Check(!CheckEligibility(true, {other, twin}, WantedDevice {false, {}, "Something else"}).ok, "unknown name with several GPUs is refused");
	Check(CheckEligibility(true, {m3}, WantedDevice {}).ok && CheckEligibility(true, {m3}, WantedDevice {false, {}, "renamed"}).ok,
	      "a single GPU is the one MoltenVK uses");
	Check(!CheckEligibility(true, {m3, other}, WantedDevice {}).ok, "several GPUs and no identity is refused");
}

// ---- command buffers in flight, auto-pause, resume (a model clock: the times are arguments) ----

// At least two buffers are in flight from the first fill and after every completion, while the guest is active.
void TestTwoInFlight() {
	InFlightPolicy p;
	Check(p.Fill(0) == kMinInFlight && p.InFlight() == kMinInFlight, "first fill encodes 2 buffers");
	Check(p.Fill(0) == 0, "nothing to add while 2 are in flight");
	uint64_t now = 0;
	for (int i = 0; i < 100; i++) {
		now += 2 * kMs;
		Check(p.NoteActivity(now) == 0, "activity while running adds nothing");
		Check(p.OnCompleted(now) == 1, "a completion is replaced at once");
		Check(p.InFlight() == kMinInFlight, "still 2 in flight after the replacement");
	}
	Check(!p.Paused() && p.Pauses() == 0, "an active guest never pauses it");
}

// Stop(): no buffer is encoded any more; the ones in flight drain and the policy goes idle.
void TestStop() {
	InFlightPolicy p;
	(void)p.Fill(0);
	p.Stop();
	Check(p.Stopped(), "stopped");
	Check(p.OnCompleted(1 * kMs) == 0, "no replacement after Stop");
	Check(p.Fill(1 * kMs) == 0, "no fill after Stop");
	Check(p.OnCompleted(2 * kMs) == 0 && p.Idle(), "drained: idle");
	Check(p.NoteActivity(3 * kMs) == 0, "no resume after Stop");
}

// No guest submit for kIdleNs: the spin pauses (the buffers drain); the first submit after it resumes with 2 buffers.
void TestAutoPauseAndResume() {
	{
		// The limit in absolute time: 250 ms without a submit.
		InFlightPolicy q;
		(void)q.Fill(0);
		Check(q.OnCompleted(249 * kMs) == 1 && !q.Paused(), "249 ms of silence: still spinning");
		Check(q.OnCompleted(250 * kMs) == 0 && q.Paused(), "250 ms of silence: paused");
	}
	InFlightPolicy p;
	Check(p.Fill(0) == 2, "fill");
	Check(p.OnCompleted(kIdleNs - 1) == 1 && !p.Paused(), "just under the idle limit: still replaced");
	Check(p.OnCompleted(kIdleNs) == 0 && p.Paused() && p.Pauses() == 1, "at the idle limit: paused, not replaced");
	Check(p.InFlight() == 1, "one buffer still draining");
	Check(p.OnCompleted(kIdleNs + 1 * kMs) == 0 && p.Idle(), "drained while paused");
	Check(p.NoteActivity(kIdleNs + 10 * kMs) == kMinInFlight && p.InFlight() == kMinInFlight && !p.Paused(), "the first submit resumes");
	Check(p.Resumes() == 1, "resume counted");
	Check(p.PausedNs(kIdleNs + 10 * kMs) == 10 * kMs, "pause time = from the pause to the submit");
	Check(p.NoteActivity(kIdleNs + 11 * kMs) == 0, "the second submit adds nothing");
	Check(p.OnCompleted(kIdleNs + 12 * kMs) == 1, "after the resume completions are replaced again");
	// The idle clock counts from the last activity, not from the start.
	const uint64_t t = kIdleNs + 100 * kMs;
	Check(p.NoteActivity(t) == 0, "activity");
	Check(p.OnCompleted(t + kIdleNs - 1) == 1 && !p.Paused(), "idle limit counted from the last submit");
	Check(p.OnCompleted(t + kIdleNs) == 0 && p.Paused() && p.Pauses() == 2, "paused again");
	// A pause that is still going is part of PausedNs; a stop in the pause keeps it paused and idle.
	Check(p.PausedNs(t + kIdleNs + 7 * kMs) == 10 * kMs + 7 * kMs, "open pause counted");
	p.Stop();
	Check(p.OnCompleted(t + kIdleNs + 8 * kMs) == 0 && p.Idle(), "drained");
	Check(p.NoteActivity(t + kIdleNs + 9 * kMs) == 0 && p.Idle() && p.Resumes() == 1, "no resume after Stop in a pause");
}

} // namespace

int main() {
	TestParseGroups();
	TestParseDispatchUs();
	TestLuid();
	TestEligibility();
	TestTwoInFlight();
	TestStop();
	TestAutoPauseAndResume();
	std::puts("GpuKeepWarmTests: OK");
	return 0;
}

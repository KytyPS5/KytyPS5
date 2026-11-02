// Smoke of KYTY_GPU_KEEPWARM on the real MTLDevice (opt-in, needs the GPU; do not run while another GPU benchmark is measuring).
// 1. active guest (NoteActivity every 20 ms): command buffers complete, the spin does not pause;
// 2. idle guest: the spin pauses by itself and no buffer completes any more;
// 3. the first NoteActivity resumes it; Stop() drains and nothing completes afterwards.
// Run: gpu_keepwarm_gpu_tests [K]. Exit code 2 = no eligible device (not run).
#include "graphics/host_gpu/renderer/gpuKeepWarm.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

using namespace Libs::Graphics::GpuKeepWarm;
using std::chrono::milliseconds;

int g_failed = 0;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "FAILED: %s\n", message);
		g_failed = 1;
	}
}

void Report(const char* label, const Stats& s) {
	std::printf("%-8s cb_n=%llu gpu_ns=%llu iters=%llu rate=%.4f iters/ns pauses=%llu resumes=%llu paused_ms=%.1f\n", label,
	            static_cast<unsigned long long>(s.cb_n), static_cast<unsigned long long>(s.gpu_ns), static_cast<unsigned long long>(s.iters),
	            s.gpu_ns ? static_cast<double>(s.iters) / static_cast<double>(s.gpu_ns) : 0.0, static_cast<unsigned long long>(s.pauses),
	            static_cast<unsigned long long>(s.resumes), static_cast<double>(s.paused_ns) / 1e6);
}

void ActiveFor(int ms) {
	for (int t = 0; t < ms; t += 20) {
		NoteActivity();
		std::this_thread::sleep_for(milliseconds(20));
	}
}

} // namespace

int main(int argc, char** argv) {
	const uint32_t groups = argc > 1 ? ParseGroups(argv[1]) : 1;
	std::string    reason;
	if (!Start(groups, kDefaultUs, WantedDevice {}, reason)) {
		std::fprintf(stderr, "gpu-keepwarm-gpu-tests: not started (%s)\n", reason.c_str());
		return 2;
	}
	std::printf("device=%s K=%u\n", DeviceName(), groups);

	ActiveFor(1500);
	const Stats active = GetStats();
	Report("active", active);
	Check(active.cb_n > 0 && active.gpu_ns > 0 && active.iters > 0, "no buffer completed while the guest was active");
	Check(active.pauses == 0, "paused although the guest was active");

	std::this_thread::sleep_for(milliseconds(kIdleNs / 1000000 + 400));
	const Stats paused = GetStats();
	Report("paused", paused);
	Check(paused.pauses == 1, "did not pause when idle");
	std::this_thread::sleep_for(milliseconds(200));
	Check(GetStats().cb_n == paused.cb_n, "buffers still completing during the pause");

	NoteActivity();
	std::this_thread::sleep_for(milliseconds(200));
	const Stats resumed = GetStats();
	Report("resumed", resumed);
	Check(resumed.resumes == 1, "did not resume at the first submit");
	Check(resumed.cb_n > paused.cb_n, "no buffer completed after the resume");

	Stop();
	const uint64_t after = GetStats().cb_n;
	std::this_thread::sleep_for(milliseconds(200));
	Check(GetStats().cb_n == after, "buffers completed after Stop");
	if (g_failed == 0) std::puts("gpu-keepwarm-gpu-tests: OK");
	return g_failed;
}

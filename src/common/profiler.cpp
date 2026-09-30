#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <chrono>
#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <thread>
#include <tracy/Tracy.hpp>

namespace Profiler {

void SetThreadName(const char* name) {
	if (tracy::ProfilerAvailable() && name != nullptr) {
		tracy::SetThreadName(name);
	}
}

void Initialize() {
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("KytyPS5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
}

void Shutdown() {
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

// A connected server waits for zones this failing thread never closes; bound the wait.
void EmergencyShutdown() {
	if (!tracy::ProfilerAvailable()) {
		return;
	}
	auto& profiler = tracy::GetProfiler();
	profiler.RequestShutdown();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!profiler.HasShutdownFinished() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

} // namespace Profiler

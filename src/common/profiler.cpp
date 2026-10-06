#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <string>
#include <tracy/Tracy.hpp>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <pthread.h>
#endif

namespace Profiler {

void SetThreadName(const char* name) {
	if (name == nullptr) {
		return;
	}
	// Name the OS thread too, so host tools and the performance panel can tell threads apart.
#if defined(__APPLE__)
	pthread_setname_np(name);
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	pthread_setname_np(pthread_self(), std::string(name).substr(0, 15).c_str());
#endif
	if (tracy::ProfilerAvailable()) {
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

} // namespace Profiler

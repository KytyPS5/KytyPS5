#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_REFUSALREPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_REFUSALREPORT_H_

#include "common/threads.h"
#include "kernel/memory.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics {

// Reports a recurring substitution on its first sighting and at exponentially sparser repeats.
class RefusalReporter final {
public:
	RefusalReporter() = default;
	KYTY_CLASS_NO_COPY(RefusalReporter);

	// Zero when this sighting should stay quiet; otherwise how many times it has been seen.
	[[nodiscard]] uint64_t Observe(uint64_t first, uint64_t second, uint64_t third) {
		Common::LockGuard lock(m_mutex);
		const auto        decision = m_policy.Observe(first, second, third);
		return decision.report ? decision.occurrences : 0;
	}

	// " (x12)" for a repeat, empty for a first sighting, for appending to the report line.
	static const char* RepeatSuffix(char (&storage)[32], uint64_t occurrences) {
		storage[0] = '\0';
		if (occurrences > 1) {
			std::snprintf(storage, sizeof(storage), " (x%" PRIu64 ")", occurrences);
		}
		return storage;
	}

private:
	Common::Mutex                                    m_mutex;
	Libs::LibKernel::Memory::BufferRangeReportPolicy m_policy;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_REFUSALREPORT_H_

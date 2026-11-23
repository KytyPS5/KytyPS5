#include "kernel/descriptors.h"

#include "common/assert.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace Libs::LibKernel::Descriptors {

static std::mutex        g_mutex;
static std::vector<bool> g_used;

int Allocate() {
	std::lock_guard lock(g_mutex);

	const auto free  = std::find(g_used.begin(), g_used.end(), false);
	const auto index = static_cast<int>(free - g_used.begin());
	if (free == g_used.end()) {
		g_used.push_back(true);
	} else {
		*free = true;
	}
	return index + DESCRIPTOR_MIN;
}

void Release(int d) {
	std::lock_guard lock(g_mutex);

	const auto index = static_cast<size_t>(d - DESCRIPTOR_MIN);
	EXIT_IF(index >= g_used.size() || !g_used[index]);
	g_used[index] = false;
}

} // namespace Libs::LibKernel::Descriptors

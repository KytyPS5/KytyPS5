#include "graphics/host_gpu/renderer/occlusionQueries.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace Libs::Graphics;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "OcclusionQueriesTests:%d: %s\n", __LINE__, #condition);          \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

using Results = std::array<uint64_t, OCCLUSION_QUERY_DBS * 2>;

// The samples a query passed, as ZPass predication sums them; -1 before every counter is written.
int64_t Passed(const Results& results) {
	int64_t samples = 0;
	for (uint32_t db = 0; db < OCCLUSION_QUERY_DBS; db++) {
		const auto begin = results[db * 2];
		const auto end   = results[db * 2 + 1];
		if ((begin & end & OCCLUSION_QUERY_READY) == 0) {
			return -1;
		}
		samples += static_cast<int64_t>(end - begin);
	}
	return samples;
}

void TestCounters() {
	Results results {};
	WriteZPassCounters(results.data(), 100, true);
	CHECK(Passed(results) == -1);
	WriteZPassCounters(results.data() + 1, 100, false);
	CHECK(Passed(results) == 0);
	WriteZPassCounters(results.data() + 1, 142, false);
	CHECK(Passed(results) == 42);
	CHECK(results[0] == (OCCLUSION_QUERY_READY | 100) && results[30] == OCCLUSION_QUERY_READY);

	// A begin clears the end a previous query wrote late, so the query is not ready until its own.
	WriteZPassCounters(results.data(), 142, true);
	CHECK(Passed(results) == -1);
	WriteZPassCounters(results.data() + 1, 150, false);
	CHECK(Passed(results) == 8);
}
} // namespace

int main() {
	TestCounters();
	std::puts("Occlusion query tests passed");
}

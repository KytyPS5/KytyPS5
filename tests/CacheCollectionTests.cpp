// Tests for the rule that decides what a garbage-collection pass of the buffer and texture caches
// frees.

#include "graphics/host_gpu/cacheCollection.h"

#include <iostream>

namespace {

using Libs::Graphics::CollectionAge;
using Libs::Graphics::CollectionKind;
using Libs::Graphics::CollectionState;
using Libs::Graphics::IDLE_COLLECT_AGE;
using Libs::Graphics::PlanCollection;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

constexpr uint64_t GiB = 1024ull * 1024 * 1024;

void TestOwnGpuMemoryCollectsOnlyAtTheTrigger() {
	Check(PlanCollection({.used_memory = 1 * GiB, .trigger_memory = 4 * GiB, .passes = 10'000}) ==
	          CollectionKind::None,
	      "below the trigger a GPU with its own memory keeps everything");
	Check(PlanCollection({.used_memory = 4 * GiB, .trigger_memory = 4 * GiB, .passes = 10'000}) ==
	          CollectionKind::Memory,
	      "at the trigger a GPU with its own memory collects for memory");
}

void TestSystemMemoryAlsoCollectsIdleItems() {
	Check(PlanCollection({.used_memory    = 1 * GiB,
	                      .trigger_memory = 32 * GiB,
	                      .system_memory  = true,
	                      .passes         = 10'000}) == CollectionKind::Idle,
	      "below a trigger it never reaches, a GPU on system memory still frees idle items");
	Check(PlanCollection({.used_memory    = 32 * GiB,
	                      .trigger_memory = 32 * GiB,
	                      .system_memory  = true,
	                      .passes         = 10'000}) == CollectionKind::Memory,
	      "at the trigger a GPU on system memory collects for memory as before");
}

void TestNoIdlePassBeforeAnItemCanBeIdle() {
	Check(PlanCollection({.used_memory    = 1 * GiB,
	                      .trigger_memory = 32 * GiB,
	                      .system_memory  = true,
	                      .passes         = IDLE_COLLECT_AGE - 1}) == CollectionKind::None,
	      "before the idle age has passed once, no item can have been idle for it");
	Check(PlanCollection({.used_memory    = 1 * GiB,
	                      .trigger_memory = 32 * GiB,
	                      .system_memory  = true,
	                      .passes         = IDLE_COLLECT_AGE}) == CollectionKind::Idle,
	      "from the idle age on, idle passes run");
}

void TestEachKindUsesItsOwnAge() {
	Check(CollectionAge(CollectionKind::Idle, 160, 10'000) == IDLE_COLLECT_AGE,
	      "an idle pass frees only items unused for the idle age");
	Check(CollectionAge(CollectionKind::Memory, 160, 10'000) == 160,
	      "a memory pass uses the cache's own age");
	Check(CollectionAge(CollectionKind::Memory, 160, 100) == 100,
	      "early in a run a memory pass caps its age at the passes done so far");
}

} // namespace

int main() {
	TestOwnGpuMemoryCollectsOnlyAtTheTrigger();
	TestSystemMemoryAlsoCollectsIdleItems();
	TestNoIdlePassBeforeAnItemCanBeIdle();
	TestEachKindUsesItsOwnAge();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "cache collection tests passed\n";
	return 0;
}

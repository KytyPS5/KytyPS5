#include "graphics/host_gpu/renderer/cache/memoryPressurePolicy.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

using Policy = Libs::Graphics::MemoryPressurePolicy;
using Level  = Policy::Level;

static void Require(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "MemoryPressurePolicyTests: %s\n", message);
		std::abort();
	}
}

int main() {
	Policy policy;
	Require(policy.Update(699, 1000) == Level::None, "collected below entry threshold");
	Require(policy.Update(700, 1000) == Level::Collect, "missed collect threshold");
	Require(policy.Update(800, 1000) == Level::Pressure, "missed pressure threshold");
	Require(policy.Update(900, 1000) == Level::Critical, "missed critical threshold");
	Require(policy.Update(850, 1000) == Level::Critical, "critical hysteresis boundary");
	Require(policy.Update(849, 1000) == Level::Pressure, "critical release threshold");
	Require(policy.Update(750, 1000) == Level::Pressure, "pressure hysteresis boundary");
	Require(policy.Update(749, 1000) == Level::Collect, "pressure release threshold");
	Require(policy.Update(650, 1000) == Level::Collect, "collect hysteresis boundary");
	Require(policy.Update(649, 1000) == Level::None, "collect release threshold");
	Require(policy.Update(1000, 1000) == Level::Critical, "critical jump");
	Require(policy.Update(0, 0) == Level::None, "lost budget retained stale pressure");
	Require(policy.Update(660, 1000) == Level::None, "restored budget reused stale hysteresis");
	Require(Policy::TextureAge(Level::Collect, false) == 16,
	        "fallback delayed ordinary collection");
	Require(Policy::TextureAge(Level::Pressure, false) == 80, "fallback changed pressure age");
	Require(Policy::TextureAge(Level::Critical, false) == 160, "fallback changed critical age");
	Require(Policy::TextureAge(Level::Collect, true) == 160, "live-budget retention lost");
	Require(Policy::TextureAge(Level::Pressure, true) == 80, "live-budget pressure age");
	Require(Policy::TextureAge(Level::Critical, true) == 16, "live-budget critical recovery age");
	constexpr auto maximum = std::numeric_limits<uint64_t>::max();
	Require(Policy::Percent(maximum, 90) == 16602069666338596453ull, "percentage overflow");
	Require(policy.Update(maximum, maximum) == Level::Critical, "large budget overflow");
	Require(policy.Update(maximum, 1000) == Level::Critical, "over-budget usage was ignored");
	std::puts("Memory pressure policy tests passed");
}

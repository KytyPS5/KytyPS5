// Tests for the benchmark script: key presses replayed at frames or after delays.

#include "graphics/presentation/window/benchScript.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using Libs::Graphics::BenchScript;
using Libs::Graphics::BenchStep;
using Libs::Graphics::ParseBenchScript;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

std::vector<BenchStep> Parse(const char* text) {
	std::string error;
	auto        steps = ParseBenchScript(text, error);
	Check(steps.has_value(), "a valid script parses");
	return steps.value_or(std::vector<BenchStep> {});
}

void TestParse() {
	const auto steps = Parse("# menu\n"
	                         "f2500 J down\n"
	                         "+200 J up   # release\n"
	                         "\n"
	                         "+50 Left Shift down\n"
	                         "f9000 quit\n");
	Check(steps.size() == 4, "comments and blank lines are skipped");
	if (steps.size() != 4) {
		return;
	}
	Check(steps[0].when == BenchStep::When::Frame && steps[0].value == 2500 &&
	          steps[0].key == "J" && steps[0].down,
	      "a frame step names its frame, key and action");
	Check(steps[1].when == BenchStep::When::AfterMs && steps[1].value == 200 && !steps[1].down,
	      "a delayed step names its delay");
	Check(steps[2].key == "Left Shift", "a key name may hold a space");
	Check(steps[3].quit && steps[3].value == 9000, "quit ends the run at a frame");
}

void TestParseErrors() {
	for (const char* bad: {"2500 J down", "fx J down", "f10 J press", "f10 J", "+ J down"}) {
		std::string error;
		Check(!ParseBenchScript(bad, error).has_value() && error.starts_with("line 1: "),
		      "a wrong step is refused with its line");
	}
	std::string error;
	Check(!ParseBenchScript("f1 J down\n\nf2 J sideways\n", error).has_value() &&
	          error.starts_with("line 3: "),
	      "the error names the line of the wrong step");
}

void TestFramesAndDelays() {
	BenchScript              script(Parse("f10 J down\n+100 J up\n+50 quit\n"));
	std::vector<std::string> fired;
	const auto               fire = [&](const BenchStep& step) {
        fired.push_back(step.quit ? "quit" : step.key + (step.down ? " down" : " up"));
	};
	script.Advance(5, 1000, fire);
	Check(fired.empty(), "nothing fires before its frame");
	Check(script.WaitMs(1000, 5, 1000) == 5, "a frame step is polled");
	script.Advance(12, 1010, fire);
	Check(fired.size() == 1 && fired[0] == "J down", "a frame step fires at a later frame too");
	Check(script.WaitMs(1050, 5, 1000) == 60, "the loop waits for the delay that is left");
	script.Advance(13, 1109, fire);
	Check(fired.size() == 1, "a delay counts from the step before it");
	script.Advance(13, 1110, fire);
	Check(fired.size() == 2 && fired[1] == "J up", "a delayed step fires when its delay ends");
	script.Advance(13, 2000, fire);
	Check(fired.size() == 3 && fired[2] == "quit", "quit fires like a step");
	Check(script.Done() && script.WaitMs(2000, 5, 1000) == 1000, "a finished script stops waking");
}

void TestStepsDueTogetherFireInOrder() {
	BenchScript              script(Parse("f10 J down\n+0 J up\nf10 I down\n"));
	std::vector<std::string> fired;
	script.Advance(10, 0, [&](const BenchStep& step) {
		fired.push_back(step.key + (step.down ? " down" : " up"));
	});
	Check(fired == std::vector<std::string> {"J down", "J up", "I down"},
	      "steps due at one call fire in script order");
}

} // namespace

int main() {
	TestParse();
	TestParseErrors();
	TestFramesAndDelays();
	TestStepsDueTogetherFireInOrder();
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "bench_script: all checks passed\n";
	return 0;
}

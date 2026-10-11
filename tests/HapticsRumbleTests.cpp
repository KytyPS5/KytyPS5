// Envelope math for the haptics-to-rumble conversion (no SDL needed).
#include "libs/hapticsRumble.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using namespace Libs::Controller::HapticsRumble;

void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "HapticsRumbleTests: %s\n", text);
		std::abort();
	}
}

std::vector<float> Sine(float freq, float amp, uint32_t frames, uint32_t& phase_frame) {
	std::vector<float> d(frames * 2);
	for (uint32_t i = 0; i < frames; i++, phase_frame++) {
		const float v = amp * std::sin(6.2831853f * freq * static_cast<float>(phase_frame) / 48000.0f);
		d[i * 2] = d[i * 2 + 1] = v;
	}
	return d;
}

Levels Run(Envelope& e, float freq, float amp, int buffers) {
	uint32_t phase = 0;
	Levels   l;
	for (int i = 0; i < buffers; i++) {
		const auto d = Sine(freq, amp, 512, phase);
		l            = e.Process(d.data(), 512, 2, 48000);
	}
	return l;
}
} // namespace

int main() {
	Check(Curve(0.0f) == 0.0f && Curve(0.003f) == 0.0f, "gate");
	Check(Curve(0.03f) > 0.2f && Curve(0.03f) < 0.45f, "0.03 should be faint");
	Check(Curve(0.3f) >= 0.999f && Curve(1.0f) == 1.0f, "0.3 should saturate");

	{
		Envelope e;
		const auto l = Run(e, 60.0f, 0.2f, 30);
		Check(l.low > 0.5f && l.high < 0.7f * l.low, "60 Hz should drive the large motor");
	}
	{
		Envelope e;
		const auto l = Run(e, 1500.0f, 0.2f, 30);
		Check(l.high > 0.5f && l.low < 0.7f * l.high, "1.5 kHz should drive the small motor");
	}
	{
		Envelope e;
		Run(e, 60.0f, 0.3f, 30);
		std::vector<float> silence(1024, 0.0f);
		Levels             l = e.Current();
		Check(l.low > 0.5f, "pre-decay level");
		const auto first = e.Process(silence.data(), 512, 2, 48000);
		Check(first.low > 0.0f && first.low < l.low, "decay is gradual");
		for (int i = 0; i < 60; i++) {
			l = e.Process(silence.data(), 512, 2, 48000);
		}
		Check(l.low == 0.0f && l.high == 0.0f, "silence must decay to exactly zero");
	}
	{
		Envelope e;
		const auto l = Run(e, 60.0f, 0.002f, 30);
		Check(l.low == 0.0f && l.high == 0.0f, "below the gate stays silent");
	}
	Check(ToMotor16(1.0f, 1.0f) == 65535 && ToMotor16(1.0f, 0.0f) == 0 && ToMotor16(0.5f, 2.0f) == 65535,
	      "motor scaling");
	std::puts("HapticsRumbleTests: ok");
	return 0;
}

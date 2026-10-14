#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_WINDOW_BENCHSCRIPT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_WINDOW_BENCHSCRIPT_H_

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// A benchmark script (--bench-script): key presses replayed without a window in front, so a
// measured run takes the same route every time, also while the screen is locked.
//
// One step per line; '#' starts a comment:
//   f<frame> <key> down|up   at the first presented frame >= <frame>
//   +<ms> <key> down|up      <ms> milliseconds after the step before it
//   f<frame> quit / +<ms> quit   close the emulator
// <key> is an SDL key name ("J", "W", "Left Shift"), so the keyboard mapping applies.
struct BenchStep {
	enum class When { Frame, AfterMs };
	When        when  = When::Frame;
	uint64_t    value = 0;
	std::string key;
	bool        down = false;
	bool        quit = false;
};

namespace BenchScriptDetail {

inline std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
		s.remove_suffix(1);
	}
	return s;
}

inline bool ParseNumber(std::string_view s, uint64_t& value) {
	const auto* end    = s.data() + s.size();
	const auto  result = std::from_chars(s.data(), end, value);
	return !s.empty() && result.ec == std::errc {} && result.ptr == end;
}

} // namespace BenchScriptDetail

// The steps of a script, or nullopt with the line that is wrong in `error`.
inline std::optional<std::vector<BenchStep>> ParseBenchScript(std::string_view text,
                                                              std::string&     error) {
	using BenchScriptDetail::ParseNumber;
	using BenchScriptDetail::Trim;
	std::vector<BenchStep> steps;
	uint32_t               line_number = 0;
	while (!text.empty()) {
		const auto       eol  = text.find('\n');
		std::string_view line = text.substr(0, eol);
		text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
		line_number++;
		if (const auto hash = line.find('#'); hash != std::string_view::npos) {
			line = line.substr(0, hash);
		}
		line = Trim(line);
		if (line.empty()) {
			continue;
		}
		const auto fail = [&](const char* what) {
			error = "line " + std::to_string(line_number) + ": " + what;
			return std::nullopt;
		};
		const auto when_end = line.find_first_of(" \t");
		const auto when     = line.substr(0, when_end);
		BenchStep  step;
		if (when.size() > 1 && when[0] == 'f') {
			step.when = BenchStep::When::Frame;
		} else if (when.size() > 1 && when[0] == '+') {
			step.when = BenchStep::When::AfterMs;
		} else {
			return fail("a step starts with f<frame> or +<ms>");
		}
		if (!ParseNumber(when.substr(1), step.value)) {
			return fail("the frame or delay is not a number");
		}
		auto rest =
		    when_end == std::string_view::npos ? std::string_view {} : Trim(line.substr(when_end));
		if (rest == "quit") {
			step.quit = true;
			steps.push_back(std::move(step));
			continue;
		}
		const auto action_start = rest.find_last_of(" \t");
		if (action_start == std::string_view::npos) {
			return fail("a step names a key and down or up, or quit");
		}
		const auto action = rest.substr(action_start + 1);
		if (action == "down") {
			step.down = true;
		} else if (action != "up") {
			return fail("the action is not down or up");
		}
		step.key = std::string(Trim(rest.substr(0, action_start)));
		steps.push_back(std::move(step));
	}
	return steps;
}

// Which steps are due. The window loop calls Advance with the presented frame count and a
// millisecond clock, and waits no longer than WaitMs between calls.
class BenchScript {
public:
	explicit BenchScript(std::vector<BenchStep> steps): m_steps(std::move(steps)) {}

	// Runs `fire` for each due step, in order. A delay counts from the time the step before it
	// fired; the first delayed step counts from the first call.
	template <typename Fire>
	void Advance(uint64_t frame, uint64_t now_ms, Fire&& fire) {
		if (!m_started) {
			m_started = true;
			m_last_ms = now_ms;
		}
		while (m_next < m_steps.size()) {
			const auto& step = m_steps[m_next];
			const bool  due  = step.when == BenchStep::When::Frame ? frame >= step.value
			                                                       : now_ms >= m_last_ms + step.value;
			if (!due) {
				return;
			}
			m_last_ms = now_ms;
			m_next++;
			fire(step);
		}
	}

	[[nodiscard]] bool Done() const { return m_next >= m_steps.size(); }

	// How long the loop may wait before the next step can be due: the delay left for a delayed
	// step, `frame_poll_ms` for a step that waits for a frame, `max_ms` when the script is done.
	[[nodiscard]] int WaitMs(uint64_t now_ms, int frame_poll_ms, int max_ms) const {
		if (Done()) {
			return max_ms;
		}
		const auto& step = m_steps[m_next];
		if (step.when == BenchStep::When::Frame || !m_started) {
			return std::min(frame_poll_ms, max_ms);
		}
		const auto due = m_last_ms + step.value;
		return due <= now_ms ? 0 : static_cast<int>(std::min<uint64_t>(due - now_ms, max_ms));
	}

private:
	std::vector<BenchStep> m_steps;
	size_t                 m_next    = 0;
	uint64_t               m_last_ms = 0;
	bool                   m_started = false;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_PRESENTATION_WINDOW_BENCHSCRIPT_H_ */

#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_GAMEPADQUIT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_GAMEPADQUIT_H_

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

// The close holds of a gamepad, for a player away from the keyboard. A short press of the PS
// (Guide) button, which the game never reads, seats the pad like any first press (playerSeats.h).
// A hold of the PS button, or of both triggers (L2 + R2), closes the game when the pad is player
// 1's. The triggers still reach the game. They serve the pads that keep their PS button to
// themselves: a Logitech Cordless RumblePad 2 sends nothing for its Mode and Vibration buttons.

namespace Libs::Graphics {

inline constexpr uint64_t QUIT_HOLD_MS = 2000;

// A trigger counts as pressed from half of SDL's trigger range (0 to 32767).
constexpr bool TriggerPressed(int value) {
	return value >= 16384;
}

class GamepadQuitHold {
public:
	// The PS button of gamepad `pad` went down or up at `now_ms`. Returns true when a short press
	// ended: the button came up before a hold closed the game.
	bool Press(int pad, bool down, uint64_t now_ms) {
		auto& state = Get(pad);
		if (down) {
			if (!state.ps_since) {
				state.ps_since = now_ms;
			}
			return false;
		}
		const bool short_press = state.ps_since && now_ms - *state.ps_since < QUIT_HOLD_MS;
		state.ps_since.reset();
		return short_press;
	}

	// The left (`right` false) or right trigger of gamepad `pad` is pressed or not at `now_ms`
	// (TriggerPressed). The hold counts from the moment both are pressed.
	void Trigger(int pad, bool right, bool pressed, uint64_t now_ms) {
		auto& state                   = Get(pad);
		(right ? state.r2 : state.l2) = pressed;
		if (!state.l2 || !state.r2) {
			state.triggers_since.reset();
		} else if (!state.triggers_since) {
			state.triggers_since = now_ms;
		}
	}

	// The gamepad left: what it held is forgotten.
	void Forget(int pad) {
		std::erase_if(m_pads, [pad](const State& state) { return state.pad == pad; });
	}

	// Milliseconds left until a hold of gamepad `closer` closes the game (0: close now), or none
	// when it holds nothing. The holds of other pads never close the game.
	[[nodiscard]] std::optional<uint64_t> Remaining(uint64_t now_ms, int closer) const {
		const auto it = std::ranges::find_if(
		    m_pads, [closer](const State& state) { return state.pad == closer; });
		if (it == m_pads.end()) {
			return std::nullopt;
		}
		std::optional<uint64_t> left;
		for (const auto& since: {it->ps_since, it->triggers_since}) {
			if (since) {
				const auto elapsed   = now_ms > *since ? now_ms - *since : 0;
				const auto hold_left = elapsed < QUIT_HOLD_MS ? QUIT_HOLD_MS - elapsed : 0;
				left                 = left ? std::min(*left, hold_left) : hold_left;
			}
		}
		return left;
	}

private:
	struct State {
		int                     pad = 0;
		std::optional<uint64_t> ps_since;
		bool                    l2 = false;
		bool                    r2 = false;
		std::optional<uint64_t> triggers_since;
	};

	State& Get(int pad) {
		const auto it =
		    std::ranges::find_if(m_pads, [pad](const State& state) { return state.pad == pad; });
		return it != m_pads.end() ? *it : m_pads.emplace_back(State {.pad = pad});
	}

	std::vector<State> m_pads;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_GAMEPADQUIT_H_ */

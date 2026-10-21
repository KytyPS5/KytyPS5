#ifndef EMULATOR_SRC_LIBS_PLAYERSEATS_H_
#define EMULATOR_SRC_LIBS_PLAYERSEATS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace Libs::Controller {

// Who plays as which player: up to four players, each with one user and one gamepad.
//
// A gamepad takes the lowest free seat at its first button press, so the first pad pressed is
// player 1 and the next is player 2. It keeps the seat until it disconnects. A pad without a seat
// plays as nobody: a copy of a pad that the host reports twice never presses, and stays out.
// Player 1 is logged in from the start and stays logged in, also without a pad; the keyboard
// plays as player 1. Players 2 to 4 log in with their pad's seat and log out when it frees.
//
// The keyboard can instead have a seat of its own (SetKeyboard): its player is logged in while it
// holds the seat, and pads take the other seats.
class PlayerSeats final {
public:
	static constexpr int MAX_PLAYERS = 4;
	static constexpr int NO_PLAYER   = -1;
	static constexpr int NO_PAD      = 0; // never a host gamepad id
	// The keyboard shares player 1 with player 1's pad.
	static constexpr int KEYBOARD_SHARED = NO_PLAYER;
	// Logins and logouts the game has not read yet, at most; the oldest go first.
	static constexpr size_t MAX_EVENTS = 64;

	struct Event {
		bool login  = false;
		int  player = NO_PLAYER;
	};

	// A pad that SetKeyboard moved out of the keyboard's new seat: to the lowest free seat, or to
	// none (NO_PLAYER) until it presses again with a seat free.
	struct Move {
		int pad  = NO_PAD;
		int from = NO_PLAYER;
		int to   = NO_PLAYER;
	};

	// `users`: the user ids of players 1 to 4 (Config::GetPlayerUserId), which keep the saves and
	// trophies of each player apart.
	explicit PlayerSeats(const std::array<int32_t, MAX_PLAYERS>& users): m_users(users) {
		Push({.login = true, .player = 0});
	}

	// A button press of `pad`. Returns its player: its seat, a new seat, or NO_PLAYER when all
	// seats are taken.
	int Seat(int pad) {
		if (const int player = PlayerOf(pad); player != NO_PLAYER || pad == NO_PAD) {
			return player;
		}
		const int player = FreeSeat();
		if (player != NO_PLAYER) {
			const auto before = LoggedIn();
			m_pads[player]    = pad;
			PushChanges(before);
		}
		return player;
	}

	// The keyboard takes the seat of `player`, or shares player 1 (KEYBOARD_SHARED). A pad in that
	// seat moves (Move).
	Move SetKeyboard(int player) {
		const auto before = LoggedIn();
		m_keyboard        = player;
		Move move;
		if (player != KEYBOARD_SHARED && m_pads[player] != NO_PAD) {
			move.pad       = m_pads[player];
			move.from      = player;
			m_pads[player] = NO_PAD;
			move.to        = FreeSeat();
			if (move.to != NO_PLAYER) {
				m_pads[move.to] = move.pad;
			}
		}
		PushChanges(before);
		return move;
	}

	// The keyboard's seat, or KEYBOARD_SHARED.
	[[nodiscard]] int Keyboard() const { return m_keyboard; }
	// The player the keyboard plays as.
	[[nodiscard]] int KeyboardPlayer() const { return m_keyboard == KEYBOARD_SHARED ? 0 : m_keyboard; }

	// `pad` disconnected. Returns the player whose seat it freed, or NO_PLAYER when the pad had no
	// seat. That player logs out, unless it is player 1.
	int Leave(int pad) {
		const int player = PlayerOf(pad);
		if (player == NO_PLAYER) {
			return NO_PLAYER;
		}
		const auto before = LoggedIn();
		m_pads[player]    = NO_PAD;
		PushChanges(before);
		return player;
	}

	// The player `pad` plays as, or NO_PLAYER for a pad without a seat.
	[[nodiscard]] int PlayerOf(int pad) const {
		for (int player = 0; player < MAX_PLAYERS; ++player) {
			if (pad != NO_PAD && m_pads[player] == pad) {
				return player;
			}
		}
		return NO_PLAYER;
	}

	// The pad in the seat of `player`, or NO_PAD.
	[[nodiscard]] int PadOf(int player) const { return m_pads[player]; }

	[[nodiscard]] bool IsLoggedIn(int player) const {
		return player == 0 ||
		       (player > 0 && player < MAX_PLAYERS && (m_pads[player] != NO_PAD || m_keyboard == player));
	}

	[[nodiscard]] int32_t UserOf(int player) const { return m_users[player]; }

	// The player of a logged-in user, or NO_PLAYER.
	[[nodiscard]] int PlayerOfUser(int32_t user) const {
		for (int player = 0; player < MAX_PLAYERS; ++player) {
			if (m_users[player] == user && IsLoggedIn(player)) {
				return player;
			}
		}
		return NO_PLAYER;
	}

	// The oldest login or logout the game has not seen. The first is the login of player 1.
	bool TakeEvent(Event* event) {
		if (m_events.empty()) {
			return false;
		}
		*event = m_events.front();
		m_events.pop_front();
		return true;
	}

private:
	[[nodiscard]] int FreeSeat() const {
		for (int player = 0; player < MAX_PLAYERS; ++player) {
			if (m_pads[player] == NO_PAD && player != m_keyboard) {
				return player;
			}
		}
		return NO_PLAYER;
	}

	[[nodiscard]] std::array<bool, MAX_PLAYERS> LoggedIn() const {
		std::array<bool, MAX_PLAYERS> logged_in {};
		for (int player = 0; player < MAX_PLAYERS; ++player) {
			logged_in[player] = IsLoggedIn(player);
		}
		return logged_in;
	}

	void PushChanges(const std::array<bool, MAX_PLAYERS>& before) {
		for (int player = 0; player < MAX_PLAYERS; ++player) {
			if (const bool now = IsLoggedIn(player); now != before[player]) {
				Push({.login = now, .player = player});
			}
		}
	}

	void Push(Event event) {
		if (m_events.size() == MAX_EVENTS) {
			m_events.pop_front();
		}
		m_events.push_back(event);
	}

	std::array<int, MAX_PLAYERS>     m_pads {};
	int                              m_keyboard = KEYBOARD_SHARED;
	std::array<int32_t, MAX_PLAYERS> m_users {};
	std::deque<Event>                m_events;
};

} // namespace Libs::Controller

#endif /* EMULATOR_SRC_LIBS_PLAYERSEATS_H_ */

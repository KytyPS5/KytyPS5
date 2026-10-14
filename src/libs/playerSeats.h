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
class PlayerSeats final {
public:
	static constexpr int MAX_PLAYERS = 4;
	static constexpr int NO_PLAYER   = -1;
	static constexpr int NO_PAD      = 0; // never a host gamepad id
	// Logins and logouts the game has not read yet, at most; the oldest go first.
	static constexpr size_t MAX_EVENTS = 64;

	struct Event {
		bool login  = false;
		int  player = NO_PLAYER;
	};

	// `first_user`: the user id of player 1. The other players get the ids that follow it, without
	// the two ids the system keeps for itself.
	explicit PlayerSeats(int32_t first_user) {
		int32_t user = first_user;
		for (auto& id: m_users) {
			while (user == USER_EVERYONE || user == USER_SYSTEM) {
				++user;
			}
			id = user++;
		}
		Push({.login = true, .player = 0});
	}

	// A button press of `pad`. Returns its player: its seat, a new seat, or NO_PLAYER when all
	// seats are taken.
	int Seat(int pad) {
		if (const int player = PlayerOf(pad); player != NO_PLAYER || pad == NO_PAD) {
			return player;
		}
		for (int player = 0; player < MAX_PLAYERS; ++player) {
			if (m_pads[player] == NO_PAD) {
				m_pads[player] = pad;
				if (player != 0) {
					Push({.login = true, .player = player});
				}
				return player;
			}
		}
		return NO_PLAYER;
	}

	// `pad` disconnected. Returns the player whose seat it freed, or NO_PLAYER when the pad had no
	// seat. That player logs out, unless it is player 1.
	int Leave(int pad) {
		const int player = PlayerOf(pad);
		if (player == NO_PLAYER) {
			return NO_PLAYER;
		}
		m_pads[player] = NO_PAD;
		if (player != 0) {
			Push({.login = false, .player = player});
		}
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
		return player == 0 || (player > 0 && player < MAX_PLAYERS && m_pads[player] != NO_PAD);
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
	void Push(Event event) {
		if (m_events.size() == MAX_EVENTS) {
			m_events.pop_front();
		}
		m_events.push_back(event);
	}

	static constexpr int32_t USER_EVERYONE = 0xfe;
	static constexpr int32_t USER_SYSTEM   = 0xff;

	std::array<int, MAX_PLAYERS>     m_pads {};
	std::array<int32_t, MAX_PLAYERS> m_users {};
	std::deque<Event>                m_events;
};

} // namespace Libs::Controller

#endif /* EMULATOR_SRC_LIBS_PLAYERSEATS_H_ */

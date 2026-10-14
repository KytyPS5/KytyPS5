// Who plays as which player: the seats of up to four players, and the logins the game sees.
#include "libs/playerSeats.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Controller::PlayerSeats;

constexpr int FIRST_USER = 0x10000;

void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "PlayerSeatsTests: %s\n", text);
		std::abort();
	}
}

void DrainEvents(PlayerSeats& seats) {
	PlayerSeats::Event event;
	while (seats.TakeEvent(&event)) {
	}
}

void TestPlayerOneIsLoggedInFromTheStart() {
	PlayerSeats        seats(FIRST_USER);
	PlayerSeats::Event event;
	Check(seats.TakeEvent(&event) && event.login && event.player == 0,
	      "player 1 did not log in first");
	Check(!seats.TakeEvent(&event), "a login came without a seated pad");
	Check(seats.IsLoggedIn(0) && !seats.IsLoggedIn(1) && seats.PlayerOfUser(FIRST_USER) == 0,
	      "player 1 is not the only logged-in player");
}

void TestAPadWithoutASeatPlaysAsNobody() {
	PlayerSeats seats(FIRST_USER);
	Check(seats.PlayerOf(7) == PlayerSeats::NO_PLAYER, "a pad without a seat plays as a player");
}

// The first pad that presses a button is player 1, the next is player 2.
void TestPadsTakeSeatsInTheOrderTheyPress() {
	PlayerSeats seats(FIRST_USER);
	DrainEvents(seats);
	PlayerSeats::Event event;
	Check(seats.Seat(7) == 0 && seats.PlayerOf(7) == 0 && !seats.TakeEvent(&event),
	      "the first pad is not player 1, or logged player 1 in again");
	Check(seats.Seat(8) == 1 && seats.PlayerOf(8) == 1 && seats.PadOf(1) == 8,
	      "the second pad is not player 2");
	Check(seats.TakeEvent(&event) && event.login && event.player == 1, "player 2 did not log in");
	Check(seats.Seat(8) == 1 && !seats.TakeEvent(&event), "a seated pad was seated again");
	Check(seats.Seat(9) == 2 && seats.Seat(10) == 3, "the next pads did not take players 3 and 4");
	Check(seats.Seat(11) == PlayerSeats::NO_PLAYER && seats.PlayerOf(11) == PlayerSeats::NO_PLAYER,
	      "a pad past four players took a seat");
}

void TestALeavingPadFreesItsSeat() {
	PlayerSeats seats(FIRST_USER);
	(void)seats.Seat(7);
	(void)seats.Seat(8);
	DrainEvents(seats);
	PlayerSeats::Event event;
	Check(seats.Leave(8) == 1 && !seats.IsLoggedIn(1) &&
	          seats.PlayerOf(8) == PlayerSeats::NO_PLAYER,
	      "the pad of player 2 did not free its seat");
	Check(seats.TakeEvent(&event) && !event.login && event.player == 1, "player 2 did not log out");
	Check(seats.Leave(7) == 0 && seats.IsLoggedIn(0) && !seats.TakeEvent(&event),
	      "player 1 logged out with its pad");
	Check(seats.Seat(9) == 0, "the free seat of player 1 was not taken first");
	Check(seats.Leave(42) == PlayerSeats::NO_PLAYER, "an unknown pad freed a seat");
}

void TestUsersSkipTheSystemIds() {
	PlayerSeats seats(0xfd);
	Check(seats.UserOf(0) == 0xfd && seats.UserOf(1) == 0x100 && seats.UserOf(2) == 0x101,
	      "a player got a user id the system keeps");
	Check(seats.PlayerOfUser(0x100) == PlayerSeats::NO_PLAYER,
	      "a user without a seated pad is logged in");
}

// A game that never reads the logins does not make the seats grow without end: the oldest are
// dropped.
void TestUnreadEventsAreBounded() {
	PlayerSeats seats(FIRST_USER);
	(void)seats.Seat(6);
	for (int i = 0; i < 1000; ++i) {
		(void)seats.Seat(7);
		(void)seats.Leave(7);
	}
	int                count = 0;
	PlayerSeats::Event event;
	PlayerSeats::Event last;
	while (seats.TakeEvent(&event)) {
		last = event;
		++count;
	}
	Check(count <= static_cast<int>(PlayerSeats::MAX_EVENTS) && !last.login && last.player == 1,
	      "unread logins grew without end or lost the newest");
}

} // namespace

int main() {
	TestPlayerOneIsLoggedInFromTheStart();
	TestAPadWithoutASeatPlaysAsNobody();
	TestPadsTakeSeatsInTheOrderTheyPress();
	TestALeavingPadFreesItsSeat();
	TestUsersSkipTheSystemIds();
	TestUnreadEventsAreBounded();
	std::puts("PlayerSeatsTests: passed");
	return 0;
}

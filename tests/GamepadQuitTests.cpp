// The close holds of a gamepad: a short PS press logs a player in; a long hold of the PS button, or
// of both triggers (L2 + R2), on player 1's pad closes the game.
#include "graphics/presentation/window/gamepadQuit.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Libs::Graphics::GamepadQuitHold;
using Libs::Graphics::QUIT_HOLD_MS;
using Libs::Graphics::TriggerPressed;

constexpr int NO_PAD = 0;

void Check(bool condition, const char* text) {
	if (!condition) {
		std::fprintf(stderr, "GamepadQuitTests: %s\n", text);
		std::abort();
	}
}

void TestAHoldClosesTheGame() {
	GamepadQuitHold hold;
	Check(!hold.Remaining(0, 7), "a hold runs before a press");
	Check(!hold.Press(7, true, 1000), "a press counted as a short press before its release");
	Check(hold.Remaining(1000, 7) == QUIT_HOLD_MS && hold.Remaining(1500, 7) == QUIT_HOLD_MS - 500,
	      "the hold does not count from the press");
	Check(hold.Remaining(1000 + QUIT_HOLD_MS, 7) == 0u, "a full hold does not close");
	Check(!hold.Press(7, false, 1000 + QUIT_HOLD_MS), "a full hold also counted as a short press");
}

void TestAShortPressIsNotAHold() {
	GamepadQuitHold hold;
	(void)hold.Press(7, true, 0);
	Check(hold.Press(7, false, QUIT_HOLD_MS - 1), "a short press was not reported");
	Check(!hold.Remaining(10 * QUIT_HOLD_MS, 7), "a hold went on after the release");
}

void TestEachPadHoldsItsOwn() {
	GamepadQuitHold hold;
	(void)hold.Press(7, true, 0);
	(void)hold.Press(8, true, 500);
	Check(hold.Press(7, false, 600), "the release of one pad was not its own");
	Check(hold.Remaining(600, 8) == QUIT_HOLD_MS - 100, "the other pad lost its hold");
}

void TestARemovedPadIsForgotten() {
	GamepadQuitHold hold;
	(void)hold.Press(7, true, 0);
	hold.Trigger(7, false, true, 0);
	hold.Trigger(7, true, true, 0);
	hold.Forget(7);
	Check(!hold.Remaining(QUIT_HOLD_MS, 7), "a removed pad's hold closed the game");
	Check(!hold.Press(7, false, 100), "a release after the removal counted as a short press");
	hold.Trigger(7, false, true, 200);
	Check(!hold.Remaining(QUIT_HOLD_MS + 200, 7), "a removed pad kept its other trigger");
}

// Only the pad of player 1 closes the game: the hold of another pad, or of no pad, does not.
void TestOnlyTheCloserPadCloses() {
	GamepadQuitHold hold;
	(void)hold.Press(8, true, 0);
	hold.Trigger(9, false, true, 0);
	hold.Trigger(9, true, true, 0);
	Check(!hold.Remaining(QUIT_HOLD_MS, 7), "the hold of another pad closed the game");
	Check(!hold.Remaining(QUIT_HOLD_MS, NO_PAD), "a hold closed the game without a player 1 pad");
	Check(hold.Remaining(QUIT_HOLD_MS, 8) == 0u && hold.Remaining(QUIT_HOLD_MS, 9) == 0u,
	      "a pad lost its hold when another pad was player 1");
}

// A pad without a PS button holds both triggers; the hold counts from the second press.
void TestBothTriggersCloseTheGame() {
	GamepadQuitHold hold;
	hold.Trigger(7, false, true, 0);
	Check(!hold.Remaining(10 * QUIT_HOLD_MS, 7), "one trigger started a hold");
	hold.Trigger(7, true, true, 300);
	Check(hold.Remaining(300, 7) == QUIT_HOLD_MS, "the hold does not count from the second press");
	hold.Trigger(7, true, true, 900);
	Check(hold.Remaining(900, 7) == QUIT_HOLD_MS - 600, "a repeated press restarted the hold");
	Check(hold.Remaining(300 + QUIT_HOLD_MS, 7) == 0u,
	      "a full hold of both triggers does not close");
}

void TestAReleasedTriggerStopsTheHold() {
	GamepadQuitHold hold;
	hold.Trigger(7, false, true, 0);
	hold.Trigger(7, true, true, 0);
	hold.Trigger(7, false, false, 500);
	Check(!hold.Remaining(QUIT_HOLD_MS, 7), "the hold went on after one trigger came up");
	hold.Trigger(7, false, true, 1000);
	Check(hold.Remaining(1000, 7) == QUIT_HOLD_MS, "a new press did not start a new hold");
}

// Triggers of two pads never make one hold.
void TestTriggersOfTwoPadsDoNotMix() {
	GamepadQuitHold hold;
	hold.Trigger(7, false, true, 0);
	hold.Trigger(8, true, true, 0);
	Check(!hold.Remaining(QUIT_HOLD_MS, 7) && !hold.Remaining(QUIT_HOLD_MS, 8),
	      "the triggers of two pads made a hold");
}

// The PS hold and the trigger hold of one pad run apart: the end of one leaves the other.
void TestThePsAndTriggerHoldsAreApart() {
	GamepadQuitHold hold;
	(void)hold.Press(7, true, 0);
	hold.Trigger(7, false, true, 500);
	hold.Trigger(7, true, true, 500);
	Check(hold.Remaining(500, 7) == QUIT_HOLD_MS - 500, "the earlier hold does not count first");
	Check(hold.Press(7, false, 600), "a short PS press was lost to the trigger hold");
	Check(hold.Remaining(600, 7) == QUIT_HOLD_MS - 100, "the PS release ended the trigger hold");
	hold.Trigger(7, true, false, 700);
	(void)hold.Press(7, true, 800);
	Check(hold.Remaining(800, 7) == QUIT_HOLD_MS, "a trigger release ended the PS hold");
}

// A trigger counts as pressed from half of SDL's range (0 to 32767).
void TestATriggerIsPressedFromHalfway() {
	Check(!TriggerPressed(0) && !TriggerPressed(16383), "a trigger below half counted as pressed");
	Check(TriggerPressed(16384) && TriggerPressed(32767), "a trigger from half did not count");
}

} // namespace

int main() {
	TestAHoldClosesTheGame();
	TestAShortPressIsNotAHold();
	TestEachPadHoldsItsOwn();
	TestARemovedPadIsForgotten();
	TestOnlyTheCloserPadCloses();
	TestBothTriggersCloseTheGame();
	TestAReleasedTriggerStopsTheHold();
	TestTriggersOfTwoPadsDoNotMix();
	TestThePsAndTriggerHoldsAreApart();
	TestATriggerIsPressedFromHalfway();
	std::puts("GamepadQuitTests: passed");
	return 0;
}

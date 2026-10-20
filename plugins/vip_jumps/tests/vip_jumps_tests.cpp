// vip_jumps unit tests — the engine-independent part: the per-tick jump state machine, including
// the fix for the tick where the engine has already launched but FL_ONGROUND is still
// set (mouse-wheel spam must not gain an extra jump).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "jump_logic.hpp"

#include <cstdio>
#include <string>

namespace {

using vip_jumps::kFlagOnGround;
using vip_jumps::kJumpMask;
using vip_jumps::TickUser;
using vip_jumps::User;

constexpr int kAir = 0; // no FL_ONGROUND

int g_checks = 0;
int g_failures = 0;

void CheckEq(bool actual, bool expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected ? 1 : 0, actual ? 1 : 0);
	}
}

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

// A user with the given total number of jumps.
User MakeUser(int jumps) {
	User user;
	user.numberOfJumps = jumps;
	return user;
}

void TestGroundReset() {
	User user = MakeUser(2);
	user.jumpsCount = 1;
	user.jumpReleased = true;
	TickUser(user, kFlagOnGround, 0, 0.0f, 1);
	CheckEq(user.jumpsCount, 0, "on the ground the counter resets");
	CheckEq(user.jumpReleased, false, "on the ground the release flag resets");
}

void TestSingleJumpByDefault() {
	// jumps = 1: no extra jump at all.
	User user = MakeUser(1);
	const bool boost = TickUser(user, kAir, kJumpMask, -100.0f, 0);
	CheckEq(boost, false, "1 jump in the feature = no extra jump");
}

void TestExtraJump() {
	// jumps = 2: one extra jump per airtime.
	User user = MakeUser(2);
	user.jumpReleased = false;
	user.lastButtons = kJumpMask; // the player is still holding the first jump
	CheckEq(TickUser(user, kAir, kJumpMask, -50.0f, 1), false, "holding the key does not boost");
	CheckEq(TickUser(user, kAir, 0, -50.0f, 1), false, "releasing arms the next press");
	CheckEq(TickUser(user, kAir, kJumpMask, -20.0f, 1), true, "the next press boosts");
	CheckEq(user.jumpsCount, 1, "one extra jump consumed");
	CheckEq(TickUser(user, kAir, 0, 10.0f, 1), false, "no second boost while airborne");
	CheckEq(TickUser(user, kAir, kJumpMask, 10.0f, 1), false, "the limit is enforced");

	// Landing resets everything.
	TickUser(user, kFlagOnGround, kJumpMask, 0.0f, 1);
	CheckEq(user.jumpsCount, 0, "landing resets the counter");
}

void TestLaunchWindow() {
	// The fix: the tick where the engine launched the jump (velocity up) but FL_ONGROUND is still
	// set must not reset the counter, otherwise holding jumps at the limit gains an extra one.
	User user = MakeUser(2);
	user.jumpsCount = 1; // the extra jump of this airtime is used up
	user.jumpReleased = true;
	const bool boost = TickUser(user, kFlagOnGround, kJumpMask, 25.0f /* already launched */, 1);
	CheckEq(boost, false, "no extra jump in the launch window");
	CheckEq(user.jumpsCount, 1, "the counter survives the launch window");

	// With the wheel (two quick presses, positive velocity), the same tick must not give a jump.
	User wheel = MakeUser(2);
	wheel.jumpsCount = 1;
	wheel.jumpReleased = true;
	CheckEq(TickUser(wheel, kFlagOnGround, kJumpMask, 5.0f, 1), false, "wheel spam stays limited");
}

void TestFallingNotBoostedOnGround() {
	// Standing on the ground with zero velocity and a fresh press: the ground branch runs, no boost.
	User user = MakeUser(3);
	CheckEq(TickUser(user, kFlagOnGround, kJumpMask, 0.0f, 2), false, "the first jump is the engine's");
	CheckEq(user.jumpsCount, 0, "the counter stays reset on the ground");
}

} // namespace

int main() {
	std::printf("vip_jumps tests\n");
	TestGroundReset();
	TestSingleJumpByDefault();
	TestExtraJump();
	TestLaunchWindow();
	TestFallingNotBoostedOnGround();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}

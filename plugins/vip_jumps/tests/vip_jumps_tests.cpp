// vip_jumps unit tests — the engine-independent part of the plugin:
//
//   * the per-tick jump state machine, including the fix for the tick where the engine has already
//     launched but FL_ONGROUND is still set (mouse-wheel spam must not gain an extra jump);
//   * the two pure predicates the plugin's C2 client guard and timer body use: IsSlotInRange()
//     (0 <= slot < 64) and AllowedExtra() (max(0, jumps - 1) - the "jumps" feature counts the
//     engine's own first jump).
//
// No engine and no Plugify host: only the plugin's own jump_logic.hpp is compiled in, so this test
// has no pisexcompat dependency (docs/PLUGIN_STANDARD.md §7).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "jump_logic.hpp"

#include <cstdio>
#include <limits>
#include <string>

namespace {

using vip_jumps::AllowedExtra;
using vip_jumps::IsSlotInRange;
using vip_jumps::kFlagOnGround;
using vip_jumps::kJumpMask;
using vip_jumps::kMaxSlots;
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

// --- the pure C2 predicates -------------------------------------------------------------------

void TestSlotRange() {
	// The plugin calls IsAlive() for every slot of the per-frame walk; the range check is what
	// keeps -1 and 64+ away from the s2sdk client exports (PITFALLS C2).
	CheckEq(IsSlotInRange(-1), false, "slot -1 is refused");
	CheckEq(IsSlotInRange(-1000), false, "a negative slot is refused");
	CheckEq(IsSlotInRange(0), true, "slot 0 is valid");
	CheckEq(IsSlotInRange(kMaxSlots - 1), true, "the last slot is valid");
	CheckEq(IsSlotInRange(kMaxSlots), false, "the first out-of-range slot is refused");
	CheckEq(IsSlotInRange(1 << 20), false, "a far out-of-range slot is refused");
	CheckEq(kMaxSlots, 64, "CS2 exposes 64 player slots");
}

void TestAllowedExtra() {
	// The original's max(0, numberOfJumps - 1): the engine gives the first jump of an airtime.
	CheckEq(AllowedExtra(0), 0, "jumps = 0 grants no extra jump");
	CheckEq(AllowedExtra(1), 0, "jumps = 1 grants no extra jump");
	CheckEq(AllowedExtra(2), 1, "jumps = 2 grants one extra jump");
	CheckEq(AllowedExtra(5), 4, "jumps = 5 grants four extra jumps");
	CheckEq(AllowedExtra(-3), 0, "a negative feature value grants nothing");
}

// --- the round policy -------------------------------------------------------------------------

// The exact helper src/plugin.cpp calls before granting a jump (the live round is re-queried once
// per frame; this is the per-slot decision), with the production scenarios of the reported bug.
void TestRoundPolicy() {
	using vip_jumps::kDefaultRoundMin;
	using vip_jumps::kRoundMinFeature;
	using vip_jumps::RoundAllowed;

	// The exact per-group key the runtime reads from vip/groups.ini.
	CheckEq(std::string(kRoundMinFeature) == "jumps_round_min", true, "the groups.ini key is exact");
	CheckEq(kDefaultRoundMin, 1, "the missing-key fallback is 1 (fail closed)");

	// First map before round_prestart: pistol is still false, but no round has completed.
	CheckEq(RoundAllowed(-1, false, 0), false, "missing key: first map before round_prestart is refused");
	CheckEq(RoundAllowed(kDefaultRoundMin, false, 0), false, "fallback 1: total 0 is refused even when not pistol");

	// The first pistol round.
	CheckEq(RoundAllowed(-1, true, 0), false, "missing key: first pistol round is refused");

	// Second regular round (one completed round).
	CheckEq(RoundAllowed(-1, false, 1), true, "missing key: second regular round (total 1) is allowed");

	// Second-half pistol: 12 completed rounds, but VIP_PistolRound is set for the switch.
	CheckEq(RoundAllowed(-1, true, 12), false, "missing key: second-half pistol is refused");

	// Ordinary later rounds.
	CheckEq(RoundAllowed(-1, false, 5), true, "missing key: an ordinary later round is allowed");
	CheckEq(RoundAllowed(1, false, 23), true, "min 1: a later non-pistol round is allowed");

	// 0 explicitly allows every round, the pistol ones and the pre-first-round state included.
	CheckEq(RoundAllowed(0, false, 0), true, "0: the first map is allowed");
	CheckEq(RoundAllowed(0, true, 0), true, "0: the first pistol round is allowed");
	CheckEq(RoundAllowed(0, true, 12), true, "0: the second-half pistol is allowed");

	// A negative total is the SDK's "round state unavailable" answer: no effect may run, not even
	// for an explicit 0 (the check runs before the roundMin == 0 branch).
	CheckEq(RoundAllowed(0, false, -1), false, "unavailable round state (total -1) refuses an explicit 0");
	CheckEq(RoundAllowed(-1, false, -1), false, "unavailable round state refuses the missing-key fallback");

	// A higher minimum counts COMPLETED rounds and never allows a pistol round.
	CheckEq(RoundAllowed(2, false, 1), false, "min 2 waits for the second completed round");
	CheckEq(RoundAllowed(2, false, 2), true, "min 2 allows once two rounds completed");
	CheckEq(RoundAllowed(2, true, 5), false, "min 2 refuses a pistol round");
	CheckEq(RoundAllowed(3, false, 2), false, "min 3 still waits at two completed rounds");
	CheckEq(RoundAllowed(3, false, 3), true, "min 3 allows at three completed rounds");
	CheckEq(RoundAllowed(5, false, 4), false, "min 5 waits at four completed rounds");
	CheckEq(RoundAllowed(5, false, 9), true, "min 5 allows later");

	// Any negative value is treated like the core's missing -1 (the fallback is 1).
	CheckEq(RoundAllowed(-7, false, 0), false, "a negative setting falls back to 1: total 0 refused");
	CheckEq(RoundAllowed(-7, false, 1), true, "a negative setting falls back to 1: total 1 allowed");

	// A huge value merely keeps waiting; no wrap turns it into an early allow.
	CheckEq(RoundAllowed(std::numeric_limits<int>::max(), false, 100), false, "INT_MAX does not wrap");

	// A group/config switch without a respawn: a spawn-cached 0 passes the cheap prefilter, but
	// the fresh 1 the runtime reads before SetClientAbsVelocity must deny the grant; a fresh 0
	// permits again after a switch the other way.
	CheckEq(RoundAllowed(0, true, 12), true, "the cached 0 passes the prefilter on the second-half pistol");
	CheckEq(RoundAllowed(1, true, 12), false, "the fresh 1 denies the grant without a respawn");
	CheckEq(RoundAllowed(0, true, 12), true, "a fresh 0 permits again without a respawn");
}

// A forbidden round must cancel the pending extra-jump bookkeeping: nothing buffered there may be
// paid back on an eligible round (the runtime calls ResetPending() at the actual tick).
void TestDeniedRoundResetsPending() {
	User user = MakeUser(3);
	user.jumpReleased = true;
	CheckEq(TickUser(user, kAir, kJumpMask, -30.0f, AllowedExtra(3)), true, "the pending extra jump is granted");
	CheckEq(user.jumpsCount, 1, "one jump pending before the forbidden round");

	vip_jumps::ResetPending(user);
	CheckEq(user.jumpsCount, 0, "a forbidden round cancels the pending extra jump");
	CheckEq(user.jumpReleased, false, "the reset leaves no armed press");

	// Back on an eligible round the budget is clean: releasing and pressing grants only the
	// configured jump, not a payback of the forbidden-round press.
	CheckEq(TickUser(user, kAir, 0, -30.0f, AllowedExtra(3)), false, "releasing after the reset does not boost");
	CheckEq(TickUser(user, kAir, kJumpMask, -30.0f, AllowedExtra(3)), true, "the eligible round grants one extra jump");
	CheckEq(user.jumpsCount, 1, "only the eligible-round jump is counted");
}

// --- the per-tick state machine ---------------------------------------------------------------

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
	const bool boost = TickUser(user, kAir, kJumpMask, -100.0f, AllowedExtra(1));
	CheckEq(boost, false, "1 jump in the feature = no extra jump");
}

void TestExtraJump() {
	// jumps = 2: one extra jump per airtime.
	User user = MakeUser(2);
	user.jumpReleased = false;
	user.lastButtons = kJumpMask; // the player is still holding the first jump
	CheckEq(TickUser(user, kAir, kJumpMask, -50.0f, AllowedExtra(2)), false, "holding the key does not boost");
	CheckEq(TickUser(user, kAir, 0, -50.0f, AllowedExtra(2)), false, "releasing arms the next press");
	CheckEq(TickUser(user, kAir, kJumpMask, -20.0f, AllowedExtra(2)), true, "the next press boosts");
	CheckEq(user.jumpsCount, 1, "one extra jump consumed");
	CheckEq(TickUser(user, kAir, 0, 10.0f, AllowedExtra(2)), false, "no second boost while airborne");
	CheckEq(TickUser(user, kAir, kJumpMask, 10.0f, AllowedExtra(2)), false, "the limit is enforced");

	// Landing resets everything.
	TickUser(user, kFlagOnGround, kJumpMask, 0.0f, AllowedExtra(2));
	CheckEq(user.jumpsCount, 0, "landing resets the counter");
}

void TestLaunchWindow() {
	// The fix: the tick where the engine launched the jump (velocity up) but FL_ONGROUND is still
	// set must not reset the counter, otherwise holding jumps at the limit gains an extra one.
	User user = MakeUser(2);
	user.jumpsCount = 1; // the extra jump of this airtime is used up
	user.jumpReleased = true;
	const bool boost = TickUser(user, kFlagOnGround, kJumpMask, 25.0f /* already launched */, AllowedExtra(2));
	CheckEq(boost, false, "no extra jump in the launch window");
	CheckEq(user.jumpsCount, 1, "the counter survives the launch window");

	// With the wheel (two quick presses, positive velocity), the same tick must not give a jump.
	User wheel = MakeUser(2);
	wheel.jumpsCount = 1;
	wheel.jumpReleased = true;
	CheckEq(TickUser(wheel, kFlagOnGround, kJumpMask, 5.0f, AllowedExtra(2)), false, "wheel spam stays limited");
}

void TestFallingNotBoostedOnGround() {
	// Standing on the ground with zero velocity and a fresh press: the ground branch runs, no boost.
	User user = MakeUser(3);
	CheckEq(TickUser(user, kFlagOnGround, kJumpMask, 0.0f, AllowedExtra(3)), false, "the first jump is the engine's");
	CheckEq(user.jumpsCount, 0, "the counter stays reset on the ground");
}

void TestLandingRearmsTheExtraJump() {
	// jumps = 2: after using the extra jump and landing, the next airtime has it again.
	User user = MakeUser(2);
	user.jumpReleased = true; // the player already let go after the engine's first jump
	CheckEq(TickUser(user, kAir, kJumpMask, -40.0f, AllowedExtra(2)), true, "the extra jump is granted");
	CheckEq(user.jumpsCount, 1, "the extra jump is consumed");

	// Land (velocity zero, no buttons): the ground branch resets the counter and the release flag.
	TickUser(user, kFlagOnGround, 0, 0.0f, AllowedExtra(2));
	CheckEq(user.jumpsCount, 0, "landing gives the airtime back");

	// The next airtime. The old jump is still held when the player becomes airborne again, so this
	// tick only records the held key (no edge, no release yet).
	CheckEq(TickUser(user, kAir, kJumpMask, -10.0f, AllowedExtra(2)), false, "a held key does not boost");
	CheckEq(TickUser(user, kAir, 0, -10.0f, AllowedExtra(2)), false, "releasing arms the next press");
	CheckEq(TickUser(user, kAir, kJumpMask, -10.0f, AllowedExtra(2)), true, "the extra jump is back after landing");
}

void TestMultipleExtraJumps() {
	// jumps = 3: exactly two extra jumps per airtime, no matter how hard the wheel spins.
	User user = MakeUser(3);
	int boosts = 0;
	for (int i = 0; i < 8; ++i) {
		const int buttons = (i % 2 == 0) ? kJumpMask : 0; // press / release / press / ...
		if (TickUser(user, kAir, buttons, -30.0f, AllowedExtra(3))) {
			++boosts;
		}
	}
	CheckEq(boosts, 2, "jumps = 3 grants exactly two extra jumps");
	CheckEq(user.jumpsCount, 2, "both extra jumps are counted");
}

} // namespace

int main() {
	std::printf("vip_jumps tests\n");
	TestSlotRange();
	TestAllowedExtra();
	TestRoundPolicy();
	TestDeniedRoundResetsPending();
	TestGroundReset();
	TestSingleJumpByDefault();
	TestExtraJump();
	TestLaunchWindow();
	TestFallingNotBoostedOnGround();
	TestLandingRearmsTheExtraJump();
	TestMultipleExtraJumps();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}

#pragma once
// Engine-free state machine of the VIP_Jumps port: one tick of the original timer body.
//
// The original (fixed) rules:
//   * on the ground (FL_ONGROUND) and not already launched this tick, the counter resets;
//   * while airborne, releasing IN_JUMP arms the next press; a new press below the limit consumes
//     one extra jump and boosts the vertical velocity (the caller applies the velocity).
//
// The "already launched" part is the fix of the original's fixed revision: the engine clears
// FL_ONGROUND during movement processing, so there is a tick where the jump has been launched
// (positive vertical velocity) but the flag is still set. Resetting the counter there would hand
// out one jump too many, and mouse-wheel spam hits that window far more often than holding the key.

namespace vip_jumps {

// CS2 exposes 64 player slots; a slot outside [0, kMaxSlots) has no client record and must never
// reach an s2sdk export (PITFALLS C2).
inline constexpr int kMaxSlots = 64;

// FL_ONGROUND of CBaseEntity::m_fFlags (bit 0).
inline constexpr int kFlagOnGround = 1 << 0;
// IN_JUMP of m_nButtons->m_pButtonStates[0] (bit 1).
inline constexpr int kJumpMask = 1 << 1;
// The velocity the original wrote into m_vecAbsVelocity().z.
inline constexpr float kJumpVelocity = 300.0f;

// The slot-range half of the plugin's C2 guard; the s2sdk client exports are called only when it
// is true (the map-ready half lives in plugin.cpp, next to the map listeners).
constexpr bool IsSlotInRange(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// The original's extra-jump budget: "jumps" is the total number of jumps of one airtime and the
// engine already gives the first one, so the module may grant max(0, numberOfJumps - 1) extra
// jumps (the original computed that maximum inline in its timer).
constexpr int AllowedExtra(int numberOfJumps) {
	return numberOfJumps > 1 ? numberOfJumps - 1 : 0;
}

// --- the round policy -------------------------------------------------------------------------
//
// The module's optional per-group key in the existing vip/groups.ini (read through the core's
// VIP_GetClientFeatureInt). The key follows the vip_bf/vip_items "round_min" convention, with a
// fail-closed floor for this module:
//   * missing key / no VIP record: the core answers -1, and the fallback is 1 - a missing setting
//     must not hand out extra jumps on the first (pistol) round, which is the reported bug;
//   * 0 explicitly allows every round (the menu toggle still has to be on: this only removes the
//     round restriction);
//   * N >= 1 allows only a non-pistol round with at least N completed rounds (m_totalRoundsPlayed,
//     the same threshold vip_bf/vip_items compare). The >= 1 requirement makes the first map fail
//     closed: before round_prestart the pistol flag is false and no round has completed.
//     VIP_PistolRound() is authoritative for both pistol rounds (first and second half).
//   * a negative totalRounds is the SDK's "round state unavailable" answer (-1) and refuses the
//     effect even for an explicit 0: with no trustworthy round answer nothing may be enabled.
inline constexpr char kRoundMinFeature[] = "jumps_round_min";
inline constexpr int kDefaultRoundMin = 1;

constexpr bool RoundAllowed(int roundMin, bool pistolRound, int totalRounds) {
	if (totalRounds < 0) {
		return false;
	}
	const int minRounds = roundMin < 0 ? kDefaultRoundMin : roundMin;
	return minRounds == 0 || (!pistolRound && totalRounds >= minRounds);
}

struct User {
	int lastButtons = 0;
	int lastFlags = 0;
	int jumpsCount = 0;
	int numberOfJumps = 0;
	bool jumpReleased = false;
};

// A round that refuses the effect cancels the pending extra-jump bookkeeping: a jump buffered
// before the round became forbidden must never be handed out once an eligible round starts. The
// input edge tracking (lastButtons/lastFlags) is kept, so a key held through the forbidden round
// is not mistaken for a fresh press when the effect comes back.
inline void ResetPending(User& user) {
	user.jumpsCount = 0;
	user.jumpReleased = false;
}

// One timer tick for one player. Returns true when an extra jump must be granted (the caller sets
// the vertical velocity); `allowedExtra` is max(0, numberOfJumps - 1), the caller's job like in
// the original.
inline bool TickUser(User& user, int flags, int buttonsDown, float velocityZ, int allowedExtra) {
	const bool jumpPressedThisFrame =
		((user.lastButtons & kJumpMask) == 0) && ((buttonsDown & kJumpMask) != 0);
	const bool launchedThisTick = velocityZ > 0.0f;

	bool boost = false;
	if ((flags & kFlagOnGround) != 0 && !launchedThisTick) {
		user.jumpsCount = 0;
		user.jumpReleased = false;
	} else {
		if ((buttonsDown & kJumpMask) == 0) {
			user.jumpReleased = true;
		}
		if (user.jumpReleased && jumpPressedThisFrame && user.jumpsCount < allowedExtra) {
			++user.jumpsCount;
			user.jumpReleased = false;
			boost = true;
		}
	}

	user.lastFlags = flags;
	user.lastButtons = buttonsDown;
	return boost;
}

} // namespace vip_jumps

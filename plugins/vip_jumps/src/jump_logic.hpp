#pragma once
// Engine-free state machine of the VIP_Jumps port: one tick of the original timer body.
//
// The original (fixed) rules:
//   * on the ground (FL_ONGROUND) and not already launched this tick, the counter resets;
//   * while airborne, releasing IN_JUMP arms the next press; a new press below the limit consumes
//     one extra jump and boosts the vertical velocity (the caller applies the velocity).
//
// The "already launched" part is the production fix: the engine clears FL_ONGROUND during movement
// processing, so there is a tick where the jump has been launched (positive vertical velocity)
// but the flag is still set. Resetting the counter there would hand out one jump too many, and
// mouse-wheel spam hits that window far more often than holding the key.

namespace vip_jumps {

// FL_ONGROUND of CBaseEntity::m_fFlags (bit 0).
inline constexpr int kFlagOnGround = 1 << 0;
// IN_JUMP of m_nButtons->m_pButtonStates[0] (bit 1).
inline constexpr int kJumpMask = 1 << 1;
// The velocity the original wrote into m_vecAbsVelocity().z.
inline constexpr float kJumpVelocity = 300.0f;

struct User {
	int lastButtons = 0;
	int lastFlags = 0;
	int jumpsCount = 0;
	int numberOfJumps = 0;
	bool jumpReleased = false;
};

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

// vip_jumps — Plugify port of the Pisex/ABKAM VIP module "VIP_Jumps"
// (the original's vip_jumps.cpp, the fixed revision).
//
// Behaviour kept identical:
//   * the feature is registered as "jumps" (VIP_INT, TOGGLABLE): the value is the total number of
//     jumps per airtime (1 = no extra jump).
//   * every frame the 64 slots are walked. For a living VIP whose "jumps" feature is on:
//       - while on the ground and not already launched this tick, the jump counter resets;
//       - a newly pressed IN_JUMP (edge, not held) while airborne and below the limit consumes one
//         extra jump and sets the vertical velocity to 300.
//     The ground test is the FL_ONGROUND flag plus "vertical velocity not positive yet"
//     (the fix for the tick where the engine has already launched the player but FL_ONGROUND is
//     still set - wheel-spam could otherwise gain an extra jump).
//
// Port notes:
//   * Pure s2sdk: no pisexcompat and no guard library (docs/PLUGIN_STANDARD.md §7), so every
//     safety rule the original inherited from pisexcompat lives in this file:
//       - map gate (PITFALLS B1/B2): the OnMapStart/OnMapEnd listeners are registered in
//         OnPluginStart, g_mapReady is latched by OnMapStart and by the first OnPluginUpdate (that
//         covers a plugin loaded into a running map, which never sees the OnMapStart) and dropped
//         by OnMapEnd; OnPluginEnd removes the listeners again, so `plg reload` leaves nothing
//         pointing into this library. No client call is made while the gate is closed - between
//         OnMapEnd and the next map start s2sdk reads an emptied client list and segfaults.
//       - client checks (PITFALLS C2): IsAlive() layers the map gate, the slot range, connection,
//         in-game, SourceTV, a controller entity pointer and the alive check, because s2sdk
//         exports do not validate their arguments.
//       - the per-frame body catches per-slot exceptions and checks every __s2sdk_* export it
//         calls, so a missing binding disables the feature instead of crashing.
//   * m_fFlags / m_vecAbsVelocity / m_nButtons[0] come from s2sdk's GetClientFlags,
//     GetClientAbsVelocity/SetClientAbsVelocity and GetClientButtons (the same pawn fields the
//     original read through SchemaEntity).
//   * the original's CreateTimer(0.0f) repeat timer ran its body once per frame ("next frame");
//     the port runs it from OnPluginUpdate, the host's per-frame callback (vip_endurance does its
//     GameFrame work the same way). s2sdk client queries are not safe before a map, so the body
//     waits for the map gate - the original's timer could not have done anything before one
//     anyway.
//   * m_numberOfJumps is refreshed from the feature on VIP_OnPlayerSpawn, like the original.
//   * the original timer asked the core for VIP_GetClientFeatureBool(i, "jumps") on every alive
//     slot, every tick. That call is a cross-plugin call into the vip core, which composes the
//     '<group>.jumps' cookie key and reads the permissions cookie store (two string allocations
//     and two map lookups per call, plus the permissions boundary call). The port caches the
//     answer per slot and refreshes it wherever it can change: the toggle callback, VIP load,
//     spawn, VIP removal and disconnect - i.e. every path the core notifies a module about - plus
//     a full re-read once a second, which also covers the paths that notify nothing (see the
//     m_featureOn member).
//   * Round policy: the extra jump used to be granted on every round, including pistol rounds
//     (the reported bug). The optional per-group key "jumps_round_min" (vip/groups.ini, read
//     through VIP_GetClientFeatureInt) is cached next to m_featureOn by RefreshFeature (the core's
//     spawn/load/removal callbacks and the once-a-second full re-read), while the LIVE round
//     (VIP_PistolRound / VIP_GetTotalRounds) is queried once per frame. The cached threshold is a
//     cheap prefilter; when TickUser is about to grant a boost, the key is re-read FRESH right
//     before SetClientAbsVelocity and the gate is re-evaluated against the frame's round state,
//     so a stale 0 can never grant a jump after a group/config switch to 1 (a cached 1 -> 0
//     propagates with the existing once-a-second refresh). The cache therefore never stores an
//     "allowed" decision, and the menu toggle cannot bypass the round check. A refused round also
//     drops the pending extra-jump bookkeeping (jump_logic.hpp's ResetPending), so no buffered
//     jump survives into an eligible round. If the core does not expose the round calls, no jump
//     is granted (fail closed).

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "jump_logic.hpp"
#include "vip_api_compat.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

namespace {

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE,
// DISABLED, ...): the consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

// The engine's player-slot count, shared with the engine-free half (jump_logic.hpp).
using vip_jumps::kMaxSlots;

constexpr const char* kFeature = "jumps";

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// --- map gate (PITFALLS B1/B2) ----------------------------------------------------------------
//
// s2sdk's client lookups read the engine's client list, which is empty before the first map and
// from OnMapEnd until the next OnMapStart: any client query in those windows segfaults. The three
// flags are the state machine pisexcompat kept internally, now driven by the s2sdk listeners
// directly:
//   g_mapReady             a map is live - the only state in which a client call is allowed
//   g_mapStartSeen         an OnMapStart arrived since OnPluginStart / the last OnMapEnd
//   g_mapEndSeen           an OnMapEnd arrived since OnPluginStart
bool g_mapReady = false;
bool g_mapStartSeen = false;
bool g_mapEndSeen = false;
bool g_mapListenersRegistered = false;

// s2sdk listeners are plain function pointers called on the game thread: nothing here may throw,
// and no client or entity call is made - state only.
void OnMapStartListener() {
	// The engine calls this once the new map and its entity system exist: readiness is granted
	// here for everything that follows (PITFALLS B1).
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and every
	// client query crashes - so this handler only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void InitMapGate() {
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without both listeners nothing could ever close the gate again, so it is never opened
		// (fail safe, PITFALLS A1: log and stay inactive, never return Failed).
		LogError("vip_jumps: the s2sdk map listeners are unavailable - client calls stay disabled");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void PollMapGate() {
	if (!g_mapListenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart proves a live map (the host pumps updates from the server frame). After an
	// OnMapEnd only a new OnMapStart may re-open the gate - the host keeps pumping updates through
	// a changelevel, so "no OnMapEnd seen" must not be enough once one was seen.
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

void ShutdownMapGate() {
	if (g_mapListenersRegistered) {
		g_mapListenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister != nullptr) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister != nullptr) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	}
	// Drop readiness last: nothing can reach a client through this plugin after OnPluginEnd.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
}

// --- client checks (PITFALLS C2) ---------------------------------------------------------------
//
// True when the slot may be queried and holds a living player. s2sdk exports do not validate
// their arguments, so the layers run in the cheap-to-expensive order and every export is checked
// for a null binding before it is called; a throwing export counts as "not alive".
bool IsAlive(int iSlot) {
	if (!g_mapReady || !vip_jumps::IsSlotInRange(iSlot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(iSlot) && s2sdk::IsClientInGame(iSlot) &&
			   !s2sdk::IsClientSourceTV(iSlot) &&
			   s2sdk::PlayerSlotToEntPointer(iSlot) != nullptr && s2sdk::IsClientAlive(iSlot);
	} catch (...) {
		return false;
	}
}

// The round threshold cache starts at the fail-closed fallback, so a slot whose refresh did not
// run yet can never be mistaken for an explicit "0 = every round".
std::array<int, kMaxSlots> DefaultRoundMins() {
	std::array<int, kMaxSlots> mins{};
	mins.fill(vip_jumps::kDefaultRoundMin);
	return mins;
}

class JumpsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	void Tick();
	// Refreshes the cached "jumps" state of one slot (the result of
	// VIP_GetClientFeatureBool(slot, "jumps"), i.e. the original timer's per-tick query).
	void RefreshFeature(int iSlot, bool bIsVIP);
	// Re-reads every slot; runs at most once a second (the safety net for changes no callback
	// reports, and the first fill when the core becomes ready).
	void RefreshAll();

	std::array<vip_jumps::User, kMaxSlots> m_users{};
	// The per-tick VIP_GetClientFeatureBool() of the original, cached: that call is a cross-plugin
	// call which builds the '<group>.jumps' cookie key and reads the permissions cookie store
	// (string allocations + two map lookups) on every alive slot, every tick, for every vip module.
	// Every way the answer can change notifies this module - the menu toggle callback, VIP load
	// (VIP_OnClientLoaded), VIP spawn, VIP removal and disconnect - and on top of that the whole
	// table is re-read once a second (RefreshAll), which is also the initial fill: it bounds the
	// staleness of the cases no callback covers (a third party writing the '<group>.jumps' cookie,
	// vip_reload's group swap - neither fires anything - and a module reloaded mid-map, whose
	// players already spawned). 64 calls once a second instead of 64 per tick.
	std::array<bool, kMaxSlots> m_featureOn{};
	// The per-slot "jumps_round_min" group key (-1 = missing), cached by RefreshFeature together
	// with m_featureOn (the core callbacks and the once-a-second full re-read; the toggle callback
	// updates only the bool, see its comment). Only the threshold is cached and only as a cheap
	// prefilter: the live round is queried once per frame in Tick(), the gate is re-evaluated for
	// every slot, and the key is re-read fresh right before a grant, so no stale "allowed" bool
	// can exist.
	std::array<int, kMaxSlots> m_roundMin = DefaultRoundMins();
	std::chrono::steady_clock::time_point m_lastRefresh{};
};

JumpsPlugin g_jumpsPlugin;

plg::PluginResult JumpsPlugin::OnPluginStart() {
	try {
		// Map listeners first: they gate every client call this plugin ever makes, and neither the
		// registration nor the latch touches a client (PITFALLS B1). Never fatal: a missing
		// binding only logs and keeps the gate closed.
		InitMapGate();

		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int /*iTeam*/, bool bIsVIP) {
			if (bIsVIP && iSlot >= 0 && iSlot < kMaxSlots) {
				m_users[static_cast<std::size_t>(iSlot)].numberOfJumps =
					g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature);
			}
			RefreshFeature(iSlot, bIsVIP);
		});
		// The other places the cached "jumps" state can change: the VIP row/cookies arriving or
		// being reloaded, the VIP status being removed (an admin command or the expiry loop) and a
		// disconnect (the next client in the slot starts from "off").
		g_pVIPCore->VIP_OnClientLoaded([this](int iSlot, bool bIsVIP) { RefreshFeature(iSlot, bIsVIP); });
		g_pVIPCore->VIP_OnVIPClientRemoved([this](int iSlot, int /*iReason*/) {
			if (iSlot >= 0 && iSlot < kMaxSlots) {
				m_featureOn[static_cast<std::size_t>(iSlot)] = false;
			}
		});
		g_pVIPCore->VIP_OnClientDisconnect([this](int iSlot, bool /*bIsVIP*/) {
			if (iSlot >= 0 && iSlot < kMaxSlots) {
				m_featureOn[static_cast<std::size_t>(iSlot)] = false;
			}
		});
		// The menu toggle: the core calls this before it writes the cookie, and the new status is
		// the value it is about to store (this callback never blocks the toggle, like the original
		// feature which registered no callback at all). Only the feature bool is updated here: a
		// cross-plugin feature read from inside the core's menu callback would re-enter s2sdk
		// (menu callbacks run under its mutex); the round threshold does not change when the
		// "jumps" item is toggled, and RefreshFeature/RefreshAll keep it current.
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_INT, TOGGLABLE, nullptr,
										[this](int iSlot, const char* /*szFeature*/, VIP_ToggleState /*eOldStatus*/,
											   VIP_ToggleState& eNewStatus) {
											if (iSlot >= 0 && iSlot < kMaxSlots) {
												m_featureOn[static_cast<std::size_t>(iSlot)] = (eNewStatus == ENABLED);
											}
											return false;
										});

		LogInfo("vip_jumps: ready");
	} catch (const std::exception& ex) {
		LogError(std::string("vip_jumps: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("vip_jumps: OnPluginStart failed with an unknown exception");
	}
	return {};
}

plg::PluginResult JumpsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// The map gate: opens on OnMapStart (the listener) or, for a plugin loaded into a running
		// map, on this first update after OnPluginStart (PITFALLS B1); after an OnMapEnd it stays
		// closed until the next OnMapStart (PITFALLS B2).
		PollMapGate();
		if (!g_mapReady) {
			return {}; // no client query before the first map / between maps
		}
		// The original's per-frame timer body. Tick() catches its own per-slot errors; this catch
		// keeps the core query and the refresh clock inside the plugin as well.
		Tick();
	} catch (const std::exception& ex) {
		LogError(std::string("vip_jumps: tick failed: ") + ex.what());
	} catch (...) {
		LogError("vip_jumps: tick failed with an unknown exception");
	}
	return {};
}

plg::PluginResult JumpsPlugin::OnPluginEnd() {
	// Every core -> module callback registered in OnPluginStart, dropped while this library is
	// still mapped. Each call is guarded: the core may already be going away on a reload.
	try {
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();
	} catch (...) {
	}
	try {
		g_pVIPCore->VIP_OnClientLoadedUnregister();
	} catch (...) {
	}
	try {
		g_pVIPCore->VIP_OnVIPClientRemovedUnregister();
	} catch (...) {
	}
	try {
		g_pVIPCore->VIP_OnClientDisconnectUnregister();
	} catch (...) {
	}
	// The vip API has no feature unregister, so the toggle callback registered in OnPluginStart
	// would keep pointing into this library after it is unmapped (a crash if a player toggled the
	// item afterwards). Re-registering the feature without callbacks replaces the core's entry with
	// null delegates - same value/feature type, so the menu entry is unchanged, and the core holds
	// no pointer into this library any more (the vip_vampirism note documents the same hazard).
	try {
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_INT, TOGGLABLE);
	} catch (...) {
	}
	// The map listeners point into this library as well: remove them and close the gate
	// (PITFALLS A7 - `plg reload` must leave nothing behind).
	try {
		ShutdownMapGate();
	} catch (...) {
	}
	return {};
}

void JumpsPlugin::RefreshFeature(int iSlot, bool bIsVIP) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	const auto index = static_cast<std::size_t>(iSlot);
	m_featureOn[index] = bIsVIP && g_pVIPCore->VIP_GetClientFeatureBool(iSlot, kFeature);
	// The round threshold lives and refreshes with the feature cache; -1 (missing key / no VIP
	// record) is kept as is and resolved by RoundAllowed() on every use.
	m_roundMin[index] = bIsVIP ? g_pVIPCore->VIP_GetClientFeatureInt(iSlot, vip_jumps::kRoundMinFeature)
							   : vip_jumps::kDefaultRoundMin;
}

void JumpsPlugin::RefreshAll() {
	for (int i = 0; i < kMaxSlots; ++i) {
		try {
			RefreshFeature(i, g_pVIPCore->VIP_IsClientVIP(i));
		} catch (...) {
		}
	}
}

void JumpsPlugin::Tick() {
	if (!g_pVIPCore->VIP_IsVIPLoaded()) {
		return;
	}
	if (!__s2sdk_GetClientFlags || !__s2sdk_GetClientAbsVelocity || !__s2sdk_SetClientAbsVelocity ||
		!__s2sdk_GetClientButtons) {
		return;
	}
	// The round API is optional at runtime (an older core): without the core or the two round
	// bindings the effect stays off and every pending extra jump is cancelled. A disabled API must
	// never enable the effect.
	if (!__vip_VIP_IsVIPLoaded || !__vip_VIP_PistolRound || !__vip_VIP_GetTotalRounds) {
		for (vip_jumps::User& user : m_users) {
			vip_jumps::ResetPending(user);
		}
		return;
	}
	// The safety net (and the initial fill: m_lastRefresh starts at the epoch): a full re-read at
	// most once a second, 64 calls, instead of the original's 64 per tick.
	const auto now = std::chrono::steady_clock::now();
	if (now - m_lastRefresh >= std::chrono::seconds(1)) {
		m_lastRefresh = now;
		RefreshAll();
	}
	// The live round, queried once per frame for the whole module (not once per slot). The gate
	// is re-evaluated per slot below, so this is never a cached "allowed" decision.
	const bool bPistolRound = g_pVIPCore->VIP_PistolRound();
	const int iTotalRounds = g_pVIPCore->VIP_GetTotalRounds();
	for (int i = 0; i < kMaxSlots; ++i) {
		try {
			// Map gate + slot/connection/controller/pawn checks (PITFALLS C2); nothing below runs
			// for a slot IsAlive() refused.
			if (!IsAlive(i)) {
				continue;
			}
			const auto index = static_cast<std::size_t>(i);
			vip_jumps::User& user = m_users[index];
			// The cached threshold is only a cheap prefilter here (the fresh re-read runs right
			// before the actual grant below). A refused round drops the pending extra-jump
			// bookkeeping, so no jump is banked for a later eligible round.
			if (!vip_jumps::RoundAllowed(m_roundMin[index], bPistolRound, iTotalRounds)) {
				vip_jumps::ResetPending(user);
				continue;
			}
			// The cached VIP_GetClientFeatureBool(i, "jumps") of the original (see the member).
			if (!m_featureOn[index]) {
				continue;
			}
			const int flags = s2sdk::GetClientFlags(i);
			const int buttonsDown = static_cast<int>(s2sdk::GetClientButtons(i, 0));
			const int allowedExtra = vip_jumps::AllowedExtra(user.numberOfJumps);
			plg::vec3 velocity = s2sdk::GetClientAbsVelocity(i);
			// The original's rules (and its fix for the tick where the engine has already
			// launched but FL_ONGROUND is still set) live in jump_logic.hpp - shared with the test.
			if (vip_jumps::TickUser(user, flags, buttonsDown, velocity.z, allowedExtra)) {
				// The cached gate above is only a cheap prefilter. Right before the actual write,
				// re-read the threshold FRESH: a group/config switch can turn a cached 0 into 1
				// (deny) without a respawn, and a fresh 0 must also be honoured once the cached 1
				// has been refreshed. The frame's round state is reused; on a denied answer the
				// consumed jump is cancelled and the velocity is not written.
				int iRoundMin = m_roundMin[index];
				bool bRoundAllowed = false;
				try {
					iRoundMin = g_pVIPCore->VIP_GetClientFeatureInt(i, vip_jumps::kRoundMinFeature);
					m_roundMin[index] = iRoundMin;
					bRoundAllowed = vip_jumps::RoundAllowed(iRoundMin, bPistolRound, iTotalRounds);
				} catch (...) {
					bRoundAllowed = false;
				}
				if (!bRoundAllowed) {
					vip_jumps::ResetPending(user);
					continue;
				}
				velocity.z = vip_jumps::kJumpVelocity;
				s2sdk::SetClientAbsVelocity(i, velocity);
			}
		} catch (const std::exception& ex) {
			LogError("vip_jumps: slot " + std::to_string(i) + " failed: " + ex.what());
		} catch (...) {
			LogError("vip_jumps: slot " + std::to_string(i) + " failed with an unknown exception");
		}
	}
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_jumpsPlugin)

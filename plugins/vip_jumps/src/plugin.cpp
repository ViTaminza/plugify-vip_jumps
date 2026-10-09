// vip_jumps — Plugify port of the Pisex/ABKAM VIP module "VIP_Jumps"
// (ref/src/cs2-vip-modules/VIP_Jumps/source/vip_jumps.cpp, the fixed revision that ships next to
// vip_jumps.cpp.fixed and is the one in vip_jumps.cpp).
//
// Behaviour kept identical:
//   * the feature is registered as "jumps" (VIP_INT, TOGGLABLE): the value is the total number of
//     jumps per airtime (1 = no extra jump).
//   * every frame a repeating 0 s timer walks the 64 slots. For a living VIP whose "jumps"
//     feature is on:
//       - while on the ground and not already launched this tick, the jump counter resets;
//       - a newly pressed IN_JUMP (edge, not held) while airborne and below the limit consumes one
//         extra jump and sets the vertical velocity to 300.
//     The ground test is `m_fFlags & FL_ONGROUND` plus "vertical velocity not positive yet"
//     (the fix for the tick where the engine has already launched the player but FL_ONGROUND is
//     still set - wheel-spam could otherwise gain an extra jump).
//
// Port notes:
//   * m_fFlags / m_vecAbsVelocity / m_nButtons[0] come from s2sdk's GetClientFlags,
//     GetClientAbsVelocity/SetClientAbsVelocity and GetClientButtons (the same pawn fields the
//     original read through SchemaEntity).
//   * the timer is created on the first update with a running map - s2sdk client queries are not
//     safe before that, and the original's timer could not have done anything before a map anyway.
//   * m_numberOfJumps is refreshed from the feature on VIP_OnPlayerSpawn, like the original.
//   * the original timer asked the core for VIP_GetClientFeatureBool(i, "jumps") on every alive
//     slot, every tick. That call is a cross-plugin call into the vip core, which composes the
//     '<group>.jumps' cookie key and reads the permissions cookie store (two string allocations
//     and two map lookups per call, plus the permissions boundary call). The port caches the
//     answer per slot and refreshes it wherever it can change: the toggle callback, VIP load,
//     spawn, VIP removal and disconnect - i.e. every path the core notifies a module about - plus
//     a full re-read once a second, which also covers the paths that notify nothing (see the
//     m_featureOn member).

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "jump_logic.hpp"
#include "vip_api_compat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE,
// DISABLED, ...): the consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

constexpr int kMaxSlots = 64;
constexpr const char* kFeature = "jumps";
// The original's timer was CreateTimer(0.0f, ...) (every frame). A 0 second delay is FATAL on
// s2sdk: TimerSystem::RunFrame() re-inserts a repeat timer with executeTime = universalTime +
// delay and keeps looping while any timer is due, so a 0 delay stays due forever (the whole
// server frame never returns -> "WatchDog! Server took too long to process" -> fatal). 1 ms is
// below one engine tick (0.015625 s), so the timer still runs exactly once per tick.
constexpr float kTimerInterval = 0.001f;
static_assert(kTimerInterval > 0.0f, "a 0 second interval hangs s2sdk's TimerSystem::RunFrame loop");

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

class JumpsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	void EnsureTimer();
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
	std::chrono::steady_clock::time_point m_lastRefresh{};
	pisex::CTimer* m_timer = nullptr;
};

JumpsPlugin g_jumpsPlugin;

plg::PluginResult JumpsPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_jumps");

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
		// feature which registered no callback at all).
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
	pisex::Poll();
	if (!pisex::IsGameReady()) {
		return {}; // client queries before the first map crash s2sdk
	}
	EnsureTimer();
	return {};
}

plg::PluginResult JumpsPlugin::OnPluginEnd() {
	if (m_timer != nullptr) {
		pisex::RemoveTimer(m_timer);
		m_timer = nullptr;
	}
	// Every core -> module callback registered in OnPluginStart, dropped while this library is
	// still mapped.
	g_pVIPCore->VIP_OnPlayerSpawnUnregister();
	g_pVIPCore->VIP_OnClientLoadedUnregister();
	g_pVIPCore->VIP_OnVIPClientRemovedUnregister();
	g_pVIPCore->VIP_OnClientDisconnectUnregister();
	// The vip API has no feature unregister, so the toggle callback registered in OnPluginStart
	// would keep pointing into this library after it is unmapped (a crash if a player toggled the
	// item afterwards). Re-registering the feature without callbacks replaces the core's entry with
	// null delegates - same value/feature type, so the menu entry is unchanged, and the core holds
	// no pointer into this library any more (the vip_vampirism note documents the same hazard).
	g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_INT, TOGGLABLE);
	pisex::Shutdown();
	return {};
}

void JumpsPlugin::RefreshFeature(int iSlot, bool bIsVIP) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	m_featureOn[static_cast<std::size_t>(iSlot)] =
		bIsVIP && g_pVIPCore->VIP_GetClientFeatureBool(iSlot, kFeature);
}

void JumpsPlugin::RefreshAll() {
	for (int i = 0; i < kMaxSlots; ++i) {
		try {
			RefreshFeature(i, g_pVIPCore->VIP_IsClientVIP(i));
		} catch (...) {
		}
	}
}

void JumpsPlugin::EnsureTimer() {
	if (m_timer != nullptr) {
		return;
	}
	m_timer = pisex::CreateTimer(kTimerInterval, [this]() -> float {
		try {
			Tick();
		} catch (const std::exception& ex) {
			LogError(std::string("vip_jumps: tick failed: ") + ex.what());
		} catch (...) {
			LogError("vip_jumps: tick failed with an unknown exception");
		}
		return kTimerInterval; // positive on purpose: 0.0 would hang s2sdk's RunFrame loop
	});
}

void JumpsPlugin::Tick() {
	if (!g_pVIPCore->VIP_IsVIPLoaded()) {
		return;
	}
	if (!__s2sdk_GetClientFlags || !__s2sdk_GetClientAbsVelocity || !__s2sdk_SetClientAbsVelocity ||
		!__s2sdk_GetClientButtons) {
		return;
	}
	// The safety net (and the initial fill: m_lastRefresh starts at the epoch): a full re-read at
	// most once a second, 64 calls, instead of the original's 64 per tick.
	const auto now = std::chrono::steady_clock::now();
	if (now - m_lastRefresh >= std::chrono::seconds(1)) {
		m_lastRefresh = now;
		RefreshAll();
	}
	for (int i = 0; i < kMaxSlots; ++i) {
		try {
			if (!pisex::IsAlive(i)) {
				continue;
			}
			// The cached VIP_GetClientFeatureBool(i, "jumps") of the original (see the member).
			if (!m_featureOn[static_cast<std::size_t>(i)]) {
				continue;
			}
			vip_jumps::User& user = m_users[static_cast<std::size_t>(i)];
			const int flags = s2sdk::GetClientFlags(i);
			const int buttonsDown = static_cast<int>(s2sdk::GetClientButtons(i, 0));
			const int allowedExtra = std::max(0, user.numberOfJumps - 1);
			plg::vec3 velocity = s2sdk::GetClientAbsVelocity(i);
			// The original's rules (and its fix for the tick where the engine has already
			// launched but FL_ONGROUND is still set) live in jump_logic.hpp - shared with the test.
			if (vip_jumps::TickUser(user, flags, buttonsDown, velocity.z, allowedExtra)) {
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

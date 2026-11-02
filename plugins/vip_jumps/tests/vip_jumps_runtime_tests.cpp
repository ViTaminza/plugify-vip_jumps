// vip_jumps_runtime_tests — the real plugin driven through the generated s2sdk/vip bindings.
//
// The plugin translation unit is compiled into this binary (../src/plugin.cpp), exactly like the
// production .so; the generated binding globals come from the s2sdk_bindings / vip_bindings object
// libraries and only their call targets are replaced by the recording fakes below. The test
// therefore drives the actual OnPluginStart() / OnPluginUpdate() path and the actual captured
// VIP_OnPlayerSpawn delegate, and counts the real SetClientAbsVelocity() grants.
//
// Covered (round policy):
//   * missing 'jumps_round_min' (-1) falls back to 1: no extra jump on the first round, an extra
//     jump on the second non-pistol round;
//   * both pistol rounds (first and second half) are refused at the default threshold;
//   * an explicit 0 permits a pistol round;
//   * a threshold >= 2 waits for that many completed rounds;
//   * a threshold change 0 -> 1 without a respawn blocks the next grant immediately, independent
//     of the once-a-second RefreshAll (the fake flips during the press tick, so only the
//     per-grant reread can see it; fails on the spawn/refresh-cached implementation);
//   * OnMapEnd blocks the effect and every round/client query;
//   * a missing round binding fails closed, and restoring it resumes the extra jump.
//
// Run by ./test.sh (build/tests/vip_jumps_runtime_tests).

#include "../src/plugin.cpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEqInt(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

void CheckEqFloat(float actual, float expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %g, got %g)\n", what.c_str(), static_cast<double>(expected),
					static_cast<double>(actual));
	}
}

std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

constexpr int kSlots = vip_jumps::kMaxSlots;

// --- fake client state -----------------------------------------------------------------------

std::array<bool, static_cast<std::size_t>(kSlots)> g_connected{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_inGame{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_sourceTV{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_alive{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_hasController{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_isVIP{};

std::array<int32_t, static_cast<std::size_t>(kSlots)> g_flags{};
std::array<uint64_t, static_cast<std::size_t>(kSlots)> g_buttons{};
std::array<plg::vec3, static_cast<std::size_t>(kSlots)> g_velocity{};
std::array<char, static_cast<std::size_t>(kSlots)> g_entityMarkers{};

std::array<std::map<std::string, bool, std::less<>>, static_cast<std::size_t>(kSlots)> g_featureBools;
std::array<std::map<std::string, int, std::less<>>, static_cast<std::size_t>(kSlots)> g_featureInts;

bool g_vipLoaded = true;
bool g_pistolRound = false;
int g_totalRounds = 0;

int g_pistolCalls = 0;
int g_totalRoundsCalls = 0;
int g_flagsCalls = 0;
int g_buttonsCalls = 0;
int g_clientCalls = 0;
int g_setVelocityCalls = 0;
int g_lastSetSlot = -1;
plg::vec3 g_lastSetVelocity{};
// One-shot hook for the 0 -> 1 regression: the fake group value changes during the press tick's
// GetClientAbsVelocity, i.e. after any RefreshAll and before the freshness read at the grant.
bool g_flipRoundMinOnVelocityRead = false;

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
::vip::SpawnDelegate g_spawnDelegate = nullptr;
::vip::ClientLoadedOrDisconnectDelegate g_clientLoadedDelegate = nullptr;
::vip::ClientLoadedOrDisconnectDelegate g_disconnectDelegate = nullptr;
::vip::VipRemoveDelegate g_removedDelegate = nullptr;

bool InRange(int32_t slot) {
	return slot >= 0 && slot < kSlots;
}

// --- fake s2sdk exports ----------------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_connected[static_cast<std::size_t>(slot)];
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_inGame[static_cast<std::size_t>(slot)];
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_sourceTV[static_cast<std::size_t>(slot)];
}

bool FakeIsClientAlive(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_alive[static_cast<std::size_t>(slot)];
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	if (!InRange(slot) || !g_hasController[static_cast<std::size_t>(slot)]) {
		return nullptr;
	}
	return &g_entityMarkers[static_cast<std::size_t>(slot)];
}

int32_t FakeGetClientFlags(int32_t slot) {
	++g_clientCalls;
	++g_flagsCalls;
	return InRange(slot) ? g_flags[static_cast<std::size_t>(slot)] : 0;
}

uint64_t FakeGetClientButtons(int32_t slot, int32_t buttonIndex) {
	++g_clientCalls;
	++g_buttonsCalls;
	if (!InRange(slot) || buttonIndex != 0) {
		return 0;
	}
	return g_buttons[static_cast<std::size_t>(slot)];
}

plg::vec3 FakeGetClientAbsVelocity(int32_t slot) {
	++g_clientCalls;
	if (!InRange(slot)) {
		return plg::vec3{};
	}
	if (g_flipRoundMinOnVelocityRead) {
		g_flipRoundMinOnVelocityRead = false;
		g_featureInts[static_cast<std::size_t>(slot)][vip_jumps::kRoundMinFeature] = 1;
	}
	return g_velocity[static_cast<std::size_t>(slot)];
}

void FakeSetClientAbsVelocity(int32_t slot, const plg::vec3& velocity) {
	++g_setVelocityCalls;
	g_lastSetSlot = slot;
	g_lastSetVelocity = velocity;
}

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
}

// --- fake vip exports ------------------------------------------------------------------------

bool FakeIsVIPLoaded() {
	return g_vipLoaded;
}

bool FakePistolRound() {
	++g_pistolCalls;
	return g_pistolRound;
}

int32_t FakeGetTotalRounds() {
	++g_totalRoundsCalls;
	return g_totalRounds;
}

bool FakeIsClientVIP(int32_t slot) {
	return InRange(slot) && g_isVIP[static_cast<std::size_t>(slot)];
}

bool FakeGetClientFeatureBool(int32_t slot, const plg::string& feature) {
	if (!InRange(slot)) {
		return false;
	}
	const auto& values = g_featureBools[static_cast<std::size_t>(slot)];
	const auto it = values.find(ToStd(feature));
	return it != values.end() && it->second;
}

int32_t FakeGetClientFeatureInt(int32_t slot, const plg::string& feature) {
	if (!InRange(slot)) {
		return -1;
	}
	const auto& values = g_featureInts[static_cast<std::size_t>(slot)];
	const auto it = values.find(ToStd(feature));
	return it == values.end() ? -1 : it->second; // -1 = missing key
}

void FakeRegisterFeature(const plg::string&, ::vip::ValueType, ::vip::FeatureType,
						 ::vip::ItemSelectableDelegate, ::vip::ItemTogglableDelegate,
						 ::vip::ItemDisplayDelegate) {
}

void FakeOnPlayerSpawnRegister(::vip::SpawnDelegate callback) {
	g_spawnDelegate = callback;
}

void FakeOnPlayerSpawnUnregister(::vip::SpawnDelegate callback) {
	if (g_spawnDelegate == callback) {
		g_spawnDelegate = nullptr;
	}
}

void FakeOnClientLoadedRegister(::vip::ClientLoadedOrDisconnectDelegate callback) {
	g_clientLoadedDelegate = callback;
}

void FakeOnClientLoadedUnregister(::vip::ClientLoadedOrDisconnectDelegate callback) {
	if (g_clientLoadedDelegate == callback) {
		g_clientLoadedDelegate = nullptr;
	}
}

void FakeOnClientDisconnectRegister(::vip::ClientLoadedOrDisconnectDelegate callback) {
	g_disconnectDelegate = callback;
}

void FakeOnClientDisconnectUnregister(::vip::ClientLoadedOrDisconnectDelegate callback) {
	if (g_disconnectDelegate == callback) {
		g_disconnectDelegate = nullptr;
	}
}

void FakeOnVIPClientRemovedRegister(::vip::VipRemoveDelegate callback) {
	g_removedDelegate = callback;
}

void FakeOnVIPClientRemovedUnregister(::vip::VipRemoveDelegate callback) {
	if (g_removedDelegate == callback) {
		g_removedDelegate = nullptr;
	}
}

// --- fixtures --------------------------------------------------------------------------------

void InstallFakes() {
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_GetClientFlags = &FakeGetClientFlags;
	__s2sdk_GetClientButtons = &FakeGetClientButtons;
	__s2sdk_GetClientAbsVelocity = &FakeGetClientAbsVelocity;
	__s2sdk_SetClientAbsVelocity = &FakeSetClientAbsVelocity;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;

	__vip_VIP_IsVIPLoaded = &FakeIsVIPLoaded;
	__vip_VIP_PistolRound = &FakePistolRound;
	__vip_VIP_GetTotalRounds = &FakeGetTotalRounds;
	__vip_VIP_IsClientVIP = &FakeIsClientVIP;
	__vip_VIP_GetClientFeatureBool = &FakeGetClientFeatureBool;
	__vip_VIP_GetClientFeatureInt = &FakeGetClientFeatureInt;
	__vip_VIP_RegisterFeature = &FakeRegisterFeature;
	__vip_VIP_OnPlayerSpawn_Register = &FakeOnPlayerSpawnRegister;
	__vip_VIP_OnPlayerSpawn_Unregister = &FakeOnPlayerSpawnUnregister;
	__vip_VIP_OnClientLoaded_Register = &FakeOnClientLoadedRegister;
	__vip_VIP_OnClientLoaded_Unregister = &FakeOnClientLoadedUnregister;
	__vip_VIP_OnClientDisconnect_Register = &FakeOnClientDisconnectRegister;
	__vip_VIP_OnClientDisconnect_Unregister = &FakeOnClientDisconnectUnregister;
	__vip_VIP_OnVIPClientRemoved_Register = &FakeOnVIPClientRemovedRegister;
	__vip_VIP_OnVIPClientRemoved_Unregister = &FakeOnVIPClientRemovedUnregister;
}

void ResetVelocityCounter() {
	g_setVelocityCalls = 0;
	g_lastSetSlot = -1;
	g_lastSetVelocity = plg::vec3{};
}

void ResetFakes() {
	for (int32_t slot = 0; slot < kSlots; ++slot) {
		const auto index = static_cast<std::size_t>(slot);
		g_connected[index] = false;
		g_inGame[index] = false;
		g_sourceTV[index] = false;
		g_alive[index] = false;
		g_hasController[index] = false;
		g_isVIP[index] = false;
		g_flags[index] = 0;
		g_buttons[index] = 0;
		g_velocity[index] = plg::vec3{};
		g_entityMarkers[index] = 0;
		g_featureBools[index].clear();
		g_featureInts[index].clear();
	}
	g_vipLoaded = true;
	g_pistolRound = false;
	g_totalRounds = 0;
	g_pistolCalls = 0;
	g_totalRoundsCalls = 0;
	g_flagsCalls = 0;
	g_buttonsCalls = 0;
	g_clientCalls = 0;
	g_flipRoundMinOnVelocityRead = false;
	ResetVelocityCounter();
	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_spawnDelegate = nullptr;
	g_clientLoadedDelegate = nullptr;
	g_disconnectDelegate = nullptr;
	g_removedDelegate = nullptr;
	InstallFakes();
}

void MakeAlive(int32_t slot) {
	const auto index = static_cast<std::size_t>(slot);
	g_connected[index] = true;
	g_inGame[index] = true;
	g_alive[index] = true;
	g_hasController[index] = true;
}

void MakeVip(int32_t slot, int jumps, bool hasRoundMin, int roundMin) {
	const auto index = static_cast<std::size_t>(slot);
	g_isVIP[index] = true;
	g_featureBools[index]["jumps"] = true;
	g_featureInts[index]["jumps"] = jumps;
	if (hasRoundMin) {
		g_featureInts[index][vip_jumps::kRoundMinFeature] = roundMin;
	} else {
		g_featureInts[index].erase(vip_jumps::kRoundMinFeature);
	}
}

void SetAirborne(int32_t slot) {
	const auto index = static_cast<std::size_t>(slot);
	g_flags[index] = 0; // no FL_ONGROUND
	g_velocity[index].z = -50.0f;
}

void SetButtons(int32_t slot, uint64_t buttons) {
	g_buttons[static_cast<std::size_t>(slot)] = buttons;
}

void FireSpawn(int32_t slot) {
	if (g_spawnDelegate != nullptr) {
		g_spawnDelegate(slot, 2, true);
	}
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

void RunTick() {
	g_jumpsPlugin.OnPluginUpdate(std::chrono::milliseconds(16));
}

void StartPlugin() {
	ResetFakes();
	// Idempotent unload of a previous test's instance; the fakes above keep it safe.
	g_jumpsPlugin.OnPluginEnd();
	g_jumpsPlugin.OnPluginStart();
	FireMapStart();
}

void StopPlugin() {
	g_jumpsPlugin.OnPluginEnd();
}

// --- tests -----------------------------------------------------------------------------------

// Missing 'jumps_round_min' means -1 and falls back to 1: the first round grants nothing, the
// second non-pistol round grants the configured extra jump.
void TestDefaultPolicy() {
	StartPlugin();
	MakeAlive(1);
	MakeVip(1, 3, false, 0);
	SetAirborne(1);
	FireSpawn(1);

	g_pistolRound = false;
	g_totalRounds = 0;
	SetButtons(1, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(1, vip_jumps::kJumpMask); // fresh press, refused by the round gate
	RunTick();
	CheckEqInt(g_setVelocityCalls, 0, "missing round_min grants no extra jump on the first round");

	g_totalRounds = 1;
	SetButtons(1, 0); // release, arming the next press
	RunTick();
	SetButtons(1, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "the second non-pistol round grants one extra jump");
	CheckEqInt(g_lastSetSlot, 1, "the velocity of the right slot is set");
	CheckEqFloat(g_lastSetVelocity.z, vip_jumps::kJumpVelocity, "the grant writes the jump velocity");
	StopPlugin();
}

// The default fallback of 1 excludes both pistol rounds: the first one (total 0) and the
// second-half switch (total 12).
void TestPistolRoundsRefused() {
	StartPlugin();
	MakeAlive(2);
	MakeVip(2, 3, false, 0);
	SetAirborne(2);
	FireSpawn(2);

	g_pistolRound = true;
	g_totalRounds = 0;
	SetButtons(2, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(2, vip_jumps::kJumpMask); // fresh press, refused
	RunTick();
	CheckEqInt(g_setVelocityCalls, 0, "the first pistol round grants no extra jump");

	g_totalRounds = 12;
	SetButtons(2, 0); // fresh release for the second half
	RunTick();
	SetButtons(2, vip_jumps::kJumpMask); // fresh press, refused
	RunTick();
	CheckEqInt(g_setVelocityCalls, 0, "the second-half pistol round grants no extra jump");
	StopPlugin();
}

// An explicit 0 removes the round restriction: it permits both pistol halves.
void TestExplicitZeroPermitsPistol() {
	StartPlugin();
	MakeAlive(3);
	MakeVip(3, 3, true, 0); // two extra jumps of budget for the two halves
	SetAirborne(3);
	FireSpawn(3);

	g_pistolRound = true;
	g_totalRounds = 0;
	SetButtons(3, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(3, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "explicit 0 grants the extra jump on the first pistol round");
	CheckEqFloat(g_lastSetVelocity.z, vip_jumps::kJumpVelocity, "the grant writes the jump velocity");

	g_totalRounds = 12; // the second-half pistol round
	SetButtons(3, 0); // release, arming the next press
	RunTick();
	SetButtons(3, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 2, "explicit 0 grants the extra jump on the second-half pistol round");
	StopPlugin();
}

// A higher threshold counts completed rounds and never permits a pistol round.
void TestHigherThreshold() {
	StartPlugin();
	MakeAlive(4);
	MakeVip(4, 3, true, 2); // two extra jumps of budget, so the pistol check below can see a grant
	SetAirborne(4);
	FireSpawn(4);

	g_pistolRound = false;
	g_totalRounds = 1;
	SetButtons(4, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(4, vip_jumps::kJumpMask); // fresh press, refused by the threshold
	RunTick();
	CheckEqInt(g_setVelocityCalls, 0, "round_min 2 grants nothing at one completed round");

	g_totalRounds = 2;
	SetButtons(4, 0); // release, arming the next press
	RunTick();
	SetButtons(4, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "round_min 2 grants the extra jump after two completed rounds");

	g_pistolRound = true; // the same completed-round count, but a pistol round
	SetButtons(4, 0); // release, arming the next press
	RunTick();
	SetButtons(4, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "round_min 2 still refuses a pistol round");
	StopPlugin();
}

// The production contract: the threshold is read at the grant, not only by spawn/RefreshAll, so a
// group change 0 -> 1 blocks the very next grant without a respawn (and before the 1 s refresh).
void TestThresholdChangeWithoutRespawn() {
	StartPlugin();
	MakeAlive(5);
	MakeVip(5, 3, true, 0);
	SetAirborne(5);
	FireSpawn(5);

	g_pistolRound = true;
	g_totalRounds = 0;
	SetButtons(5, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(5, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "explicit 0 grants an extra jump before the change");

	// No respawn. The one-shot hook keeps the fake value at 0 through any RefreshAll and through
	// the pre-grant check, then flips it to 1 inside the press tick's GetClientAbsVelocity - before
	// the freshness read that guards the actual setter. This is independent of the 1 s refresh.
	SetButtons(5, 0); // allowed release, arming the next press
	RunTick();
	g_flipRoundMinOnVelocityRead = true;
	SetButtons(5, vip_jumps::kJumpMask); // the press the stale cached 0 would have paid out
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1,
			   "a 0 -> 1 threshold change without a respawn blocks the next grant immediately");
	StopPlugin();
}

// OnMapEnd closes the gate: no round API, client query or velocity write may happen.
void TestMapEndBlocksEverything() {
	StartPlugin();
	MakeAlive(6);
	MakeVip(6, 2, false, 0);
	SetAirborne(6);
	FireSpawn(6);

	g_pistolRound = false;
	g_totalRounds = 1;
	SetButtons(6, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(6, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "the effect works before the map ends");

	FireMapEnd();
	SetButtons(6, vip_jumps::kJumpMask);
	ResetVelocityCounter();
	g_pistolCalls = 0;
	g_totalRoundsCalls = 0;
	g_flagsCalls = 0;
	g_buttonsCalls = 0;
	RunTick();
	CheckEqInt(g_setVelocityCalls, 0, "the map-end window grants no extra jump");
	CheckEqInt(g_pistolCalls, 0, "the map-end window does not query the round API");
	CheckEqInt(g_totalRoundsCalls, 0, "the map-end window does not read the completed rounds");
	CheckEqInt(g_flagsCalls, 0, "the map-end window makes no client flags call");
	CheckEqInt(g_buttonsCalls, 0, "the map-end window makes no client buttons call");
	StopPlugin();
}

// A missing round binding must not enable the effect (fail closed); restoring it resumes the
// extra jump without a respawn.
void TestMissingRoundApiFailsClosed() {
	StartPlugin();
	MakeAlive(7);
	MakeVip(7, 3, false, 0);
	SetAirborne(7);
	FireSpawn(7);

	g_pistolRound = false;
	g_totalRounds = 1;
	SetButtons(7, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(7, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "the effect works with the round API available");

	__vip_VIP_PistolRound = nullptr;
	SetButtons(7, 0);
	g_pistolCalls = 0;
	g_totalRoundsCalls = 0;
	RunTick();
	SetButtons(7, vip_jumps::kJumpMask);
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "a missing round binding fails closed");
	CheckEqInt(g_pistolCalls, 0, "no round call is made when the API is incomplete");
	CheckEqInt(g_totalRoundsCalls, 0, "no completed-round read is made when the API is incomplete");

	__vip_VIP_PistolRound = &FakePistolRound;
	SetButtons(7, 0); // release, arming the next press
	RunTick();
	SetButtons(7, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 2, "restoring the round binding resumes the extra jump");
	StopPlugin();
}

// VIP_IsVIPLoaded() == false must fail closed even though the client/feature/round state would
// otherwise be eligible; restoring the core resumes the effect without a respawn. Slot 8 is
// otherwise unused, so no persisted jumps user state can influence the result.
void TestCoreNotLoadedFailsClosed() {
	StartPlugin();
	MakeAlive(8);
	MakeVip(8, 3, false, 0);
	SetAirborne(8);
	FireSpawn(8);

	g_pistolRound = false;
	g_totalRounds = 1;
	SetButtons(8, 0); // release, arming the next press
	ResetVelocityCounter();
	RunTick();
	SetButtons(8, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "the effect works with the VIP core loaded");

	g_vipLoaded = false;
	SetButtons(8, 0); // release
	ResetVelocityCounter();
	g_pistolCalls = 0;
	g_totalRoundsCalls = 0;
	g_clientCalls = 0;
	RunTick();
	SetButtons(8, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 0, "VIP_IsVIPLoaded false grants no extra jump");
	CheckEqInt(g_pistolCalls, 0, "no round call is made with the core unloaded");
	CheckEqInt(g_totalRoundsCalls, 0, "no completed-round read is made with the core unloaded");
	CheckEqInt(g_clientCalls, 0, "no client call is made with the core unloaded");

	g_vipLoaded = true;
	SetButtons(8, 0); // release, arming the next press
	RunTick();
	SetButtons(8, vip_jumps::kJumpMask); // fresh press
	RunTick();
	CheckEqInt(g_setVelocityCalls, 1, "restoring the VIP core resumes the extra jump");
	StopPlugin();
}

} // namespace

int main() {
	std::printf("vip_jumps runtime tests\n");
	TestDefaultPolicy();
	TestPistolRoundsRefused();
	TestExplicitZeroPermitsPistol();
	TestHigherThreshold();
	TestThresholdChangeWithoutRespawn();
	TestMapEndBlocksEverything();
	TestMissingRoundApiFailsClosed();
	TestCoreNotLoadedFailsClosed();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}

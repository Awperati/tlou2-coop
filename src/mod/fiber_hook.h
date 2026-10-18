// Runs work inside the game's own job fibers.
// Engine code reads per-fiber context from TEB FiberData; on any other thread
// (our thread, the window thread) that is 0x1E00 and the engine faults.
// We replace GetPlayer(), which game jobs call every frame, with an exact
// re-implementation that first drains our queue when called on a fiber.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <utility>

namespace coop {

// getPlayer: start of GetPlayer(); playerHandle: the {ptr,id} global it reads.
bool FiberHookInstall(uintptr_t getPlayer, uintptr_t playerHandle);
void FiberHookRemove();
// Heavy tasks run only from GameTaskGraphMgr::Update (game-logic phase).
bool PhaseHookInstall(uintptr_t taskUpdate);
// Script-owned commands (npc-move-to, script modes) run inside a real native call,
// so the engine sees a live state-script context. Hooks NativeObject::Invoke.
bool ScriptHookInstall(uintptr_t nativeInvoke, uintptr_t ssContextGetter);
void RunInScriptContext(std::function<void()> fn);
// Player bump sender (rva 0xB3C330): skip bumps against the process with this id
// (the reverse puppet), so she does not step out of the hidden player's way. 0 = off.
bool BumpHookInstall(uintptr_t target);
void SetNoBumpProcessId(uint32_t id);
uint64_t BumpsSkipped();
// Engine print functions (script error print rva 0x156B280, TTY print rva 0x1240C70) copied
// into the mod log. mode 0 off, 1 only lines containing filter, 2 everything.
bool PrintHookInstall(uintptr_t scriptPrint, uintptr_t ttyPrint);
void SetEngineLog(int mode, const std::string& filter);
// DialogManager::RequestDialog (rva 0x316C50): log requests, remap speaker names.
bool DialogHookInstall(uintptr_t target);
// Speaker remap on the path every RequestDialog overload ends in (sub_1403172A0).
// Barks/callouts: per-actor VoxController requests (sub_140331A80), remapped by owner.
bool VoxRequestHookInstall(uintptr_t target);
// M7, log-only: count checkpoint writes (sub_141200770) and restarts (sub_14105F4E0).
bool CheckpointHooksInstall(uintptr_t remember, uintptr_t restart);
// Npc demeanor setters (rva 0x1D4A120 forced, 0x1D51060 normal): the puppet NPC keeps the
// demeanor SetDemeanorOverride asks for (-1 = no override).
bool DemeanorHooksInstall(uintptr_t forced, uintptr_t normal);
void DemeanorHooksRemove();
void SetDemeanorOverride(uintptr_t npc, int demeanor);
int DemeanorGameAsked();   // last demeanor the game itself asked for on the puppet (-1 = none yet)
uint32_t DemeanorOverrides(uintptr_t* lastCaller);
// Process::SetUserId (rva 0x133EC60): while a fix id is set, a 0 id becomes it (second player).
bool UserIdHookInstall(uintptr_t setUserId);
void SetUserIdFix(uint64_t id);
// Player::Active::Update (rva 0xAD61E0): per-player call counts since the last read.
bool ActiveUpdateHookInstall(uintptr_t target);
// Trace script natives whose first argument is `sid` (0 stops); dump = {NativeObject*, calls}.
void NativeTraceStart(uint64_t sid);
std::vector<std::pair<uintptr_t, uint32_t>> NativeTraceDump();
std::string ActiveUpdateCounts();
// Clone drive: write this world stick into player 1 before each Active::Update (needs the hook).
void SetCloneWorldStick(bool on, float x, float z);
// Raw pad -> slot copy (rva 0x158F710): slot 3 gets this stick (slot space) while on.
bool RawCopyHookInstall(uintptr_t target);
void SetCloneRawStick(bool on, float x, float y);
void SetCloneRawRightStick(float x, float y);
void SetCloneRawButtons(uint64_t held);
uint32_t CloneRawHits();
// CameraControlStrafe::UpdateCameraAssist (rva 0x8215E0) runs for player 0 only.
bool CamAssistHookInstall(uintptr_t target);
uint32_t CamAssistSkips();
bool CamPickHookInstall(uintptr_t target);
bool RestartEncounterHookInstall(uintptr_t target);
bool LevelListenerHooksInstall(uintptr_t add, uintptr_t remove);
bool CommandGroupHooksInstall(uintptr_t push, uintptr_t pop);
// GetPlayer() returns this player on the calling thread until reset with 0.
void SetPlayerOverride(uintptr_t player);
uint32_t CommandGroupSkips();
std::string LevelListenerSkips();
uint32_t CamPickSkips();
uint32_t CheckpointWrites();
uint32_t CheckpointRestarts();
bool DialogFactsHookInstall(uintptr_t target, uintptr_t lockFn, uintptr_t unlockFn, uintptr_t speakerKeys, uintptr_t speakerFn);
void SetSpeakerMap(uint64_t from, uint64_t to);
void ClearSpeakerMap();
void SetDialogLog(bool on);
bool FiberHookActive();
// heavy = needs a deep stack (spawning, script natives); waits for a caller with room.
void RunOnGameFiber(std::function<void()> fn, bool heavy = false);
void FiberLogStats();

} // namespace coop

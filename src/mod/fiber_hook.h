// Runs work inside the game's own job fibers.
// Engine code reads per-fiber context from TEB FiberData; on any other thread
// (our thread, the window thread) that is 0x1E00 and the engine faults.
// We replace GetPlayer(), which game jobs call every frame, with an exact
// re-implementation that first drains our queue when called on a fiber.
#pragma once
#include <cstdint>
#include <functional>

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
bool FiberHookActive();
// heavy = needs a deep stack (spawning, script natives); waits for a caller with room.
void RunOnGameFiber(std::function<void()> fn, bool heavy = false);
void FiberLogStats();

} // namespace coop

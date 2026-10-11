// coop.dll: the co-op runtime. version.dll (loader.cpp) loads a shadow copy of it
// and calls CoopStart/CoopStop, so a rebuilt coop.dll hot-reloads in the running game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/roster.h"
#include "core/session.h"
#include "core/teleport.h"
#include "mod/game_bridge.h"
#include "mod/log.h"
#include "mod/main_thread.h"
#include "mod/input_inject.h"
#include "mod/overlay.h"

using namespace coop;

namespace {

std::atomic<bool> g_run{false};
bool g_reloaded = false;
std::thread g_thread;
HMODULE g_self = nullptr;

std::wstring ModDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);   // game folder
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

int ParseKey(const std::string& name) {
    auto n = Lower(Trim(name));
    if (n.empty()) return 'T';
    if (n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0]))) return std::toupper(n[0]);
    if (n[0] == 'f' && n.size() <= 3) { int f = std::atoi(n.c_str() + 1); if (f >= 1 && f <= 12) return VK_F1 + f - 1; }
    if (n == "space") return VK_SPACE;
    if (n == "tab") return VK_TAB;
    try { return std::stoi(n, nullptr, 0); } catch (...) { return 'T'; }
}

WORD ParsePad(const std::string& name) {
    auto n = Lower(Trim(name));
    if (n == "back") return XINPUT_GAMEPAD_BACK;
    if (n == "lb") return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (n == "rb") return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (n == "ls") return XINPUT_GAMEPAD_LEFT_THUMB;
    if (n == "rs") return XINPUT_GAMEPAD_RIGHT_THUMB;
    return 0;
}

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn LoadXInput() {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    return m ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
}

std::wstring KeyLabel(const std::string& key) { return Wide(Trim(key).empty() ? "T" : Trim(key)); }

// Which input stacks the game has loaded: tells us where synthetic input can enter.
void LogInputModules() {
    const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"GameInput.dll",
                              L"dinput8.dll", L"hid.dll", L"steam_api64.dll", L"libScePad.dll", L"PlayStationSdk.dll"};
    for (auto* n : names) Log("module %ls: %s", n, GetModuleHandleW(n) ? "loaded" : "-");
    RAWINPUTDEVICE devs[16]; UINT count = 16;
    UINT got = GetRegisteredRawInputDevices(devs, &count, sizeof(RAWINPUTDEVICE));
    for (UINT i = 0; i < got && i < 16; ++i)
        Log("raw input: usagePage 0x%X usage 0x%X flags 0x%X", devs[i].usUsagePage, devs[i].usUsage, devs[i].dwFlags);
}

void CoopMain() {
    const std::wstring dir = ModDir();
    Ini cfg, rosterIni, offsets;
    cfg.LoadFile(Narrow(dir + L"tlou2-coop\\coop.ini"));
    rosterIni.LoadFile(Narrow(dir + L"tlou2-coop\\roster.ini"));
    offsets.LoadFile(Narrow(dir + L"tlou2-coop\\offsets.ini"));
    LogOpen(dir + Wide(cfg.get("debug", "LogFile", "tlou2-coop.log")));
    Log("TLOU2 Co-op starting. Mode=%s", cfg.get("session", "Mode", "host").c_str());

    Roster roster; roster.Load(rosterIni);
    Log("roster: %zu characters, %zu chapters", roster.Playable().size(), roster.chapterCount());

    TeleportConfig tc;
    tc.holdSeconds = cfg.getFloat("teleport", "HoldSeconds", 1.5f);
    tc.farDistance = cfg.getFloat("teleport", "FarDistance", 35.f);
    tc.cooldownSeconds = cfg.getFloat("teleport", "CooldownSeconds", 5.f);
    tc.blockInCombat = cfg.getBool("teleport", "BlockInCombat", false);
    tc.transitionForceSeconds = cfg.getFloat("teleport", "TransitionForceSeconds", 0.f);
    const int holdKey = ParseKey(cfg.get("teleport", "HoldKey", "T"));
    const WORD holdPad = ParsePad(cfg.get("teleport", "HoldPad", "back"));
    const bool pressMode = cfg.getBool("accessibility", "PressInsteadOfHold", false);
    const std::wstring keyLabel = KeyLabel(cfg.get("teleport", "HoldKey", "T"));
    auto xinput = LoadXInput();

    OverlayOptions oo;
    oo.speak = cfg.getBool("accessibility", "SpeakPrompts", true);
    oo.sounds = cfg.getBool("accessibility", "SoundCues", true);
    OverlayStart(oo);

    // Let the engine finish booting before scanning memory.
    for (int i = 0; i < (g_reloaded ? 0 : 100) && g_run; ++i) Sleep(100);
    GameBridge bridge;
    bridge.Init(offsets);
    if (cfg.getBool("debug", "InputInject", true)) InputInjectInstall();

    Session session;
    std::mt19937 rng{std::random_device{}()};
    const bool isHost = Lower(cfg.get("session", "Mode", "host")) != "client";
    const uint16_t port = static_cast<uint16_t>(cfg.getInt("session", "Port", kDefaultPort));
    CharacterId wanted = CharacterFromName(cfg.get("character", "Character", "random"));
    CharacterId myCharacter = CharacterId::None;

    std::map<uint8_t, PlayerState> remote;
    std::optional<Objective> objective;
    TeleportController teleporter(tc);
    TransitionGate gate(tc);

    Session::Callbacks cb;
    cb.onPeerJoined = [&](uint8_t slot, const Hello& h) {
        Log("player %u joined: %s", slot, h.playerName.c_str());
        OverlayNotify(Wide(h.playerName) + L" joined");
        OverlayCue(2);
        if (objective) session.SendObjective(*objective);
    };
    cb.onPeerLeft = [&](uint8_t slot) {
        Log("player %u left", slot);
        remote.erase(slot);
        gate.ForgetPlayer(slot);
        OverlayNotify(slot == 0 ? L"Host left the session" : L"Your partner left");
        OverlayCue(3);
    };
    cb.onWelcome = [&](const Welcome& w) {
        auto pack = bridge.CurrentWorldPack();
        myCharacter = roster.AssignForSlot(pack, w.slot, wanted, rng);
        Log("joined as slot %u, character %s (chapter '%s')", w.slot, CharacterName(myCharacter), pack.c_str());
        session.SendCharacterSelect(myCharacter);
        OverlayNotify(L"Connected. You are " + Wide(CharacterName(myCharacter)));
    };
    cb.onRejected = [&](const std::string& why) { Log("rejected: %s", why.c_str()); OverlayNotify(L"Could not join: " + Wide(why)); };
    cb.onPlayerState = [&](uint8_t slot, const PlayerState& s) {
        remote[slot] = s;
        if (isHost) gate.ReportPosition(slot, s.pos);
        bridge.DrivePuppet(slot, s);
    };
    cb.onObjective = [&](const Objective& o) { objective = o; };
    cb.onTeleportRequest = [&](uint8_t slot) {
        auto self = bridge.ReadLocalPlayer(0);
        if (!self) { Log("teleport request from %u ignored: host position unknown", slot); return; }
        auto t = ComputeTeleportTarget(self->pos, self->yaw, objective ? &*objective : nullptr, slot, tc);
        session.SendTeleportTo(slot, t);
        gate.MarkTeleported(slot);
        Log("teleporting player %u to %.1f %.1f %.1f", slot, t.pos.x, t.pos.y, t.pos.z);
    };
    cb.onTeleportTo = [&](const TeleportTo& t) {
        bool ok = bridge.TeleportLocalPlayer(t);
        Log("teleport to %.1f %.1f %.1f: %s", t.pos.x, t.pos.y, t.pos.z, ok ? "done" : "not mapped yet");
        OverlayCue(1);
    };
    cb.onCharacterSelect = [&](uint8_t slot, CharacterId c) { Log("player %u is %s", slot, CharacterName(c)); };
    session.setCallbacks(cb);

    if (isHost) {
        if (!session.StartHost(cfg.get("session", "SessionName", "TLOU2 Co-op"), port,
                               static_cast<uint8_t>(cfg.getInt("session", "MaxPlayers", 2)))) {
            Log("could not open UDP port %u", port);
            OverlayNotify(L"Co-op: could not open the network port");
        } else {
            Log("hosting on UDP %u", port);
            OverlayNotify(L"Co-op session open. Waiting for a partner.");
        }
    } else {
        std::string addr = cfg.get("session", "HostAddress");
        uint16_t hostPort = port;
        while (addr.empty() && g_run) {
            OverlaySetStatus(L"Searching LAN for a host...");
            auto hosts = Session::Discover(1500);
            if (!hosts.empty()) { addr = hosts[0].address; hostPort = hosts[0].info.gamePort; Log("found host %s (%s)", addr.c_str(), hosts[0].info.sessionName.c_str()); }
        }
        Hello hello; hello.playerName = cfg.get("session", "PlayerName", "Player"); hello.wanted = wanted;
        if (!session.StartClient(addr, hostPort, hello)) Log("could not start client to %s", addr.c_str());
    }

    uint32_t tick = 0;
    ULONGLONG last = GetTickCount64(), lastSend = 0, lastStatus = 0, lastDiscover = 0;
    uintptr_t lastPlayerObj = 1;
    ULONGLONG lastPosLog = 0, lastCmdPoll = 0, lastLevelPoll = 0;
    bool mainHooked = false, testWasDown = false;
    const int testKey = cfg.get("debug", "TestTeleportKey", "F10").empty() ? 0 : ParseKey(cfg.get("debug", "TestTeleportKey", "F10"));
    bool prevPressed = false, toggled = false;
    PromptState prevPrompt = PromptState::Hidden;

    while (g_run) {
        ULONGLONG now = GetTickCount64();
        float dt = static_cast<float>(now - last) / 1000.f;
        last = now;
        ++tick;
        session.Poll();

        auto self = bridge.ReadLocalPlayer(tick);
        if (self) {
            self->character = myCharacter;
            if (now - lastSend >= 50) { session.SendPlayerState(*self); lastSend = now; }
            if (isHost) gate.ReportPosition(0, self->pos);
        }

        if (isHost) {
            auto o = bridge.CurrentObjective();
            if (o && (!objective || o->id != objective->id || o->isTransition != objective->isTransition)) {
                objective = o;
                session.SendObjective(*o);
                if (o->isTransition && session.peerCount()) {
                    gate.Begin(*o);
                    Log("transition held until everyone arrives");
                }
            }
            // Holding the engine's level load needs a hook (see docs/ROADMAP.md, M4).
            if (gate.active() && gate.Ready(dt)) Log("transition released");
        }

        // Hold-to-teleport input (keyboard when focused, or controller).
        bool pressed = false;
        if (bridge.GameWindowFocused() && (GetAsyncKeyState(holdKey) & 0x8000)) pressed = true;
        if (xinput && holdPad) {
            XINPUT_STATE xs{};
            if (xinput(0, &xs) == ERROR_SUCCESS && (xs.Gamepad.wButtons & holdPad)) pressed = true;
        }
        if (pressMode) { if (pressed && !prevPressed) toggled = !toggled; prevPressed = pressed; pressed = toggled; }

        // No teleport prompt while a cinematic plays: it owns the characters.
        if (self && session.connected() && !bridge.InCinematic()) {
            TeleportController::Inputs in;
            in.dt = dt;
            in.selfPos = self->pos;
            in.holdPressed = pressed;
            in.inCombat = bridge.InCombat();
            in.dead = self->flags & 8;
            in.objective = (!isHost && objective) ? &*objective : nullptr;
            auto anchor = remote.find(isHost ? 1 : 0);
            if (anchor != remote.end()) { in.anchorPos = anchor->second.pos; in.anchorValid = true; }

            if (teleporter.Update(in)) {
                toggled = false;
                if (isHost && anchor != remote.end()) {
                    bridge.TeleportLocalPlayer(ComputeTeleportTarget(anchor->second.pos, anchor->second.yaw, nullptr, 0, tc));
                } else {
                    session.SendTeleportRequest();
                }
                OverlayCue(1);
            }
            auto st = teleporter.state();
            const std::wstring verb = pressMode ? L"Press " : L"Hold ";
            if (st == PromptState::Available) OverlaySetPrompt(verb + keyLabel + L" to teleport to your partner", -1);
            else if (st == PromptState::Charging) OverlaySetPrompt(verb + keyLabel + L" to teleport to your partner", teleporter.progress());
            else if (st == PromptState::Blocked) OverlaySetPrompt(std::wstring(L"Teleport unavailable: ") + Wide(teleporter.reason()), -1);
            else OverlayClearPrompt();
            if (st == PromptState::Available && prevPrompt == PromptState::Hidden) OverlayCue(0);
            prevPrompt = st;
        } else {
            OverlayClearPrompt();
        }

        if (!mainHooked) mainHooked = MainThreadInit();

        // Command file: lets a developer drive tests without touching the game window.
        bool cmdTeleport = false;
        if (now - lastCmdPoll >= 500) {
            lastCmdPoll = now;
            std::wstring cmdPath = dir + L"tlou2-coop\\cmd.txt";
            Ini cmd;
            if (GetFileAttributesW(cmdPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::string text;
                if (FILE* f = _wfopen(cmdPath.c_str(), L"rb")) {
                    char b[256]{}; size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); text.assign(b, n);
                }
                DeleteFileW(cmdPath.c_str());
                // One command per line (or ';'). Input lines queue up and play in order.
                for (char& c : text) if (c == ';' || c == '\r') c = '\n';
                for (auto& raw : SplitList(text, '\n')) {
                    std::string line = Lower(Trim(raw));
                    Log("command: %s", line.c_str());
                    if (line == "teleport") cmdTeleport = true;
                    else if (line == "modules") LogInputModules();
                    else if (line == "inputstats") InputLogStats();
                    else if (line == "buddy") bridge.QueueBuddyReport();
                    else if (line == "levels") bridge.DumpLevels();
                    else if (line == "tasks") bridge.DumpTasks();
                    else if (line == "cine") Log("cinematic: %s", bridge.InCinematic() ? "playing" : "not playing");
                    else if (line == "buddy here" && self) {
                        // 2 m to the player's right.
                        Vec3 p = {self->pos.x + std::cos(self->yaw) * 2.f, self->pos.y + 0.2f, self->pos.z - std::sin(self->yaw) * 2.f};
                        bridge.QueueBuddyTeleport(p, self->yaw);
                    }
                    else if (!InputScriptAdd(line)) Log("command not understood: %s", line.c_str());
                }
            }
        }

        // Solo test: teleport 3 m ahead through the engine. Proves M2 without a partner.
        bool testDown = cmdTeleport || (testKey && bridge.GameWindowFocused() && (GetAsyncKeyState(testKey) & 0x8000));
        if (testDown && !testWasDown && self) {
            TeleportTo t;
            t.yaw = self->yaw;
            t.pos = {self->pos.x + std::sin(self->yaw) * 3.f, self->pos.y + 0.2f, self->pos.z + std::cos(self->yaw) * 3.f};
            Log("test teleport from %.2f %.2f %.2f yaw %.2f", self->pos.x, self->pos.y, self->pos.z, self->yaw);
            bridge.TeleportLocalPlayer(t);
        }
        testWasDown = testDown;

        if (now - lastLevelPoll >= 500) { lastLevelPoll = now; bridge.PollLevels(); bridge.PollTasks(); bridge.PollCinematic(); }

        if (self && now - lastPosLog >= 5000) {
            lastPosLog = now;
            Log("player pos %.2f %.2f %.2f yaw %.2f", self->pos.x, self->pos.y, self->pos.z, self->yaw);
        }

        if (bridge.needsDiscovery() && now - lastDiscover >= 1000) {
            lastDiscover = now;
            uintptr_t obj = bridge.PlayerObject();
            if (obj != lastPlayerObj) { Log("player object: %p", reinterpret_cast<void*>(obj)); lastPlayerObj = obj; }
            bridge.DiscoverTick();
        }

        if (now - lastStatus >= 500) {
            lastStatus = now;
            std::wstring s = L"Co-op ";
            if (isHost) s += L"host - " + std::to_wstring(session.peerCount()) + L" partner(s)";
            else if (session.connected()) s += L"client - ping " + std::to_wstring(session.rttMs(0)) + L" ms";
            else s += L"connecting...";
            if (!bridge.features().localPlayer) s += L" - game link not mapped";
            OverlaySetStatus(s);
        }
        Sleep(16);
    }
    session.Stop();
    bridge.Shutdown();
    InputInjectRemove();
    MainThreadShutdown();
    OverlayStop();
    Log("TLOU2 Co-op stopped");
    LogClose();
}

} // namespace

extern "C" __declspec(dllexport) void CoopStart(int reloaded) {
    if (g_run) return;
    g_reloaded = reloaded != 0;
    g_run = true;
    g_thread = std::thread(CoopMain);
}

// Called by the loader (never under the loader lock): stops and joins everything.
extern "C" __declspec(dllexport) void CoopStop() {
    if (!g_run) return;
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_self = mod;
    } else if (reason == DLL_PROCESS_DETACH) {
        g_run = false;
        if (g_thread.joinable()) g_thread.detach();   // process exit: never join here
    }
    return TRUE;
}

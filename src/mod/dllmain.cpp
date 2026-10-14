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
#include <sstream>
#include <thread>

#include "core/config.h"
#include "core/roster.h"
#include "core/session.h"
#include "core/teleport.h"
#include "mod/fiber_hook.h"
#include "mod/game_bridge.h"
#include "mod/log.h"
#include "mod/main_thread.h"
#include "mod/input_inject.h"
#include "mod/overlay.h"

using namespace coop;

namespace {

std::atomic<bool> g_run{false};
bool g_reloaded = false;
// Heap-allocated and never destroyed: a joinable std::thread destroyed during process
// exit calls std::terminate (seen as a second crash dump after a game crash).
std::thread* g_thread = nullptr;
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

// Self-puppet survives hot reloads and restarts through a marker file: without it, every
// redeploy dropped the mode and the companion wandered off with her AI back.
void SetSelfPuppetMarker(const std::wstring& dir, bool on) {
    std::wstring path = dir + L"tlou2-coop\\selfpuppet.on";
    if (on) { if (FILE* f = _wfopen(path.c_str(), L"wb")) fclose(f); }
    else DeleteFileW(path.c_str());
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
        bridge.ReleasePuppet();
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
    ULONGLONG lastPosLog = 0, lastCmdPoll = 0, lastLevelPoll = 0, lastHide = 0, lastStance = 0;
    bool mainHooked = false, testWasDown = false;
    std::string keepLoad;   // package re-requested every 200 ms (want-loads look rebuilt each frame)
    unsigned long long keepLoadLog = 0;
    bool companionOff = GetFileAttributesW((dir + L"tlou2-coop\\companion.off").c_str()) != INVALID_FILE_ATTRIBUTES;
    unsigned long long companionAt = 0;
    bool selfPuppet = GetFileAttributesW((dir + L"tlou2-coop\\selfpuppet.on").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (selfPuppet) Log("selfpuppet: on (restored)");
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
                    static char b[16384]; size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); text.assign(b, n);
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
                    else if (line == "fiberstats") FiberLogStats();
                    else if (line == "buddy") bridge.QueueBuddyReport();
                    else if (line == "levels") bridge.DumpLevels();
                    else if (line == "tasks") bridge.DumpTasks();
                    else if (line == "spawnlist") bridge.QueueSpawnListDump();
                    else if (line.rfind("native ", 0) == 0) bridge.QueueNativeCall(line.substr(7));
                    else if (line == "scriptctx") bridge.QueueScriptContextReport();
                    else if (line == "links") bridge.QueuePlayerLinks();
                    else if (line == "hideplayer") bridge.QueuePlayerVisible(false);
                    // Reverse puppet: this PC's story companion follows the (hidden) local
                    // player, so the partner sees the companion where they move.
                    else if (line == "selfpuppet on") { selfPuppet = true; bridge.QueuePlayerVisible(false); SetSelfPuppetMarker(dir, true); Log("selfpuppet: on"); }
                    else if (line == "selfpuppet off") { selfPuppet = false; bridge.ReleasePuppet(); bridge.QueuePlayerVisible(true); SetSelfPuppetMarker(dir, false); Log("selfpuppet: off"); }
                    else if (line == "showplayer") bridge.QueuePlayerVisible(true);
                    else if (line == "showall") bridge.QueueShowAllAttachables();
                    else if (line == "showcompanion") bridge.QueueShowCompanion();
                    else if (line.rfind("openfiles", 0) == 0) {
                        // Read-only: which files this process has open (e.g. mounted .psarc archives).
                        std::string flt = line.size() > 10 ? Trim(line.substr(10)) : std::string();
                        int n = 0;
                        for (uintptr_t hv = 4; hv < 0x20000; hv += 4) {
                            HANDLE fh = reinterpret_cast<HANDLE>(hv);
                            if (GetFileType(fh) != FILE_TYPE_DISK) continue;
                            wchar_t wp[1024];
                            DWORD len = GetFinalPathNameByHandleW(fh, wp, 1024, 0);
                            if (!len || len >= 1024) continue;
                            char p[2048]; WideCharToMultiByte(CP_UTF8, 0, wp, -1, p, sizeof(p), nullptr, nullptr);
                            std::string s = p, low = s;
                            for (auto& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                            if (!flt.empty() && low.find(flt) == std::string::npos) continue;
                            Log("openfile %llx %s", static_cast<unsigned long long>(hv), s.c_str()); ++n;
                        }
                        Log("openfiles: %d matching '%s'", n, flt.c_str());
                    }
                    else if (line.rfind("keepload ", 0) == 0) { keepLoad = Trim(line.substr(9)); Log("keepload: %s", keepLoad.c_str()); }
                    else if (line == "keepload") { keepLoad.clear(); Log("keepload: off"); }
                    else if (line.rfind("peek ", 0) == 0) {   // peek <hex addr> [bytes]: read-only memory dump
                        std::istringstream ss(line.substr(5)); std::string a; size_t n = 64; ss >> a >> n;
                        uintptr_t addr = std::stoull(a, nullptr, 16); n = std::min<size_t>(n, 512);
                        std::vector<uint8_t> buf(n);
                        SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), buf.data(), n, &got)) { Log("peek %llx: unreadable", static_cast<unsigned long long>(addr)); }
                        for (size_t o = 0; o < got; o += 16) {
                            char hex[64] = {}; int p = 0;
                            for (size_t k = 0; k < 16 && o + k < got; ++k) p += snprintf(hex + p, sizeof(hex) - p, "%02x ", buf[o + k]);
                            Log("peek %llx: %s", static_cast<unsigned long long>(addr + o), hex);
                        }
                    }
                    else if (line.rfind("enginelog", 0) == 0) {   // enginelog off | all | <text filter>
                        std::string a = line.size() > 10 ? Trim(line.substr(10)) : std::string();
                        if (a.empty() || a == "off") SetEngineLog(0, ""); else if (a == "all") SetEngineLog(2, ""); else SetEngineLog(1, a);
                        Log("enginelog: %s", a.empty() ? "off" : a.c_str());
                    }
                    else if (line == "mounts") {
                        // Dearchiver mount list (head at rva 0x36A8658), as the game's own dump (sub_141271220)
                        // prints it: node+0 next, +64 file handle id, +184 char* mount point, +256 u16 order.
                        // Archive name: sub_14220DA60(*rva 0x383ED70, fh) -> entry; name at +104 (inline or pointer).
                        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                        SIZE_T got = 0; uintptr_t node = 0;
                        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + 0x36A8658), &node, 8, &got);
                        int n = 0;
                        for (; node && n < 200; ++n) {
                            uint8_t m[272] = {};
                            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(node), m, sizeof(m), &got)) break;
                            uintptr_t mp = *reinterpret_cast<uintptr_t*>(m + 184);
                            char point[128] = {};
                            if (mp) ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(mp), point, 127, &got);
                            Log("mount %d: at '%s' order %u fh %u", n, point, *reinterpret_cast<uint16_t*>(m + 256), *reinterpret_cast<uint32_t*>(m + 64));
                            node = *reinterpret_cast<uintptr_t*>(m);
                        }
                        Log("mounts: %d (dearchiver list)", n);
                        // GameLoading's mounted archives (game-loading.cpp): count at rva 0x9363E60, 16-byte
                        // entries at rva 0x9363E70 = {mount object*, flags}; object+8 = inline archive path.
                        uint64_t cnt = 0;
                        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + 0x9363E60), &cnt, 8, &got);
                        for (uint64_t k = 0; k < cnt && k < 100; ++k) {
                            uintptr_t obj = 0;
                            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + 0x9363E70 + 16 * k), &obj, 8, &got);
                            char path[200] = {};
                            if (obj) ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(obj + 8), path, 199, &got);
                            const char* tail = std::strstr(path, "main/");
                            Log("archive %llu: %s", static_cast<unsigned long long>(k), obj ? (tail ? tail + 5 : path) : "(empty)");
                        }
                    }
                    else if (line.rfind("findwant ", 0) == 0) {
                        // LoadRegistry (*rva 0x3603788): want lists at +0 and +168, each {u32 count; ...; WantLoadInfo** at +8}.
                        // WantLoadInfo: +8 sid, +32 u16 type, +34 flags. Read-only.
                        std::string nm = Trim(line.substr(9));
                        uint64_t sid = 0xcbf29ce484222325ull; for (unsigned char ch : nm) { sid ^= ch; sid *= 0x100000001b3ull; }
                        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                        uintptr_t reg = 0; SIZE_T got = 0;
                        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + 0x3603788), &reg, 8, &got);
                        int hits = 0;
                        for (int li = 0; li < 2 && reg; ++li) {
                            uintptr_t lst = reg + (li == 0 ? 168 : 0);
                            uint32_t cnt = 0; uintptr_t arr = 0;
                            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(lst), &cnt, 4, &got);
                            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(lst + 8), &arr, 8, &got);
                            for (uint32_t k = 0; k < cnt && k < 4096; ++k) {
                                uintptr_t e = 0; uint8_t w[40] = {};
                                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(arr + 8 * k), &e, 8, &got) || !e) continue;
                                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(e), w, 40, &got)) continue;
                                if (*reinterpret_cast<uint64_t*>(w + 8) != sid) continue;
                                ++hits;
                                Log("findwant: '%s' in list %s entry %llx type %u flags %02x def %llx", nm.c_str(), li == 0 ? "+168" : "+0",
                                    static_cast<unsigned long long>(e), *reinterpret_cast<uint16_t*>(w + 32), w[34],
                                    static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(w + 16)));
                            }
                            Log("findwant: list %s has %u wants", li == 0 ? "+168" : "+0", cnt);
                        }
                        if (!hits) Log("findwant: '%s' not wanted", nm.c_str());
                    }
                    else if (line == "companion off" || line == "companion on") {
                        companionOff = line == "companion off";
                        std::wstring mk = dir + L"tlou2-coop\\companion.off";
                        if (companionOff) { if (HANDLE fh = CreateFileW(mk.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); fh != INVALID_HANDLE_VALUE) CloseHandle(fh); }
                        else DeleteFileW(mk.c_str());
                        if (companionOff && selfPuppet) { selfPuppet = false; bridge.ReleasePuppet(); SetSelfPuppetMarker(dir, false); Log("selfpuppet: off (companion off)"); }
                        bridge.QueueCompanionHidden(companionOff);
                    }
                    else if (line == "voxnames") bridge.QueueVoxNames();
                    else if (line.rfind("look ", 0) == 0) bridge.StartLookSwap(Trim(line.substr(5)));
                    else if (line.rfind("unwant ", 0) == 0) {   // unwant <name> [times]
                        std::istringstream ss(line.substr(7)); std::string nm; int times = 1; ss >> nm >> times;
                        bridge.QueueUnwant(nm, std::clamp(times, 1, 2000));
                    }
                    else if (line.rfind("loadactor ", 0) == 0) bridge.QueueLoadActor(Trim(line.substr(10)));
                    else if (line.rfind("loadpkg ", 0) == 0) bridge.QueueLoadPackage(Trim(line.substr(8)), true);
                    else if (line.rfind("pkgloaded ", 0) == 0) bridge.QueueLoadPackage(Trim(line.substr(10)), false);
                    else if (line.rfind("selfoffset ", 0) == 0) { bridge.SetSelfOffset(std::stof(line.substr(11))); Log("selfoffset: %s m", line.substr(11).c_str()); }
                    else if (line.rfind("selfdirlead ", 0) == 0) { bridge.selfDirLead_ = std::stof(line.substr(12)); Log("selfdirlead: %s m", line.substr(12).c_str()); }
                    else if (line.rfind("selfspeed ", 0) == 0) {   // selfspeed <walk m/s> <run m/s> <gain>
                        std::istringstream ss(line.substr(10)); ss >> bridge.selfWalkSpeed_ >> bridge.selfRunSpeed_ >> bridge.selfSpeedGain_;
                        Log("selfspeed: walk %.2f run %.2f gain %.2f", bridge.selfWalkSpeed_, bridge.selfRunSpeed_, bridge.selfSpeedGain_);
                    }
                    else if (line.rfind("selfstyle ", 0) == 0) { bridge.selfStyle_ = std::stoi(line.substr(10)); Log("selfstyle: %d", bridge.selfStyle_); }
                    else if (line == "selfdir on" || line == "selfdir off") { bridge.SetSelfDir(line == "selfdir on"); Log("selfdir: %s", line.substr(8).c_str()); }
                    else if (line.rfind("selfnudge ", 0) == 0) { bridge.SetSelfNudge(std::stof(line.substr(10))); Log("selfnudge: %s", line.substr(10).c_str()); }
                    else if (line.rfind("selflead ", 0) == 0) { bridge.SetSelfLead(std::stof(line.substr(9))); Log("selflead: %s m", line.substr(9).c_str()); }
                    else if (line == "nofade on" || line == "nofade off") { bridge.SetNoCameraFade(line == "nofade on"); Log("nofade: %s", line.substr(7).c_str()); }
                    else if (line == "demeanors") bridge.QueueDemeanorNames();
                    else if (line.rfind("npcarg ", 0) == 0) bridge.QueueResolveNpcArg(Trim(line.substr(7)));
                    else if (line.rfind("moveto ", 0) == 0 || line.rfind("movetox ", 0) == 0) {
                        bool owned = line.rfind("moveto ", 0) == 0;   // movetox: no script owner
                        std::istringstream ms(line.substr(owned ? 7 : 8)); std::string name; float m = 6.f; int motion = 1, goal = 0;
                        ms >> name >> m >> motion >> goal;
                        bridge.QueueMoveTo(name, m, motion, goal, owned);
                    }
                    else if (line == "release") bridge.ReleasePuppet();
                    else if (line.rfind("puppet ", 0) == 0) {
                        std::string target = Trim(line.substr(7));
                        bridge.SetPuppetSpawner(target == "buddy" ? std::string() : target);
                        Log("puppet: now driving %s", target.c_str());
                    }
                    else if (line.rfind("spawned ", 0) == 0) {
                        std::istringstream ps(line.substr(8)); std::string name, mode; ps >> name >> mode;
                        if (mode == "far") bridge.QueueSpawned(name, true, 8.f);
                        else bridge.QueueSpawned(name, mode == "here");
                    }
                    else if (line.rfind("proc ", 0) == 0) {
                        std::istringstream ps(line.substr(5)); std::string addr, mode; ps >> addr >> mode;
                        bridge.QueueProcess(static_cast<uintptr_t>(std::stoull(addr, nullptr, 16)), mode == "here");
                    }
                    else if (line.rfind("spawners", 0) == 0) bridge.DumpSpawners(Trim(line.substr(8)));
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

        if (now - lastStance >= 200) {
            lastStance = now; bridge.PollStance(); bridge.PollLookSwap();
            if (companionOff && now - companionAt > 2000) { companionAt = now; bridge.QueueCompanionHidden(true); }
            if (!keepLoad.empty()) {
                bool logIt = now - keepLoadLog > 3000;
                if (logIt) keepLoadLog = now;
                bridge.QueueLoadPackage(keepLoad, true, logIt);
            }
        }
        if (selfPuppet && self) {
            bridge.DrivePuppet(0, *self);
            // The game re-shows weapons when they are drawn or swapped; re-hide each second.
            if (now - lastHide >= 1000) { lastHide = now; bridge.QueuePlayerVisible(false, true); }
        }

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
    if (!selfPuppet) bridge.ReleasePuppet();   // a reload keeps her; the new instance retakes her
    for (int i = 0; i < 20; ++i) Sleep(50);   // let the release run in a script call
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
    g_thread = new std::thread(CoopMain);
}

// Called by the loader (never under the loader lock): stops and joins everything.
extern "C" __declspec(dllexport) void CoopStop() {
    if (!g_run) return;
    g_run = false;
    if (g_thread && g_thread->joinable()) g_thread->join();
    delete g_thread;
    g_thread = nullptr;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_self = mod;
    } else if (reason == DLL_PROCESS_DETACH) {
        g_run = false;
        if (g_thread && g_thread->joinable()) g_thread->detach();   // process exit: never join here
    }
    return TRUE;
}

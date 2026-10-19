#include "mod/hw_watch.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace coop {

namespace {
constexpr int kMaxHits = 256;
struct Hit {
    std::atomic<uint64_t> key{0};   // rip | slot << 62 (user-mode rips fit in 47 bits)
    std::atomic<uint32_t> count{0};
    uint64_t callers[3] = {};
    // Per rdi (often `this`, e.g. the controller in sub_140C31340): hits, and hits where the
    // watched bytes were not all zero right after the access.
    std::atomic<uint64_t> rdi[2] = {};
    std::atomic<uint32_t> rdiHits[2] = {}, rdiSet[2] = {};
    std::atomic<uint32_t> rdiOther{0};
};
Hit g_hits[kMaxHits];
std::atomic<uint32_t> g_dropped{0};
std::atomic<bool> g_armed{false};
// The registers every thread should have now. The handler writes them into the context it
// continues with: a thread trapped while they were being cleared would otherwise get the old
// ones back from its exception context (seen offline: late traps after disarm, 7 of 10 runs).
DWORD64 g_dr[4] = {};
std::atomic<DWORD64> g_dr7{0};
PVOID g_veh = nullptr;
uintptr_t g_base = 0, g_end = 0;
HwWatchSlot g_slots[4];
int g_slotCount = 0;

bool InImage(uint64_t v) { return v >= g_base && v < g_end; }

// A return address: the bytes before it are a call (E8 rel32, FF 15 / FF /2 forms).
bool LooksLikeReturn(uint64_t v) {
    if (!InImage(v) || v < g_base + 8) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(v);
    if (p[-5] == 0xE8) return true;
    if (p[-6] == 0xFF && p[-5] == 0x15) return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;
    return false;
}

bool SlotSet(int slot) {
    const auto* p = reinterpret_cast<const uint8_t*>(g_slots[slot].addr);
    for (int i = 0; i < g_slots[slot].len; ++i) if (p[i]) return true;
    return false;
}

void Count(Hit& e, int slot, const CONTEXT* c) {
    e.count.fetch_add(1, std::memory_order_relaxed);
    bool set = SlotSet(slot);
    for (int j = 0; j < 2; ++j) {
        uint64_t cur = e.rdi[j].load(std::memory_order_acquire);
        if (cur == 0) { uint64_t z = 0; if (e.rdi[j].compare_exchange_strong(z, c->Rdi)) cur = c->Rdi; else cur = z; }
        if (cur == c->Rdi) { e.rdiHits[j].fetch_add(1, std::memory_order_relaxed); if (set) e.rdiSet[j].fetch_add(1, std::memory_order_relaxed); return; }
    }
    e.rdiOther.fetch_add(1, std::memory_order_relaxed);
}

void Record(int slot, const CONTEXT* c) {
    uint64_t key = (c->Rip & 0x3FFFFFFFFFFFFFFFull) | (static_cast<uint64_t>(slot) << 62);
    uint32_t h = static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 56) % kMaxHits;
    for (int i = 0; i < kMaxHits; ++i) {
        Hit& e = g_hits[(h + i) % kMaxHits];
        uint64_t cur = e.key.load(std::memory_order_acquire);
        if (cur == key) { Count(e, slot, c); return; }
        if (cur == 0) {
            uint64_t z = 0;
            if (e.key.compare_exchange_strong(z, key)) {
                // First hit at this instruction: up to 3 return addresses from the stack.
                int n = 0;
                const auto* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
                __try {
                    for (int k = 0; k < 96 && n < 3; ++k) if (LooksLikeReturn(sp[k])) e.callers[n++] = sp[k];
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Count(e, slot, c);
                return;
            }
            if (z == key) { Count(e, slot, c); return; }
        }
    }
    ++g_dropped;
}

thread_local bool t_inVeh = false;
LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    // No B0-B3 bit: a trap that was in flight while the registers were being cleared still has
    // no trap-flag single step behind it (BS bit 14, TF 0x100), so it is ours as well.
    if (!(dr6 & 0xF) && ((dr6 & 0x4000) || (c->EFlags & 0x100))) return EXCEPTION_CONTINUE_SEARCH;
    // SlotSet reads the watched bytes, which traps again inside this handler: count nothing then.
    if (!t_inVeh && g_armed.load(std::memory_order_relaxed)) {
        t_inVeh = true;
        for (int i = 0; i < 4; ++i) if (dr6 & (1ull << i)) Record(i, c);
        t_inVeh = false;
    }
    c->Dr6 = 0;
    c->Dr0 = g_dr[0]; c->Dr1 = g_dr[1]; c->Dr2 = g_dr[2]; c->Dr3 = g_dr[3];
    c->Dr7 = g_dr7.load();
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    return EXCEPTION_CONTINUE_EXECUTION;   // data breakpoints trap after the access
}

// Sets the same debug registers on every thread of the process except this one.
int SetAllThreads(const DWORD64 dr[4], DWORD64 dr7) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int done = 0;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = dr[0]; c.Dr1 = dr[1]; c.Dr2 = dr[2]; c.Dr3 = dr[3]; c.Dr6 = 0; c.Dr7 = dr7;
                if (SetThreadContext(th, &c)) ++done;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return done;
}

void Disarm() {
    const DWORD64 zero[4] = {};
    g_armed = false;
    g_dr7 = 0;
    SetAllThreads(zero, 0);
}
} // namespace

bool HwWatchArm(const std::vector<HwWatchSlot>& slots, std::string& err) {
    if (g_armed) { err = "already armed"; return false; }
    if (slots.empty() || slots.size() > 4) { err = "1-4 slots"; return false; }
    if (!g_base) {
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        g_end = g_base + nt->OptionalHeader.SizeOfImage;
    }
    DWORD64 dr[4] = {}, dr7 = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& s = slots[i];
        uint64_t lenBits;
        switch (s.len) { case 1: lenBits = 0; break; case 2: lenBits = 1; break; case 4: lenBits = 3; break; case 8: lenBits = 2; break; default: err = "bad length"; return false; }
        if (!s.addr || s.addr % s.len) { err = "address missing or not aligned: " + s.name; return false; }
        dr[i] = s.addr;
        uint64_t rw = s.writeOnly ? 1 : 3;
        dr7 |= 1ull << (i * 2);                                  // local enable
        dr7 |= (rw | (lenBits << 2)) << (16 + i * 4);
        g_slots[i] = s;
    }
    g_slotCount = static_cast<int>(slots.size());
    for (auto& h : g_hits) {
        h.key = 0; h.count = 0; std::memset(h.callers, 0, sizeof(h.callers)); h.rdiOther = 0;
        for (int j = 0; j < 2; ++j) { h.rdi[j] = 0; h.rdiHits[j] = 0; h.rdiSet[j] = 0; }
    }
    g_dropped = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &Veh);
    if (!g_veh) { err = "no exception handler"; return false; }
    for (int i = 0; i < 4; ++i) g_dr[i] = dr[i];
    g_dr7 = dr7;
    g_armed = true;
    int n = SetAllThreads(dr, dr7);
    if (!n) { Disarm(); err = "no thread took the registers"; return false; }
    err = std::to_string(n) + " threads";
    return true;
}

bool HwWatchArmed() { return g_armed.load(); }

std::vector<std::string> HwWatchReport() {
    Disarm();
    Sleep(20);   // exceptions already in flight finish counting
    std::vector<std::string> out;
    auto where = [](uint64_t v) {
        char b[48];
        if (InImage(v)) snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v - g_base));
        else snprintf(b, sizeof(b), "abs %p", reinterpret_cast<void*>(v));
        return std::string(b);
    };
    for (int s = 0; s < g_slotCount; ++s) {
        int lines = 0;
        for (auto& h : g_hits) {
            uint64_t k = h.key.load();
            if (!k || static_cast<int>(k >> 62) != s) continue;
            std::string line = "dr" + std::to_string(s) + " " + g_slots[s].name + ": rva " + where(k & 0x3FFFFFFFFFFFFFFFull) + " x" + std::to_string(h.count.load());
            std::string from;
            for (uint64_t c : h.callers) if (c) from += (from.empty() ? "" : ", ") + where(c);
            if (!from.empty()) line += " (from " + from + ")";
            for (int j = 0; j < 2; ++j) if (uint64_t r = h.rdi[j].load()) {
                char b[96];
                snprintf(b, sizeof(b), "; rdi %p x%u, set x%u", reinterpret_cast<void*>(r), h.rdiHits[j].load(), h.rdiSet[j].load());
                line += b;
            }
            if (h.rdiOther) line += "; other rdi x" + std::to_string(h.rdiOther.load());
            out.push_back(line);
            ++lines;
        }
        if (!lines) out.push_back("dr" + std::to_string(s) + " " + g_slots[s].name + ": no access");
    }
    if (g_dropped) out.push_back("hits dropped (table full): " + std::to_string(g_dropped.load()));
    return out;
}

void HwWatchStop() {
    if (g_armed) Disarm();
    if (g_veh) { Sleep(50); RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
}

} // namespace coop

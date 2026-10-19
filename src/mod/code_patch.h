// Writing over code that other threads may be running (2026-10-20).
// A hot reload rewrote GetPlayer's first 14 bytes (rva 0xDA8A80) with a plain memcpy while game
// job threads called it constantly; one thread decoded half-old, half-new bytes and PC A's game
// died at exactly GetPlayer+0 (05:26, access violation). WriteCodeSafely suspends every other
// thread of the process, waits until none of them is stopped inside the bytes being replaced,
// writes, flushes, and resumes them: the same idea Detours and MinHook use.
// Header-only so tests/patch_race.cpp can test it without the game.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>

namespace coop {

namespace codepatch_detail {
constexpr int kMaxThreads = 1024;

// The ids of every other thread of this process, listed before any thread is suspended (the
// snapshot may take locks a suspended thread holds).
inline int ListOthers(DWORD* ids, int cap) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && n < cap; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) ids[n++] = te.th32ThreadID;
    CloseHandle(snap);
    return n;
}

// Suspends the listed threads (OpenThread and SuspendThread take no user-mode lock); returns how
// many are held in hs.
inline int SuspendListed(const DWORD* ids, int count, HANDLE* hs) {
    int n = 0;
    for (int i = 0; i < count; ++i) {
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
        if (!th) continue;   // exited since the list was taken
        if (SuspendThread(th) == static_cast<DWORD>(-1)) { CloseHandle(th); continue; }
        hs[n++] = th;
    }
    return n;
}

inline void ResumeAll(HANDLE* hs, int n) {
    for (int i = 0; i < n; ++i) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
}

// True when some suspended thread would resume strictly inside (addr, addr + len): it would then
// run the middle of the new bytes. A thread stopped exactly at addr runs the new code from its start.
inline bool AnyInside(HANDLE* hs, int n, uintptr_t addr, size_t len) {
    for (int i = 0; i < n; ++i) {
        CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(hs[i], &c)) continue;   // a thread we cannot read: assume not inside
        if (c.Rip > addr && c.Rip < addr + len) return true;
    }
    return false;
}
} // namespace codepatch_detail

// Replaces len bytes of code at addr with bytes. Returns false if the page could not be made
// writable or no moment with every thread outside the range was found within maxTries.
// Never call it from a thread that holds a lock other threads need to reach their next
// instruction boundary (it only suspends; it takes no lock itself and allocates nothing while
// threads are held).
inline bool WriteCodeSafely(void* addr, const void* bytes, size_t len, int maxTries = 200) {
    using namespace codepatch_detail;
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    static HANDLE hs[kMaxThreads];   // static: nothing is allocated while threads are suspended
    static DWORD ids[kMaxThreads];
    static SRWLOCK lock = SRWLOCK_INIT;   // one patch at a time (the arrays are shared)
    AcquireSRWLockExclusive(&lock);
    bool done = false;
    for (int t = 0; t < maxTries && !done; ++t) {
        int listed = ListOthers(ids, kMaxThreads);
        int n = SuspendListed(ids, listed, hs);
        if (!AnyInside(hs, n, reinterpret_cast<uintptr_t>(addr), len)) {
            std::memcpy(addr, bytes, len);
            FlushInstructionCache(GetCurrentProcess(), addr, len);
            done = true;
        }
        ResumeAll(hs, n);
        if (!done) Sleep(t < 20 ? 0 : 1);
    }
    ReleaseSRWLockExclusive(&lock);
    VirtualProtect(addr, len, old, &old);
    return done;
}

} // namespace coop

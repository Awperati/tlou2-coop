#include "mod/main_thread.h"
#include "mod/log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <deque>
#include <mutex>

namespace coop {

namespace {
std::mutex g_mu;
std::deque<std::function<void()>> g_queue;
HHOOK g_hook = nullptr;
HWND g_wnd = nullptr;

void Drain() {
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard lk(g_mu);
            if (g_queue.empty()) return;
            fn = std::move(g_queue.front());
            g_queue.pop_front();
        }
        fn();
    }
}

LRESULT CALLBACK GetMsgProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) Drain();
    return CallNextHookEx(g_hook, code, wp, lp);
}

HWND FindMainWindow() {
    struct Ctx { DWORD pid; HWND found; } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64]{}; GetClassNameW(h, cls, 64);
        if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && lstrcmpW(cls, L"TLOU2CoopOverlay") != 0) {
            c->found = h; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}
} // namespace

bool MainThreadInit() {
    if (g_hook) return true;
    g_wnd = FindMainWindow();
    if (!g_wnd) return false;
    DWORD tid = GetWindowThreadProcessId(g_wnd, nullptr);
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, tid);
    Log("main thread hook on window %p thread %lu: %s", g_wnd, tid, g_hook ? "ok" : "FAILED");
    return g_hook != nullptr;
}

void MainThreadShutdown() {
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

void RunOnMainThread(std::function<void()> fn) {
    {
        std::lock_guard lk(g_mu);
        g_queue.push_back(std::move(fn));
    }
    if (g_wnd) PostMessageW(g_wnd, WM_NULL, 0, 0);   // wake the pump
}

} // namespace coop

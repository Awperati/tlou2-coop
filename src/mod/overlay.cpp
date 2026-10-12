#include "mod/overlay.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>

namespace coop {

namespace {
struct Shared {
    std::mutex mu;
    std::wstring prompt, status, notice;
    float progress = -1;
    ULONGLONG noticeUntil = 0;
    std::deque<std::wstring> speech;
} g;

OverlayOptions g_opt;
std::atomic<bool> g_run{false};
// Heap-allocated and never destroyed: a joinable std::thread destroyed during process
// exit calls std::terminate (the second dump after every game crash).
std::thread* g_thread = nullptr;
HWND g_wnd = nullptr;

HWND FindGameWindow() {
    struct Ctx { DWORD pid; HWND best; } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == c->pid && h != g_wnd && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { c->best = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

void Paint(HWND w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc; GetClientRect(w, &rc);
    HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(dc, &rc, key);
    DeleteObject(key);

    std::wstring prompt, status, notice; float progress;
    {
        std::lock_guard lk(g.mu);
        prompt = g.prompt; status = g.status; progress = g.progress;
        if (GetTickCount64() < g.noticeUntil) notice = g.notice;
    }
    SetBkMode(dc, TRANSPARENT);
    int h = rc.bottom - rc.top;
    HFONT big = CreateFontW(-(h / 28 > 18 ? h / 28 : 18), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT smallFont = CreateFontW(-(h / 48 > 14 ? h / 48 : 14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              ANTIALIASED_QUALITY, 0, L"Segoe UI");
    auto shadowText = [&](const std::wstring& s, RECT r, UINT fmt) {
        RECT s1 = r; OffsetRect(&s1, 2, 2);
        SetTextColor(dc, RGB(0, 0, 1));
        DrawTextW(dc, s.c_str(), -1, &s1, fmt);
        SetTextColor(dc, RGB(245, 240, 225));
        DrawTextW(dc, s.c_str(), -1, &r, fmt);
    };

    if (!prompt.empty()) {
        SelectObject(dc, big);
        RECT r{rc.left, rc.top + h * 62 / 100, rc.right, rc.top + h * 70 / 100};
        shadowText(prompt, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (progress >= 0) {
            int bw = (rc.right - rc.left) / 4, bx = (rc.right - rc.left - bw) / 2, by = r.bottom + 6;
            RECT back{bx, by, bx + bw, by + 10};
            HBRUSH dark = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(dc, &back, dark); DeleteObject(dark);
            RECT fill{bx, by, bx + static_cast<int>(bw * (progress > 1 ? 1 : progress)), by + 10};
            HBRUSH light = CreateSolidBrush(RGB(230, 210, 150));
            FillRect(dc, &fill, light); DeleteObject(light);
        }
    }
    if (!notice.empty()) {
        SelectObject(dc, big);
        RECT r{rc.left, rc.top + h * 12 / 100, rc.right, rc.top + h * 20 / 100};
        shadowText(notice, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (!status.empty()) {
        SelectObject(dc, smallFont);
        RECT r{rc.left + 16, rc.top + 12, rc.right - 16, rc.top + 60};
        shadowText(status, r, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }
    DeleteObject(big); DeleteObject(smallFont);
    EndPaint(w, &ps);
}

LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_PAINT) { Paint(w); return 0; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(w, m, wp, lp);
}

void ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISpVoice* voice = nullptr;
    if (g_opt.speak) CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice));

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"TLOU2CoopOverlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(g_wnd, RGB(255, 0, 255), 0, LWA_COLORKEY);

    while (g_run) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

        HWND game = FindGameWindow();
        bool show = game && !IsIconic(game) && GetForegroundWindow() == game;
        if (show) {
            RECT rc; GetClientRect(game, &rc);
            POINT tl{0, 0}; ClientToScreen(game, &tl);
            SetWindowPos(g_wnd, HWND_TOPMOST, tl.x, tl.y, rc.right, rc.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(g_wnd, nullptr, FALSE);
        } else {
            ShowWindow(g_wnd, SW_HIDE);
        }

        std::wstring say;
        {
            std::lock_guard lk(g.mu);
            if (!g.speech.empty()) { say = g.speech.front(); g.speech.pop_front(); }
        }
        if (voice && !say.empty()) voice->Speak(say.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
        Sleep(33);
    }
    if (voice) voice->Release();
    DestroyWindow(g_wnd);
    UnregisterClassW(L"TLOU2CoopOverlay", GetModuleHandleW(nullptr));   // hot reload re-registers it
    g_wnd = nullptr;
    CoUninitialize();
}

void Speak(const std::wstring& s) {
    if (!g_opt.speak) return;
    std::lock_guard lk(g.mu);
    g.speech.push_back(s);
    if (g.speech.size() > 3) g.speech.pop_front();
}
} // namespace

void OverlayStart(const OverlayOptions& opt) {
    if (g_run) return;
    g_opt = opt;
    g_run = true;
    g_thread = new std::thread(ThreadMain);
}

void OverlayStop() {
    if (!g_run) return;
    g_run = false;
    if (g_thread && g_thread->joinable()) g_thread->join();
    delete g_thread;
    g_thread = nullptr;
}

void OverlaySetPrompt(const std::wstring& text, float progress) {
    bool fresh;
    {
        std::lock_guard lk(g.mu);
        fresh = g.prompt != text;
        g.prompt = text; g.progress = progress;
    }
    if (fresh) Speak(text);
}

void OverlayClearPrompt() {
    std::lock_guard lk(g.mu);
    g.prompt.clear(); g.progress = -1;
}

void OverlaySetStatus(const std::wstring& text) {
    std::lock_guard lk(g.mu);
    g.status = text;
}

void OverlayNotify(const std::wstring& text) {
    {
        std::lock_guard lk(g.mu);
        g.notice = text; g.noticeUntil = GetTickCount64() + 4000;
    }
    Speak(text);
}

void OverlayCue(int kind) {
    if (!g_opt.sounds) return;
    static const UINT kinds[] = {MB_ICONASTERISK, MB_OK, MB_ICONEXCLAMATION, MB_ICONHAND};
    MessageBeep(kinds[kind & 3]);
}

} // namespace coop

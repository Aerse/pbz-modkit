// overlay.cpp: the HUD and the AWP scope, drawn in a click-through layered window over the game's client
// area (windowed / borderless only). Own thread; the game thread hands over a HudState under a lock.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include "overlay.h"

namespace overlay {
static std::mutex g_mx;
static HudState g_st;  // under g_mx
static std::atomic<bool> g_run;
static std::atomic<int> g_hitKind, g_hitDmg;
static std::atomic<ULONGLONG> g_hitAt;
static HANDLE g_thread;
static HWND g_wnd, g_game;
constexpr COLORREF kKey = RGB(255, 0, 255), kInk = RGB(1, 1, 1);
constexpr ULONGLONG kHitMs = 650;

static HudState Get() { std::lock_guard<std::mutex> l(g_mx); return g_st; }
void Set(const HudState& s) { std::lock_guard<std::mutex> l(g_mx); g_st = s; }
void Hit(int kind, int dmg) { g_hitKind = kind; g_hitDmg = dmg; g_hitAt = GetTickCount64(); }

static BOOL CALLBACK FindGame(HWND w, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && GetClassNameW(w, cls, 64) && !wcscmp(cls, L"UnrealWindow")) {
        g_game = w; return FALSE;
    }
    return TRUE;
}

// text with a 1px dark outline (the key colour is magenta, so near-black stays visible)
static void Text(HDC dc, int x, int y, const wchar_t* t, COLORREF c, UINT align) {
    SetTextAlign(dc, align); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kInk);
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) if (dx || dy) TextOutW(dc, x + dx, y + dy, t, lstrlenW(t));
    SetTextColor(dc, c); TextOutW(dc, x, y, t, lstrlenW(t));
}
static void Bar(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void Line(HDC dc, int x0, int y0, int x1, int y1, int th, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, th, c); HGDIOBJ o = SelectObject(dc, p);
    MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1);
    SelectObject(dc, o); DeleteObject(p);
}
static int TextW(HDC dc, const wchar_t* t) { SIZE z{}; GetTextExtentPoint32W(dc, t, lstrlenW(t), &z); return z.cx; }

static void Scope(HDC dc, int w, int h) {
    RECT all{0, 0, w, h};
    int r = (h < w ? h : w) * 47 / 100, cx = w / 2, cy = h / 2;
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)), key = CreateSolidBrush(kKey);
    FillRect(dc, &all, black);
    HGDIOBJ ob = SelectObject(dc, key);
    HPEN ring = CreatePen(PS_SOLID, 3, RGB(0, 0, 0)); HGDIOBJ op = SelectObject(dc, ring);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op); DeleteObject(ring); SelectObject(dc, ob);
    DeleteObject(black); DeleteObject(key);
    // reticle: hairlines in the middle, thick posts towards the rim
    int th = h >= 900 ? 2 : 1, in = r * 55 / 100, post = h * 5 / 1000 + 2;
    Line(dc, cx - in, cy, cx + in, cy, th, RGB(0, 0, 0));
    Line(dc, cx, cy - in, cx, cy + in, th, RGB(0, 0, 0));
    for (int s = -1; s <= 1; s += 2) {
        Line(dc, cx + s * in, cy, cx + s * r, cy, post, RGB(0, 0, 0));
        Line(dc, cx, cy + s * in, cx, cy + s * r, post, RGB(0, 0, 0));
    }
}

static void Paint(HDC dc, int w, int h, const HudState& s, ULONGLONG now) {
    RECT all{0, 0, w, h};
    int cx = w / 2, cy = h / 2;
    if (s.scope) Scope(dc, w, h);
    else { HBRUSH key = CreateSolidBrush(kKey); FillRect(dc, &all, key); DeleteObject(key); }
    if (!s.on) return;
    int sc = h * 100 / 1080; if (sc < 50) sc = 50;  // ui scale percent
    auto S = [&](int v) { return v * sc / 100 > 0 ? v * sc / 100 : 1; };
    HFONT big = CreateFontW(-S(46), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Consolas");
    HFONT mid = CreateFontW(-S(26), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Consolas");
    HFONT sml = CreateFontW(-S(21), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Microsoft YaHei");
    HGDIOBJ of = SelectObject(dc, big);
    wchar_t t[96];
    // ammo: magazine big, reserve after it; weapon name above, the slots above that
    int rx = w - S(40), by = h - S(100);
    SelectObject(dc, mid);
    wsprintfW(t, L" / \x221E");
    Text(dc, rx, by + S(14), t, RGB(200, 190, 160), TA_RIGHT | TA_TOP);
    int rw = TextW(dc, t);
    SelectObject(dc, big);
    wsprintfW(t, L"%d", s.ammo);
    bool low = s.ammo <= (s.clip > 5 ? s.clip / 5 : 1);
    Text(dc, rx - rw, by, t, s.ammo == 0 ? RGB(255, 70, 50) : low ? RGB(255, 190, 60) : RGB(240, 232, 205), TA_RIGHT | TA_TOP);
    SelectObject(dc, sml);
    Text(dc, rx, by - S(30), s.label, RGB(230, 215, 170), TA_RIGHT | TA_TOP);
    for (int i = 2, x = rx; i >= 0; --i) {
        if (!s.slots[i]) continue;
        wsprintfW(t, L"%d %s", i + 1, s.slots[i]);
        bool cur = i == s.sel;
        Text(dc, x, by - S(60), t, cur ? RGB(255, 225, 120) : RGB(150, 145, 135), TA_RIGHT | TA_TOP);
        x -= TextW(dc, t) + S(18);
    }
    if (s.help)
        Text(dc, cx, h - S(70), L"F6 收枪   1/2/3 切枪   左键 射击   右键 开镜(AWP) / 按住瞄准(步枪)   R 换弹", RGB(235, 235, 225), TA_CENTER | TA_TOP);
    if (s.kind) {  // progress bar under the crosshair
        float p = s.prog < 0 ? 0 : s.prog > 1 ? 1 : s.prog;
        int bw = S(150), bh = S(6), bx = cx - bw / 2, py = cy + S(70);
        Bar(dc, {bx - 1, py - 1, bx + bw + 1, py + bh + 1}, kInk);
        Bar(dc, {bx, py, bx + int(bw * p), py + bh}, s.kind == 2 ? RGB(255, 200, 60) : RGB(225, 225, 225));
        Text(dc, cx, py + S(10), s.kind == 2 ? L"换弹中" : s.kind == 3 ? L"拔枪" : L"拉栓", RGB(230, 230, 230), TA_CENTER | TA_TOP);
    }
    if (s.xmode == 1 && !s.scope) {  // CS crosshair: four lines, the gap follows the spread cone
        double half = tan(s.fov * 0.00872664626);
        int gap = S(4) + int(tan(s.xgap) / (half > 1e-3 ? half : 1e-3) * w / 2);
        if (gap > h / 3) gap = h / 3;
        int len = S(9), th = S(2) < 2 ? 2 : S(2), a = th / 2;
        COLORREF c = RGB(80, 255, 110);
        for (int k = 0; k < 2; ++k) {
            int o = k ? 0 : 1; COLORREF cc = k ? c : kInk;
            Bar(dc, {cx - a - o, cy - gap - len - o, cx - a + th + o, cy - gap + o}, cc);
            Bar(dc, {cx - a - o, cy + gap - o, cx - a + th + o, cy + gap + len + o}, cc);
            Bar(dc, {cx - gap - len - o, cy - a - o, cx - gap + o, cy - a + th + o}, cc);
            Bar(dc, {cx + gap - o, cy - a - o, cx + gap + len + o, cy - a + th + o}, cc);
        }
    }
    ULONGLONG age = now - g_hitAt;
    if (g_hitAt && age < kHitMs) {  // hit marker: four ticks round the centre, the damage floating up
        int hk = g_hitKind;
        COLORREF c = hk >= 2 ? RGB(255, 45, 35) : hk == 1 ? RGB(255, 210, 40) : RGB(255, 255, 255);
        int a = S(9), b = S(hk >= 2 ? 24 : 19), th = S(3) > 2 ? S(3) : 2;
        for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) {
            Line(dc, cx + sx * a, cy + sy * a, cx + sx * b, cy + sy * b, th + 2, kInk);
            Line(dc, cx + sx * a, cy + sy * a, cx + sx * b, cy + sy * b, th, c);
        }
        SelectObject(dc, sml);
        static const wchar_t* tag[] = {L"%d", L"%d  爆头", L"%d  击杀", L"%d  爆头击杀"};
        wsprintfW(t, tag[hk < 0 || hk > 3 ? 0 : hk], int(g_hitDmg));
        Text(dc, cx + S(30), cy - S(46) - int(age * S(30) / kHitMs), t, c, TA_LEFT | TA_TOP);
    }
    SelectObject(dc, of); DeleteObject(big); DeleteObject(mid); DeleteObject(sml);
}

static LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        RECT rc; GetClientRect(w, &rc);
        // draw off-screen, then copy: no flicker while the crosshair moves
        HDC md = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(md, bm);
        Paint(md, rc.right, rc.bottom, Get(), GetTickCount64());
        BitBlt(dc, 0, 0, rc.right, rc.bottom, md, 0, 0, SRCCOPY);
        SelectObject(md, ob); DeleteObject(bm); DeleteDC(md);
        EndPaint(w, &ps); return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(w, m, wp, lp);
}

static unsigned long long Sig(const HudState& s, RECT rc, ULONGLONG now) {
    ULONGLONG age = now - g_hitAt;
    unsigned long long h = 1469598103934665603ull;
    auto mix = [&](unsigned long long v) { h = (h ^ v) * 1099511628211ull; };
    mix(s.on); mix(s.scope); mix(s.help); mix(s.ammo); mix(s.reserve); mix(s.sel); mix(s.kind);
    mix(int(s.prog * 50)); mix(s.xmode); mix(int(s.xgap * 20000)); mix(int(s.fov)); mix((uintptr_t)s.label);
    mix(g_hitAt && age < kHitMs + 50 ? age / 25 + 1 : 0); mix(rc.right); mix(rc.bottom);
    return h;
}

static DWORD WINAPI Main(LPVOID) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"pbzSniperScope";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_wnd, kKey, 0, LWA_COLORKEY);
    bool shown = false; RECT last{}; unsigned long long sig0 = 0;
    while (g_run) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (!g_game || !IsWindow(g_game)) { g_game = nullptr; EnumWindows(FindGame, 0); }
        HudState s = Get();
        bool want = (s.scope || s.on) && g_game && GetForegroundWindow() == g_game && !IsIconic(g_game);
        if (want) {
            RECT rc; GetClientRect(g_game, &rc);
            POINT o{0, 0}; ClientToScreen(g_game, &o); OffsetRect(&rc, o.x, o.y);
            if (!shown || !EqualRect(&rc, &last)) {
                SetWindowPos(g_wnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                last = rc; sig0 = 0;
            }
            unsigned long long sig = Sig(s, rc, GetTickCount64());
            if (sig != sig0) { sig0 = sig; InvalidateRect(g_wnd, nullptr, FALSE); }
            shown = true;
        } else if (shown) { ShowWindow(g_wnd, SW_HIDE); shown = false; }
        Sleep(8);
    }
    DestroyWindow(g_wnd); g_wnd = nullptr;
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}

void Start() { if (g_thread) return; g_run = true; g_thread = CreateThread(nullptr, 0, Main, nullptr, 0, nullptr); }
void Stop() {
    if (!g_thread) return;
    g_run = false; WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr;
    Set(HudState{});
}

// the overlay as it would be drawn now, at the game window's client size (1280x720 without one)
bool Snap(const wchar_t* path) {
    int w = 1280, h = 720;
    if (g_game) { RECT rc; GetClientRect(g_game, &rc); if (rc.right > 0 && rc.bottom > 0) { w = rc.right; h = rc.bottom; } }
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HDC sdc = GetDC(nullptr);
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC md = CreateCompatibleDC(sdc); ReleaseDC(nullptr, sdc);
    if (!bm || !md) { if (bm) DeleteObject(bm); if (md) DeleteDC(md); return false; }
    HGDIOBJ ob = SelectObject(md, bm);
    Paint(md, w, h, Get(), GetTickCount64());
    GdiFlush();
    bool ok = false;
    if (FILE* f = _wfopen(path, L"wb")) {
        DWORD img = DWORD(w) * h * 4;
        BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader; fh.bfSize = fh.bfOffBits + img;
        fwrite(&fh, sizeof fh, 1, f); fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f); fwrite(bits, 1, img, f);
        ok = !ferror(f); fclose(f);
    }
    SelectObject(md, ob); DeleteObject(bm); DeleteDC(md);
    return ok;
}
}  // namespace overlay

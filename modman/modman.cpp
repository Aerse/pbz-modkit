#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
// modman.cpp: Mod 管理器. Sits next to version.dll (platform\steam\), lists the mods in pbzmp\ (one
// <name>.dll each, optional <name>.txt info) and writes the checked ones to pbzmp\autorun.txt, which the
// loader reads once at game start. No install, no runtime: one exe.
// <name>.txt (UTF-8, key=value per line, all optional):
//   name=显示名  desc=一句话说明  version=1.0  author=...  hidden=1 (framework module, not listed)
//   requires=a,b>=1.2   conflicts=c,d   api=1 (loader host API it needs)   mp=allow|block (online play)
// A mod is pbzmp\<id>.dll or pbzmp\<id>\<id>.dll (own folder). Use a namespaced id (author.name).
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

struct Req { std::wstring id, minVer; };
struct Mod { std::wstring id, name, desc, version, author, dir; std::vector<Req> req; std::vector<std::wstring> conflicts;
             int api = 0; bool mpBlock = false, hidden = false, on = false; };
constexpr int kHostApi = 2;  // main.cpp kHostApi
static std::wstring g_loaderNote;  // set when version.dll is not our loader
static std::wstring g_dir;          // <exe dir>pbzmp (with a trailing backslash)
static std::vector<Mod> g_mods;     // listed (not hidden) mods, list view order
static std::vector<std::wstring> g_auto;  // autorun.txt as read (order kept)
static HWND g_list, g_info;
enum { kList = 100, kSave, kSaveRun, kFolder };

static std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == 0xfeff)) ++i;
    return s.substr(i);
}
static std::vector<std::wstring> Lines(const std::wstring& path) {
    std::vector<std::wstring> out;
    FILE* f = _wfopen(path.c_str(), L"r, ccs=UTF-8"); if (!f) return out;
    wchar_t b[1024]; while (fgetws(b, 1023, f)) out.push_back(Trim(b));
    fclose(f); return out;
}
static std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> out; size_t a = 0;
    for (;;) { size_t c = s.find(L',', a); auto t = Trim(s.substr(a, c == std::wstring::npos ? c : c - a)); if (!t.empty()) out.push_back(t); if (c == std::wstring::npos) break; a = c + 1; }
    return out;
}
// "1.10" > "1.9": compare dot-separated numbers
static int VerCmp(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != L'.') { if (iswdigit(a[i])) x = x * 10 + (a[i] - L'0'); ++i; }
        while (j < b.size() && b[j] != L'.') { if (iswdigit(b[j])) y = y * 10 + (b[j] - L'0'); ++j; }
        if (x != y) return x < y ? -1 : 1;
        ++i; ++j;
    }
    return 0;
}
static bool Lower(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
static bool InAuto(const std::wstring& id) { for (auto& a : g_auto) if (Lower(a, id)) return true; return false; }

static std::vector<Mod> g_all;      // every mod found, hidden ones too
static Mod* Find(const std::wstring& id) { for (auto& m : g_all) if (Lower(m.id, id)) return &m; return nullptr; }

// Our version.dll exports PbzLoaderApi. Another tool's version.dll proxy in its place means our mods never load.
static bool FileHas(const std::wstring& path, const char* text) {
    FILE* f = _wfopen(path.c_str(), L"rb"); if (!f) return false;
    std::string s; char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f); return s.find(text) != std::string::npos;
}
static void CheckLoader(const std::wstring& exeDir) {
    g_loaderNote.clear();
    std::wstring v = exeDir + L"version.dll";
    if (GetFileAttributesW(v.c_str()) == INVALID_FILE_ATTRIBUTES) g_loaderNote = L"没找到 version.dll（Mod 加载器），Mod 不会生效。请重新解压 Mod 包。";
    else if (!FileHas(v, "PbzLoaderApi"))
        g_loaderNote = L"version.dll 不是本 Mod 加载器（可能是别的 Mod 的）。\n要两个都用：把它改名为 version_chain.dll，再放回本包的 version.dll，加载器会顺带加载它。";
}
// version.dll that is not ours: rename it to version_chain.dll (the loader chain-loads it). Ours must then be put back.
static bool ChainForeign(const std::wstring& exeDir, std::wstring* msg) {
    std::wstring v = exeDir + L"version.dll", c = exeDir + L"version_chain.dll";
    if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) { *msg = L"已有 version_chain.dll，不覆盖。"; return false; }
    if (!MoveFileW(v.c_str(), c.c_str())) { *msg = L"改名失败（游戏是否在运行？）"; return false; }
    *msg = L"已把原 version.dll 改名为 version_chain.dll。\n现在把 Mod 包里的 version.dll 复制回这里即可两个都生效。";
    return true;
}
static void Scan() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe; d = d.substr(0, d.find_last_of(L'\\') + 1); g_dir = d + L"pbzmp\\";
    g_auto.clear();
    for (auto& l : Lines(g_dir + L"autorun.txt")) if (!l.empty() && l[0] != L'#') g_auto.push_back(l);
    std::vector<std::pair<std::wstring, std::wstring>> found;  // id, folder
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((g_dir + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring f = fd.cFileName;
            if (f == L"." || f == L".." || f.rfind(L"_", 0) == 0) continue;  // _loaded_* copies the loader makes
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (GetFileAttributesW((g_dir + f + L"\\" + f + L".dll").c_str()) != INVALID_FILE_ATTRIBUTES) found.push_back({f, g_dir + f + L"\\"});
            } else if (f.size() > 4 && _wcsicmp(f.c_str() + f.size() - 4, L".dll") == 0) found.push_back({f.substr(0, f.size() - 4), g_dir});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    {
        for (auto& [id, dir] : found) {
            if (Find(id)) continue;  // the same id in both places: the loose dll wins, like the loader
            Mod m; m.id = m.name = id; m.dir = dir;
            for (auto& l : Lines(dir + id + L".txt")) {
                size_t e = l.find(L'='); if (e == std::wstring::npos) continue;
                auto k = Trim(l.substr(0, e)), v = Trim(l.substr(e + 1));
                if (k == L"name") m.name = v; else if (k == L"desc") m.desc = v; else if (k == L"version") m.version = v;
                else if (k == L"author") m.author = v; else if (k == L"hidden") m.hidden = v == L"1";
                else if (k == L"conflicts") m.conflicts = Split(v); else if (k == L"api") m.api = _wtoi(v.c_str());
                else if (k == L"mp") m.mpBlock = v == L"block";
                else if (k == L"requires") for (auto& r : Split(v)) {
                    size_t g = r.find(L">=");
                    m.req.push_back(g == std::wstring::npos ? Req{r, L""} : Req{Trim(r.substr(0, g)), Trim(r.substr(g + 2))});
                }
            }
            m.on = InAuto(id);
            g_all.push_back(m);
        }
    }
    CheckLoader(d);
    for (auto& m : g_all) if (!m.hidden) g_mods.push_back(m);
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
}

// autorun.txt: the checked mods (and what they require, framework ones too) in load order: requirements
// first, then the old order, then new ones. Lines for mods whose dll is gone are kept (not ours to drop).
static bool Save(std::wstring* msg) {
    std::map<std::wstring, bool> want;
    for (auto& m : g_all) want[m.id] = m.hidden ? InAuto(m.id) : false;
    for (auto& m : g_mods) want[m.id] = m.on;
    std::vector<std::wstring> added, missing;
    for (bool again = true; again;) {
        again = false;
        for (auto& m : g_all) if (want[m.id]) for (auto& r : m.req) {
            auto q = Find(r.id);
            std::wstring need = r.id + (r.minVer.empty() ? L"" : L" ≥ " + r.minVer);
            if (q && !r.minVer.empty() && VerCmp(q->version, r.minVer) < 0) need += L"（现有 " + (q->version.empty() ? L"未标版本" : q->version) + L"）", q = nullptr;
            if (!q) { if (std::find(missing.begin(), missing.end(), need) == missing.end()) missing.push_back(need); continue; }
            if (!want[q->id]) { want[q->id] = again = true; if (!q->hidden) added.push_back(q->name); }
        }
    }
    if (!missing.empty()) {
        *msg = L"缺少前置 Mod：";
        for (auto& r : missing) *msg += L" " + r;
        *msg += L"\n请把它放进 pbzmp 文件夹后再保存。";
        return false;
    }
    std::wstring bad;
    for (auto& m : g_all) if (want[m.id]) {
        if (m.api > kHostApi) bad += L"\n" + m.name + L" 需要更新的加载器（api " + std::to_wstring(m.api) + L"），请更新 version.dll";
        for (auto& c : m.conflicts) if (auto q = Find(c); q && want[q->id] && q != &m)
            if (bad.find(q->name + L" 与 " + m.name) == std::wstring::npos) bad += L"\n" + m.name + L" 与 " + q->name + L" 冲突，不能同时启用";
    }
    if (!bad.empty()) { *msg = L"无法保存：" + bad; return false; }
    std::vector<std::wstring> order, out;
    for (auto& a : g_auto) { auto q = Find(a); order.push_back(q ? q->id : a); }
    for (auto& m : g_all) if (std::find_if(order.begin(), order.end(), [&](auto& o) { return Lower(o, m.id); }) == order.end()) order.push_back(m.id);
    std::map<std::wstring, int> state;  // 1 visiting, 2 done
    std::vector<std::wstring> stack;
    auto emit = [&](auto& self, const std::wstring& id) -> void {
        if (state[id]) return; state[id] = 1;
        auto q = Find(id);
        if (q) for (auto& r : q->req) if (auto rq = Find(r.id)) self(self, rq->id);
        state[id] = 2;
        if (!q || want[q->id]) out.push_back(id);
    };
    for (auto& o : order) emit(emit, o);
    std::wstring tmp = g_dir + L"autorun.txt.tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { *msg = L"无法写入 " + g_dir + L"autorun.txt"; return false; }
    fputs("# 由 Mod管理器 生成：每行一个要加载的 Mod（游戏启动时读取）\r\n", f);
    for (auto& id : out) {
        int n = WideCharToMultiByte(CP_UTF8, 0, id.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string u(n, 0); WideCharToMultiByte(CP_UTF8, 0, id.c_str(), -1, u.data(), n, nullptr, nullptr);
        fputs(u.c_str(), f); fputs("\r\n", f);
    }
    bool ok = fclose(f) == 0 && MoveFileExW(tmp.c_str(), (g_dir + L"autorun.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) { DeleteFileW(tmp.c_str()); *msg = L"无法写入 " + g_dir + L"autorun.txt"; return false; }
    g_auto = out;
    *msg = L"已保存，下次启动游戏生效。";
    if (!added.empty()) { *msg += L"\n已自动启用前置 Mod："; for (auto& a : added) *msg += L" " + a; }
    for (auto& m : g_mods) m.on = want[m.id];
    return true;
}

static bool GameRunning() {
    HWND w = FindWindowW(nullptr, L"Phantom Blade Zero");
    return w != nullptr;
}

static void ShowInfo(int i) {
    if (i < 0 || i >= int(g_mods.size())) { SetWindowTextW(g_info, L"勾选要启用的 Mod，然后点「保存」。新 Mod 放进 pbzmp 文件夹即可出现在列表里。"); return; }
    auto& m = g_mods[i];
    std::wstring t = m.name + (m.version.empty() ? L"" : L"  v" + m.version) + (m.author.empty() ? L"" : L"  作者 " + m.author);
    t += L"\r\n" + (m.desc.empty() ? L"（没有说明）" : m.desc);
    if (!m.req.empty()) { t += L"\r\n前置："; for (auto& r : m.req) { auto q = Find(r.id); t += L" " + (q ? q->name : r.id + L"（缺失）") + (r.minVer.empty() ? L"" : L" ≥" + r.minVer); } }
    if (!m.conflicts.empty()) { t += L"  冲突："; for (auto& c : m.conflicts) { auto q = Find(c); t += L" " + (q ? q->name : c); } }
    if (m.mpBlock) t += L"\r\n联机：房间里有人启用此 Mod 时不能开始对战";
    t += L"\r\n文件：" + m.dir.substr(g_dir.size() - 6) + m.id + L".dll";
    SetWindowTextW(g_info, t.c_str());
}

static HFONT g_font;
static bool g_filling;  // list being filled: check changes are not the user's
static void Fill() {
    g_filling = true;
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < int(g_mods.size()); ++i) {
        auto& m = g_mods[i];
        LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = const_cast<wchar_t*>(m.name.c_str());
        ListView_InsertItem(g_list, &it);
        ListView_SetItemText(g_list, i, 1, const_cast<wchar_t*>(m.version.c_str()));
        ListView_SetItemText(g_list, i, 2, const_cast<wchar_t*>(m.desc.c_str()));
        ListView_SetCheckState(g_list, i, m.on);
    }
    g_filling = false;
}
static void DoSave(HWND w, bool launch) {
    for (int i = 0; i < int(g_mods.size()); ++i) g_mods[i].on = ListView_GetCheckState(g_list, i) != 0;
    std::wstring msg;
    bool ok = Save(&msg);
    if (ok) Fill();
    if (ok && GameRunning() && !launch) msg += L"\n游戏正在运行：重启游戏后生效。";
    if (ok && launch) {
        if (GameRunning()) msg += L"\n游戏已在运行，请先退出游戏再启动。";
        else { ShellExecuteW(nullptr, L"open", L"steam://rungameid/4115450", nullptr, nullptr, SW_SHOWNORMAL); return; }
    }
    MessageBoxW(w, msg.c_str(), L"Mod管理器", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}

static HWND Btn(HWND p, const wchar_t* t, int id, int x, int y, int w) {
    HWND b = CreateWindowW(L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w, 32, p, HMENU(INT_PTR(id)), nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, WPARAM(g_font), TRUE); return b;
}
static LRESULT CALLBACK Wnd(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 12, 12, 600, 260, w, HMENU(kList), nullptr, nullptr);
        SendMessageW(g_list, WM_SETFONT, WPARAM(g_font), TRUE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* cols[] = {L"Mod", L"版本", L"说明"}; int widths[] = {170, 70, 340};
        for (int i = 0; i < 3; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = const_cast<wchar_t*>(cols[i]); c.cx = widths[i]; ListView_InsertColumn(g_list, i, &c); }
        g_info = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 282, 600, 72, w, nullptr, nullptr, nullptr);
        SendMessageW(g_info, WM_SETFONT, WPARAM(g_font), TRUE);
        Btn(w, L"打开 Mod 文件夹", kFolder, 12, 362, 150);
        Btn(w, L"保存", kSave, 352, 362, 110);
        Btn(w, L"保存并启动游戏", kSaveRun, 472, 362, 140);
        Scan(); Fill(); ShowInfo(-1);
        if (GetFileAttributesW(g_dir.c_str()) == INVALID_FILE_ATTRIBUTES)
            MessageBoxW(w, (L"没找到 Mod 文件夹：\n" + g_dir + L"\n\n请把 Mod管理器.exe 放在游戏的 platform\\steam 文件夹里（和 PBZ.exe、version.dll 同一层）。").c_str(), L"Mod管理器", MB_ICONWARNING);
        else if (g_mods.empty()) SetWindowTextW(g_info, L"pbzmp 文件夹里还没有 Mod。把 Mod 的 dll 放进去后重新打开管理器。");
        if (!g_loaderNote.empty()) {
            std::wstring exeDir = g_dir.substr(0, g_dir.size() - 6);
            bool foreign = GetFileAttributesW((exeDir + L"version.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
            if (!foreign) MessageBoxW(w, g_loaderNote.c_str(), L"Mod管理器", MB_ICONWARNING);
            else if (MessageBoxW(w, (g_loaderNote + L"\n\n现在帮你改名吗？").c_str(), L"Mod管理器", MB_ICONWARNING | MB_YESNO) == IDYES) {
                std::wstring m; bool ok = ChainForeign(exeDir, &m);
                MessageBoxW(w, m.c_str(), L"Mod管理器", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
            }
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto h = reinterpret_cast<NMHDR*>(lp);
        if (h->idFrom == kList && h->code == LVN_ITEMCHANGED) {
            auto n = reinterpret_cast<NMLISTVIEW*>(lp);
            if (n->uNewState & LVIS_SELECTED) ShowInfo(n->iItem);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kSave) DoSave(w, false);
        else if (LOWORD(wp) == kSaveRun) DoSave(w, true);
        else if (LOWORD(wp) == kFolder) ShellExecuteW(nullptr, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// test mode, no window: modman --list > file  /  modman --set a,b > file (check exactly those, then Save)
static int Cli(const std::wstring& cmd) {
    Scan();
    std::wstring out;
    if (cmd.rfind(L"--set", 0) == 0) {
        auto ids = Split(cmd.size() > 6 ? cmd.substr(6) : L"");
        for (auto& m : g_mods) m.on = std::any_of(ids.begin(), ids.end(), [&](auto& i) { return Lower(i, m.id); });
        std::wstring msg; bool ok = Save(&msg);
        out += (ok ? L"saved: " : L"refused: ") + msg + L"\n";
    }
    for (auto& m : g_mods) out += (m.on ? L"[x] " : L"[ ] ") + m.id + L" | " + m.name + L" | " + m.version + L"\n";
    out += L"autorun:"; for (auto& a : g_auto) out += L" " + a; out += L"\n";
    if (cmd == L"--chain") { std::wstring m; ChainForeign(g_dir.substr(0, g_dir.size() - 6), &m); out += L"chain: " + m + L"\n"; }
    else if (!g_loaderNote.empty()) out += L"loader: " + g_loaderNote + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u(n, 0); WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, u.data(), n, nullptr, nullptr);
    DWORD wr; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u.data(), DWORD(n - 1), &wr, nullptr);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmdline, int show) {
    if (cmdline && cmdline[0] == L'-') return Cli(Trim(cmdline));
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&ic);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    WNDCLASSW wc{}; wc.lpfnWndProc = Wnd; wc.hInstance = hi; wc.lpszClassName = L"PbzModMan";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{0, 0, 624, 406}; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND w = CreateWindowW(L"PbzModMan", L"影之刃零 Mod管理器", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
    ShowWindow(w, show);
    MSG m; while (GetMessageW(&m, nullptr, 0, 0)) { if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    return 0;
}

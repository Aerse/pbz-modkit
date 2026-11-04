// fileguard: mp's own copy of nosave's save-file block (_nosave.cpp). The game writes the save
// about twice a second in levels mp opens (L_Longzhu_xigu_Level), even under SaveVeto, so
// whenever mp opens a level every file write, move, copy or delete under ...\SaveGames\ is
// refused at the Win32 level until mp unloads. Reads are untouched. When nosave.dll already
// guards this process (it sets PBZMP_NOSAVE) mp does not hook the same functions a second time.
#include "fileguard.h"
#include "uex.h"
#include <windows.h>
#include <cwchar>
#include <string>
#include "MinHook.h"

namespace fileguard {
namespace {
static CRITICAL_SECTION g_cs; static std::wstring g_blocked[16]; static int g_nb, g_total;
static bool IsSave(LPCWSTR p) {
    if (!p) return false;
    // the game's write test of the save folder: refusing it makes the game move its save folder
    // to %LOCALAPPDATA%, so let it through (it writes and deletes an empty probe file)
    if (wcsstr(p, L".write-probe-")) return false;
    for (const wchar_t* s = p; *s; ++s)
        if ((*s == L'\\' || *s == L'/') && _wcsnicmp(s + 1, L"SaveGames", 9) == 0 && (s[10] == L'\\' || s[10] == L'/' || !s[10])) return true;
    return false;
}
static void Note(const wchar_t* op, LPCWSTR p) {
    EnterCriticalSection(&g_cs);
    if (g_nb < 16) g_blocked[g_nb++] = std::wstring(op) + L" " + p;
    ++g_total;
    LeaveCriticalSection(&g_cs);
}
static bool Deny(const wchar_t* op, LPCWSTR p) {
    if (!IsSave(p)) return false;
    Note(op, p); SetLastError(ERROR_ACCESS_DENIED); return true;
}

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using MoveFileExW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using MoveFileWithProgressW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, DWORD);
using ReplaceFileW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
using CopyFileExW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
using DeleteFileW_t = BOOL(WINAPI*)(LPCWSTR);
static CreateFileW_t oCreateFileW; static CreateFile2_t oCreateFile2; static MoveFileExW_t oMoveFileExW;
static MoveFileWithProgressW_t oMoveFileWithProgressW; static ReplaceFileW_t oReplaceFileW;
static CopyFileExW_t oCopyFileExW; static DeleteFileW_t oDeleteFileW;

constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | FILE_WRITE_ATTRIBUTES;
static bool Writes(DWORD access, DWORD disp) { return (access & kWriteAccess) || disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING; }

static HANDLE WINAPI hCreateFileW(LPCWSTR p, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    if (Writes(a, disp) && Deny(L"open", p)) return INVALID_HANDLE_VALUE;
    return oCreateFileW(p, a, sh, sa, disp, fl, t);
}
static HANDLE WINAPI hCreateFile2(LPCWSTR p, DWORD a, DWORD sh, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS x) {
    if (Writes(a, disp) && Deny(L"open2", p)) return INVALID_HANDLE_VALUE;
    return oCreateFile2(p, a, sh, disp, x);
}
static BOOL WINAPI hMoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) {
    if (Deny(L"move", b) || Deny(L"move", a)) return FALSE;
    return oMoveFileExW(a, b, f);
}
static BOOL WINAPI hMoveFileWithProgressW(LPCWSTR a, LPCWSTR b, LPPROGRESS_ROUTINE r, LPVOID d, DWORD f) {
    if (Deny(L"move", b) || Deny(L"move", a)) return FALSE;
    return oMoveFileWithProgressW(a, b, r, d, f);
}
static BOOL WINAPI hReplaceFileW(LPCWSTR a, LPCWSTR b, LPCWSTR c, DWORD f, LPVOID e1, LPVOID e2) {
    if (Deny(L"replace", a) || Deny(L"replace", b)) return FALSE;
    return oReplaceFileW(a, b, c, f, e1, e2);
}
static BOOL WINAPI hCopyFileExW(LPCWSTR a, LPCWSTR b, LPPROGRESS_ROUTINE r, LPVOID d, LPBOOL c, DWORD f) {
    if (Deny(L"copy", b)) return FALSE;
    return oCopyFileExW(a, b, r, d, c, f);
}
static BOOL WINAPI hDeleteFileW(LPCWSTR p) {
    if (Deny(L"delete", p)) return FALSE;
    return oDeleteFileW(p);
}

bool g_hooked, g_other;
static void Hook() {
    if (MH_Initialize() != MH_OK) { uex::Log(L"fileguard: MH_Initialize failed"); return; }
    struct H { const char* fn; void* det; void** orig; } hs[] = {
        {"CreateFileW", (void*)hCreateFileW, (void**)&oCreateFileW},
        {"CreateFile2", (void*)hCreateFile2, (void**)&oCreateFile2},
        {"MoveFileExW", (void*)hMoveFileExW, (void**)&oMoveFileExW},
        {"MoveFileWithProgressW", (void*)hMoveFileWithProgressW, (void**)&oMoveFileWithProgressW},
        {"ReplaceFileW", (void*)hReplaceFileW, (void**)&oReplaceFileW},
        {"CopyFileExW", (void*)hCopyFileExW, (void**)&oCopyFileExW},
        {"DeleteFileW", (void*)hDeleteFileW, (void**)&oDeleteFileW},
    };
    int ok = 0;
    for (auto& h : hs) {
        auto st = MH_CreateHookApi(L"kernelbase", h.fn, h.det, h.orig);
        if (st == MH_OK) ++ok; else uex::Log(L"fileguard: hook %S failed %d", h.fn, int(st));
    }
    g_hooked = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    uex::Log(L"fileguard: file guard %s (%d hooks)", g_hooked ? L"on" : L"FAILED", ok);
}

}  // namespace

bool On() {
    if (g_hooked || g_other) return true;
    if (GetEnvironmentVariableW(L"PBZMP_NOSAVE", nullptr, 0) > 0 || GetEnvironmentVariableW(L"PBZMP_FILEGUARD", nullptr, 0) > 0) {
        g_other = true; uex::Log(L"fileguard: another guard is active (nosave)"); return true;
    }
    static bool cs; if (!cs) { InitializeCriticalSection(&g_cs); cs = true; }
    Hook();
    if (g_hooked) SetEnvironmentVariableW(L"PBZMP_FILEGUARD", L"1");
    return g_hooked;
}
bool Active() { return g_hooked || g_other; }
void Tick() {
    if (!g_hooked) return;
    std::wstring b[16]; int n = 0, total = 0;
    EnterCriticalSection(&g_cs);
    for (; n < g_nb; ++n) b[n] = std::move(g_blocked[n]);
    g_nb = 0; total = g_total;
    LeaveCriticalSection(&g_cs);
    static int logged;
    for (int i = 0; i < n; ++i) if (logged++ < 200 || (total - n + i + 1) % 500 == 0)
        uex::Log(L"fileguard: BLOCKED save write #%d %s", total - n + i + 1, b[i].c_str());
}
void Off() {
    if (!g_hooked) return;
    MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); g_hooked = false; Sleep(50);
    SetEnvironmentVariableW(L"PBZMP_FILEGUARD", nullptr);
}
void Pin() {
    if (!g_hooked) return;
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Pin), &m);
    uex::Log(L"fileguard: kept on, module pinned");
}
}  // namespace fileguard

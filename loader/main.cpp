// pbzmp: mod loader for Phantom Blade Zero, loaded as a version.dll proxy.
// Owns the only hook on UGameEngine::Tick and gives modules a PbzHost (host.h): per-frame
// callbacks and one-shot jobs on the game thread, each run under SEH.
// Commands: one line in <mod dir>\cmd.txt, polled every 500 ms.
//   run X   copy X.dll to a temp name, load it, call PbzInit and PbzRun. An X loaded earlier is
//           unloaded first if it exports PbzUnload (legacy modules without it stay loaded).
//   stop X  unload X (PbzUnload modules only)
// autorun.txt in the mod dir: module names, one per line, run once the engine is up.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "proxy_exports.h"
#include "host.h"
#include "MinHook.h"
#include "discover.h"

static HMODULE g_self;
static std::wstring g_dir;
static std::mutex g_logMx;

static void LogLine(const std::wstring& s) {
    std::lock_guard lk(g_logMx);
    FILE* f = _wfopen((g_dir + L"loader.log").c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s.c_str());
    fclose(f);
}

// Test instances get their own mod folder (cmd.txt, logs) and mutex name: "-pbzmpdir=<name>" on
// the command line (visible to the scripts that stop instances), or the PBZMP_DIR variable.
static std::wstring InstanceTag() {
    std::wstring cl = GetCommandLineW();
    if (size_t i = cl.find(L"-pbzmpdir="); i != std::wstring::npos) {
        size_t b = i + 10, e = cl.find_first_of(L" \t\"", b);
        return cl.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
    }
    wchar_t env[MAX_PATH];
    if (DWORD n = GetEnvironmentVariableW(L"PBZMP_DIR", env, MAX_PATH); n && n < MAX_PATH) return env;
    return L"";
}

static std::wstring ModDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p);
    s.resize(s.find_last_of(L"\\/") + 1);
    std::wstring d = InstanceTag();
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return s + (d.empty() ? L"pbzmp" : d) + L"\\";
}

// ---- tick hub ------------------------------------------------------------------------------
// Engine addresses are found at runtime (discover.cpp), so a game update does not need a new
// loader. These are the values for the 2026-11-04 build, used only when a search fails and the
// value still checks out.
constexpr uintptr_t kRvaObjects = 0xde28c68, kRvaNamePool = 0xdd86ac0, kRvaEngineVt = 0xb1f9290;
constexpr int kTickSlot = 0x5e, kProcessEvent = 0x4c;
static disc::Result g_disc;
using TickFn = void(__fastcall*)(void*, float, bool);

struct TickEntry { int id; void (*fn)(void*, float); void* ctx; HMODULE owner; };
struct JobEntry { void (*fn)(void*); void* ctx; HMODULE owner; uint64_t seq; };
static std::mutex g_mx;  // guards g_ticks, g_jobs, g_nextId; never held while calling a module
static std::vector<TickEntry> g_ticks;
static std::deque<JobEntry> g_jobs;
static int g_nextId = 1;
static uint64_t g_jobSeq;
static TickFn g_origTick;
static void* volatile g_engine;

static HMODULE OwnerOf(const void* addr) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addr), &m);
    return m;
}
static std::wstring ModName(HMODULE m) {
    wchar_t p[MAX_PATH] = L"?";
    if (m) GetModuleFileNameW(m, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(s.find_last_of(L"\\/") + 1);
}

static int AddTick(void (*fn)(void*, float), void* ctx) {
    std::lock_guard lk(g_mx);
    g_ticks.push_back({g_nextId, fn, ctx, OwnerOf(reinterpret_cast<const void*>(fn))});
    return g_nextId++;
}
static void RemoveTick(int id) {
    std::lock_guard lk(g_mx);
    std::erase_if(g_ticks, [id](const TickEntry& t) { return t.id == id; });
}
static void Post(void (*fn)(void*), void* ctx) {
    std::lock_guard lk(g_mx);
    g_jobs.push_back({fn, ctx, OwnerOf(reinterpret_cast<const void*>(fn)), ++g_jobSeq});
}
static void HostLog(const wchar_t* s) { LogLine(s ? s : L""); }
static void* Engine() { return g_engine; }
// ---- shared hooks and hotkeys (host v2): one owner per function / key, freed with the module ----
struct HookEntry { int id; void* target; HMODULE owner; };
struct HotEntry { int id, vk, mods; void (*fn)(void*); void* ctx; HMODULE owner; bool down; };
static std::vector<HookEntry> g_hooks;  // g_mx
static std::vector<HotEntry> g_hot;     // g_mx
static int HookFn(void* target, void* detour, void** original) {
    HMODULE me = OwnerOf(detour);
    {
        std::lock_guard lk(g_mx);
        for (auto& h : g_hooks) if (h.target == target) {
            LogLine(L"hook conflict at " + std::to_wstring(reinterpret_cast<uintptr_t>(target)) + L": " + ModName(me) + L" refused, owned by " + ModName(h.owner));
            return 0;
        }
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        LogLine(L"hook failed for " + ModName(me));
        return 0;
    }
    std::lock_guard lk(g_mx);
    g_hooks.push_back({g_nextId, target, me});
    return g_nextId++;
}
static void Unhook(int id) {
    void* t = nullptr;
    {
        std::lock_guard lk(g_mx);
        for (auto it = g_hooks.begin(); it != g_hooks.end(); ++it) if (it->id == id) { t = it->target; g_hooks.erase(it); break; }
    }
    if (t) { MH_DisableHook(t); MH_RemoveHook(t); }
}
static int AddHotkey(int vk, int mods, void (*fn)(void*), void* ctx) {
    HMODULE me = OwnerOf(reinterpret_cast<const void*>(fn));
    std::lock_guard lk(g_mx);
    for (auto& h : g_hot) if (h.vk == vk && h.mods == mods && h.owner != me) {
        LogLine(L"hotkey conflict vk " + std::to_wstring(vk) + L" mods " + std::to_wstring(mods) + L": " + ModName(me) + L" refused, owned by " + ModName(h.owner));
        return 0;
    }
    g_hot.push_back({g_nextId, vk, mods, fn, ctx, me, false});
    return g_nextId++;
}
static void RemoveHotkey(int id) { std::lock_guard lk(g_mx); std::erase_if(g_hot, [id](const HotEntry& h) { return h.id == id; }); }
static PbzHost g_host{3, nullptr, Engine, AddTick, RemoveTick, Post, HostLog, HookFn, Unhook, AddHotkey, RemoveHotkey};

static bool GameInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static bool SehHot(void (*fn)(void*), void* ctx);
// game thread, once per frame: key-down edges of registered hotkeys
static void PollHotkeys() {
    std::vector<HotEntry> fire;
    {
        std::lock_guard lk(g_mx);
        if (g_hot.empty()) return;
        bool front = GameInFront();
        int mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000 ? 1 : 0) | (GetAsyncKeyState(VK_SHIFT) & 0x8000 ? 2 : 0) | (GetAsyncKeyState(VK_MENU) & 0x8000 ? 4 : 0);
        for (auto& h : g_hot) {
            bool d = front && (GetAsyncKeyState(h.vk) & 0x8000);
            if (d && !h.down && mods == h.mods) fire.push_back(h);
            h.down = d;
        }
    }
    for (auto& h : fire) if (!SehHot(h.fn, h.ctx)) { LogLine(L"hotkey crashed in " + ModName(h.owner) + L", removed"); RemoveHotkey(h.id); }
}

// __try cannot share a function with C++ objects that need unwinding, hence these wrappers.
static bool SehTick(void (*fn)(void*, float), void* ctx, float dt) {
    __try { fn(ctx, dt); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SehJob(void (*fn)(void*), void* ctx) {
    __try { fn(ctx); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SehHot(void (*fn)(void*), void* ctx) { return SehJob(fn, ctx); }

// Jobs and ticks are taken one at a time under the lock, so an unload (which removes a
// module's entries) takes effect immediately, even in the middle of a frame. Only entries that
// existed when the frame started run in it; ones added meanwhile wait for the next frame.
static void __fastcall TickHook(void* self, float dt, bool idle) {
    g_engine = self;
    g_origTick(self, dt, idle);
    uint64_t lastJob; int lastTick;
    { std::lock_guard lk(g_mx); lastJob = g_jobSeq; lastTick = g_nextId - 1; }
    for (;;) {
        JobEntry j;
        {
            std::lock_guard lk(g_mx);
            if (g_jobs.empty() || g_jobs.front().seq > lastJob) break;
            j = g_jobs.front(); g_jobs.pop_front();
        }
        if (!SehJob(j.fn, j.ctx)) LogLine(L"job crashed in " + ModName(j.owner));
    }
    for (int done = 0;;) {
        TickEntry t{};
        {
            std::lock_guard lk(g_mx);
            auto it = std::find_if(g_ticks.begin(), g_ticks.end(), [&](const TickEntry& e) { return e.id > done; });
            if (it == g_ticks.end() || it->id > lastTick) break;
            t = *it;
        }
        done = t.id;
        if (!SehTick(t.fn, t.ctx, dt)) {
            LogLine(L"tick crashed in " + ModName(t.owner) + L", removed");
            RemoveTick(t.id);
        }
    }
    PollHotkeys();
}

static bool HookTick() {
    auto slot = reinterpret_cast<TickFn*>(g_disc.engineVt + g_disc.tickSlot * 8);
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto cur = reinterpret_cast<uintptr_t>(*slot);
    if (cur < base || cur >= base + nt->OptionalHeader.SizeOfImage) return false;  // not unpacked yet
    g_origTick = *slot;
    DWORD old; VirtualProtect(slot, 8, PAGE_READWRITE, &old);
    *slot = TickHook;
    VirtualProtect(slot, 8, old, &old);
    return true;
}

// Finds the engine globals; called every loader poll until it returns true. Each piece is
// searched for (discover.cpp); a known address from the 11-04 build is used only when the search
// gives no single answer and that address still checks out.
static void DiscLog(const std::wstring& s) { LogLine(s); }
static bool Discover() {
    static int tries;
    ++tries;
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto& r = g_disc;
    if (!r.objects && !disc::FindObjectArray(r, DiscLog)) {
        if (tries < 40) return false;
        disc::Result k; k.objects = base + kRvaObjects;
        if (!disc::FindObjectArray(k, DiscLog)) return false;  // k.objects is checked first
        r.objects = k.objects; r.numObjects = k.numObjects; r.how += L" objects (known)";
    }
    if (!r.namePool && !disc::FindNamePool(r, DiscLog)) {
        if (tries < 40) return false;
        disc::Result k; k.namePool = base + kRvaNamePool;
        if (!disc::FindNamePool(k, DiscLog)) return false;
        r.namePool = k.namePool; r.how += L" names (known)";
    }
    static int engTries;
    if (*reinterpret_cast<volatile int32_t*>(r.numObjects) <= 50000) return false;
    if (!disc::FindEngine(r, DiscLog)) {
        if (++engTries < 40 || r.engineVt != base + kRvaEngineVt) return false;
        r.tickSlot = kTickSlot; r.processEvent = kProcessEvent; r.how += L" tick/ProcessEvent (known)";
    }
    if (!HookTick()) return false;  // vtable slot not pointing into the exe yet
    g_host.objects = reinterpret_cast<void*>(r.objects);
    g_host.namePool = reinterpret_cast<void*>(r.namePool);
    g_host.processEvent = r.processEvent;
    g_host.tickSlot = r.tickSlot;
    wchar_t b[200];
    swprintf_s(b, L"engine: objects %s, names %s, engine vtable %s, tick slot 0x%x, ProcessEvent slot 0x%x",
               disc::Hex(r.objects).c_str(), disc::Hex(r.namePool).c_str(), disc::Hex(r.engineVt).c_str(), r.tickSlot, r.processEvent);
    LogLine(b);
    LogLine(L"engine found by:" + r.how);
    return true;
}

// ---- modules (loader thread) ---------------------------------------------------------------
using InitFn = void(__cdecl*)(const PbzHost*);
using RunFn = void(__cdecl*)(const wchar_t*);
using UnloadFn = void(__cdecl*)();
struct Loaded { HMODULE m; std::wstring tmp; };
static std::map<std::wstring, Loaded> g_mods;  // modules with PbzUnload, by name

static bool SehInit(InitFn fn) { __try { fn(&g_host); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool SehRun(RunFn fn, const wchar_t* dir) { __try { fn(dir); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool SehUnload(UnloadFn fn) { __try { fn(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

static void Purge(HMODULE m) {
    std::vector<int> hooks;
    {
        std::lock_guard lk(g_mx);
        std::erase_if(g_ticks, [m](const TickEntry& t) { return t.owner == m; });
        std::erase_if(g_jobs, [m](const JobEntry& j) { return j.owner == m; });
        std::erase_if(g_hot, [m](const HotEntry& h) { return h.owner == m; });
        for (auto& h : g_hooks) if (h.owner == m) hooks.push_back(h.id);
    }
    for (int id : hooks) Unhook(id);
}
static bool Registered(HMODULE m) {
    std::lock_guard lk(g_mx);
    return std::any_of(g_ticks.begin(), g_ticks.end(), [m](auto& t) { return t.owner == m; }) ||
           std::any_of(g_jobs.begin(), g_jobs.end(), [m](auto& j) { return j.owner == m; }) ||
           std::any_of(g_hooks.begin(), g_hooks.end(), [m](auto& h) { return h.owner == m; }) ||
           std::any_of(g_hot.begin(), g_hot.end(), [m](auto& h) { return h.owner == m; });
}

struct UnloadJob { HMODULE m; UnloadFn fn; HANDLE done; };
static void RunUnload(void* p) {
    auto u = static_cast<UnloadJob*>(p);
    if (!SehUnload(u->fn)) LogLine(L"PbzUnload crashed in " + ModName(u->m));
    Purge(u->m);
    SetEvent(u->done);
}

// PbzUnload runs on the game thread (it touches UE objects); the DLL is freed here afterwards.
static bool Unload(const std::wstring& name) {
    auto it = g_mods.find(name);
    if (it == g_mods.end()) return true;
    auto u = new UnloadJob{it->second.m, reinterpret_cast<UnloadFn>(GetProcAddress(it->second.m, "PbzUnload")),
                           CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!g_origTick) RunUnload(u);  // engine not ticking yet: nothing in the game to undo
    else { std::lock_guard lk(g_mx); g_jobs.push_back({RunUnload, u, g_self, ++g_jobSeq}); }
    if (WaitForSingleObject(u->done, 60000) != WAIT_OBJECT_0) {
        LogLine(L"unload timed out: " + name);  // u stays allocated: the job may still run
        return false;
    }
    CloseHandle(u->done);
    FreeLibrary(u->m);
    DeleteFileW(it->second.tmp.c_str());
    delete u;
    g_mods.erase(it);
    LogLine(L"unloaded " + name);
    return true;
}

static std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == 0xfeff)) ++i;
    return s.substr(i);
}

// Host API level modules may ask for with "api=N" in <name>.txt. Bump when PbzHost grows.
constexpr int kHostApi = 3;
// Mod managers find our version.dll by this export name (they scan the file for "PbzLoaderApi").
extern "C" __declspec(dllexport) int PbzLoaderApi() { return kHostApi; }

// "<name>.txt" next to the dll: key=value lines. Returns the value of key or "".
static std::wstring InfoValue(const std::wstring& txt, const wchar_t* key) {
    FILE* f = _wfopen(txt.c_str(), L"r, ccs=UTF-8");
    if (!f) return L"";
    wchar_t line[1024];
    std::wstring k = std::wstring(key) + L"=", out;
    while (fgetws(line, 1023, f)) {
        std::wstring s = Trim(line);
        if (s.rfind(k, 0) == 0) { out = Trim(s.substr(k.size())); break; }
    }
    fclose(f);
    return out;
}

static void RunModule(const std::wstring& name) {
    static int counter = 0;
    if (!Unload(name)) return;  // the old copy is still busy
    // a mod lives in its own subfolder (<name>\<name>.dll, preferred) or flat in the mod dir
    // (<name>.dll, legacy). PbzRun gets that folder; host->dir stays the mod dir root.
    std::wstring base = g_dir;
    if (GetFileAttributesW((g_dir + name + L"\\" + name + L".dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        base = g_dir + name + L"\\";
    if (auto api = InfoValue(base + name + L".txt", L"api"); !api.empty() && _wtoi(api.c_str()) > kHostApi) {
        LogLine(L"skip " + name + L": needs loader api " + api + L", this loader has " + std::to_wstring(kHostApi));
        return;
    }
    std::wstring src = base + name + L".dll";
    std::wstring tmp = base + L"_loaded_" + name + std::to_wstring(++counter) + L".dll";
    if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE)) { LogLine(L"copy failed: " + src); return; }
    HMODULE m = LoadLibraryW(tmp.c_str());
    if (!m) { LogLine(L"load failed: " + tmp + L" error " + std::to_wstring(GetLastError())); return; }
    auto init = reinterpret_cast<InitFn>(GetProcAddress(m, "PbzInit"));
    auto run = reinterpret_cast<RunFn>(GetProcAddress(m, "PbzRun"));
    bool unloadable = GetProcAddress(m, "PbzUnload") != nullptr;
    bool keep = unloadable || GetProcAddress(m, "PbzKeep") != nullptr;
    if (unloadable) g_mods[name] = {m, tmp};
    if (init && !SehInit(init)) LogLine(L"PbzInit crashed in " + name);
    if (run) {
        LogLine(L"run " + name);
        if (!SehRun(run, base.c_str())) LogLine(L"module crashed (SEH) " + name);
        LogLine(L"done " + name);
    }
    if (!init && !run) LogLine(L"no PbzInit/PbzRun in " + name);
    if (keep) return;
    if (Registered(m)) { LogLine(L"kept " + name + L": it left callbacks but exports neither PbzKeep nor PbzUnload"); return; }
    FreeLibrary(m);
    DeleteFileW(tmp.c_str());
}

static std::vector<std::wstring> ReadLines(const std::wstring& path, bool remove) {
    std::vector<std::wstring> out;
    FILE* f = _wfopen(path.c_str(), L"r, ccs=UTF-8");
    if (!f) return out;
    wchar_t line[512];
    while (fgetws(line, 511, f)) {
        auto s = Trim(line);
        if (!s.empty() && s[0] != L'#') out.push_back(s);
    }
    fclose(f);
    if (remove) DeleteFileW(path.c_str());
    return out;
}

// Copies left by earlier runs (_loaded_<name>N.dll): modules that stay loaded (PbzKeep, or that left
// callbacks) can't be deleted while the game runs, so they are cleared at the next start, before any
// module loads. A copy still mapped by another game process fails to delete and is left alone.
static int DeleteStaleCopies(const std::wstring& dir) {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"_loaded_*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileW((dir + fd.cFileName).c_str())) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
static void CleanStaleCopies() {
    int n = DeleteStaleCopies(g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                n += DeleteStaleCopies(g_dir + fd.cFileName + L"\\");
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n) LogLine(L"removed " + std::to_wstring(n) + L" old module copies");
}

static DWORD WINAPI Worker(LPVOID) {
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_host.dir = g_dir.c_str();
    LogLine(L"loader started");
    CleanStaleCopies();
    MH_Initialize();
    // Another mod that also ships a version.dll proxy: rename theirs to version_chain.dll next to
    // ours and it is loaded here (its own exports are not used; ours forward to System32).
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring chain(exe);
        chain.resize(chain.find_last_of(L"\\/") + 1);
        chain += L"version_chain.dll";
        if (GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES) {
            HMODULE c = LoadLibraryW(chain.c_str());
            LogLine(c ? L"chain loaded version_chain.dll" : L"chain load failed: version_chain.dll error " + std::to_wstring(GetLastError()));
        }
    }
    bool autorun = false;
    for (;;) {
        Sleep(g_origTick ? 250 : 500);
        // hook once the engine is up (object array filled, vtable slot points into the exe)
        if (!g_origTick && Discover()) LogLine(L"tick hooked");
        if (g_origTick && !autorun) {
            autorun = true;
            for (auto& n : ReadLines(g_dir + L"autorun.txt", false)) RunModule(n);
        }
        for (auto& cmd : ReadLines(g_dir + L"cmd.txt", true)) {
            if (cmd.rfind(L"run ", 0) == 0) RunModule(Trim(cmd.substr(4)));
            else if (cmd.rfind(L"stop ", 0) == 0) Unload(Trim(cmd.substr(5)));
            else LogLine(L"unknown cmd: " + cmd);
        }
    }
}

// Second local instance: the game refuses to start while PBZ_SingleInstance_Mutex exists. We
// run before the game's main, so rename the mutex in the exe image, derived from the folder name
// so every PBZMP_DIR gets its own.
static void RenameInstanceMutex(const wchar_t* tag) {
    uint32_t h = 2166136261u;
    for (const wchar_t* c = tag; *c; ++c) h = (h ^ uint32_t(*c)) * 16777619u;
    wchar_t repl[3] = {wchar_t(L'A' + h % 26), wchar_t(L'A' + h / 26 % 26), wchar_t(L'A' + h / 676 % 26)};
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;
    const wchar_t pat[] = L"PBZ_SingleInstance_Mutex";
    for (size_t i = 0; i + sizeof pat < size; i += 2) {
        MEMORY_BASIC_INFORMATION mi;
        if ((i & 0xfff) == 0 && (!VirtualQuery(base + i, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
                                 (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))) { i += 0xffe; continue; }
        if (memcmp(base + i, pat, sizeof pat - 2) != 0) continue;
        DWORD old; VirtualProtect(base + i, 6, PAGE_READWRITE, &old);
        memcpy(base + i, repl, 6);
        VirtualProtect(base + i, 6, old, &old);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = h;
    DisableThreadLibraryCalls(h);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!wcsstr(exe, L"PBZ.exe")) return TRUE;
    g_dir = ModDir();
    // modules built before host.h (gamethread.h) find the host through this variable
    wchar_t ptr[32];
    swprintf(ptr, 32, L"%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&g_host)));
    SetEnvironmentVariableW(L"PBZMP_HOST", ptr);
    if (auto tag = InstanceTag(); !tag.empty()) RenameInstanceMutex(tag.c_str());
    CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    return TRUE;
}

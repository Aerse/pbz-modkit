#include "uex.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <eh.h>

using namespace ue;

namespace uex {
static const PbzHost* g_host;
void SetHost(const PbzHost* h) { g_host = h; }
const PbzHost* Host() { return g_host; }

void Log(const wchar_t* fmt, ...) {
    wchar_t b[1024] = L"mp: ";
    va_list a; va_start(a, fmt); _vsnwprintf_s(b + 4, 1020, _TRUNCATE, fmt, a); va_end(a);
    if (g_host) g_host->log(b);
}

UObject* Lib(const wchar_t* n) { return FindObject(n); }
UObject* GameplayStatics() { return FindObject(L"GameplayStatics /Script/Engine.Default__GameplayStatics"); }
UObject* SystemLib() { return FindObject(L"KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary"); }
UObject* Class(const wchar_t* n) { return FindObject(n); }

static UObject* WidgetLib() { return FindObject(L"WidgetBlueprintLibrary /Script/UMG.Default__WidgetBlueprintLibrary"); }
static UObject* StringLib() { return FindObject(L"KismetStringLibrary /Script/Engine.Default__KismetStringLibrary"); }
static UObject* TextLib() { return FindObject(L"KismetTextLibrary /Script/Engine.Default__KismetTextLibrary"); }

UObject* Engine() { return g_host ? static_cast<UObject*>(g_host->engine()) : nullptr; }
UObject* GameInstance() { auto e = Engine(); return e ? At<UObject*>(e, 0x12e0) : nullptr; }
UObject* World() {
    auto e = Engine(); if (!e) return nullptr;
    auto vp = At<UObject*>(e, 0xc88);
    return vp ? At<UObject*>(vp, 0x78) : nullptr;
}
UObject* PC() {
    auto gi = GameInstance(); if (!gi) return nullptr;
    auto arr = At<UObject**>(gi, 0x38);
    if (!arr || At<int32_t>(gi, 0x40) < 1 || !arr[0]) return nullptr;
    return At<UObject*>(arr[0], 0x30);
}
UObject* Me() { auto pc = PC(); return pc ? At<UObject*>(pc, 0x2f0) : nullptr; }

// a stale pointer (the object freed, its page released) faults on the first read: no object then
static int SafeIndex(UObject* o) {
    __try { return o->index; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool Alive(UObject* o) {
    if (!o) return false;
    int i = SafeIndex(o);
    if (i < 0 || i >= NumObjects() || ObjectAt(i) != o) return false;
    auto f = Fn(o, L"IsActorBeingDestroyed");
    if (!f) return true;
    Params p(f); p.Call(o);
    return !p.Get<bool>(L"ReturnValue");
}
Ref Hold(UObject* o) { return o ? Ref{o, o->index, o->name.comparison, o->name.number, o->cls} : Ref{}; }
UObject* Get(const Ref& r) {
    auto o = r.o;  // the slot check first: only then is o known to be an object we may read
    if (!o || r.index < 0 || r.index >= NumObjects() || ObjectAt(r.index) != o) return nullptr;
    if (o->index != r.index || o->name.comparison != r.name || o->name.number != r.number || o->cls != r.cls) return nullptr;
    return Alive(o) ? o : nullptr;
}

static bool InChain(UObject* cls, UObject* owner) {
    for (auto c = cls; c; c = At<UObject*>(c, kUStruct_Super)) if (c == owner) return true;
    return false;
}
UObject* Fn(UObject* self, const wchar_t* name) {
    struct E { UObject* fn; uint32_t clsName; };
    static std::map<std::pair<UObject*, std::wstring>, E> cache;
    if (!self || !self->cls) return nullptr;
    UObject* cls = self->cls;
    auto key = std::make_pair(cls, std::wstring(name));
    if (auto it = cache.find(key); it != cache.end()) {
        // classes are freed too (level blueprints) and their addresses reused: check the entry
        auto [f, cn] = it->second;
        if (cn == cls->name.comparison && (!f || (SafeIndex(f) >= 0 && f->index < NumObjects() && ObjectAt(f->index) == f &&
                                                  NameIs(f->name, name) && InChain(cls, f->outer))))
            return f;
    }
    UObject* f = FindFunction(cls, name);
    cache[key] = {f, cls->name.comparison};
    return f;
}

// ---- diagnostics ------------------------------------------------------------------------------
namespace {
struct SehError {};
struct SehInfo { unsigned code; ULONG_PTR kind, addr; void* at[32]; int n; };
SehInfo g_seh;
// return addresses from the faulting context; stops outside this thread's stack
int Walk(const CONTEXT* from, void** out, int max) {
    CONTEXT c = *from;
    int n = 0;
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    __try {
        if (!c.Rip && c.Rsp >= lo && c.Rsp < hi) {  // called a null function pointer: start at its caller
            out[n++] = nullptr; c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); c.Rsp += 8;
        }
        while (n < max && c.Rip) {
            out[n++] = reinterpret_cast<void*>(c.Rip);
            if (c.Rsp < lo || c.Rsp >= hi) break;
            DWORD64 base = 0;
            auto fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (fe) { void* hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &ef, nullptr); }
            else { c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); c.Rsp += 8; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
void __cdecl Translate(unsigned code, EXCEPTION_POINTERS* ep) {
    g_seh = {};
    g_seh.code = code;
    auto r = ep ? ep->ExceptionRecord : nullptr;
    if (r && r->NumberParameters >= 2) { g_seh.kind = r->ExceptionInformation[0]; g_seh.addr = r->ExceptionInformation[1]; }
    if (ep && ep->ContextRecord && code != EXCEPTION_STACK_OVERFLOW) g_seh.n = Walk(ep->ContextRecord, g_seh.at, 32);
    else if (r) { g_seh.at[0] = r->ExceptionAddress; g_seh.n = 1; }
    throw SehError{};
}
HMODULE ModuleOf(const void* p) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(p), &m);
    return m;
}
std::wstring Where(void* p) {
    wchar_t b[MAX_PATH + 40];
    HMODULE m = ModuleOf(p);
    if (!m) { swprintf_s(b, L"%p", p); return b; }
    wchar_t f[MAX_PATH] = L"?";
    GetModuleFileNameW(m, f, MAX_PATH);
    const wchar_t* base = wcsrchr(f, L'\\');
    swprintf_s(b, L"%s+%llx", base ? base + 1 : f, (unsigned long long)(static_cast<uint8_t*>(p) - reinterpret_cast<uint8_t*>(m)));
    return b;
}
}  // namespace
SehScope::SehScope() : prev(reinterpret_cast<void*>(_set_se_translator(Translate))) {}
SehScope::~SehScope() { _set_se_translator(reinterpret_cast<_se_translator_function>(prev)); }
std::wstring ExcWhere() {
    if (!g_seh.code) return L"c++";
    wchar_t h[96];
    swprintf_s(h, L"seh %08x", g_seh.code);
    std::wstring s = h;
    if (g_seh.code == 0xC0000005) {
        swprintf_s(h, L" (%s %llx)", g_seh.kind == 1 ? L"write" : g_seh.kind == 8 ? L"exec" : L"read", (unsigned long long)g_seh.addr);
        s += h;
    }
    // the faulting address, then the first few frames in mp.dll (the game's own frames in between are skipped)
    HMODULE self = ModuleOf(reinterpret_cast<const void*>(&ExcWhere));
    for (int i = 0, shown = 0; i < g_seh.n && shown < 6; ++i)
        if (i == 0 || ModuleOf(g_seh.at[i]) == self) { s += (i ? L" < " : L" at ") + Where(g_seh.at[i]); ++shown; }
    g_seh = {};
    return s;
}

struct alignas(16) FTransform { double rot[4]; double loc[3]; double pad0; double scale[3]; double pad1; };

UObject* Spawn(UObject* cls, Vec loc, double yaw, UObject* owner) {
    auto gs = GameplayStatics(); auto ctx = World();
    if (!gs || !cls || !ctx) return nullptr;
    FTransform tf{};
    double h = yaw * 3.14159265358979 / 360.0;
    tf.rot[2] = std::sin(h); tf.rot[3] = std::cos(h);
    tf.loc[0] = loc.x; tf.loc[1] = loc.y; tf.loc[2] = loc.z;
    tf.scale[0] = tf.scale[1] = tf.scale[2] = 1;
    Params b(Fn(gs, L"BeginDeferredActorSpawnFromClass"));
    b.Set(L"WorldContextObject", ctx); b.Set(L"ActorClass", cls); b.Set(L"SpawnTransform", tf);
    b.Set<uint8_t>(L"CollisionHandlingOverride", 1); b.Set(L"Owner", owner);
    b.Set<uint8_t>(L"TransformScaleMethod", 1);
    b.Call(gs);
    auto a = b.Get<UObject*>(L"ReturnValue");
    if (!a) return nullptr;
    Params f(Fn(gs, L"FinishSpawningActor"));
    f.Set(L"Actor", a); f.Set(L"SpawnTransform", tf); f.Set<uint8_t>(L"TransformScaleMethod", 1);
    f.Call(gs);
    return f.Get<UObject*>(L"ReturnValue");
}

void Destroy(UObject* a) { if (Alive(a)) { Params p(Fn(a, L"K2_DestroyActor")); p.Call(a); } }

Vec Loc(UObject* a) { Params p(Fn(a, L"K2_GetActorLocation")); p.Call(a); return p.Get<Vec>(L"ReturnValue"); }
Rot GetRot(UObject* a) { Params p(Fn(a, L"K2_GetActorRotation")); p.Call(a); return p.Get<Rot>(L"ReturnValue"); }

bool Teleport(UObject* a, Vec loc, double yaw) {
    Params p(Fn(a, L"K2_SetActorLocationAndRotation"));
    p.Set(L"NewLocation", loc); p.Set(L"NewRotation", Rot{0, yaw, 0});
    p.Set<bool>(L"bSweep", false); p.Set<bool>(L"bTeleport", true);
    p.Call(a);
    return p.Get<bool>(L"ReturnValue");
}

void SetControlYaw(double yaw) {
    auto pc = PC(); if (!pc) return;
    Params p(Fn(pc, L"SetControlRotation")); p.Set(L"NewRotation", Rot{0, yaw, 0}); p.Call(pc);
}

UObject* Comp(UObject* a, UObject* cls) {
    if (!a || !cls) return nullptr;
    Params p(Fn(a, L"GetComponentByClass")); p.Set(L"ComponentClass", cls); p.Call(a);
    return p.Get<UObject*>(L"ReturnValue");
}

struct HitBuf { uint8_t b[0x100]; };
bool Trace(UObject* ctx, Vec a, Vec b, Vec* hit) {
    auto lib = SystemLib(); if (!lib || !ctx) return false;
    Params t(Fn(lib, L"LineTraceSingle"));
    t.Set(L"WorldContextObject", ctx); t.Set(L"Start", a); t.Set(L"End", b);
    t.Set<uint8_t>(L"TraceChannel", 0); t.Set<bool>(L"bIgnoreSelf", true);
    t.Call(lib);
    if (!t.Get<bool>(L"ReturnValue")) return false;
    if (hit) *hit = *reinterpret_cast<const Vec*>(t.Get<HitBuf>(L"OutHit").b + 0x10);
    return true;
}

static uint8_t* AttrSet(UObject* ch) { return ch ? At<uint8_t*>(ch, 0x758) : nullptr; }
float Attr(UObject* ch, int off) { auto s = AttrSet(ch); return s ? *reinterpret_cast<float*>(s + off + 0xC) : 0.f; }
void SetAttr(UObject* ch, int off, float v) {
    auto s = AttrSet(ch); if (!s) return;
    *reinterpret_cast<float*>(s + off + 8) = v; *reinterpret_cast<float*>(s + off + 0xC) = v;
}

struct FStr { const wchar_t* d; int32_t n, m; };
static FStr View(const std::wstring& s) { return {s.c_str(), int32_t(s.size() + 1), int32_t(s.size() + 1)}; }

UObject* LoadAsset(const wchar_t* path) {
    // a checked Ref: GC unloads assets on a level change, their memory is freed or holds another object
    static std::unordered_map<std::wstring, Ref> cache;
    if (auto it = cache.find(path); it != cache.end()) {
        if (auto o = Get(it->second)) return o;
        cache.erase(it);
    }
    auto lib = SystemLib(); if (!lib) return nullptr;
    std::wstring s(path);
    Params a(Fn(lib, L"MakeSoftObjectPath")); a.Set(L"PathString", View(s)); a.Call(lib);
    struct P32 { uint8_t b[32]; }; struct P40 { uint8_t b[40]; };
    Params b(Fn(lib, L"Conv_SoftObjPathToSoftObjRef")); b.Set(L"SoftObjectPath", a.Get<P32>(L"ReturnValue")); b.Call(lib);
    Params c(Fn(lib, L"LoadAsset_Blocking")); c.Set(L"Asset", b.Get<P40>(L"ReturnValue")); c.Call(lib);
    auto o = c.Get<UObject*>(L"ReturnValue");
    if (o) cache[path] = Hold(o); else Log(L"asset missing %s", path);
    return o;
}

void Console(const wchar_t* cmd) {
    auto lib = SystemLib(); auto w = World(); if (!lib || !w) return;
    std::wstring s(cmd);
    Params p(Fn(lib, L"ExecuteConsoleCommand"));
    p.Set(L"WorldContextObject", w); p.Set(L"Command", View(s)); p.Set<UObject*>(L"SpecificPlayer", nullptr);
    p.Call(lib);
}

FText Text(const std::wstring& s) {
    static std::unordered_map<std::wstring, FText> cache;
    if (auto it = cache.find(s); it != cache.end()) return it->second;
    auto lib = TextLib(); if (!lib) return {};
    Params p(Fn(lib, L"Conv_StringToText")); p.Set(L"InString", View(s)); p.Call(lib);
    return cache[s] = p.Get<FText>(L"ReturnValue");
}
FName Name(const std::wstring& s) {
    static std::unordered_map<std::wstring, FName> cache;
    if (auto it = cache.find(s); it != cache.end()) return it->second;
    auto lib = StringLib(); if (!lib) return {};
    Params p(Fn(lib, L"Conv_StringToName")); p.Set(L"InString", View(s)); p.Call(lib);
    return cache[s] = p.Get<FName>(L"ReturnValue");
}
std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.c_str(), int(s.size()), o.data(), n, nullptr, nullptr); return o;
}
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring o(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), o.data(), n); return o;
}

UObject* CreateWidget(UObject* cls) {
    auto lib = WidgetLib(); auto pc = PC(); if (!lib || !cls) return nullptr;
    Params p(Fn(lib, L"Create"));
    p.Set(L"WorldContextObject", pc ? pc : World()); p.Set(L"WidgetType", cls); p.Set(L"OwningPlayer", pc);
    p.Call(lib); return p.Get<UObject*>(L"ReturnValue");
}
void AddToViewport(UObject* w, int z) { if (!w) return; Params p(Fn(w, L"AddToViewport")); p.Set<int32_t>(L"ZOrder", z); p.Call(w); }
static std::unordered_map<UObject*, std::wstring> g_lastText;
void RemoveWidget(UObject* w) { if (!Alive(w)) return; Params p(Fn(w, L"RemoveFromParent")); p.Call(w); }
void SetText(UObject* tb, const std::wstring& s) {
    if (!tb) return;
    if (auto it = g_lastText.find(tb); it != g_lastText.end() && it->second == s) return;
    g_lastText[tb] = s;
    Params p(Fn(tb, L"SetText")); p.Set(L"InText", Text(s)); p.Call(tb);
}
void ForgetTexts() { g_lastText.clear(); }
void SetVisible(UObject* w, bool v) { if (!w) return; Params p(Fn(w, L"SetVisibility")); p.Set<uint8_t>(L"InVisibility", v ? 0 : 1); p.Call(w); }
void UiMode(UObject* focus) {
    auto lib = WidgetLib(); auto pc = PC(); if (!lib || !pc) return;
    Params p(Fn(lib, L"SetInputMode_UIOnlyEx"));
    p.Set(L"PlayerController", pc); p.Set(L"InWidgetToFocus", focus); p.Set<uint8_t>(L"InMouseLockMode", 0); p.Set<bool>(L"bFlushInput", true);
    p.Call(lib); reinterpret_cast<uint8_t*>(pc)[0x554] |= 1;
}
void GameMode() {
    auto lib = WidgetLib(); auto pc = PC(); if (!lib || !pc) return;
    Params p(Fn(lib, L"SetInputMode_GameOnly")); p.Set(L"PlayerController", pc); p.Set<bool>(L"bFlushInput", true);
    p.Call(lib); reinterpret_cast<uint8_t*>(pc)[0x554] &= ~1;
}

void SaveVeto(bool on, const wchar_t* source) {
    // ExitSaveVeto makes the game write the save ~5 s later even while another source still
    // holds a veto. Test instances run nosave (it sets PBZMP_NOSAVE): then never exit at all.
    // Same once mp has opened a level (fileguard on): the game would write that level's state.
    bool nosave = GetEnvironmentVariableW(L"PBZMP_NOSAVE", nullptr, 0) > 0 || GetEnvironmentVariableW(L"PBZMP_FILEGUARD", nullptr, 0) > 0;
    if (wcscmp(source, L"pbzmp_nosave") == 0) {
        if (on) SetEnvironmentVariableW(L"PBZMP_NOSAVE", L"1");
        else return;  // test instances keep the veto until the process ends
    } else if (!on && nosave) return;
    auto lib = Lib(L"SubsystemBlueprintLibrary /Script/Engine.Default__SubsystemBlueprintLibrary");
    auto cls = Class(L"Class /Script/ProjectPBZ.PBZSaveLoadSubsystem"); auto gi = GameInstance();
    if (!lib || !cls || !gi) return;
    Params g(Fn(lib, L"GetGameInstanceSubsystem")); g.Set(L"ContextObject", gi); g.Set(L"Class", cls); g.Call(lib);
    auto ss = g.Get<UObject*>(L"ReturnValue"); if (!ss) return;
    Params p(Fn(ss, on ? L"EnterSaveVeto" : L"ExitSaveVeto"));
    p.Set(L"SourceID", Name(source)); if (on) p.Set<uint8_t>(L"Reason", 2); p.Call(ss);
}
void Hostile(UObject* a, UObject* b, bool on) {
    auto cls = Class(L"Class /Script/SGameCombatSystem.FactionComponent");
    UObject* pair[2][2] = {{a, b}, {b, a}};
    for (auto& pr : pair) {
        auto fc = Comp(pr[0], cls); if (!fc) continue;
        Params p(Fn(fc, on ? L"SetFactionRelationOverride" : L"ClearFactionRelationOverride"));
        p.Set(L"OtherActor", pr[1]); if (on) p.Set<uint8_t>(L"Relation", 2); p.Set<bool>(L"Both", true);
        p.Call(fc);
    }
}
bool Ability(UObject* ch, UObject* cls) {
    auto asc = ch ? At<UObject*>(ch, 0x750) : nullptr; if (!asc || !cls) return false;
    Params p(Fn(asc, L"TryActivateAbilityByClass")); p.Set(L"InAbilityToActivate", cls); p.Set<bool>(L"bAllowRemoteActivation", true);
    p.Call(asc); return p.Get<bool>(L"ReturnValue");
}
// SetIgnoreMoveInput/LookInput are counters in UE (+1/-1 per call). Only change them when our
// own wanted state changes, so per-frame calls are harmless and we never undo the game's own.
static bool g_ignMove, g_ignLook; static Ref g_ignPc;
void IgnoreInput(bool move, bool look) {
    auto pc = PC(); if (!pc) return;
    if (Get(g_ignPc) != pc) { g_ignMove = g_ignLook = false; g_ignPc = Hold(pc); }  // a new controller (level change) ignores nothing
    if (move != g_ignMove) { Params p(Fn(pc, L"SetIgnoreMoveInput")); p.Set<bool>(L"bNewMoveInput", move); p.Call(pc); g_ignMove = move; }
    if (look != g_ignLook) { Params p(Fn(pc, L"SetIgnoreLookInput")); p.Set<bool>(L"bNewLookInput", look); p.Call(pc); g_ignLook = look; }
}
UObject* Movement(UObject* ch) {
    static UObject* cls = nullptr;
    if (!cls) cls = Class(L"Class /Script/Engine.CharacterMovementComponent");
    return Comp(ch, cls);
}
void SetMoveMode(UObject* ch, int mode) {
    auto cm = Movement(ch); if (!cm) return;
    if (mode == 0) { Params p(Fn(cm, L"DisableMovement")); p.Call(cm); return; }
    Params p(Fn(cm, L"SetMovementMode")); p.Set<uint8_t>(L"NewMovementMode", uint8_t(mode)); p.Set<uint8_t>(L"NewCustomMode", 0); p.Call(cm);
}

// ---- game menu keys ------------------------------------------------------------------------
// InputMappingContext::DefaultKeyMappings.Mappings (+0x40): FEnhancedActionKeyMapping, stride 0x50,
// Action +0x20, FKey +0x28. A blocked entry gets a null Action (the control mapping rebuild skips
// those; changing the key instead is undone by the user key profile for rebindable keys). The
// original action is kept here and written back by BlockMenuKeys(false).
namespace {
struct Blocked { UObject* imc; int i; UObject* action; };
std::vector<Blocked> g_blocked;
ULONGLONG g_blockScan;
struct MArr { uint8_t* d; int32_t n, m; };
bool MenuAction(UObject* a) { return a && (NameIs(a->name, L"IA_PushGameSystemMenu") || NameIs(a->name, L"IA_Map")); }
bool Live(UObject* o) { return o && o->index >= 0 && o->index < NumObjects() && ObjectAt(o->index) == o; }
void Rebuild(UObject* imc) {
    auto lib = Lib(L"EnhancedInputLibrary /Script/EnhancedInput.Default__EnhancedInputLibrary");
    if (!lib) return;
    Params p(Fn(lib, L"RequestRebuildControlMappingsUsingContext"));
    p.Set(L"Context", imc); p.Set<bool>(L"bForceImmediately", true); p.Call(lib);
}
UObject*& ActionAt(UObject* imc, int i) {
    auto a = *reinterpret_cast<MArr*>(reinterpret_cast<uint8_t*>(imc) + 0x40);
    return *reinterpret_cast<UObject**>(a.d + i * 0x50 + 0x20);
}
int Count(UObject* imc) { return reinterpret_cast<MArr*>(reinterpret_cast<uint8_t*>(imc) + 0x40)->n; }
}  // namespace
void BlockMenuKeys(bool on) {
    if (!on) {
        if (g_blocked.empty()) return;
        std::vector<UObject*> touched;
        for (auto& b : g_blocked) {
            if (!Live(b.imc) || b.i >= Count(b.imc) || ActionAt(b.imc, b.i)) continue;
            ActionAt(b.imc, b.i) = b.action;
            if (touched.empty() || touched.back() != b.imc) touched.push_back(b.imc);
        }
        Log(L"menu keys restored (%d mappings)", int(g_blocked.size()));
        g_blocked.clear(); g_blockScan = 0;
        for (auto imc : touched) Rebuild(imc);
        return;
    }
    // contexts load with the HUD / level: look again every few seconds
    if (g_blockScan && GetTickCount64() - g_blockScan < 3000) return;
    g_blockScan = GetTickCount64();
    static UObject* cls; if (!cls) cls = Class(L"Class /Script/EnhancedInput.InputMappingContext");
    if (!cls) return;
    for (int k = 0, n = NumObjects(); k < n; ++k) {
        auto o = ObjectAt(k);
        if (!o || o->cls != cls || (o->flags & 0x30)) continue;
        auto a = *reinterpret_cast<MArr*>(reinterpret_cast<uint8_t*>(o) + 0x40);
        bool changed = false;
        for (int i = 0; i < a.n && i < 512; ++i) {
            auto e = a.d + i * 0x50; auto act = At<UObject*>(e, 0x20);
            if (!MenuAction(act)) continue;
            g_blocked.push_back({o, i, act});
            Log(L"menu key %s blocked in %s", NameOf(*reinterpret_cast<FName*>(e + 0x28)).c_str(), NameOf(o).c_str());
            ActionAt(o, i) = nullptr; changed = true;
        }
        if (changed) Rebuild(o);
    }
}
}  // namespace uex

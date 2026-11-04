// input.cpp: keyboard / mouse state (only while the game window is in front) and blocking the game's
// own bindings for our keys. While a rifle is out, the game's LMB / RMB / R / T / 1 / 2 / 3 mappings get a
// null action (the trick uex::BlockMenuKeys uses), so LMB no longer swings the sword.
// InputMappingContext +0x40 Mappings, stride 0x50, Action +0x20, FKey (FName) +0x28.
#include "common.h"

bool Front() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
bool Down(int vk) { return Front() && (GetAsyncKeyState(vk) & 0x8000); }
static bool g_prev[256];
bool Pressed(int vk) { bool d = Down(vk), p = d && !g_prev[vk & 255]; g_prev[vk & 255] = d; return p; }

struct Blocked { UObject* imc; int i; UObject* action; };
static std::vector<Blocked> g_blocked;
static ULONGLONG g_blockScan;
struct MArr { uint8_t* d; int32_t n, m; };
static void Rebuild(UObject* imc) {
    auto lib = uex::Lib(L"EnhancedInputLibrary /Script/EnhancedInput.Default__EnhancedInputLibrary"); if (!lib) return;
    ue::Params p(uex::Fn(lib, L"RequestRebuildControlMappingsUsingContext"));
    p.Set(L"Context", imc); p.Set<bool>(L"bForceImmediately", true); p.Call(lib);
}
static MArr& Maps(UObject* imc) { return *reinterpret_cast<MArr*>(reinterpret_cast<uint8_t*>(imc) + 0x40); }
static bool OurKey(const ue::FName& k) {
    for (auto n : {L"LeftMouseButton", L"RightMouseButton", L"T", L"R", L"One", L"Two", L"Three"}) if (ue::NameIs(k, n)) return true;
    return false;
}
void BlockKeys(bool on) {
    if (!on) {
        std::vector<UObject*> touched;
        for (auto& b : g_blocked) {
            if (!Live(b.imc) || b.i >= Maps(b.imc).n) continue;
            auto& act = *reinterpret_cast<UObject**>(Maps(b.imc).d + b.i * 0x50 + 0x20);
            if (act) continue;
            act = b.action;
            if (touched.empty() || touched.back() != b.imc) touched.push_back(b.imc);
        }
        if (!g_blocked.empty()) uex::Log(L"sniper: game keys restored (%d)", int(g_blocked.size()));
        g_blocked.clear(); g_blockScan = 0;
        for (auto imc : touched) Rebuild(imc);
        return;
    }
    if (g_blockScan && GetTickCount64() - g_blockScan < 3000) return;  // contexts load later too
    g_blockScan = GetTickCount64();
    static UObject* cls; if (!cls) cls = uex::Class(L"Class /Script/EnhancedInput.InputMappingContext");
    if (!cls) return;
    for (int k = 0, n = ue::NumObjects(); k < n; ++k) {
        auto o = ue::ObjectAt(k);
        if (!o || o->cls != cls || (o->flags & 0x30)) continue;
        auto& a = Maps(o); bool changed = false;
        for (int i = 0; i < a.n && i < 512; ++i) {
            auto e = a.d + i * 0x50; auto& act = *reinterpret_cast<UObject**>(e + 0x20);
            if (!act || !OurKey(*reinterpret_cast<ue::FName*>(e + 0x28))) continue;
            uex::Log(L"sniper: block %s -> %s in %s", ue::NameOf(*reinterpret_cast<ue::FName*>(e + 0x28)).c_str(),
                ue::NameOf(act).c_str(), ue::NameOf(o).c_str());
            g_blocked.push_back({o, i, act}); act = nullptr; changed = true;
        }
        if (changed) Rebuild(o);
    }
}

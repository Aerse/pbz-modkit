// input.cpp: keyboard / mouse state (only while the game window is in front) and blocking the game's
// own bindings for our keys. While a rifle is out, the game's LMB / RMB / R / T / 1 / 2 / 3 mappings get a
// null action (the trick uex::BlockMenuKeys uses), so LMB no longer swings the sword.
// Mappings through uex::KeyMapping* (found by reflection).
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
static bool OurKey(const ue::FName& k) {
    for (auto n : {L"LeftMouseButton", L"RightMouseButton", L"T", L"R", L"One", L"Two", L"Three"}) if (ue::NameIs(k, n)) return true;
    return false;
}
void BlockKeys(bool on) {
    if (!on) {
        std::vector<UObject*> touched;
        for (auto& b : g_blocked) {
            if (!Live(b.imc) || b.i >= uex::KeyMappingCount(b.imc)) continue;
            auto& act = uex::KeyMappingAction(b.imc, b.i);
            if (act) continue;
            act = b.action;
            if (touched.empty() || touched.back() != b.imc) touched.push_back(b.imc);
        }
        if (!g_blocked.empty()) uex::Log(L"game keys restored (%d)", int(g_blocked.size()));
        g_blocked.clear(); g_blockScan = 0;
        for (auto imc : touched) uex::RebuildMappings(imc);
        return;
    }
    if (g_blockScan && GetTickCount64() - g_blockScan < 3000) return;  // contexts load later too
    g_blockScan = GetTickCount64();
    static UObject* cls; if (!cls) cls = uex::Class(L"Class /Script/EnhancedInput.InputMappingContext");
    if (!cls) return;
    for (int k = 0, n = ue::NumObjects(); k < n; ++k) {
        auto o = ue::ObjectAt(k);
        if (!o || o->cls != cls || (o->flags & 0x30)) continue;
        bool changed = false;
        for (int i = 0, n = uex::KeyMappingCount(o); i < n && i < 512; ++i) {
            auto& act = uex::KeyMappingAction(o, i); auto key = uex::KeyMappingKey(o, i);
            if (!act || !OurKey(key)) continue;
            uex::Log(L"block %s -> %s in %s", ue::NameOf(key).c_str(),
                ue::NameOf(act).c_str(), ue::NameOf(o).c_str());
            g_blocked.push_back({o, i, act}); act = nullptr; changed = true;
        }
        if (changed) uex::RebuildMappings(o);
    }
}

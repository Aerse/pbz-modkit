// houserules: see houserules.h.
// sr / ult: every frame the rise of Shachi / UltimateCharge since the last frame is scaled to the
// rule's %, so whatever raised it (regen, hits, accessories) is scaled the same; drops (hits taken,
// spending) are left alone. Nothing of the game is changed persistently, so End only forgets.
// sd / edge: Phantom Edge moves are recognised by the class name of an active GA of ours
// (IsEdgeGa; the name list is not checked in game yet). acc: AssemblyAccessory slots in use.
#include "houserules.h"
#include <cmath>
#include "uex.h"
#include "ue_call.h"

using namespace ue;

namespace houserules {
namespace {
struct Arr { uint8_t* d; int n, m; };
constexpr const wchar_t* kBp = L"Class /Script/ProjectPBZ.PBZBackPackSubsystem";
uex::Prop kAssembly{kBp, L"AssemblyAccessory", 0x560};
uex::Prop kActivatable{L"Class /Script/GameplayAbilities.AbilitySystemComponent", L"ActivatableAbilities", 0x418};
uex::Prop kSpecItems{L"ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpecContainer", L"Items", 0x108};
uex::Prop kSpecAbility{L"ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", L"Ability", 0x10};
uex::Prop kSpecActive{L"ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", L"ActiveCount", 0x28};

bool IsNone(const FName& n) { return !n.comparison && !n.number; }
bool g_on;
UObject* g_me;
float g_lastShachi = -1, g_lastUlt = -1;
int g_edgeCancels;
float g_sdScale = 1.f;

UObject* Backpack(UObject* me) {
    auto cls = uex::Class(L"Class /Script/ProjectPBZ.PBZBackPackSubsystem");
    auto lib = uex::Lib(L"SubsystemBlueprintLibrary /Script/Engine.Default__SubsystemBlueprintLibrary");
    auto f = lib ? uex::Fn(lib, L"GetWorldSubsystem") : nullptr;
    if (!me || !cls || !f) return nullptr;
    Params g(f); g.Set(L"ContextObject", me); g.Set(L"Class", cls); g.Call(lib);
    return g.Get<UObject*>(L"ReturnValue");
}

// Phantom Edge ability: GA class name mentions Shaqi / ShaChi, not the edge swap itself
bool IsEdgeGa(const std::wstring& n) {
    if (n.find(L"ChangeShachiWeapon") != std::wstring::npos) return false;
    return n.find(L"Shaqi") != std::wstring::npos || n.find(L"ShaQi") != std::wstring::npos ||
           n.find(L"Shachi") != std::wstring::npos || n.find(L"ShaChi") != std::wstring::npos;
}
// f(spec, ga) for every active ability spec of ch
template <class F> void EachActive(UObject* ch, F f) {
    auto asc = uex::ASC(ch); if (!asc) return;
    int size = uex::StructSize(L"ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec", 0xf0);
    Arr a = kSpecItems.Of<Arr>(reinterpret_cast<uint8_t*>(asc) + kActivatable);
    for (int i = 0; a.d && i < a.n && i < 500; ++i) {
        uint8_t* s = a.d + size_t(i) * size;
        if (!kSpecActive.Of<uint8_t>(s)) continue;
        if (UObject* ga = kSpecAbility.Of<UObject*>(s)) f(s, ga);
    }
}
bool EdgeActive(UObject* ch) {
    bool r = false;
    EachActive(ch, [&](uint8_t*, UObject* ga) { if (!r && IsEdgeGa(NameOf(ga->cls))) r = true; });
    return r;
}
// cancel our instanced Phantom Edge GAs (as fight's CancelMoves does for montage GAs)
int CancelEdge(UObject* ch) {
    auto ga = uex::Class(L"Class /Script/GameplayAbilities.GameplayAbility");
    int n = 0;
    for (int i = 0, c = NumObjects(); ga && i < c; ++i) {
        auto o = ObjectAt(i);
        if (!o || o->outer != ch || (o->flags & 0x10) || !IsA(o, ga) || !IsEdgeGa(NameOf(o->cls))) continue;
        if (auto f = uex::Fn(o, L"K2_CancelAbility")) { Params p(f); p.Call(o); ++n; }
    }
    return n;
}

// scale the rise of an attribute since last frame to pct %
void ScaleRise(UObject* me, int off, int maxOff, float& last, int pct) {
    float v = uex::Attr(me, off);
    if (last >= 0 && pct != 100 && v > last + 1e-4f) {
        float nv = last + (v - last) * pct / 100.f, mx = uex::Attr(me, maxOff);
        if (mx > 0 && nv > mx) nv = mx;
        uex::SetAttr(me, off, nv); v = nv;
    }
    last = v;
}
}  // namespace

void Begin(const rules::Rules& r, UObject* me) {
    g_on = me != nullptr; g_me = me;
    g_lastShachi = g_lastUlt = -1; g_edgeCancels = 0; g_sdScale = r.sd / 100.f;
    if (g_on) uex::Log(L"houserules: begin sr %d ult %d sd %d edge %d heal %d", r.sr, r.ult, r.sd, r.edge, r.heal);
}

void Tick(const rules::Rules& r, UObject* me, float) {
    if (!g_on || !me) return;
    if (me != g_me) { g_me = me; g_lastShachi = g_lastUlt = -1; }
    using namespace uex::attr;
    if (uex::Attr(me, ShachiMax) <= 0) return;  // attributes not there yet
    ScaleRise(me, Shachi, ShachiMax, g_lastShachi, r.sr);
    ScaleRise(me, Ult, UltMax, g_lastUlt, r.ult);
    if (!r.edge && EdgeActive(me)) {
        int n = CancelEdge(me);
        if (g_edgeCancels++ < 20) uex::Log(L"houserules: edge off, cancelled %d Phantom Edge GA", n);
    }
}

void End(UObject* me) {
    if (g_on && me) uex::Log(L"houserules: end");
    g_on = false; g_me = nullptr; g_lastShachi = g_lastUlt = -1;
}

float OutgoingScale(UObject* me) {
    if (!g_on || !me || g_sdScale == 1.f) return 1.f;
    return EdgeActive(me) ? g_sdScale : 1.f;
}

std::wstring Violation(const rules::Rules& r, UObject* me) {
    if (!me || r.acc >= 9) return L"";
    auto bp = Backpack(me); if (!bp) return L"";
    Arr a = kAssembly.Of<Arr>(bp); int n = 0;
    for (int i = 0; a.d && i < a.n && i < 16; ++i) if (!IsNone(reinterpret_cast<FName*>(a.d)[i])) ++n;
    if (n > r.acc) return L"饰品超过上限（" + std::to_wstring(n) + L"/" + std::to_wstring(r.acc) + L"），请卸下";
    return L"";
}
}  // namespace houserules

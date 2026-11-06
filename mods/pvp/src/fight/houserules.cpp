// houserules: see houserules.h.
// sr / ult: not applied for now (see Tick). ScaleRise scales the rise of an attribute since the
// last frame and is kept for when they come back. Nothing of the game is changed persistently.
// sd / edge: Phantom Edge moves are GAs tagged Ability.Action.Hun.ShachiAbility.* (seen in game:
// GA_BrutalAxe_Normal/Combo/Sprint). heal 0: GAs tagged *.UseItem (GA_Medicine_Common*) are cancelled;
// heal 1: only the gourd (Hulu) may be equipped. cimu: 慈母护符 is accessory AC_BreakRegen.
// acc: AssemblyAccessory slots in use.
#include "houserules.h"
#include <algorithm>
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
uex::Prop kGaTags{L"Class /Script/GameplayAbilities.GameplayAbility", L"AbilityTags", 0xa8};

bool IsNone(const FName& n) { return !n.comparison && !n.number; }
bool g_on;
UObject* g_me;
float g_lastShachi = -1, g_lastUlt = -1;
int g_edgeCancels, g_healCancels;
float g_sdScale = 1.f;
// ecd: a Phantom Edge move running sets g_edgeBusy; when no move is active any more the cooldown
// starts (a Normal -> Combo chain counts as one move). A move started during it is cancelled.
bool g_edgeBusy; double g_edgeReadyAt, g_edgeBlockedAt = -100, g_clock; float g_ecdLen;

UObject* Backpack(UObject* me) {
    auto cls = uex::Class(L"Class /Script/ProjectPBZ.PBZBackPackSubsystem");
    auto lib = uex::Lib(L"SubsystemBlueprintLibrary /Script/Engine.Default__SubsystemBlueprintLibrary");
    auto f = lib ? uex::Fn(lib, L"GetWorldSubsystem") : nullptr;
    if (!me || !cls || !f) return nullptr;
    Params g(f); g.Set(L"ContextObject", me); g.Set(L"Class", cls); g.Call(lib);
    return g.Get<UObject*>(L"ReturnValue");
}

// any AbilityTags entry of the GA (class default or instance) contains sub
bool HasTag(UObject* ga, const wchar_t* sub) {
    Arr t = kGaTags.Of<Arr>(ga);
    for (int i = 0; t.d && i < t.n && i < 32; ++i)
        if (NameOf(reinterpret_cast<FName*>(t.d)[i]).find(sub) != std::wstring::npos) return true;
    return false;
}
constexpr const wchar_t* kEdgeTag = L"ShachiAbility";
constexpr const wchar_t* kItemTag = L"UseItem";
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
bool Active(UObject* ch, const wchar_t* tag) {
    bool r = false;
    EachActive(ch, [&](uint8_t*, UObject* ga) { if (!r && HasTag(ga, tag)) r = true; });
    return r;
}
// cancel our instanced GAs with the tag (as fight's CancelMoves does for montage GAs)
int Cancel(UObject* ch, const wchar_t* tag) {
    auto ga = uex::Class(L"Class /Script/GameplayAbilities.GameplayAbility");
    int n = 0;
    for (int i = 0, c = NumObjects(); ga && i < c; ++i) {
        auto o = ObjectAt(i);
        if (!o || o->outer != ch || (o->flags & 0x10) || !IsA(o, ga) || !HasTag(o, tag)) continue;
        if (auto f = uex::Fn(o, L"K2_CancelAbility")) { Params p(f); p.Call(o); ++n; }
    }
    return n;
}

// scale the rise of an attribute since last frame to pct %
[[maybe_unused]] void ScaleRise(UObject* me, int off, int maxOff, float& last, int pct) {
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
    g_lastShachi = g_lastUlt = -1; g_edgeCancels = g_healCancels = 0; g_sdScale = r.sd / 100.f;
    g_edgeBusy = false; g_edgeReadyAt = 0; g_edgeBlockedAt = -100; g_clock = 0; g_ecdLen = float(r.ecd);  // every round starts ready
    if (g_on) uex::Log(L"houserules: begin sd %d edge %d ecd %d heal %d", r.sd, r.edge, r.ecd, r.heal);
}

void Tick(const rules::Rules& r, UObject* me, float dt) {
    if (!g_on || !me) return;
    if (me != g_me) { g_me = me; g_lastShachi = g_lastUlt = -1; }
    using namespace uex::attr;
    if (uex::Attr(me, ShachiMax) <= 0) return;  // attributes not there yet
    // sr / ult: off for now (hidden in the room settings). Measured online: UltimateCharge stays 0
    // through hits dealt and taken, and Shachi (posture) only drops on hits taken, so a rise scale
    // changes nothing; a room text from an older build with sr/ult != 100 is ignored.
    g_clock += dt; g_ecdLen = float(r.ecd);
    if (r.edge && r.ecd > 0) {
        bool act = Active(me, kEdgeTag);
        if (act && !g_edgeBusy && g_clock < g_edgeReadyAt) {  // started while cooling down
            int n = Cancel(me, kEdgeTag); act = false; g_edgeBlockedAt = g_clock;
            if (g_edgeCancels++ < 20) uex::Log(L"houserules: edge cooldown %.1f s left, cancelled %d GA", g_edgeReadyAt - g_clock, n);
        }
        if (g_edgeBusy && !act) g_edgeReadyAt = g_clock + r.ecd;
        g_edgeBusy = act;
    }
    if (!r.edge && Active(me, kEdgeTag)) {
        int n = Cancel(me, kEdgeTag);
        if (g_edgeCancels++ < 20) uex::Log(L"houserules: edge off, cancelled %d Phantom Edge GA", n);
    }
    if (r.heal == 0 && Active(me, kItemTag)) {
        int n = Cancel(me, kItemTag);
        if (g_healCancels++ < 20) uex::Log(L"houserules: heal off, cancelled %d use-item GA", n);
    }
}

void End(UObject* me) {
    if (g_on && me) uex::Log(L"houserules: end");
    g_on = false; g_me = nullptr; g_lastShachi = g_lastUlt = -1;
}

EdgeCd EdgeCooldown() {
    EdgeCd c;
    if (!g_on || g_ecdLen <= 0) return c;
    c.len = g_ecdLen; c.left = g_edgeBusy ? 0.f : float((std::max)(0.0, g_edgeReadyAt - g_clock));
    c.sinceBlocked = g_clock - g_edgeBlockedAt;
    return c;
}

float OutgoingScale(UObject* me) {
    if (!g_on || !me || g_sdScale == 1.f) return 1.f;
    return Active(me, kEdgeTag) ? g_sdScale : 1.f;
}

std::wstring Violation(const rules::Rules& r, UObject* me) {
    if (!me || (r.acc >= 9 && !r.cimu && r.heal != 1)) return L"";
    auto bp = Backpack(me); if (!bp) return L"";
    Arr a = kAssembly.Of<Arr>(bp); int n = 0;
    bool cimu = false;
    for (int i = 0; a.d && i < a.n && i < 16; ++i) {
        FName f = reinterpret_cast<FName*>(a.d)[i];
        if (IsNone(f)) continue;
        ++n; if (NameOf(f) == L"AC_BreakRegen") cimu = true;
    }
    if (r.cimu && cimu) return L"本房间禁用慈母护符，请卸下";
    if (r.heal == 1) for (int i = 0; i < 4; ++i) {
        auto f = uex::Fn(bp, L"GetMedicineByIndex"); if (!f) break;
        Params p(f); p.Set<int>(L"Index", i); p.Call(bp);
        FName m = p.Get<FName>(L"ReturnValue");
        if (!IsNone(m) && NameOf(m) != L"Hulu") return L"本房间只能带药葫芦，请卸下其他药品";
    }
    if (r.acc < 9 && n > r.acc) return L"饰品超过上限（" + std::to_wstring(n) + L"/" + std::to_wstring(r.acc) + L"），请卸下";
    return L"";
}
}  // namespace houserules

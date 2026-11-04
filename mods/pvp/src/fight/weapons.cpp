// weapons: lobby weapon and Phantom Edge lists and equipping (see weapons.h).
// Local player: PBZBackPackSubsystem::ReplaceWeaponByIndex(id, i) at the weapon component's
// current main index swaps the weapon in hand (actors, anim layers, GA_Hun_<key>_* abilities),
// owned or not. Phantom Edge (杀器): ReplaceShaChiWeaponByIndex(id, i) on an occupied ShaqiWeapons
// slot (no ownership check; an empty slot would add the item to the inventory). The save's slots
// and the weapon infos the swaps touch (red points) are restored by RestoreWeapons (fight::MatchOver).
// Ghosts: CharacterWeaponsComponent::SetSlotWeapon/SetCurrentWeapon with the remote's config.
#include "weapons.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "uex.h"

using namespace ue;

namespace fight {
namespace {
struct Arr { uint8_t* d; int n, m; };

// DT_WeaponData ItemIds of the player weapons (prologue/cart/wooden variants and the Placeholder
// configs left out), in the table's order
const std::vector<Weapon> kList = {
    {"", L"当前装备"},
    {"Tang", L"黑伤"},
    {"IronBlade", L"残钢刃"},
    {"Double", L"黑伤双刃"},
    {"Long", L"长形黑伤"},
    {"GuiTouBlade", L"红妆鬼斩"},
    {"ChiZheCanJian", L"痴者残剑"},
    {"ZhanMaBlade", L"亡命斩马刀"},
    {"SnakeDual", L"白蟒赤练"},
    {"RenOuQingTongJian", L"天玄青铜剑"},
    {"DualHook", L"别离残钩"},
    {"TianShaShuangYue", L"天煞双钺"},
    {"PoJun", L"破军"},
    {"SlitheringSword", L"灵蛇软剑"},
    {"BaiShanShuangJian", L"白山风雷剑"},
    {"HorizontalBlade", L"荡寇凶刃"},
    {"JadeDualChakram", L"玉双环"},
    {"YuuBlade", L"妖锯"},
    {"Jigsaw", L"断肢锯"},
    {"RenOuYanYueDao", L"偃月刀"},
    {"CiGuHunDun", L"刺骨混沌"},
    {"BaiYing", L"白影"},
    {"FangHuaXieShan", L"圣母邪扇"},
    {"ZiMingJian", L"紫冥剑"},
    {"LuanYing", L"乱莺"},
    {"HeiMoShang", L"黑魔伤"},
};
// config folder -> GA_Hun_<key>_ (only where they differ; checked against the pak's GA names)
const std::map<std::wstring, std::wstring> kGaKey = {
    {L"BaiShanFengLeiJian", L"FengLeiJian"}, {L"ChiZheCanJian", L"ChiZheCanJian_Normal"}, {L"ZhanMaBlade", L"ZhanMa"},
    {L"JadeDualChakram", L"Jade"}, {L"Jigsaw", L"JigSaw"}, {L"ChaosBlade", L"Chaos"}, {L"ZiMingJian", L"ZiMingJian_New"},
};

bool g_saved;                      // backpack snapshot taken (first Equip of a match)
std::vector<FName> g_origSlots;    // PBZBackPackSubsystem::MainWeapons
bool g_edgeSaved;                  // Phantom Edge snapshot taken (first EquipEdge of a match)
std::vector<FName> g_origEdges;    // PBZBackPackSubsystem::ShaqiWeapons

UObject* Backpack() {
    auto me = uex::Me(); if (!me) return nullptr;
    auto cls = uex::Class(L"Class /Script/ProjectPBZ.PBZBackPackSubsystem");
    auto lib = uex::Lib(L"SubsystemBlueprintLibrary /Script/Engine.Default__SubsystemBlueprintLibrary");
    if (!cls || !lib) return nullptr;
    Params g(uex::Fn(lib, L"GetWorldSubsystem")); g.Set(L"ContextObject", me); g.Set(L"Class", cls); g.Call(lib);
    return g.Get<UObject*>(L"ReturnValue");
}
Arr MainSlots(UObject* bp) { return *reinterpret_cast<Arr*>(reinterpret_cast<uint8_t*>(bp) + 0x708); }
Arr EdgeSlots(UObject* bp) { return *reinterpret_cast<Arr*>(reinterpret_cast<uint8_t*>(bp) + 0x770); }  // ShaqiWeapons: 2 slots, None = empty
bool Same(const FName& a, const FName& b) { return a.comparison == b.comparison && a.number == b.number; }
bool IsNone(const FName& n) { return !n.comparison && !n.number; }
// soft object pointer (package FName +8, asset FName +0x10) -> "/Pkg/Path.Asset", "" if unset
std::wstring Soft(const uint8_t* p) {
    auto pkg = NameOf(*reinterpret_cast<const FName*>(p + 8));
    return pkg.empty() || pkg == L"None" ? L"" : pkg + L"." + NameOf(*reinterpret_cast<const FName*>(p + 0x10));
}

// CharacterWeaponsComponent of an actor (outer == actor), cached per actor
UObject* WComp(UObject* ch) {
    static std::map<UObject*, UObject*> cache;
    if (!ch) return nullptr;
    auto cls = uex::Class(L"Class /Script/SGameCombatSystem.CharacterWeaponsComponent");
    if (auto it = cache.find(ch); it != cache.end() && uex::Alive(it->second) && it->second->cls == cls && it->second->outer == ch) return it->second;
    for (int i = 0, n = NumObjects(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (o && o->cls == cls && o->outer == ch && !(o->flags & 0x10)) return cache[ch] = o;
    }
    return nullptr;
}
// RuntimeWeaponData (TMap<GameplayTag, ...> at +0xf8, element stride 0x30): the key of a slot
// (Weapon.Slot.Main / .Shachi), its weapons array at element +8
bool SlotTag(UObject* comp, const wchar_t* name, FName& tag, int* count = nullptr, Arr* weapons = nullptr) {
    auto m = *reinterpret_cast<Arr*>(reinterpret_cast<uint8_t*>(comp) + 0xf8);
    for (int k = 0; m.d && k < m.n && k < 16; ++k) {
        auto e = m.d + k * 0x30;
        if (NameIs(*reinterpret_cast<FName*>(e), name)) {
            tag = *reinterpret_cast<FName*>(e);
            if (count) *count = reinterpret_cast<Arr*>(e + 8)->n;
            if (weapons) *weapons = *reinterpret_cast<Arr*>(e + 8);  // RuntimeWeaponSingleData, stride 0x120, config object at +0
            return true;
        }
    }
    return false;
}
bool MainTag(UObject* comp, FName& tag, int* count = nullptr) { return SlotTag(comp, L"Weapon.Slot.Main", tag, count); }
// current main weapon: config object and index
UObject* CurConfig(UObject* comp, const FName& tag, int* index) {
    Params g(uex::Fn(comp, L"GetCurrentWeapon"));
    g.Set(L"SlotTag", tag); g.Set<bool>(L"WithPending", true); g.Call(comp);
    if (index) *index = g.Get<int>(L"CurrentIndex");
    return g.Get<UObject*>(L"WeaponData");  // RuntimeWeaponSingleData starts with the config interface
}
void Replace(UObject* bp, const FName& id, int index) {
    Params p(uex::Fn(bp, L"ReplaceWeaponByIndex"));
    p.Set(L"NewWeaponName", id); p.Set<int>(L"ReplaceIndex", index); p.Call(bp);
}
// DT_WeaponData row -> WeaponConfig soft path ("/Pkg/Path.Asset"), "" if unknown
std::wstring ConfigPath(const std::string& id) {
    static std::map<std::string, std::wstring> cache;
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    auto dt = FindObject(L"DataTable /Game/PBZContent/DataTable/UI/DT_WeaponData.DT_WeaponData");
    if (!dt) return L"";
    auto rm = *reinterpret_cast<Arr*>(reinterpret_cast<uint8_t*>(dt) + 0x30);
    std::wstring want = uex::Wide(id);
    for (int k = 0; k < rm.n; ++k) {
        auto e = rm.d + k * 0x18;
        auto row = At<uint8_t*>(e, 8);
        if (!row || !NameIs(*reinterpret_cast<FName*>(row + 8), want)) continue;  // ItemId (row keys are display names)
        auto pkg = NameOf(*reinterpret_cast<FName*>(row + 0x520 + 0x8));
        auto asset = NameOf(*reinterpret_cast<FName*>(row + 0x520 + 0x10));
        return cache[id] = pkg.empty() ? L"" : pkg + L"." + asset;
    }
    return cache[id] = L"";
}

// Phantom Edges: DT_ShaqiWeaponData rows (ItemId +8, WeaponName.ValueCache FText +0x50, Icon +0x60,
// WhiteIcon +0xb0, RenderIcon +0x128, WeaponConfig soft +0x5f8)
struct EdgeRow { FName id; std::wstring cfg, icon; bool pick; };
std::map<std::string, EdgeRow> g_edgeRows;   // ItemId -> row (also _Interact / Placeholder ones: not picks)
std::vector<Weapon> g_edges = {{"", L"当前装备"}};
bool g_edgesLoaded;
void LoadEdges() {
    if (g_edgesLoaded) return;
    static std::chrono::steady_clock::time_point next{};
    static bool loadTried;
    auto now = std::chrono::steady_clock::now(); if (now < next) return;
    next = now + std::chrono::seconds(5);  // a FindObject miss scans every object
    const wchar_t* path = L"/Game/PBZContent/DataTable/UI/DT_ShaqiWeaponData.DT_ShaqiWeaponData";
    auto dt = FindObject(std::wstring(L"DataTable ") + path);
    if (!dt && !loadTried) { loadTried = true; dt = uex::LoadAsset(path); }
    if (!dt) return;
    auto rm = *reinterpret_cast<Arr*>(reinterpret_cast<uint8_t*>(dt) + 0x30);
    auto lib = uex::Lib(L"KismetTextLibrary /Script/Engine.Default__KismetTextLibrary");
    auto conv = uex::Fn(lib, L"Conv_TextToString");
    for (int k = 0; k < rm.n && k < 256; ++k) {
        auto e = rm.d + k * 0x18; auto row = At<uint8_t*>(e, 8); if (!row) continue;
        FName idn = *reinterpret_cast<FName*>(row + 8);
        // icon: RenderIcon (Temp_Render, like the weapon cards), else Icon, else WhiteIcon
        std::wstring cfg = Soft(row + 0x5f8), icon = Soft(row + 0x128);
        if (icon.empty()) icon = Soft(row + 0x60);
        if (icon.empty()) icon = Soft(row + 0xb0);
        if (IsNone(idn) || cfg.empty()) { uex::Log(L"weapons: Phantom Edge row %s skipped (no id/config)", NameOf(*reinterpret_cast<FName*>(e)).c_str()); continue; }
        std::string id = uex::Utf8(NameOf(idn));
        if (g_edgeRows.count(id)) continue;  // first row of an id wins
        bool pick = cfg.find(L"_Interact") == std::wstring::npos && cfg.find(L"Placeholder") == std::wstring::npos;
        std::wstring label;
        if (pick && conv && At<void*>(row, 0x50)) {  // once per row at load (the FString leaks)
            Params b(conv); b.Set(L"InText", *reinterpret_cast<uex::FText*>(row + 0x50)); b.Call(lib);
            auto s = b.Get<FString>(L"ReturnValue"); if (s.data && s.num > 1) label.assign(s.data, s.num - 1);
        }
        // unreleased rows: no localized name (the text is the row key "PhantomEdges_NNN") or another
        // row's config (BaWangLianQiu / MoShou use JinGua's); still known to IsEdgeConfig, not offered
        if (pick && (label.empty() || label.rfind(L"PhantomEdges_", 0) == 0)) pick = false;
        for (auto& [k2, r2] : g_edgeRows) if (pick && r2.pick && r2.cfg == cfg) pick = false;
        g_edgeRows[id] = {idn, cfg, icon, pick};
        if (!pick) { uex::Log(L"weapons: Phantom Edge %S not offered (%s)", id.c_str(), label.empty() ? L"no name" : label.c_str()); continue; }
        g_edges.push_back({id, label});
    }
    g_edgesLoaded = true;
    uex::Log(L"weapons: %d Phantom Edges (%d rows)", int(g_edges.size()) - 1, rm.n);
}
const EdgeRow* EdgeById(const FName& id) { for (auto& [k, r] : g_edgeRows) if (Same(r.id, id)) return &r; return nullptr; }
void ReplaceEdge(UObject* bp, const FName& id, int index) {
    auto f = uex::Fn(bp, L"ReplaceShaChiWeaponByIndex"); if (!f) { uex::Log(L"weapons: no ReplaceShaChiWeaponByIndex"); return; }
    // like Equip: load the config first so the swap is not done asynchronously
    if (auto r = EdgeById(id); r && !uex::LoadAsset(r->cfg.c_str())) uex::Log(L"weapons: could not preload %s", r->cfg.c_str());
    Params p(f);
    if (!p.Set(L"NewWeaponName", id) || !p.Set<int>(L"ReplaceIndex", index)) { uex::Log(L"weapons: ReplaceShaChiWeaponByIndex params not found"); return; }
    p.Call(bp);
}
// id into the occupied slot i. The game ignores an id that is in a slot already, so one sitting in
// the other slot is swapped through a parked row (a pick in neither slot).
void PutEdge(UObject* bp, const FName& id, int i) {
    Arr a = EdgeSlots(bp); auto s = reinterpret_cast<FName*>(a.d);
    if (i < 0 || i >= a.n || i > 1 || IsNone(s[i]) || Same(s[i], id)) return;
    int j = -1; for (int k = 0; k < a.n && k < 2; ++k) if (k != i && Same(s[k], id)) j = k;
    if (j < 0) { ReplaceEdge(bp, id, i); return; }
    FName old = s[i], park{};
    for (auto& [k, r] : g_edgeRows) if (r.pick && !Same(r.id, s[0]) && !(a.n > 1 && Same(r.id, s[1]))) { park = r.id; break; }
    if (IsNone(park)) { uex::Log(L"weapons: no free Phantom Edge to swap slots"); return; }
    ReplaceEdge(bp, park, j); ReplaceEdge(bp, id, i); ReplaceEdge(bp, old, j);
}
// MainWeaponInfos (+0x718) / ShaqiWeaponInfos (+0x780): TMap<FName, Info*> (element stride 0x18, value +8,
// allocation bits inline at +0x10 or at the pointer at +0x20, NumBits +0x28). The swaps clear red points
// (DisableRedPoint), so the plain data of every info (bytes 0x28..0x50, no pointers) is snapshotted.
template <class F> void EachInfo(UObject* bp, int off, F f) {
    auto b = reinterpret_cast<uint8_t*>(bp) + off;
    auto d = At<uint8_t*>(b, 0); int n = At<int32_t>(b, 8), nbits = At<int32_t>(b, 0x28);
    auto bits = At<uint32_t*>(b, 0x20); if (!bits) bits = reinterpret_cast<uint32_t*>(b + 0x10);
    if (!d) return;
    for (int i = 0; i < n && i < nbits && i < 4096; ++i) {
        if (!((bits[i >> 5] >> (i & 31)) & 1)) continue;
        auto e = d + i * 0x18; auto info = At<UObject*>(e, 8);
        if (info && uex::Alive(info)) f(*reinterpret_cast<FName*>(e), info);
    }
}
struct InfoSave { int map; FName id; uint8_t b[0x28]; };
std::vector<InfoSave> g_infos; bool g_infosSaved;
void SaveInfos(UObject* bp) {
    if (g_infosSaved) return;
    g_infosSaved = true; g_infos.clear();
    for (int off : {0x718, 0x780}) EachInfo(bp, off, [&](const FName& id, UObject* o) {
        InfoSave s{off, id}; memcpy(s.b, reinterpret_cast<uint8_t*>(o) + 0x28, 0x28); g_infos.push_back(s); });
}
int RestoreInfos(UObject* bp) {
    int n = 0;
    for (int off : {0x718, 0x780}) EachInfo(bp, off, [&](const FName& id, UObject* o) {
        for (auto& s : g_infos) if (s.map == off && Same(s.id, id)) {
            auto p = reinterpret_cast<uint8_t*>(o) + 0x28;
            if (memcmp(p, s.b, 0x28)) { memcpy(p, s.b, 0x28); ++n; }
            break;
        } });
    return n;
}
}  // namespace

const std::vector<Weapon>& Weapons() { return kList; }
const std::vector<Weapon>& Edges() { LoadEdges(); return g_edges; }
bool NoEdgeEquipped() {  // polled by the lobby: the backpack is read every 2 s at most
    static std::chrono::steady_clock::time_point next{};
    static bool none;
    auto now = std::chrono::steady_clock::now(); if (now < next) return none;
    next = now + std::chrono::seconds(2);
    auto bp = Backpack(); none = false;
    if (!bp) return none;  // title screen: no player, unknown
    Arr a = EdgeSlots(bp); none = true;
    for (int k = 0; a.d && k < a.n && k < 2; ++k) if (!IsNone(reinterpret_cast<FName*>(a.d)[k])) none = false;
    return none;
}

void EquipEdge(const std::string& id) {
    if (id.empty()) return;
    LoadEdges();
    auto it = g_edgeRows.find(id);
    if (it == g_edgeRows.end() || !it->second.pick) { uex::Log(L"weapons: unknown Phantom Edge %S", id.c_str()); return; }
    auto bp = Backpack(); if (!bp) { uex::Log(L"weapons: no backpack"); return; }
    Arr a = EdgeSlots(bp); auto s = reinterpret_cast<FName*>(a.d);
    if (!s) a.n = 0;
    int cur = At<int32_t>(bp, 0x768), i = -1;  // CurrentShaqiWeaponIndex
    if (cur >= 0 && cur < a.n && cur < 2 && !IsNone(s[cur])) i = cur;
    else for (int k = 0; k < a.n && k < 2; ++k) if (!IsNone(s[k])) { i = k; break; }
    if (i < 0) { uex::Log(L"weapons: no Phantom Edge equipped in this save, %S skipped (an empty slot would add it to the inventory)", id.c_str()); return; }
    if (Same(s[i], it->second.id)) return;  // already in that slot
    SaveInfos(bp);
    if (!g_edgeSaved) { g_origEdges.assign(s, s + a.n); g_edgeSaved = true; }
    PutEdge(bp, it->second.id, i);
    a = EdgeSlots(bp);
    uex::Log(L"weapons: Phantom Edge %S in slot %d -> %s", id.c_str(), i, a.d && i < a.n ? NameOf(reinterpret_cast<FName*>(a.d)[i]).c_str() : L"-");
}

void Equip(const std::string& id) {
    if (id.empty()) return;
    bool known = false; for (auto& w : kList) known |= w.id == id;
    if (!known) { uex::Log(L"weapons: unknown id %S", id.c_str()); return; }
    auto me = uex::Me(); auto bp = Backpack(); auto comp = WComp(me);
    FName tag{}; if (!bp || !comp || !MainTag(comp, tag)) { uex::Log(L"weapons: no backpack/component"); return; }
    Arr a = MainSlots(bp);
    if (!g_saved) {
        g_origSlots.assign(reinterpret_cast<FName*>(a.d), reinterpret_cast<FName*>(a.d) + a.n);
        g_saved = true;
    }
    int idx = 0; CurConfig(comp, tag, &idx);
    if (idx < 0 || idx >= a.n) idx = 0;
    if (idx < a.n && NameIs(reinterpret_cast<FName*>(a.d)[idx], uex::Wide(id))) return;  // already in hand
    // a weapon that is not in memory yet is swapped in asynchronously (the hand still shows the old
    // one); both game crashes at round start happened during such a pending swap. Load it first.
    if (auto path = ConfigPath(id); !path.empty() && !uex::LoadAsset(path.c_str())) uex::Log(L"weapons: could not preload %s", path.c_str());
    SaveInfos(bp);
    Replace(bp, uex::Name(uex::Wide(id)), idx);
    int after = -1; auto cfg = CurConfig(comp, tag, &after);
    uex::Log(L"weapons: equip %S at %d -> %s", id.c_str(), idx, cfg ? NameOf(cfg).c_str() : L"-");
}

void RestoreWeapons() {
    if (!g_saved && !g_edgeSaved && !g_infosSaved) return;
    auto clear = [] { g_saved = g_edgeSaved = g_infosSaved = false; g_origSlots.clear(); g_origEdges.clear(); g_infos.clear(); };
    auto bp = Backpack(); if (!bp) { uex::Log(L"weapons: restore without backpack"); clear(); return; }
    if (g_saved) {
        Arr a = MainSlots(bp);
        for (int i = 0; i < int(g_origSlots.size()) && i < a.n; ++i) {
            if (Same(reinterpret_cast<FName*>(a.d)[i], g_origSlots[i])) continue;
            Replace(bp, g_origSlots[i], i);
            uex::Log(L"weapons: restored slot %d %s", i, NameOf(g_origSlots[i]).c_str());
            a = MainSlots(bp);
        }
    }
    if (g_edgeSaved) {
        Arr a = EdgeSlots(bp);
        for (int i = 0; a.d && i < int(g_origEdges.size()) && i < a.n && i < 2; ++i) {
            FName cur = reinterpret_cast<FName*>(a.d)[i];
            if (Same(cur, g_origEdges[i]) || IsNone(cur) || IsNone(g_origEdges[i])) continue;
            PutEdge(bp, g_origEdges[i], i);  // swaps through a parked row when the id sits in the other slot
            uex::Log(L"weapons: restored Phantom Edge slot %d %s", i, NameOf(g_origEdges[i]).c_str());
            a = EdgeSlots(bp);
        }
    }
    // last: the swaps above clear red points again
    if (g_infosSaved) uex::Log(L"weapons: restored %d weapon infos", RestoreInfos(bp));
    clear();
}

std::string CurrentWeaponPath(UObject* ch) {
    auto comp = WComp(ch); FName tag{};
    if (!comp || !MainTag(comp, tag)) return "";
    auto cfg = CurConfig(comp, tag, nullptr);
    return cfg ? uex::Utf8(PathOf(cfg)) : "";
}

std::string WeaponConfigPath(const std::string& id) { return uex::Utf8(ConfigPath(id)); }

bool SetGhostWeapon(UObject* ghost, const std::string& cfgPath, bool armed) {
    auto comp = WComp(ghost); FName tag{}; int count = 0;
    if (!comp || !MainTag(comp, tag, &count)) { static int n; if (n++ < 10) uex::Log(L"weapons: ghost has no weapon component"); return false; }
    bool ok = true;
    if (!cfgPath.empty()) {
        std::wstring w = uex::Wide(cfgPath);
        UObject* cfg = uex::LoadAsset(w.c_str());
        if (!cfg) { static int n; if (n++ < 10) uex::Log(L"weapons: ghost weapon %s did not load", w.c_str()); return false; }
        int idx = 0; auto cur = CurConfig(comp, tag, &idx);
        if (cur != cfg) {
            if (idx < 0 || idx >= count) idx = 0;
            struct If { UObject* obj; void* iface; } v{cfg, reinterpret_cast<uint8_t*>(cfg) + 0x30};  // PBZ_MainWeaponConfig_C: interface at +0x30
            Params s(uex::Fn(comp, L"SetSlotWeapon"));
            s.Set(L"SlotTag", tag); s.Set<int>(L"Index", idx); bool okc = s.Set(L"Config", v); s.Call(comp);
            Params c(uex::Fn(comp, L"SetCurrentWeapon")); c.Set(L"SlotTag", tag); c.Set<int>(L"Index", idx); c.Call(comp);
            auto now = CurConfig(comp, tag, nullptr);
            ok = now == cfg;
            uex::Log(L"weapons: ghost %s weapon %s at %d (param %d) -> %s", NameOf(ghost).c_str(), NameOf(cfg).c_str(), idx, int(okc), now ? NameOf(now).c_str() : L"-");
        }
    }
    Params a(uex::Fn(comp, L"SetArmed")); a.Set(L"SlotTag", tag); a.Set<bool>(L"Armed", armed); a.Call(comp);
    return ok;
}

std::wstring AbilityKey(UObject* ch) {
    std::wstring p = uex::Wide(CurrentWeaponPath(ch));  // /PBZCombatAsset/.../Weapon/<Folder>/Weapon_Hun_X.Weapon_Hun_X
    auto e = p.rfind(L'/'); if (e == std::wstring::npos || e == 0) return L"";
    auto b = p.rfind(L'/', e - 1); if (b == std::wstring::npos) return L"";
    std::wstring folder = p.substr(b + 1, e - b - 1);
    if (auto it = kGaKey.find(folder); it != kGaKey.end()) return it->second;
    return folder;
}

std::string EdgeConfigPath(const std::string& id) {
    LoadEdges();
    auto it = g_edgeRows.find(id);
    return it == g_edgeRows.end() ? "" : uex::Utf8(it->second.cfg);
}
std::string EdgeIconPath(const std::string& id) {
    LoadEdges();
    auto it = g_edgeRows.find(id);
    return it == g_edgeRows.end() ? "" : uex::Utf8(it->second.icon);
}
bool IsEdgeConfig(const std::string& path) {
    LoadEdges();
    std::wstring p = uex::Wide(path);
    if (p.rfind(L"/PBZCombatAsset/ActorData/Players/Hun/ShaqiWeapon/", 0) != 0) return false;
    if (p.find(L"..") != std::wstring::npos || p.find(L"_Interact") != std::wstring::npos) return false;
    auto folder = [](const std::wstring& s) { auto e = s.rfind(L'/'); return e == std::wstring::npos ? s : s.substr(0, e); };
    for (auto& [k, r] : g_edgeRows) if (r.cfg == p || folder(r.cfg) == folder(p)) return true;
    return false;
}
std::string CurrentEdgePath(UObject* ch) {
    auto comp = WComp(ch); FName tag{}; Arr w{};
    if (!comp || !SlotTag(comp, L"Weapon.Slot.Shachi", tag, nullptr, &w)) return "";
    auto cfg = CurConfig(comp, tag, nullptr);
    for (int j = 0; !cfg && w.d && j < w.n && j < 4; ++j)  // no current one: the slot's first weapon
        if (auto o = At<UObject*>(w.d + j * 0x120, 0); o && uex::Alive(o)) cfg = o;
    return cfg ? uex::Utf8(PathOf(cfg)) : "";
}
void SetGhostEdge(UObject* ghost, const std::string& cfgPath) {
    if (cfgPath.empty()) return;
    auto comp = WComp(ghost); if (!comp) { uex::Log(L"weapons: ghost has no weapon component"); return; }
    std::wstring w = uex::Wide(cfgPath);
    UObject* cfg = uex::LoadAsset(w.c_str()); if (!cfg) return;
    FName tag{}; if (!SlotTag(comp, L"Weapon.Slot.Shachi", tag)) tag = uex::Name(L"Weapon.Slot.Shachi");
    auto fs = uex::Fn(comp, L"SetSlotWeapon"), fc = uex::Fn(comp, L"SetCurrentWeapon");
    if (!fs || !fc) { uex::Log(L"weapons: no SetSlotWeapon/SetCurrentWeapon"); return; }
    // the weapon config interface sits at +0x30 of PBZ_WeaponConfigBase (8-byte gap before its first property)
    auto base = uex::Class(L"Class /Script/PBZCombatAsset.PBZ_WeaponConfigBase");
    if (base && !IsA(cfg, base)) { uex::Log(L"weapons: %s is no weapon config", NameOf(cfg).c_str()); return; }
    int idx = -1; auto cur = CurConfig(comp, tag, &idx);
    if (cur == cfg && idx == 0) return;
    struct If { UObject* obj; void* iface; } v{cfg, reinterpret_cast<uint8_t*>(cfg) + 0x30};
    Params s(fs); s.Set(L"SlotTag", tag); s.Set<int>(L"Index", 0); bool okc = s.Set(L"Config", v); s.Call(comp);
    Params c(fc); c.Set(L"SlotTag", tag); c.Set<int>(L"Index", 0); c.Call(comp);
    auto now = CurConfig(comp, tag, nullptr);
    uex::Log(L"weapons: ghost %s Phantom Edge %s (param %d) -> %s", NameOf(ghost).c_str(), NameOf(cfg).c_str(), int(okc), now ? NameOf(now).c_str() : L"-");
}
std::wstring LoadoutReport() {
    auto bp = Backpack(); if (!bp) return L"no backpack";
    auto names = [](Arr a) {
        std::wstring s; for (int i = 0; a.d && i < a.n && i < 8; ++i) s += (i ? L"," : L"") + NameOf(reinterpret_cast<FName*>(a.d)[i]);
        return s;
    };
    std::wstring r = L"main [" + names(MainSlots(bp)) + L"] cur " + std::to_wstring(At<int32_t>(bp, 0x6f8)) + L"\n";
    r += L"edges [" + names(EdgeSlots(bp)) + L"] cur " + std::to_wstring(At<int32_t>(bp, 0x768)) + L"\n";
    wchar_t b[128];
    for (int off : {0x718, 0x780}) {  // order-independent hash of every info's id and plain data
        int n = 0, red = 0; uint64_t hash = 0;
        EachInfo(bp, off, [&](const FName& id, UObject* o) {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](const uint8_t* p, int len) { for (int i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; } };
            mix(reinterpret_cast<const uint8_t*>(&id), 8); mix(reinterpret_cast<const uint8_t*>(o) + 0x28, 0x28);
            hash += h; ++n; red += At<uint8_t>(o, 0x49) != 0;
        });
        swprintf(b, 128, L"%s infos %d red %d hash %016llx\n", off == 0x718 ? L"main" : L"edge", n, red, (unsigned long long)hash);
        r += b;
    }
    r += L"hand " + uex::Wide(CurrentWeaponPath(uex::Me())) + L"\n";
    r += L"shachi " + uex::Wide(CurrentEdgePath(uex::Me())) + L"\n";
    swprintf(b, 128, L"snapshot main %d edge %d infos %d (%d)", int(g_saved), int(g_edgeSaved), int(g_infosSaved), int(g_infos.size()));
    return r + b;
}
}  // namespace fight

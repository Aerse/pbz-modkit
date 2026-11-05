// outfits: lobby outfit list and wearing outfits in a match (see outfits.h). Visual only.
// PBZAppearanceComponent (the player class has BP_AppearanceComp, a subclass): DoPBZAppearanceLogic,
//   bSuppressAppearanceSave, CharacterId, DefaultVisualOutfitID, bUseConfiguredAppearance,
//   ConfiguredOutfit, CurrentOutfit, CurrentSkinColor (EPBZSkinColor: Black, GrayWhite, White,
//   PureWhite), bAppliedHatVisible, CachedOutfit (BeginSuppress's copy). Fields by name (kComp*).
// ApplyOutfitVisual(OutfitID, bShowHat, SkinColor, bForceApplyMeshes): needs a type-4 row (found
// by ItemId), never saves; writes bAppliedHatVisible and CurrentSkinColor, and CurrentOutfit only
// while DoPBZAppearanceLogic is clear; without bForceApplyMeshes it skips the meshes when the id is
// DefaultVisualOutfitID. Hair changes only with the cvar pbz.appearance.ApplyOutfitVisualHair (default 0).
// The wardrobe is saved only with DoPBZAppearanceLogic set, bSuppressAppearanceSave clear and a
// CharacterId (PreSaveGame skips the appearance while bSuppressAppearanceSave is set).
// BeginSuppressAppearanceSave caches outfit/skin/hat/owned and sets bSuppressAppearanceSave; it does
// not nest (a second Begin overwrites the cache, any End ends it).
// EndSuppressAppearanceSave(false) puts the cache back and applies it, without saving.
// DT_PBZAppearanceTable rows: uex::EachRow (the native RowMap, layout in uex::layout).
// Row = PBZAppearanceStruct : ItemDataBase: ItemId, AppearanceType (Outfit = 4), BodyMeshAsset,
//   bDefaultHatHidden, bObtainableInGame (kRow*). Probe shows the real thing.
#include "outfits.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <set>
#include "uex.h"

using namespace ue;

namespace fight::outfits {
namespace {
// lobby cards: Hun's type-4 rows (the only playable character), the free ones first, then the
// entitlement ones (MoTian, YaYu, JiGuanMoQu: worn only when owned). Hun_OutFit_Climax_Hat is
// Climax with its hat, no card of its own. Built-in names until the table's own are read.
struct Card { const char* id; const wchar_t* label; };
const Card kCards[] = {
    {"", L"当前外观"},
    {"Hun_OutFit_Default", L"普通服装"},
    {"Hun_OutFit_Climax", L"10年前服装"},
    {"Hun_OutFit_FengXue", L"风雪"},
    {"Hun_OutFit_ZhuiMing", L"追命"},
    {"Hun_OutFit_GuYou", L"故友"},
    {"Hun_OutFit_MoTian", L"魔天套装"},
    {"Hun_OutFit_YaYu", L"鸦羽"},
    {"Hun_OutFit_JiGuanMoQu", L"机关魔躯"},
};
std::vector<Weapon> g_cards;                  // List()
std::map<std::string, std::wstring> g_icons;  // card id -> texture path
bool g_named;                                 // names and icons read from the table
const wchar_t* kTable = L"/Game/PBZContent/DataTable/UI/DT_PBZAppearanceTable.DT_PBZAppearanceTable";

// reflected fields (by name; the number is this build's offset, used only if the field is gone)
constexpr const wchar_t* kComp = L"Class /Script/ProjectPBZ.PBZAppearanceComponent";
uex::Prop kCompLogic{kComp, L"DoPBZAppearanceLogic", 0xc8, 0xff}, kCompSuppress{kComp, L"bSuppressAppearanceSave", 0xc9, 0xff},
    kCompCharId{kComp, L"CharacterId", 0xcc}, kCompDefault{kComp, L"DefaultOutfitID", 0x128},
    kCompDefaultVisual{kComp, L"DefaultVisualOutfitID", 0x130}, kCompUseCfg{kComp, L"bUseConfiguredAppearance", 0x188, 0xff},
    kCompCfg{kComp, L"ConfiguredOutfit", 0x18c}, kCompCurrent{kComp, L"CurrentOutfit", 0x278},
    kCompSkin{kComp, L"CurrentSkinColor", 0x288}, kCompHat{kComp, L"bAppliedHatVisible", 0x28a, 0xff},
    kCompCached{kComp, L"CachedOutfit", 0x2e4};
uex::Prop kRowStruct{L"Class /Script/Engine.DataTable", L"RowStruct", 0x28};
uex::Prop kItemId{L"ScriptStruct /Script/ProjectPBZ.ItemDataBase", L"ItemId", 0x8};
constexpr const wchar_t* kRow = L"ScriptStruct /Script/ProjectPBZ.PBZAppearanceStruct";
uex::Prop kRowName{kRow, L"AppearanceName", 0x28}, kRowTexture{kRow, L"AppearanceTexture", 0x88},
    kRowHiTexture{kRow, L"HighPrecisionTexture", 0xb0}, kRowType{kRow, L"AppearanceType", 0xd8},
    kRowBody{kRow, L"BodyMeshAsset", 0xe0}, kRowCloth{kRow, L"ClothMeshAsset", 0x108}, kRowHat{kRow, L"HatAsset", 0x140},
    kRowHead{kRow, L"HeadAsset", 0x170}, kRowNeck{kRow, L"NeckAsset", 0x1f0}, kRowBand{kRow, L"MeshBandAsset", 0x218},
    kRowBrow{kRow, L"BrowLashCardAsset", 0x240}, kRowBodyItem{kRow, L"OutfitBodyItem", 0x268},
    kRowHatHidden{kRow, L"bDefaultHatHidden", 0x2a8, 0xff}, kRowObtainable{kRow, L"bObtainableInGame", 0x2a9, 0xff};
uex::Prop kValueCache{L"ScriptStruct /Script/ProjectPBZ.FlowLocText", L"ValueCache", 0x20};
constexpr int kOutfitItems = 8;  // OutfitBodyItem .. OutfitBrowLashCardItem: 8 consecutive FNames
using namespace uex::layout;  // kSet*, kSoftPkg / kSoftAsset

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

uex::Ref g_dt;                        // the appearance table
std::map<std::wstring, FName> g_ids;  // ItemId -> the row's own FName (type-4 rows found so far)
std::set<std::wstring> g_bad;         // ids that matched no type-4 row (capped)

UObject* Table() {
    if (auto t = uex::Get(g_dt)) return t;
    static double retryAt;  // LoadAsset blocks and logs: not again every frame when it fails
    if (Now() < retryAt) return nullptr;
    auto cls = uex::Class(L"Class /Script/Engine.DataTable");
    auto t = uex::LoadAsset(kTable);
    if (!t || !cls || !IsA(t, cls)) { retryAt = Now() + 10; return nullptr; }
    g_dt = uex::Hold(t); g_ids.clear(); g_bad.clear();
    return t;
}

// every row: f(key, row); false if the RowMap does not look like one
template <class F> bool Rows(UObject* dt, F f) { return uex::EachRow(dt, f); }

// a type-4 row's ItemId as the table's own FName (text from a peer never becomes a new name)
bool FindOutfit(const std::wstring& id, FName* out) {
    if (id.empty() || id.size() > 64) return false;
    auto dt = Table(); if (!dt) return false;
    if (auto it = g_ids.find(id); it != g_ids.end()) { *out = it->second; return true; }
    if (g_bad.count(id)) return false;
    bool found = false;
    Rows(dt, [&](const FName&, uint8_t* row) {
        if (found || kRowType.Of<uint8_t>(row) != 4 || !NameIs(kItemId.Of<FName>(row), id)) return;
        *out = kItemId.Of<FName>(row); found = true;
    });
    if (found) g_ids[id] = *out;
    else { if (g_bad.size() >= 64) g_bad.clear(); g_bad.insert(id); }
    return found;
}

// ---- the component ----
UObject* CompClass() { return uex::Class(L"Class /Script/ProjectPBZ.PBZAppearanceComponent"); }
// the actor's own appearance component (the player's is BP_AppearanceComp_C, a subclass)
UObject* AppComp(UObject* actor) {
    auto cls = CompClass();
    if (!actor || !cls || !uex::Alive(actor)) return nullptr;
    auto c = uex::Comp(actor, cls);
    return c && c->outer == actor && IsA(c, cls) ? c : nullptr;
}
void ApplyVisual(UObject* c, const FName& id, bool hat, uint8_t skin) {
    Params p(uex::Fn(c, L"ApplyOutfitVisual"));
    p.Set(L"OutfitID", id); p.Set<bool>(L"bShowHat", hat); p.Set<uint8_t>(L"SkinColor", skin);
    p.Set<bool>(L"bForceApplyMeshes", true);  // also when id is DefaultVisualOutfitID
    p.Call(c);
}
void BeginSuppress(UObject* c) { Params p(uex::Fn(c, L"BeginSuppressAppearanceSave")); p.Call(c); }
void EndNoApply(UObject* c) {  // false: put BeginSuppress's cache back, no save (true would save)
    Params p(uex::Fn(c, L"EndSuppressAppearanceSave")); p.Set<bool>(L"bApplyChanges", false); p.Call(c);
}
bool HatHidden(UObject* c, const FName& id) {
    Params p(uex::Fn(c, L"IsHatHiddenForOutfit")); p.Set(L"OutfitID", id); p.Call(c);
    return p.Get<bool>(L"ReturnValue");
}
FName NameFn(UObject* c, const wchar_t* fn) { Params p(uex::Fn(c, fn)); p.Call(c); return p.Get<FName>(L"ReturnValue"); }
bool Suppressed(UObject* c) { return kCompSuppress.Bit(c); }
// the outfit it shows now: GetEffectiveOutfit, else CurrentOutfit, else the defaults
FName Shown(UObject* c) {
    FName n = NameFn(c, L"GetEffectiveOutfit");
    for (int off : {int(kCompCurrent), int(kCompDefault), int(kCompDefaultVisual)}) if (!n.comparison) n = At<FName>(c, off);
    return n;
}
UObject* Ent() {  // PBZEntitlementSubsystem (game instance)
    auto lib = uex::Lib(L"SubsystemBlueprintLibrary /Script/Engine.Default__SubsystemBlueprintLibrary");
    auto cls = uex::Class(L"Class /Script/ProjectPBZ.PBZEntitlementSubsystem"); auto gi = uex::GameInstance();
    if (!lib || !cls || !gi) return nullptr;
    Params g(uex::Fn(lib, L"GetGameInstanceSubsystem")); g.Set(L"ContextObject", gi); g.Set(L"Class", cls); g.Call(lib);
    return g.Get<UObject*>(L"ReturnValue");
}
// DLC / preorder outfits only for an account that owns them, the others are free to wear here
// (entitlement unknown: treated as one)
bool Allowed(UObject* c, const FName& fid) {
    bool ent = true;
    if (auto s = Ent()) {
        Params e(uex::Fn(s, L"IsEntitlementOutfit"));
        if (e.Size()) { e.Set(L"OutfitID", fid); e.Call(s); ent = e.Get<bool>(L"ReturnValue"); }
    }
    if (!ent) return true;
    Params o(uex::Fn(c, L"IsOutfitOwned"));
    if (!o.Size()) return false;
    o.Set(L"OutfitID", fid); o.Call(c);
    return o.Get<bool>(L"ReturnValue");
}

// ---- lobby cards ----
using uex::SoftPath;  // TSoftObjectPtr -> "/Pkg/Path.Asset"
// a row's AppearanceName (FlowLocText, its ValueCache FText), "" if none or a key. Once per row (the FString leaks)
std::wstring RowName(const uint8_t* row) {
    auto lib = uex::Lib(L"KismetTextLibrary /Script/Engine.Default__KismetTextLibrary");
    auto conv = uex::Fn(lib, L"Conv_TextToString");
    int name = kRowName + kValueCache;
    if (!conv || !At<void*>(row, name)) return L"";
    Params b(conv); b.Set(L"InText", *reinterpret_cast<const uex::FText*>(row + name)); b.Call(lib);
    auto t = b.Get<FString>(L"ReturnValue");
    std::wstring l; if (t.data && t.num > 1) l.assign(t.data, t.num - 1);
    return l.find(L'_') == std::wstring::npos ? l : L"";
}
void LoadNames();
void LoadCards() {
    if (g_cards.empty()) for (auto& c : kCards) g_cards.push_back({c.id, c.label});
    if (g_named) return;
    uex::SehScope seh;
    try { LoadNames(); } catch (...) { auto w = uex::ExcWhere(); uex::Log(L"outfits: card names exception %s", w.c_str()); }
}
void LoadNames() {
    auto dt = Table(); if (!dt) return;
    g_named = true;
    std::map<std::wstring, uint8_t*> outfitRows, allRows;  // ItemId -> row (the first one wins)
    Rows(dt, [&](const FName&, uint8_t* row) {
        auto id = NameOf(kItemId.Of<FName>(row));
        allRows.emplace(id, row); if (kRowType.Of<uint8_t>(row) == 4) outfitRows.emplace(id, row);
    });
    for (auto& c : g_cards) {
        auto it = outfitRows.find(uex::Wide(c.id)); if (it == outfitRows.end()) continue;
        auto row = it->second;
        auto body = allRows.find(NameOf(kRowBodyItem.Of<FName>(row)));
        const wchar_t* src = L"built-in";
        if (auto n = RowName(row); !n.empty()) { c.label = n; src = L"outfit row"; }
        else if (body != allRows.end()) { if (auto n2 = RowName(body->second); !n2.empty()) { c.label = n2; src = L"body row"; } }
        std::wstring icon = SoftPath(row + kRowTexture);
        if (icon.empty()) icon = SoftPath(row + kRowHiTexture);
        if (icon.empty() && body != allRows.end()) icon = SoftPath(body->second + kRowTexture);
        if (!icon.empty()) g_icons[c.id] = icon;
        uex::Log(L"outfits: card %S %s (%s) icon %s", c.id.c_str(), c.label.c_str(), src, icon.empty() ? L"-" : icon.c_str());
    }
}

// ---- local player ----
uex::Ref g_pawn, g_comp; // pawn and its component while we hold the suppression / changed its look
UObject* g_world;        // the world they are in (only compared, never read)
FName g_origOutfit{};    // what it showed before the first Wear
uint8_t g_origSkin;
bool g_origHat, g_begun; // g_begun: our BeginSuppress (not one the game already had running)
std::wstring g_applied;  // our id on it now, "" = its own look
bool g_appHat; uint8_t g_appSkin;
std::wstring g_warned;   // unknown local id, logged once
std::string g_pick;      // the lobby pick (SetPick)
bool g_denied;           // the last Wear refused it (an entitlement this account does not have)
double g_again;          // Now() of the one re-apply after Wear (Tick), 0 = none

// ---- ghosts: the remote's choice applied to it ----
// keyed by the ghost's address, only ever compared: the Refs say whether it is still that ghost
struct G {
    uex::Ref actor, comp; UObject* world = nullptr;
    std::string id; int skin = -1, hat = -1;  // what is applied
    FName fid{}; double again = 0;            // again: one more apply 2 s later (a late appearance init)
    double retryAt = 0;                       // no component found: not every frame
};
std::map<UObject*, G> g_ghosts;

// a kept component: still that object (uex::Get: slot, index, name, number, class), still owned
// by that actor, in the world it was found in (a torn-down world's objects linger until GC)
UObject* LiveComp(const uex::Ref& actor, const uex::Ref& comp, UObject* world) {
    auto w = uex::World();
    auto a = w && w == world ? uex::Get(actor) : nullptr; auto c = a ? uex::Get(comp) : nullptr;
    return c && c->outer == a ? c : nullptr;
}
UObject* Held() { return LiveComp(g_pawn, g_comp, g_world); }  // the local component we changed

// load the outfit's meshes (its row and its Outfit*Item rows) right before applying it, like
// weapons preload before a swap: a mesh still loading when it is swapped in pops in late at best.
// Blocking; LoadAsset's cache makes it cheap while they are still loaded.
void Preload(const FName& id) {
    auto dt = Table(); if (!dt) return;
    FName items[9] = {id};
    Rows(dt, [&](const FName&, uint8_t* row) {
        auto& rid = kItemId.Of<const FName>(row);
        if (rid.comparison == id.comparison && rid.number == id.number)
            for (int i = 0; i < kOutfitItems; ++i) items[i + 1] = At<FName>(row, kRowBodyItem + 8 * i);  // Outfit*Item
    });
    Rows(dt, [&](const FName&, uint8_t* row) {
        auto& rid = kItemId.Of<const FName>(row);
        bool want = false;
        for (auto& n : items) if (n.comparison && n.comparison == rid.comparison && n.number == rid.number) want = true;
        if (!want) return;
        // the TSoftObjectPtr<SkeletalMesh> fields
        for (int off : {int(kRowBody), int(kRowCloth), int(kRowHat), int(kRowHead), int(kRowNeck), int(kRowBand), int(kRowBrow)}) {
            auto pkg = At<FName>(row, off + kSoftPkg), asset = At<FName>(row, off + kSoftAsset);
            if (!pkg.comparison || !asset.comparison) continue;
            auto p = NameOf(pkg), a = NameOf(asset);
            if (p.size() > 1 && p[0] == L'/' && !a.empty()) uex::LoadAsset((p + L"." + a).c_str());
        }
    });
}

// a peer's id: 1-64 printable ASCII bytes, no spaces (then still only matched against the table)
bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char ch : id) if (ch < 0x21 || ch > 0x7e) return false;
    return true;
}

void Add(std::wstring* out, const wchar_t* fmt, ...) {
    wchar_t b[768]; va_list a; va_start(a, fmt);
    int n = _vsnwprintf_s(b, _countof(b), _TRUNCATE, fmt, a); va_end(a);
    out->append(b, n < 0 ? wcslen(b) : size_t(n)); out->push_back(L'\n');
}
const wchar_t* W(const std::wstring& s) { return s.c_str(); }
void CompInfo(std::wstring* out, const wchar_t* who, UObject* c) {
    if (!c) { Add(out, L"%s: no appearance component", who); return; }
    Add(out, L"%s: %s logic %d suppressed %d char %s current %s effective %s skin %d hat %d useCfg %d cfg %s cached %s",
        who, W(NameOf(c->cls)), kCompLogic.Of<uint8_t>(c), kCompSuppress.Of<uint8_t>(c), W(NameOf(kCompCharId.Of<FName>(c))),
        W(NameOf(kCompCurrent.Of<FName>(c))), W(NameOf(NameFn(c, L"GetEffectiveOutfit"))), kCompSkin.Of<uint8_t>(c),
        kCompHat.Of<uint8_t>(c), kCompUseCfg.Of<uint8_t>(c), W(NameOf(kCompCfg.Of<FName>(c))), W(NameOf(kCompCached.Of<FName>(c))));
}
}  // namespace

const std::vector<Weapon>& List() { LoadCards(); return g_cards; }
std::wstring IconPath(const std::string& id) {
    LoadCards();
    auto it = g_icons.find(id);
    return it == g_icons.end() ? L"" : it->second;
}
void SetPick(const std::string& id) {
    g_pick.clear(); g_denied = false;
    for (auto& c : kCards) if (id == c.id) g_pick = id;
}
const std::string& Pick() { return g_pick; }
bool PickDenied() { return g_denied; }

int Probe(std::wstring* out) {
    uex::SehScope seh;
    try {
        auto me = uex::Me(); auto c = AppComp(me);
        CompInfo(out, L"me", c);
        Add(out, L"wear: holding %d applied %s orig %s skin %d hat %d begun %d", Held() ? 1 : 0, W(g_applied),
            W(NameOf(g_origOutfit)), g_origSkin, g_origHat ? 1 : 0, g_begun ? 1 : 0);
        for (auto& kv : g_ghosts)
            if (auto gc = LiveComp(kv.second.actor, kv.second.comp, kv.second.world)) {
                CompInfo(out, L"ghost", gc);
                Add(out, L"ghost: applied %S skin %d hat %d", kv.second.id.c_str(), kv.second.skin, kv.second.hat);
            }
        auto dt = Table();
        if (!dt) { Add(out, L"no table %s", kTable); return -1; }
        auto m = reinterpret_cast<uint8_t*>(dt) + kRowStruct + 8;  // RowMap
        Add(out, L"table %s rowstruct %s rows %d max %d free %d", W(PathOf(dt)), W(NameOf(kRowStruct.Of<UObject*>(dt))),
            At<int32_t>(m, kSetNum), At<int32_t>(m, kSetMax), At<int32_t>(m, kSetNumFree));
        auto ent = Ent(); int n = 0;
        bool ok = Rows(dt, [&](const FName& key, uint8_t* row) {
            FName id = kItemId.Of<FName>(row);
            uint8_t type = kRowType.Of<uint8_t>(row);
            std::wstring s = L"row " + NameOf(key) + L" id " + NameOf(id) + L" type " + std::to_wstring(type) +
                L" obtainable " + std::to_wstring(kRowObtainable.Of<uint8_t>(row)) + L" hatHidden " + std::to_wstring(kRowHatHidden.Of<uint8_t>(row)) +
                L" body " + NameOf(At<FName>(row, kRowBody + kSoftPkg)) + L"." + NameOf(At<FName>(row, kRowBody + kSoftAsset));
            if (type == 4) {
                if (ent) {
                    Params e(uex::Fn(ent, L"IsEntitlementOutfit")); e.Set(L"OutfitID", id); e.Call(ent);
                    Params st(uex::Fn(ent, L"GetOutfitState")); st.Set(L"OutfitID", id); st.Call(ent);
                    s += L" ent " + std::to_wstring(int(e.Get<bool>(L"ReturnValue"))) + L" state " + std::to_wstring(st.Get<uint8_t>(L"ReturnValue"));
                }
                if (c) {
                    Params o(uex::Fn(c, L"IsOutfitOwned")); o.Set(L"OutfitID", id); o.Call(c);
                    s += L" owned " + std::to_wstring(int(o.Get<bool>(L"ReturnValue")));
                }
            }
            out->append(s); out->push_back(L'\n'); ++n;
        });
        if (!ok) Add(out, L"rowmap does not look like a TSet (offsets?)");
        LoadCards();
        for (auto& o : g_cards) {
            auto ic = g_icons.find(o.id);
            Add(out, L"card '%S' %s icon %s", o.id.c_str(), W(o.label), ic == g_icons.end() ? L"-" : W(ic->second));
        }
        Add(out, L"pick '%S' denied %d", g_pick.c_str(), g_denied ? 1 : 0);
        return ok ? n : -1;
    } catch (...) { auto w = uex::ExcWhere(); Add(out, L"probe exception %s", W(w)); return -1; }
}

void Wear(UObject* pawn, const std::wstring& id) {
    uex::SehScope seh;
    try {
        auto held = Held();
        auto c = held && g_pawn.o == pawn ? held : AppComp(pawn);  // Held() checked g_pawn
        if (c != held) Restore();  // another pawn, or none: the old one gets its own look back if still there
        if (!c) {
            static UObject* told;  // only compared
            if (pawn && pawn != told && !id.empty() && uex::Alive(pawn)) {
                told = pawn; uex::Log(L"outfits: no appearance component on %s", NameOf(pawn).c_str());
            }
            return;
        }
        g_denied = false;
        if (id.empty()) { if (c == held) Restore(); return; }  // 默认: its own look
        if (c == held && g_applied == id) return;  // already on this pawn
        FName fid{};
        if (!FindOutfit(id, &fid)) {
            if (g_warned != id) { g_warned = id; uex::Log(L"outfits: no outfit row %s", id.c_str()); }
            return;
        }
        if (!Allowed(c, fid)) {  // a DLC outfit this account does not own: its own look
            if (c == held) Restore();
            g_denied = true;
            if (g_warned != id) { g_warned = id; uex::Log(L"outfits: %s is not owned (entitlement), not worn", id.c_str()); }
            return;
        }
        if (c != held) {  // first change to this component: remember its look, keep the wardrobe from saving
            g_origOutfit = Shown(c); g_origSkin = kCompSkin.Of<uint8_t>(c); g_origHat = kCompHat.Bit(c);
            g_begun = !Suppressed(c);
            if (g_begun) BeginSuppress(c);
            g_pawn = uex::Hold(pawn); g_comp = uex::Hold(c); g_world = uex::World();
        }
        Preload(fid);
        bool hat = !HatHidden(c, fid);  // the player's own hat setting for that outfit
        ApplyVisual(c, fid, hat, g_origSkin);
        g_applied = id; g_appHat = hat; g_appSkin = g_origSkin; g_again = Now() + 2;
        uex::Log(L"outfits: wear %s (own %s skin %d hat %d%s)", id.c_str(), NameOf(g_origOutfit).c_str(), g_origSkin,
                 hat ? 1 : 0, g_begun ? L"" : L", save was already suppressed");
    } catch (...) { auto w = uex::ExcWhere(); uex::Log(L"outfits: wear exception %s", w.c_str()); }
}

void Restore() {
    uex::SehScope seh;
    try {
        if (auto c = Held()) {
            if (!g_applied.empty() && g_origOutfit.comparison) ApplyVisual(c, g_origOutfit, g_origHat, g_origSkin);
            if (g_begun) EndNoApply(c);  // puts BeginSuppress's cache back; false: no save
            uex::Log(L"outfits: restored %s%s", NameOf(g_origOutfit).c_str(), g_begun ? L", save suppression ended" : L"");
        }
    } catch (...) { auto w = uex::ExcWhere(); uex::Log(L"outfits: restore exception %s", w.c_str()); }
    g_pawn = {}; g_comp = {}; g_world = nullptr; g_origOutfit = {};
    g_applied.clear(); g_begun = false; g_denied = false; g_again = 0;
}

// one more apply 2 s after Wear: an appearance init still running on a new pawn would undo it
void Tick() {
    if (!g_again || Now() < g_again) return;
    g_again = 0;
    uex::SehScope seh;
    try {
        FName fid{};
        if (auto c = Held(); c && !g_applied.empty() && FindOutfit(g_applied, &fid)) ApplyVisual(c, fid, g_appHat, g_appSkin);
    } catch (...) { auto w = uex::ExcWhere(); uex::Log(L"outfits: tick exception %s", w.c_str()); }
}

void WearGhost(UObject* ghost, const std::string& id, int skin, int hat) {
    if (!ghost || !ValidId(id) || skin < 0 || skin > 3 || (hat != 0 && hat != 1)) return;
    uex::SehScope seh;
    try {
        if (g_ghosts.size() > 16)  // ghosts are new each round: drop the gone ones
            for (auto it = g_ghosts.begin(); it != g_ghosts.end();)
                if (LiveComp(it->second.actor, it->second.comp, it->second.world)) ++it; else it = g_ghosts.erase(it);
        G& g = g_ghosts[ghost];
        double now = Now();
        auto c = LiveComp(g.actor, g.comp, g.world);
        if (!c) {  // a new ghost (or another object at that address now)
            if (now < g.retryAt) return;
            g = G{};
            if (!(c = AppComp(ghost))) { g.retryAt = now + 1; return; }
            if (!Suppressed(c)) BeginSuppress(c);  // its wardrobe must never save; never ended (dies with it)
            g.actor = uex::Hold(ghost); g.comp = uex::Hold(c); g.world = uex::World();
        }
        if (g.id == id && g.skin == skin && g.hat == hat) {
            if (g.again && now >= g.again) { g.again = 0; ApplyVisual(c, g.fid, hat != 0, uint8_t(skin)); }
            return;
        }
        FName fid{};
        if (!FindOutfit(std::wstring(id.begin(), id.end()), &fid)) {  // printable ASCII: byte-wise is exact
            static std::string bad;
            if (bad != id) { bad = id; uex::Log(L"outfits: ghost outfit %S not found", id.c_str()); }
            return;
        }
        // its "configured" look (GetEffectiveOutfit returns it): the game's own re-applies should
        // then show the remote's outfit, not the local wardrobe (TODO-probe)
        { Params p(uex::Fn(c, L"SetConfiguredAppearance")); p.Set(L"OutfitID", fid); p.Set<bool>(L"bHatHidden", hat == 0);
          p.Set<uint8_t>(L"SkinColor", uint8_t(skin)); p.Call(c); }
        { Params p(uex::Fn(c, L"SetUseConfiguredAppearance")); p.Set<bool>(L"bUseConfigured", true); p.Call(c); }
        Preload(fid);
        ApplyVisual(c, fid, hat != 0, uint8_t(skin));
        g.id = id; g.skin = skin; g.hat = hat; g.fid = fid; g.again = now + 2;
        uex::Log(L"outfits: ghost %s wears %S skin %d hat %d", NameOf(ghost).c_str(), id.c_str(), skin, hat);
    } catch (...) { static int n; auto w = uex::ExcWhere(); if (n++ < 5) uex::Log(L"outfits: ghost exception %s", w.c_str()); }
}

std::string Event() {
    uex::SehScope seh;
    try {
        if (!g_applied.empty() && Held()) {
            int skin = g_appSkin <= 3 ? g_appSkin : 0;  // the receiver takes 0-3 only
            return "O " + uex::Utf8(g_applied) + " " + std::to_string(skin) + (g_appHat ? " 1" : " 0");
        }
        // its own look (当前外观, or a pick it does not own): the ghost on the other side shows it too
        auto c = AppComp(uex::Me()); if (!c) return "";
        auto id = uex::Utf8(NameOf(Shown(c)));
        if (!ValidId(id) || id == "None") return "";
        int skin = kCompSkin.Of<uint8_t>(c); if (skin > 3) skin = 0;
        return "O " + id + " " + std::to_string(skin) + (kCompHat.Bit(c) ? " 1" : " 0");
    } catch (...) { return ""; }
}

// "O <id> <skin> <hat>": exactly these three fields; id as ValidId, skin 0-3, hat 0/1
bool Parse(const std::string& ev, std::string* id, int* skin, int* hat) {
    if (ev.size() < 7 || ev.size() > 70 || ev.compare(0, 2, "O ") != 0) return false;
    size_t b = ev.find(' ', 2);
    if (b == std::string::npos || b + 4 != ev.size() || ev[b + 2] != ' ') return false;
    std::string i = ev.substr(2, b - 2);
    char s = ev[b + 1], h = ev[b + 3];
    if (!ValidId(i) || s < '0' || s > '3' || (h != '0' && h != '1')) return false;
    if (id) *id = i;
    if (skin) *skin = s - '0';
    if (hat) *hat = h - '0';
    return true;
}

// the world went (travel, reload): its objects are not ours to touch any more. No End: the
// suppression flag lives on the old component, which goes with its world.
void Forget() {
    g_pawn = {}; g_comp = {}; g_world = nullptr; g_origOutfit = {};
    g_applied.clear(); g_begun = false; g_warned.clear(); g_denied = false; g_again = 0;
    g_ghosts.clear();
    g_dt = {}; g_ids.clear(); g_bad.clear();
}
}  // namespace fight::outfits

// ui: multiplayer menus built at runtime from plain UMG widgets (UserWidget + WidgetTree, children
// created with GameplayStatics::SpawnObject). One root UserWidget holds every section:
//   corner button "多人游戏" (title screen, no pawn; F7 opens the panel in the world), connect panel,
//   lobby panel, HUD (hp bars, round, score, timer), big count, result.
// Buttons are polled (IsPressed falling edge while hovered / focused). Text is set through
// uex::SetText (FText cached per string, skips unchanged). Edit boxes are only read on a click
// (GetText/Conv_TextToString leak a little, never per frame).
// Esc (our window in front, read with GetAsyncKeyState, never sent) closes the panel.
// Test hook: <mod dir>\ui_cmd.txt lines "open" "close" "set <id> <text>" "click <id>" "dump";
// results go to ui_test.txt. Checked every 30 ticks, file deleted after reading.
#include "ui.h"
#include <windows.h>
#include <cstdio>

#include <cmath>
#include <ctime>
#include <map>
#include <vector>
#include "uex.h"
#include "../net/net.h"
#include "../fight/fight.h"
#include "../fight/weapons.h"
#include "../fight/outfits.h"
#include "../arena/arena.h"

using namespace ue;
using uex::UObject;

namespace ui {
namespace {
struct V2 { double x, y; };
struct Anchors { V2 mn, mx; };
struct Margin { float l, t, r, b; };
struct Color { float r, g, b, a; };
struct FStr { wchar_t* d; int32_t n, m; };
struct FKey { FName name; void* d[2]; };
struct Btn { UObject* o = nullptr; UObject* label = nullptr; bool was = false; };
struct SlateColor { Color c; uint8_t rule = 0, pad[3]{}; };  // 0x14 in this build (rule 0 = specified)

constexpr uint8_t kVisible = 0, kCollapsed = 1, kHidden = 2, kHitTestInvisible = 3, kSelfHitTestInvisible = 4;
constexpr int kMaxRows = 4;  // server max players

const int kRounds[] = {1, 3, 5, 7, 9};
const int kTimes[] = {0, 60, 90, 120, 180};
const int kDmgs[] = {50, 75, 100, 150, 200};

// palette
constexpr Color kInk{0.93f, 0.9f, 0.84f, 1}, kDim{0.62f, 0.6f, 0.56f, 1}, kGold{0.98f, 0.78f, 0.38f, 1},
    kRed{0.92f, 0.3f, 0.25f, 1}, kGreen{0.45f, 0.85f, 0.45f, 1}, kWarn{1, 0.68f, 0.35f, 1};

struct G {
    UObject* world = nullptr;
    UObject* root = nullptr;  // UserWidget
    UObject* tree = nullptr;
    UObject *canvas = nullptr, *corner = nullptr, *connect = nullptr, *lobby = nullptr, *hud = nullptr, *result = nullptr,
            *count = nullptr;
    UObject *myBar = nullptr, *oppBar = nullptr;
    UObject* rows[kMaxRows]{};
    std::map<std::wstring, Btn> btn;
    std::map<std::wstring, UObject*> edit, text;
    std::map<UObject*, float> pct;    // last SetPercent
    std::map<UObject*, Color> color;  // last SetColorAndOpacity
    std::map<UObject*, uint8_t> vis;  // last SetVisibility
    uint8_t font[0x60]{}; bool haveFont = false;
    bool open = false, uiMode = false, hudTest = false;
    bool steamMode = false;  // connect panel: Steam P2P instead of the relay server
    int weapon = 0, tick = 0; double weaponAt = -10; double modsAt = -10;  // last local weapon click (server echo may lag)
    int edge = 0; double edgeAt = -10;                // last local Phantom Edge click (same echo rule)
    int outfit = 0;                                   // outfit card picked (local: no room field, sent as "O" in the match)
    std::string lastPhase;
    double fightAt = -1;  // Now() when the fight phase began ("开始" shown for a second)
    std::wstring status;
    std::wstring lobbyStatus;  // lstatus as last shown (test hook dump)
    bool edgeNote = false;     // the "Phantom Edge not equipped" note is shown (test hook dump)
    bool failed = false;
    // title menu entry
    UObject *menu = nullptr, *menuBtn = nullptr, *menuFont = nullptr; bool menuWas = false, menuInGroup = false; int menuTries = 0, menuFix = 0;
    // in-match menu (Esc): the game's own menu keys are redirected while a match runs (BlockMenuKeys)
    UObject* esc = nullptr; bool escMenu = false, escWas = false;
    uex::Ref kefMenu, menuOffRef;  // KeepEditFocus: the title menu found / the one hidden
    // edit focus: the title menu pulls keyboard focus back to its buttons; it is disabled while the
    // panel is open and focus is handed back to the edit box it was taken from
    UObject *menuOff = nullptr, *lastEdit = nullptr; uint8_t menuVis = 0; int refocus = 0;
} g;
static double g_sessionAt = -100; static bool g_sessionFile;  // crash rejoin file (SessionTick); outlives Build

// ---- small UE call helpers --------------------------------------------------------------
UObject* UmgClass(const wchar_t* n) { return uex::Class((std::wstring(L"Class /Script/UMG.") + n).c_str()); }
UObject* New(const wchar_t* cls, UObject* outer) {
    auto gs = uex::GameplayStatics(); auto c = UmgClass(cls);
    if (!gs || !c || !outer) return nullptr;
    Params p(uex::Fn(gs, L"SpawnObject")); p.Set(L"ObjectClass", c); p.Set(L"Outer", outer); p.Call(gs);
    return p.Get<UObject*>(L"ReturnValue");
}
template <class T> void Call1(UObject* o, const wchar_t* fn, const wchar_t* arg, const T& v) {
    if (!o) return; auto f = uex::Fn(o, fn); if (!f) { uex::Log(L"ui: no fn %s", fn); return; }
    Params p(f); p.Set(arg, v); p.Call(o);
}
bool CallB(UObject* o, const wchar_t* fn) {
    if (!o) return false; auto f = uex::Fn(o, fn); if (!f) return false;
    Params p(f); p.Call(o); return p.Get<bool>(L"ReturnValue");
}
UObject* Add(UObject* panel, UObject* child, const wchar_t* fn = L"AddChild") {
    if (!panel || !child) return nullptr;
    Params p(uex::Fn(panel, fn)); p.Set(L"Content", child); p.Call(panel);
    return p.Get<UObject*>(L"ReturnValue");
}
void Vis(UObject* w, uint8_t v) { Call1<uint8_t>(w, L"SetVisibility", L"InVisibility", v); }
template <class T> void Put(UObject* o, int off, const T& v) { *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(o) + off) = v; }

// copy the font (not the CompositeFont shared pointer at +0x30) of a live game TextBlock
void FindFont() {
    auto tb = UmgClass(L"TextBlock"); if (!tb) return;
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || (o->flags & 0x10) || !IsA(o, tb)) continue;
        auto f = At<UObject*>(o, 0x1e0);
        if (!f || NameOf(f).find(L"Roboto") != std::wstring::npos) continue;
        memcpy(g.font, reinterpret_cast<uint8_t*>(o) + 0x1e0, 0x60);
        memset(g.font + 0x30, 0, 0x10);
        g.haveFont = true;
        uex::Log(L"ui: font %s from %s", PathOf(f).c_str(), PathOf(o).c_str());
        return;
    }
    uex::Log(L"ui: no game font found, using default");
}
void ApplyFont(uint8_t* fi, float size) {  // fi: an FSlateFontInfo inside a widget not built yet
    if (g.haveFont) { memcpy(fi, g.font, 0x30); memcpy(fi + 0x40, g.font + 0x40, 0x20); }
    *reinterpret_cast<float*>(fi + 0x48) = size;
}

// ---- game look: the game's own textures / fonts / boss bar (all under /Game/PBZContent/UI) -
// Sizes are in 1080p slate units (the viewport DPI scale is 0.666 at 1280x720, 0.5 at 960x540).
namespace art {
const wchar_t *kPanel = L"/Game/PBZContent/UI/UINew/System/T_ConfirmBG.T_ConfirmBG",  // ink panel of the confirm dialogs
    *kSoft = L"/Game/PBZContent/UI/UINew/HUD/T_Tutorial_BG.T_Tutorial_BG",              // soft dark of the tutorial popups
    *kLine = L"/Game/PBZContent/UI/UINew/System/line_popup.line_popup",
    *kBtn = L"/Game/PBZContent/UI/Texture/Confirmation/T_NotSelected.T_NotSelected",     // WB_ConfirmButton ink bars
    *kBtnHi = L"/Game/PBZContent/UI/Texture/Confirmation/T_Selected.T_Selected",
    *kBar = L"/Game/PBZContent/UI/Settings/Texture/T_SettingsDefaultBar.T_SettingsDefaultBar",  // settings menu rows
    *kBarHi = L"/Game/PBZContent/UI/Settings/Texture/T_SettingsSelectedBar.T_SettingsSelectedBar",
    *kLeft = L"/Game/PBZContent/UI/Menu/StartMenu/T_SelectArrow_Left.T_SelectArrow_Left",
    *kRight = L"/Game/PBZContent/UI/Menu/StartMenu/T_SelectArrow_Right.T_SelectArrow_Right",
    *kBloom = L"/Game/PBZContent/UI/UINew/System/Character/T_SelectedBloom_W.T_SelectedBloom_W",
    *kCardBg = L"/Game/PBZContent/UI/UINew/System/Character/bg_weapon_bg.bg_weapon_bg",  // character screen weapon slots
    *kCardFrame = L"/Game/PBZContent/UI/UINew/System/Character/bg_weapon_frame.bg_weapon_frame",
    *kCardSel = L"/Game/PBZContent/UI/UINew/System/Character/bg_weapon_sel.bg_weapon_sel",
    *kHost = L"/Game/PBZContent/UI/Texture/PauseMenu/T_Icon_Arrow_Gold1.T_Icon_Arrow_Gold1",
    *kHpFill = L"/Game/PBZContent/UI/Hud/bar_hphud.bar_hphud",                           // player hp bar fill
    *kHpBack = L"/Game/PBZContent/UI/Hud/bar_hp.bar_hp",                                 // hp bar back
    *kBoss = L"/Game/PBZContent/UI/Hud/WB_BossHPBar.WB_BossHPBar_C",                     // boss hp + posture bar
    *kTitleFont = L"/Game/PBZContent/UI/Fonts/AaGuDianKeBenSong-2_Font.AaGuDianKeBenSong-2_Font";
}  // namespace art
// weapon card / member icons: the full colour renders the weapon data table uses (DT_WeaponData White)
std::wstring WeaponIcon(const std::string& id) {
    static const std::map<std::string, const wchar_t*> m = {{"Tang", L"HeiShang"}, {"Double", L"HeiShangShuangRen"},
        {"Long", L"ChangXingHeiShang"}, {"PoJun", L"PoJun"}, {"BaiShanShuangJian", L"BaiShanFengLeiJian"}, {"FangHuaXieShan", L"ShengMuXieShan"},
        {"IronBlade", L"CanGang"}, {"GuiTouBlade", L"HongZhuangGuiZhan"}, {"ChiZheCanJian", L"ChiZheCanJian"}, {"ZhanMaBlade", L"WangMingZhanMaDao"},
        {"SnakeDual", L"BaiMangChiLian"}, {"RenOuQingTongJian", L"TianXuanQingTongJian"}, {"DualHook", L"BieLiCanGou"},
        {"TianShaShuangYue", L"TianShaShuangYue"}, {"SlitheringSword", L"LingSheRuanJian"}, {"HorizontalBlade", L"DangKouXiongRen"},
        {"JadeDualChakram", L"YuShuangHuan"}, {"YuuBlade", L"YaoJu"}, {"Jigsaw", L"DuanZhiJu"}, {"RenOuYanYueDao", L"YanYueDao"},
        {"CiGuHunDun", L"CiGuHunDun"}, {"BaiYing", L"BaiYing"}, {"ZiMingJian", L"ZiMingJian"}, {"LuanYing", L"LuanYingJian"}, {"HeiMoShang", L"HeiMoShang"}};
    auto it = m.find(id);
    if (it == m.end()) return L"/Game/PBZContent/UI/UINew/HUD/T_Quest_V2.T_Quest_V2";  // "current gear": swordsman figure
    std::wstring n = it->second;
    return L"/Game/PBZContent/UI/Hud/Art/Temp_Render/T_Icon_Weapon_" + n + L".T_Icon_Weapon_" + n;
}
// Phantom Edge card: the row's RenderIcon (else Icon, WhiteIcon; DT_ShaqiWeaponData), "current gear" figure for [0] / no icon
std::wstring EdgeIcon(const std::string& id) {
    auto p = uex::Wide(fight::EdgeIconPath(id));
    return p.empty() ? L"/Game/PBZContent/UI/UINew/HUD/T_Quest_V2.T_Quest_V2" : p;
}
// outfit card: the outfit's picture (AppearanceTexture), "current gear" figure for [0] / none
std::wstring OutfitIcon(const std::string& id) {
    auto p = fight::outfits::IconPath(id);
    return p.empty() ? L"/Game/PBZContent/UI/UINew/HUD/T_Quest_V2.T_Quest_V2" : p;
}
struct Cards {  // a lobby card row (weapons / Phantom Edges)
    std::vector<UObject*> card, frame, name;  // button, selection frame image, name text
    int shown = -2;                           // card shown as selected
    int page = 0, pageShown = -1;             // page (kCardsPerPage cards each)
};
struct Skin {  // widgets of the new layout (reset on every Build)
    UObject *scratch = nullptr, *titleFont = nullptr, *plaque = nullptr;
    uint8_t blank[0xb0]{};
    Cards wc, ec, oc;                         // weapon cards, Phantom Edge cards, outfit cards
    int tab = 0, tabShown = -1;               // card row shown: 0 weapons, 1 Phantom Edges, 2 outfits
    std::vector<UObject*> mapSel, mapName;    // map cards: selection frame image, name text
    int mapShown = -2;                        // map card shown as selected
} k;
constexpr int kCardsPerPage = 7;  // cards per lobby page (the old single row was 7 wide)
int Pages(int n) { return n > 0 ? (n + kCardsPerPage - 1) / kCardsPerPage : 1; }
// brush for a texture as the engine makes it (SetBrushFromTexture on a scratch Image), then our draw settings.
// drawAs: 0 none, 1 box (9-slice by margin), 3 image (stretched). Build time only.
void MakeBrushTex(uint8_t* out, UObject* t, uint8_t drawAs, Color tint, Margin m = {}, float w = 0, float h = 0) {
    if (!k.scratch) { k.scratch = New(L"Image", g.tree); if (!k.scratch) return; memcpy(k.blank, reinterpret_cast<uint8_t*>(k.scratch) + 0x180, 0xb0); }
    memcpy(reinterpret_cast<uint8_t*>(k.scratch) + 0x180, k.blank, 0xb0);
    if (t) {  // bMatchSize: ImageSize = texture size unless w/h given
        Params p(uex::Fn(k.scratch, L"SetBrushFromTexture")); p.Set(L"Texture", t); p.Set<bool>(L"bMatchSize", true); p.Call(k.scratch);
    }
    memcpy(out, reinterpret_cast<uint8_t*>(k.scratch) + 0x180, 0xb0);
    out[0x1c] = drawAs; *reinterpret_cast<Color*>(out + 0x8) = tint; out[0x18] = 0; *reinterpret_cast<Margin*>(out + 0x28) = m;
    if (w > 0) { reinterpret_cast<float*>(out + 0x20)[0] = w; reinterpret_cast<float*>(out + 0x20)[1] = h; }
}
void MakeBrush(uint8_t* out, const wchar_t* tex, uint8_t drawAs, Color tint, Margin m = {}, float w = 0, float h = 0) {
    auto t = tex ? uex::LoadAsset(tex) : nullptr;
    if (tex && !t) uex::Log(L"ui: no texture %s", tex);
    MakeBrushTex(out, t, drawAs, tint, m, w, h);
}
// a PNG shipped in the mod folder (<moddir>/maps/<file>) loaded as a transient texture; the brush keeps it referenced
UObject* MkFileImg(const std::wstring& file, float w, float h) {
    struct FS { const wchar_t* d; int32_t n, m; };
    std::wstring path = std::wstring(uex::Host()->dir) + L"maps/" + file;
    UObject* t = nullptr;
    if (auto lib = uex::Lib(L"KismetRenderingLibrary /Script/Engine.Default__KismetRenderingLibrary")) {
        Params p(uex::Fn(lib, L"ImportFileAsTexture2D"));
        p.Set(L"WorldContextObject", uex::World());
        p.Set(L"Filename", FS{path.c_str(), int32_t(path.size() + 1), int32_t(path.size() + 1)});
        p.Call(lib); t = p.Get<UObject*>(L"ReturnValue");
    }
    if (!t) uex::Log(L"ui: no preview %s", path.c_str());
    auto i = New(L"Image", g.tree); if (!i) return nullptr;
    MakeBrushTex(reinterpret_cast<uint8_t*>(i) + 0x180, t, 3, {1, 1, 1, 1}, {}, w, h);
    Put(i, 0x240, Color{1, 1, 1, 1}); Put<uint8_t>(i, 0xdd, kHitTestInvisible);
    return i;
}
// plain rectangular outline (Border draw, no texture): t px thick on a w x h box
UObject* MkRect(float w, float h, float t, Color c) {  // overlay of four solid bars along the edges
    auto ov = New(L"Overlay", g.tree); if (!ov) return nullptr;
    struct E { float w, h; uint8_t ha, va; } es[4] = {{w, t, 0, 1}, {w, t, 0, 3}, {t, h, 1, 0}, {t, h, 3, 0}};
    for (auto& e : es) {
        auto i = New(L"Image", g.tree); if (!i) continue;
        MakeBrush(reinterpret_cast<uint8_t*>(i) + 0x180, L"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture", 3, {1, 1, 1, 1}, {}, e.w, e.h);
        Put(i, 0x240, c); Put<uint8_t>(i, 0xdd, kHitTestInvisible);
        auto s = Add(ov, i, L"AddChildToOverlay");
        Call1<uint8_t>(s, L"SetHorizontalAlignment", L"InHorizontalAlignment", e.ha);
        Call1<uint8_t>(s, L"SetVerticalAlignment", L"InVerticalAlignment", e.va);
    }
    Put<uint8_t>(ov, 0xdd, kHitTestInvisible);
    return ov;
}
UObject* MkImg(const wchar_t* tex, float w, float h, Color tint = {1, 1, 1, 1}) {
    auto i = New(L"Image", g.tree); if (!i) return nullptr;
    MakeBrush(reinterpret_cast<uint8_t*>(i) + 0x180, tex, 3, {1, 1, 1, 1}, {}, w, h);
    Put(i, 0x240, tint); Put<uint8_t>(i, 0xdd, kHitTestInvisible);
    return i;
}
void TextColor(UObject* t, Color c) {  // runtime, cached
    if (!t) return;
    if (auto it = g.color.find(t); it != g.color.end() && !memcmp(&it->second, &c, sizeof c)) return;
    g.color[t] = c; Call1(t, L"SetColorAndOpacity", L"InColorAndOpacity", SlateColor{c});
}

// ---- widget builders (call before the widget is added to a live panel) -------------------
UObject* MkText(const std::wstring& s, float size = 26, Color c = {0.92f, 0.9f, 0.85f, 1}) {
    auto t = New(L"TextBlock", g.tree); if (!t) return nullptr;
    ApplyFont(reinterpret_cast<uint8_t*>(t) + 0x1e0, size);
    Put(t, 0x1b8, c); Put<uint8_t>(t, 0x1c8, 0);
    Put(t, 0x2f0, V2{1.5, 1.5}); Put(t, 0x300, Color{0, 0, 0, 0.85f});  // drop shadow, like the game's hud text
    Put<uint8_t>(t, 0xdd, kHitTestInvisible);
    uex::SetText(t, s);
    return t;
}
UObject* Title(UObject* t, int spacing = 60) {  // the confirm dialog / button typeface
    if (t && k.titleFont) { Put(t, 0x1e0, k.titleFont); Put<int32_t>(t, 0x1e0 + 0x4c, spacing); }
    return t;
}
UObject* Center(UObject* t, float minW = 0) { if (t) { Put<uint8_t>(t, 0x17b, 1); if (minW > 0) Put<float>(t, 0x1dc, minW); } return t; }
UObject* MkText(const wchar_t* id, const std::wstring& s, float size, Color c = {0.92f, 0.9f, 0.85f, 1}) {
    auto t = MkText(s, size, c); g.text[id] = t; return t;
}
// Button skin: brushes Normal +0x1a0, Hovered +0x250, Pressed +0x300, Disabled +0x3b0; BackgroundColor +0x530
void SkinButton(UObject* b, const wchar_t* normal, const wchar_t* hover, Color nTint = {1, 1, 1, 1}, Color hTint = {1, 1, 1, 1}) {
    auto p = reinterpret_cast<uint8_t*>(b);
    MakeBrush(p + 0x1a0, normal, normal ? 3 : 0, nTint);
    MakeBrush(p + 0x250, hover, 3, hTint);
    MakeBrush(p + 0x300, hover, 3, {hTint.r * 0.8f, hTint.g * 0.8f, hTint.b * 0.8f, hTint.a});
    MakeBrush(p + 0x3b0, normal, normal ? 3 : 0, {nTint.r, nTint.g, nTint.b, nTint.a * 0.45f});
    Put(b, 0x530, Color{1, 1, 1, 1});
}
// ink bar button of the game's confirm dialogs (T_NotSelected / T_Selected), confirm dialog typeface
UObject* MkButton(const wchar_t* id, const std::wstring& label, float size = 26) {
    auto b = New(L"Button", g.tree); if (!b) return nullptr;
    SkinButton(b, art::kBtn, art::kBtnHi);
    auto t = Center(Title(MkText(label, size)), 150);
    if (auto s = Add(b, t)) Call1(s, L"SetPadding", L"InPadding", Margin{44, 12, 44, 12});
    g.btn[id] = {b, t, false};
    return b;
}
UObject* MkEdit(const wchar_t* id, const std::wstring& init, float width = 360) {
    auto e = New(L"EditableTextBox", g.tree); if (!e) return nullptr;
    ApplyFont(reinterpret_cast<uint8_t*>(e) + 0x1a0 + 0x2e0 + 0x8, 24);
    Put<float>(e, 0xe44, width);
    // EditableTextBoxStyle at +0x1a0: brushes Normal/Hovered/Focused +0x10/+0xc0/+0x170, Padding +0x2d0,
    // ForegroundColor +0x5c0, FocusedForegroundColor +0x5fc (SlateColor)
    auto p = reinterpret_cast<uint8_t*>(e);
    MakeBrush(p + 0x1b0, art::kSoft, 1, {1, 1, 1, 0.9f}, {0.3f, 0.3f, 0.3f, 0.3f});
    MakeBrush(p + 0x260, art::kSoft, 1, {1.15f, 1.1f, 1.f, 1}, {0.3f, 0.3f, 0.3f, 0.3f});
    MakeBrush(p + 0x310, art::kSoft, 1, {1.3f, 1.2f, 1.05f, 1}, {0.3f, 0.3f, 0.3f, 0.3f});
    Put(e, 0x1a0 + 0x2d0, Margin{14, 8, 14, 8});
    for (int off : {0x5c0, 0x5fc}) { Put(e, 0x1a0 + off, kInk); Put<uint8_t>(e, 0x1a0 + off + 0x10, 0); }
    { Params p(uex::Fn(e, L"SetText")); p.Set(L"InText", uex::Text(init)); p.Call(e); }
    g.edit[id] = e;
    return e;
}
UObject* VBox() { return New(L"VerticalBox", g.tree); }
UObject* HBox() { return New(L"HorizontalBox", g.tree); }
void VAdd(UObject* box, UObject* w, Margin pad = {0, 4, 0, 4}, uint8_t halign = 0) {
    auto s = Add(box, w, L"AddChildToVerticalBox");
    Call1(s, L"SetPadding", L"InPadding", pad);
    if (halign) Call1<uint8_t>(s, L"SetHorizontalAlignment", L"InHorizontalAlignment", halign);
}
void HAdd(UObject* box, UObject* w, Margin pad = {0, 0, 10, 0}) {
    auto s = Add(box, w, L"AddChildToHorizontalBox");
    Call1(s, L"SetPadding", L"InPadding", pad);
    Call1<uint8_t>(s, L"SetVerticalAlignment", L"InVerticalAlignment", 2);
}
UObject* Row(const wchar_t* label, UObject* w) {
    auto h = HBox(); auto l = MkText(label, 26, kDim); Put<float>(l, 0x1dc, 130);
    HAdd(h, l, {0, 0, 16, 0}); HAdd(h, w); return h;
}
UObject* Line(float w = 640) { return MkImg(art::kLine, w, 10, {1, 0.9f, 0.75f, 0.7f}); }  // popup divider
UObject* Heading(const wchar_t* id, const std::wstring& s, float size = 40) {  // gold title, confirm typeface
    auto t = Title(MkText(s, size, kGold), 120);
    if (id) g.text[id] = t;
    return t;
}
// framed box placed on the canvas; returns the frame, *inner gets its VerticalBox.
// bg.a > 0.6: the confirm dialog ink panel (T_ConfirmBG), else the soft tutorial backdrop (T_Tutorial_BG, 9-slice)
UObject* Frame(V2 anchor, V2 align, V2 pos, UObject** inner, Color bg = {0.03f, 0.03f, 0.04f, 0.88f}, UObject* parent = nullptr) {
    auto b = New(L"Border", g.tree); auto v = VBox();
    bool ink = bg.a > 0.6f;
    if (b) MakeBrush(reinterpret_cast<uint8_t*>(b) + 0x1d0, ink ? art::kPanel : art::kSoft, ink ? 3 : 1, {1, 1, 1, 1},
                     ink ? Margin{} : Margin{0.3f, 0.3f, 0.3f, 0.3f});
    Call1(b, L"SetBrushColor", L"InBrushColor", ink ? Color{1, 1, 1, 0.97f} : Color{1, 1, 1, bg.a * 1.6f});
    Call1(b, L"SetPadding", L"InPadding", ink ? Margin{90, 50, 90, 50} : Margin{36, 18, 36, 18});
    Add(b, v);
    auto s = Add(parent ? parent : g.canvas, b, L"AddChildToCanvas");
    Call1(s, L"SetAnchors", L"InAnchors", Anchors{anchor, anchor});
    Call1(s, L"SetAlignment", L"InAlignment", align);
    Call1(s, L"SetPosition", L"InPosition", pos);
    Call1<bool>(s, L"SetAutoSize", L"InbAutoSize", true);
    *inner = v; return b;
}
// child on a canvas panel (child null: a new CanvasPanel). off: slot offsets (left, top, right/width, bottom/height);
// autoSize: the child keeps its desired size, placed at (left, top)
UObject* Canvas(UObject* parent, V2 amin, V2 amax, V2 align, Margin off, UObject* child = nullptr, bool autoSize = false) {
    if (!child) child = New(L"CanvasPanel", g.tree);
    auto s = Add(parent, child, L"AddChildToCanvas"); if (!s) return child;
    Call1(s, L"SetAnchors", L"InAnchors", Anchors{amin, amax});
    Call1(s, L"SetAlignment", L"InAlignment", align);
    Call1(s, L"SetOffsets", L"InOffset", off);
    if (autoSize) Call1<bool>(s, L"SetAutoSize", L"InbAutoSize", true);
    return child;
}

// connect settings remembered across runs: <mod dir>mp_settings.txt, UTF-8 "key=value" lines
// (ip, port, name, pw, mode). Read when the panel is built, written on create/join.
void WriteEdit(const wchar_t* id, const std::wstring& s);
std::wstring SettingsPath() { auto h = uex::Host(); return h ? std::wstring(h->dir) + L"mp_settings.txt" : L""; }
void LoadSettings() {
    auto p = SettingsPath(); if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"r, ccs=UTF-8"); if (!f) return;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        auto eq = s.find(L'='); if (eq == std::wstring::npos) continue;
        std::wstring k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == L"mode") g.steamMode = v == L"steam";
        else if ((k == L"ip" || k == L"port" || k == L"name" || k == L"pw") && !v.empty() && v.size() < 128) WriteEdit(k.c_str(), v);
    }
    fclose(f);
}
void SaveSettings(const std::wstring& ip, const std::wstring& port, const std::wstring& name, const std::wstring& pw) {
    auto p = SettingsPath(); if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"w, ccs=UTF-8"); if (!f) return;
    fwprintf(f, L"ip=%s\nport=%s\nname=%s\npw=%s\nmode=%s\n", ip.c_str(), port.c_str(), name.c_str(), pw.c_str(), g.steamMode ? L"steam" : L"relay");
    fclose(f);
}

void Build() {
    g = G{.world = uex::World(), .open = g.open, .weapon = g.weapon, .edge = g.edge, .outfit = g.outfit, .status = g.status};
    uex::ForgetTexts();  // new widgets can come at old addresses
    auto cls = UmgClass(L"UserWidget");
    g.root = uex::CreateWidget(cls);
    if (!g.root) { uex::Log(L"ui: CreateWidget failed"); return; }
    g.tree = At<UObject*>(g.root, 0x2c0);
    if (!g.tree) { g.tree = New(L"WidgetTree", g.root); Put(g.root, 0x2c0, g.tree); }
    g.canvas = New(L"CanvasPanel", g.tree);
    if (!g.tree || !g.canvas) { uex::Log(L"ui: tree/canvas failed"); g.root = nullptr; return; }
    Put(g.tree, 0x30, g.canvas);
    FindFont();
    k = Skin{}; k.titleFont = uex::LoadAsset(art::kTitleFont);
    if (!k.titleFont) uex::Log(L"ui: no title font, using the body font");
    UObject* v;

    // corner entry
    g.corner = Frame({1, 0}, {1, 0}, {-40, 40}, &v, {0, 0, 0, 0.5f});
    VAdd(v, MkButton(L"mp", L"多人游戏", 28));

    // connect panel
    g.connect = Frame({0.5, 0.5}, {0.5, 0.5}, {0, 0}, &v);
    VAdd(v, Heading(nullptr, L"多人游戏", 44), {0, 0, 0, 4}, 2);
    VAdd(v, Line(560), {0, 0, 0, 14}, 2);
    { auto h = HBox(); auto l = MkText(L"连接方式", 26, kDim); Put<float>(l, 0x1dc, 130); HAdd(h, l, {0, 0, 16, 0});
      HAdd(h, MkButton(L"mode", L"中转服务器", 24)); VAdd(v, h); }
    VAdd(v, g.text[L"iprow"] = Row(L"服务器", MkEdit(L"ip", L"127.0.0.1")));
    VAdd(v, g.text[L"portrow"] = Row(L"端口", MkEdit(L"port", L"27777", 160)));
    VAdd(v, g.text[L"pwrow"] = Row(L"服务器密码", MkEdit(L"pw", L"", 260)));
    Call1<bool>(g.edit[L"pw"], L"SetIsPassword", L"InbIsPassword", true);
    VAdd(v, g.text[L"steamrow"] = Center(MkText(L"steaminfo", L"", 22, kDim)), {0, 4, 0, 4}, 2);
    {   // Steam persona name as the default player name
        std::string pn = net::SteamAvailable() ? net::SteamPersona() : "";
        VAdd(v, Row(L"名字", MkEdit(L"name", pn.empty() ? L"玩家" : uex::Wide(pn))));
    }
    LoadSettings();
    VAdd(v, MkButton(L"create", L"创建房间"), {0, 16, 0, 8}, 2);
    { auto h = HBox(); auto l = MkText(L"房间码", 26, kDim); Put<float>(l, 0x1dc, 130); HAdd(h, l, {0, 0, 16, 0});
      HAdd(h, MkEdit(L"code", L"", 160), {0, 0, 16, 0}); HAdd(h, MkButton(L"join", L"加入房间")); VAdd(v, h); }
    VAdd(v, Center(MkText(L"cstatus", L"", 24, kWarn)), {0, 10, 0, 4}, 2);
    VAdd(v, Line(560), {0, 6, 0, 6}, 2);
    VAdd(v, MkButton(L"close", L"关闭", 24), {0, 4, 0, 0}, 2);

    // lobby panel
    g.lobby = Frame({0.5, 0.5}, {0.5, 0.5}, {0, 0}, &v);
    VAdd(v, Heading(L"code", L"房间", 44), {0, 0, 0, 4}, 2);
    VAdd(v, Line(900), {0, 0, 0, 8}, 2);
    VAdd(v, MkText(L"members", L"", 26), {0, 4, 0, 10}, 2);
    {   // tabs 武器 / 杀器 / 外观 ("wtab" / "etab" / "otab"): one card row is shown at a time (same height, fits 1280x720)
        auto tabs = HBox();
        const wchar_t* tab[][2] = {{L"wtab", L"武器"}, {L"etab", L"杀器"}, {L"otab", L"外观"}};
        for (auto& t : tab) {
            auto b = New(L"Button", g.tree); if (!b) break;
            SkinButton(b, nullptr, art::kBloom, {1, 1, 1, 1}, {1, 0.9f, 0.7f, 0.5f});
            auto label = Center(MkText(t[1], 26, kDim), 120);
            if (auto s = Add(b, label)) Call1(s, L"SetPadding", L"InPadding", Margin{16, 2, 16, 2});
            g.btn[t[0]] = {b, label, false}; HAdd(tabs, b, {8, 0, 8, 0});
        }
        VAdd(v, tabs, {0, 4, 0, 6}, 2);
    }
    // card rows: character screen weapon slot (diamond bg + frame, white frame when selected) with the render.
    // ps "w": weapons (DT_WeaponData renders), "e": Phantom Edges (DT_ShaqiWeaponData RenderIcon); card ids ps<i>,
    // one page of kCardsPerPage cards between the start menu arrows (ps "pg-" / ps "pg+"), page text ps "page"
    auto cardRow = [&](const std::wstring& ps, const std::vector<fight::Weapon>& ws, std::wstring (*iconOf)(const std::string&),
                       Cards& cs, const wchar_t* rowId) {
        auto row = VBox(), h = HBox();
        auto pgArrow = [&](const std::wstring& bid, const wchar_t* tex) {
            auto b = New(L"Button", g.tree); SkinButton(b, nullptr, art::kBloom, {1, 1, 1, 1}, {1, 0.9f, 0.7f, 0.5f});
            if (auto s = Add(b, MkImg(tex, 48, 48, kGold))) Call1(s, L"SetPadding", L"InPadding", Margin{6, 40, 6, 40});
            g.btn[bid] = {b, nullptr, false}; HAdd(h, b, {0, 0, 6, 0});
        };
        pgArrow(ps + L"pg-", art::kLeft);
        for (int i = 0; i < int(ws.size()); ++i) {
            auto b = New(L"Button", g.tree); if (!b) break;
            SkinButton(b, nullptr, art::kBloom, {1, 1, 1, 1}, {1, 0.9f, 0.7f, 0.35f});
            auto col = VBox(); auto ov = New(L"Overlay", g.tree);
            auto sb = New(L"SizeBox", g.tree);
            Call1<float>(sb, L"SetWidthOverride", L"InWidthOverride", 132.f); Call1<float>(sb, L"SetHeightOverride", L"InHeightOverride", 132.f);
            auto layer = [&](UObject* w, float pad) {
                auto s = Add(ov, w, L"AddChildToOverlay");
                Call1(s, L"SetPadding", L"InPadding", Margin{pad, pad, pad, pad});
                Call1<uint8_t>(s, L"SetHorizontalAlignment", L"InHorizontalAlignment", 0);  // fill
                Call1<uint8_t>(s, L"SetVerticalAlignment", L"InVerticalAlignment", 0);
            };
            layer(MkImg(art::kCardBg, 132, 132), 0);
            auto pic = MkImg(iconOf(ws[i].id).c_str(), 96, 96);
            auto sc = New(L"ScaleBox", g.tree); Call1<uint8_t>(sc, L"SetStretch", L"InStretch", 2);  // ScaleToFit
            Add(sc, pic); layer(sc, 22);
            layer(MkImg(art::kCardFrame, 132, 132, {1, 1, 1, 0.8f}), 0);
            auto sel = MkImg(art::kCardSel, 132, 132, {1, 0.92f, 0.75f, 1}); layer(sel, 0); Put<uint8_t>(sel, 0xdd, kCollapsed);
            Add(sb, ov);
            VAdd(col, sb, {0, 0, 0, 0}, 2);
            auto name = Center(MkText(ws[i].label, 22, kDim), 132); VAdd(col, name, {0, 4, 0, 0}, 2);
            if (auto s = Add(b, col)) Call1(s, L"SetPadding", L"InPadding", Margin{4, 6, 4, 6});
            HAdd(h, b, {2, 0, 2, 0});
            g.btn[ps + std::to_wstring(i)] = {b, name, false};
            cs.card.push_back(b); cs.frame.push_back(sel); cs.name.push_back(name);
        }
        pgArrow(ps + L"pg+", art::kRight);
        VAdd(row, h, {0, 0, 0, 2}, 2);
        VAdd(row, Center(MkText((ps + L"page").c_str(), L"", 20, kDim)), {0, 0, 0, 8}, 2);
        VAdd(v, row, {0, 0, 0, 0}, 2); g.text[rowId] = row;
    };
    cardRow(L"w", fight::Weapons(), WeaponIcon, k.wc, L"wrow");
    cardRow(L"e", fight::Edges(), EdgeIcon, k.ec, L"erow");
    cardRow(L"o", fight::outfits::List(), OutfitIcon, k.oc, L"orow");
    if (auto e = g.text[L"erow"]) Put<uint8_t>(e, 0xdd, kCollapsed);  // the weapon tab is shown first (UpdateLobby)
    if (auto o = g.text[L"orow"]) Put<uint8_t>(o, 0xdd, kCollapsed);
    {   // map cards (host picks, everyone sees the pick): preview image, name below, selection frame
        VAdd(v, Center(MkText(L"maptitle", L"地图", 26, kDim)), {0, 4, 0, 6}, 2);
        auto h = HBox();
        for (int i = 0; i < arena::MapCount(); ++i) {
            auto& m = arena::Maps()[i];
            auto b = New(L"Button", g.tree); if (!b) break;
            SkinButton(b, nullptr, art::kBloom, {1, 1, 1, 1}, {1, 0.9f, 0.7f, 0.35f});
            auto col = VBox(); auto ov = New(L"Overlay", g.tree);
            auto sb = New(L"SizeBox", g.tree);
            Call1<float>(sb, L"SetWidthOverride", L"InWidthOverride", 256.f); Call1<float>(sb, L"SetHeightOverride", L"InHeightOverride", 144.f);
            auto layer = [&](UObject* w, float pad) {
                auto s = Add(ov, w, L"AddChildToOverlay");
                Call1(s, L"SetPadding", L"InPadding", Margin{pad, pad, pad, pad});
                Call1<uint8_t>(s, L"SetHorizontalAlignment", L"InHorizontalAlignment", 0);  // fill
                Call1<uint8_t>(s, L"SetVerticalAlignment", L"InVerticalAlignment", 0);
            };
            // rectangular card: preview fills it, thin dim outline; the pick gets a gold box selection frame
            layer(MkFileImg(m.preview, 248, 140), 4);
            layer(MkRect(256, 144, 1.5f, {0.75f, 0.7f, 0.6f, 0.55f}), 2);
            auto sel = MkRect(256, 144, 3.f, {0.95f, 0.8f, 0.5f, 1}); layer(sel, 0); Put<uint8_t>(sel, 0xdd, kCollapsed);
            Add(sb, ov);
            VAdd(col, sb, {0, 0, 0, 0}, 2);
            auto name = Center(MkText(m.name, 24, kDim), 256); VAdd(col, name, {0, 4, 0, 0}, 2);
            if (auto s = Add(b, col)) Call1(s, L"SetPadding", L"InPadding", Margin{4, 6, 4, 6});
            HAdd(h, b, {6, 0, 6, 0});
            g.btn[L"m" + std::to_wstring(i)] = {b, name, false};
            k.mapSel.push_back(sel); k.mapName.push_back(name);
        }
        VAdd(v, h, {0, 0, 0, 8}, 2);
        // "map" / "map+" / "map-" step through the maps (test hook / e2e); the cards replace them on screen
        for (auto id : {L"map", L"map+", L"map-"}) { auto w = MkButton(id, L"地图", 22); VAdd(v, w, {0, 0, 0, 0}, 2); Put<uint8_t>(w, 0xdd, kCollapsed); }
    }
    // "weapon" / "edge" cycle to the next card (test hook / e2e); the cards replace them on screen
    { auto w = MkButton(L"weapon", L"武器", 22); VAdd(v, w, {0, 0, 0, 0}, 2); Put<uint8_t>(w, 0xdd, kCollapsed); }
    { auto w = MkButton(L"edge", L"杀器", 22); VAdd(v, w, {0, 0, 0, 0}, 2); Put<uint8_t>(w, 0xdd, kCollapsed); }
    {   // host settings: rows of the settings menu (T_SettingsDefaultBar) with the start menu arrows
        auto rows = VBox();
        auto opt = [&](const wchar_t* id, const wchar_t* label) {
            auto bar = New(L"Border", g.tree);
            MakeBrush(reinterpret_cast<uint8_t*>(bar) + 0x1d0, art::kBar, 3, {1, 1, 1, 1});
            Call1(bar, L"SetPadding", L"InPadding", Margin{40, 2, 40, 2});
            auto h = HBox(); auto l = MkText(label, 26, kInk); Put<float>(l, 0x1dc, 300); HAdd(h, l, {0, 0, 20, 0});
            auto arrow = [&](const std::wstring& bid, const wchar_t* tex) {
                auto b = New(L"Button", g.tree); SkinButton(b, nullptr, art::kBloom, {1, 1, 1, 1}, {1, 0.9f, 0.7f, 0.5f});
                if (auto s = Add(b, MkImg(tex, 40, 40, kGold))) Call1(s, L"SetPadding", L"InPadding", Margin{6, 4, 6, 4});
                g.btn[bid] = {b, nullptr, false}; return b;
            };
            HAdd(h, arrow(std::wstring(id) + L"-", art::kLeft), {0, 0, 4, 0});
            auto b = New(L"Button", g.tree); SkinButton(b, nullptr, art::kBarHi, {1, 1, 1, 1}, {1, 1, 1, 0.6f});
            auto t = Center(MkText(L"", 26, kGold), 220);
            if (auto s = Add(b, t)) Call1(s, L"SetPadding", L"InPadding", Margin{10, 6, 10, 6});
            g.btn[id] = {b, t, false};
            HAdd(h, b, {0, 0, 4, 0});
            HAdd(h, arrow(std::wstring(id) + L"+", art::kRight), {0, 0, 0, 0});
            Add(bar, h);
            VAdd(rows, bar, {0, 3, 0, 3}, 2);
        };
        opt(L"rounds", L"回合数"); opt(L"time", L"回合时限"); opt(L"dmg", L"伤害倍率");
        VAdd(v, rows, {0, 4, 0, 4}, 2); g.text[L"hostrow"] = rows;
    }
    VAdd(v, Center(MkText(L"settings", L"", 24, kDim)), {0, 4, 0, 4}, 2);
    VAdd(v, Line(900), {0, 6, 0, 6}, 2);
    { auto h = HBox(); HAdd(h, MkButton(L"ready", L"准备"), {0, 0, 12, 0}); HAdd(h, MkButton(L"start", L"开始"), {0, 0, 12, 0});
      HAdd(h, MkButton(L"leave", L"离开"), {0, 0, 12, 0}); HAdd(h, MkButton(L"hide", L"隐藏"), {0, 0, 0, 0});
      VAdd(v, h, {0, 4, 0, 4}, 2); }
    VAdd(v, Center(MkText(L"lstatus", L"", 24, kWarn)), {0, 4, 0, 0}, 2);

    // HUD: fighting-game layout. Me top left, opponent top right (bar fills toward the centre plaque),
    // round / timer plaque in the middle. Bars use the game's hp bar textures (bar_hp back, bar_hphud fill).
    g.hud = Canvas(g.canvas, {0, 0}, {1, 0}, {0, 0}, Margin{0, 0, 0, 260});
    auto side = [&](bool right, UObject** pb, const wchar_t* nameId, const wchar_t* pipsId, Color fill) {
        auto col = VBox();
        auto h = HBox();
        auto name = Title(MkText(nameId, L"", 30, kInk), 40);
        auto hpn = MkText(right ? L"opphp" : L"myhp", L"", 24, kDim);
        if (right) { HAdd(h, hpn, {0, 0, 18, 0}); HAdd(h, name, {0, 0, 0, 0}); }
        else { HAdd(h, name, {0, 0, 18, 0}); HAdd(h, hpn, {0, 0, 0, 0}); }
        VAdd(col, h, {6, 0, 6, 4}, right ? 3 : 1);
        auto sb = New(L"SizeBox", g.tree); auto bar = New(L"ProgressBar", g.tree);
        Call1<float>(sb, L"SetWidthOverride", L"InWidthOverride", 640.f);
        Call1<float>(sb, L"SetHeightOverride", L"InHeightOverride", 26.f);
        auto st = reinterpret_cast<uint8_t*>(bar) + 0x180;
        MakeBrush(st + 0x10, art::kHpBack, 3, {0.25f, 0.22f, 0.2f, 0.85f});
        MakeBrush(st + 0xc0, art::kHpFill, 3, {1, 1, 1, 1});
        Put<uint8_t>(bar, 0x3b4, right ? 1 : 0);  // BarFillType: LeftToRight / RightToLeft
        Put(bar, 0x3b8, V2{0, 0});
        Call1(bar, L"SetFillColorAndOpacity", L"InColor", fill);
        Call1<float>(bar, L"SetPercent", L"InPercent", 1.f);
        Add(sb, bar); VAdd(col, sb, {0, 0, 0, 4}, right ? 3 : 1); *pb = bar;
        VAdd(col, MkText(pipsId, L"", 26, kGold), {8, 0, 8, 0}, right ? 3 : 1);
        Canvas(g.hud, {right ? 1. : 0., 0}, {right ? 1. : 0., 0}, {right ? 1. : 0., 0}, Margin{right ? -60.f : 60.f, 40, 0, 0}, col, true);
    };
    side(false, &g.myBar, L"myname", L"mypips", {0.86f, 0.18f, 0.14f, 1});
    side(true, &g.oppBar, L"oppname", L"opppips", {0.86f, 0.18f, 0.14f, 1});
    {   // centre plaque: round over a big timer
        UObject* pv; auto pl = Frame({0.5, 0}, {0.5, 0}, {0, 18}, &pv, {0, 0, 0, 0.45f}, g.hud);
        Call1(pl, L"SetPadding", L"InPadding", Margin{40, 8, 40, 10});
        VAdd(pv, Center(Title(MkText(L"hudround", L"", 24, kDim), 60), 180), {0, 0, 0, 0}, 2);
        VAdd(pv, Center(Title(MkText(L"hud", L"", 48, kInk), 40), 180), {0, 0, 0, 0}, 2);
        k.plaque = pl;
        // under the plaque in load / count: a Phantom Edge pick that cannot be equipped (none in the save's slots)
        UObject* nv; auto nf = Frame({0.5, 0}, {0.5, 0}, {0, 150}, &nv, {0, 0, 0, 0.45f}, g.hud);
        Call1(nf, L"SetPadding", L"InPadding", Margin{20, 4, 20, 6});
        VAdd(nv, MkText(L"hudnote", L"", 22, kWarn), {0, 0, 0, 0}, 2);
        g.text[L"hudnotebox"] = nf; Put<uint8_t>(nf, 0xdd, kCollapsed);
    }
    g.result = Frame({0.5, 0.4}, {0.5, 0.5}, {0, 0}, &v, {0, 0, 0, 0.7f});
    VAdd(v, MkText(L"result", L"", 34, {1, 0.85f, 0.5f, 1}), {0, 0, 0, 0}, 2);
    VAdd(v, MkText(L"result2", L"", 20), {0, 6, 0, 0}, 2);
    // in-match menu (Esc): the game's pause menu is blocked during a match, this replaces it
    g.esc = Frame({0.5, 0.5}, {0.5, 0.5}, {0, 0}, &v);
    VAdd(v, Heading(nullptr, L"多人游戏", 40), {0, 0, 0, 4}, 2);
    VAdd(v, Line(480), {0, 0, 0, 10}, 2);
    VAdd(v, Center(MkText(L"escinfo", L"", 24, kDim), 420), {0, 0, 0, 12}, 2);
    VAdd(v, MkButton(L"resume", L"继续"), {0, 4, 0, 4}, 2);
    VAdd(v, MkButton(L"quit", L"离开比赛"), {0, 4, 0, 4}, 2);
    VAdd(v, Center(MkText(L"eschint", L"按 Esc 返回游戏，离开比赛算作认输", 20, kDim), 420), {0, 10, 0, 0}, 2);
    Vis(g.esc, kCollapsed);
    Vis(g.hud, kHitTestInvisible); Vis(g.result, kHitTestInvisible);
    for (auto w : {g.corner, g.connect, g.lobby, g.hud, g.result}) if (w != g.hud && w != g.result) Vis(w, kCollapsed);
    Vis(g.hud, kCollapsed); Vis(g.result, kCollapsed);

    uex::AddToViewport(g.root, 200);
    uex::Log(L"ui: built root %p tree %p canvas %p (%d buttons)", g.root, g.tree, g.canvas, int(g.btn.size()));
}

bool Valid() { return g.root && g.world == uex::World() && uex::Alive(g.root); }

std::wstring ReadEdit(const wchar_t* id) {  // click time only (leaks the FText ref and FString)
    auto e = g.edit[id]; auto lib = uex::Lib(L"KismetTextLibrary /Script/Engine.Default__KismetTextLibrary");
    if (!e || !lib) return L"";
    Params a(uex::Fn(e, L"GetText")); a.Call(e);
    Params b(uex::Fn(lib, L"Conv_TextToString")); b.Set(L"InText", a.Get<uex::FText>(L"ReturnValue")); b.Call(lib);
    auto s = b.Get<FStr>(L"ReturnValue");
    return s.d && s.n > 1 ? std::wstring(s.d, s.n - 1) : L"";
}
void WriteEdit(const wchar_t* id, const std::wstring& s) {
    auto e = g.edit[id]; if (!e) return;
    Params p(uex::Fn(e, L"SetText")); p.Set(L"InText", uex::Text(s)); p.Call(e);
}

std::wstring ErrText(const std::string& e) {
    static const std::map<std::string, const wchar_t*> m = {
        {"version", L"版本不匹配"}, {"noroom", L"房间不存在"}, {"full", L"房间已满"}, {"started", L"比赛已开始"},
        {"timeout", L"连接超时"}, {"resolve", L"无法解析服务器地址"}, {"server_full", L"服务器已满"},
        {"bad", L"请求格式错误"}, {"password", L"服务器密码错误"}, {"value", L"输入内容不合法"}, {"rate", L"操作太频繁，请稍后再试"},
        {"steam", L"Steam 大厅创建或连接失败"}, {"join", L"无法进入对方的 Steam 大厅"}, {"hostleft", L"房主已离开"}, {"expired", L"上一局已结束，无法重新连接"},
        {"notready", L"还有人没准备"}, {"nothost", L"只有房主可以操作"}, {"players", L"人数不足"}, {"phase", L"当前阶段不能操作"}};
    if (auto it = m.find(e); it != m.end()) return it->second;
    return uex::Wide(e);
}
int WeaponIndex(const std::string& id) { for (int i = 0; i < int(fight::Weapons().size()); ++i) if (id == fight::Weapons()[i].id) return i; return -1; }
std::wstring WeaponLabel(const std::string& id) { int i = WeaponIndex(id); return i >= 0 ? fight::Weapons()[i].label : uex::Wide(id); }
int EdgeIndex(const std::string& id) { auto& es = fight::Edges(); for (int i = 0; i < int(es.size()); ++i) if (id == es[i].id) return i; return -1; }
std::wstring EdgeLabel(const std::string& id) { int i = EdgeIndex(id); return i >= 0 ? fight::Edges()[i].label : uex::Wide(id); }
std::wstring OutfitLabel(const std::string& id) { for (auto& o : fight::outfits::List()) if (o.id == id) return o.label; return uex::Wide(id); }
template <size_t N> int Next(const int (&a)[N], int cur) { for (size_t i = 0; i < N; ++i) if (a[i] > cur) return a[i]; return a[0]; }
template <size_t N> int Prev(const int (&a)[N], int cur) { for (size_t i = N; i-- > 0;) if (a[i] < cur) return a[i]; return a[N - 1]; }
std::wstring TimeLabel(int s) { return s ? std::to_wstring(s) + L" 秒" : L"不限"; }
const net::Member* Find(const net::Room& r, int slot) { for (auto& m : r.members) if (m.slot == slot) return &m; return nullptr; }
std::wstring NameOfSlot(const net::Room& r, int slot) { auto m = Find(r, slot); return m ? uex::Wide(m->name) : L"?"; }

// the cursor / UI input mode is on while the panel or the in-match menu is open (in the world only)
void ApplyCursor() {
    bool want = g.open || g.escMenu, pawn = uex::Me() != nullptr;
    if (want && pawn && !g.uiMode) { uex::UiMode(nullptr); g.uiMode = true; }
    if (!want && g.uiMode) { if (pawn) uex::GameMode(); g.uiMode = false; }
}
void SetOpen(bool on) { g.open = on; ApplyCursor(); }
void SetEsc(bool on) { g.escMenu = on; ApplyCursor(); }

void OnClick(const std::wstring& id) {
    uex::Log(L"ui: click %s", id.c_str());
    net::Room r; bool inRoom = net::GetRoom(r);
    if (id == L"mp") SetOpen(true);
    else if (id == L"mode") { if (!inRoom && net::GetStatus() != net::Status::Connecting) { if (net::GetStatus() == net::Status::Failed) net::Leave(); g.steamMode = !g.steamMode; g.status.clear(); g.failed = false; } }
    else if (id == L"close" || id == L"hide") {
        if (!inRoom && net::GetStatus() == net::Status::Connecting) { net::Leave(); g.status.clear(); }  // closing cancels a create / join / resume
        SetOpen(false);
    }
    else if (id == L"resume") SetEsc(false);
    else if (id == L"quit") { SetEsc(false); net::Leave(); g.status.clear(); }  // mp's Flow takes the player home
    else if (id == L"create" || id == L"join") {
        std::wstring ip = ReadEdit(L"ip"), port = ReadEdit(L"port"), name = ReadEdit(L"name"), code = ReadEdit(L"code"), pw = ReadEdit(L"pw");
        if (ip.empty()) ip = L"127.0.0.1";
        if (name.empty()) name = L"玩家";
        if (name.size() > 24) name.resize(24);
        while (!code.empty() && iswspace(code.back())) code.pop_back();
        while (!code.empty() && iswspace(code.front())) code.erase(0, 1);
        SaveSettings(ip, port, name, pw);
        net::SetPassword(g.steamMode ? "" : uex::Utf8(pw));
        if (g.steamMode) {
            if (!net::SteamAvailable()) { g.status = L"Steam 不可用（请从 Steam 启动游戏并登录）"; g.failed = true; return; }
            net::UseSteam();
        } else net::SetServer(uex::Utf8(ip), _wtoi(port.c_str()));
        if (id == L"create") { net::Create(uex::Utf8(name), 2); g.status = L"正在创建房间..."; g.failed = false; }
        else if (code.empty()) { g.status = L"请输入房间码"; g.failed = true; }
        else { net::Join(uex::Utf8(code), uex::Utf8(name)); g.status = L"正在加入房间 " + code + L"..."; g.failed = false; }
    } else if (!inRoom) return;
    else if (id == L"leave") { net::Leave(); g.status.clear(); }
    else if (id == L"ready") { if (auto m = Find(r, r.you)) net::SetReady(!m->ready); }
    else if (id == L"weapon") {
        auto m = Find(r, r.you); int i = m && net::Now() - g.weaponAt > 2 ? WeaponIndex(m->weapon) : g.weapon; g.weaponAt = net::Now();
        g.weapon = (i + 1) % int(fight::Weapons().size());
        net::SetWeapon(fight::Weapons()[g.weapon].id);
    } else if (id == L"wpg-" || id == L"wpg+") {
        int n = Pages(int(k.wc.card.size())); k.wc.page = (k.wc.page + (id == L"wpg+" ? 1 : n - 1)) % n;
    } else if (id.size() >= 2 && id[0] == L'w' && iswdigit(id[1])) {  // weapon card w<i>
        int i = _wtoi(id.c_str() + 1);
        if (i < 0 || i >= int(fight::Weapons().size())) return;
        g.weapon = i; g.weaponAt = net::Now();
        net::SetWeapon(fight::Weapons()[i].id);
    } else if (id == L"wtab" || id == L"etab" || id == L"otab") {
        k.tab = id == L"etab" ? 1 : id == L"otab" ? 2 : 0;
    } else if (id == L"edge") {
        auto& es = fight::Edges();
        auto m = Find(r, r.you); int i = m && net::Now() - g.edgeAt > 2 ? EdgeIndex(m->edge) : g.edge; g.edgeAt = net::Now();
        g.edge = (i + 1) % int(es.size());
        net::SetEdge(es[g.edge].id); k.tab = 1;
    } else if (id == L"epg-" || id == L"epg+") {
        int n = Pages(int(k.ec.card.size())); k.ec.page = (k.ec.page + (id == L"epg+" ? 1 : n - 1)) % n;
    } else if (id.size() >= 2 && id[0] == L'e' && iswdigit(id[1])) {  // Phantom Edge card e<i>
        int i = _wtoi(id.c_str() + 1);
        if (i < 0 || i >= int(fight::Edges().size()) || i >= int(k.ec.card.size())) return;
        g.edge = i; g.edgeAt = net::Now();
        net::SetEdge(fight::Edges()[i].id);
    } else if (id == L"opg-" || id == L"opg+") {
        int n = Pages(int(k.oc.card.size())); k.oc.page = (k.oc.page + (id == L"opg+" ? 1 : n - 1)) % n;
    } else if (id.size() >= 2 && id[0] == L'o' && iswdigit(id[1])) {  // outfit card o<i>: worn from the next round start
        int i = _wtoi(id.c_str() + 1);
        auto& os = fight::outfits::List();
        if (i < 0 || i >= int(os.size()) || i >= int(k.oc.card.size())) return;
        g.outfit = i; fight::outfits::SetPick(os[i].id);
    } else if (r.you != r.host) return;
    else if (id == L"map" || id == L"map+") net::ConfMap((r.map + 1) % arena::MapCount());
    else if (id == L"map-") net::ConfMap((r.map - 1 + arena::MapCount()) % arena::MapCount());
    else if (id.size() >= 2 && id[0] == L'm' && iswdigit(id[1])) {  // map card m<i>
        int i = _wtoi(id.c_str() + 1);
        if (i >= 0 && i < arena::MapCount()) net::ConfMap(i);
    }
    else if (id == L"rounds" || id == L"rounds+") net::Conf(Next(kRounds, r.rounds), -1, -1, -1);
    else if (id == L"time" || id == L"time+") net::Conf(-1, Next(kTimes, r.timeSec), -1, -1);
    else if (id == L"dmg" || id == L"dmg+") net::Conf(-1, -1, Next(kDmgs, r.dmg), -1);
    else if (id == L"rounds-") net::Conf(Prev(kRounds, r.rounds), -1, -1, -1);
    else if (id == L"time-") net::Conf(-1, Prev(kTimes, r.timeSec), -1, -1);
    else if (id == L"dmg-") net::Conf(-1, -1, Prev(kDmgs, r.dmg), -1);
    else if (id == L"start") net::Start();
}

// Enabled mods (autorun.txt, the loader's list) as "id:ver:flag;..." for the member field "mods".
// flag b = the mod's info file says mp=block (cheats, trainers): nobody may start a match with it on.
std::wstring InfoVal(const std::wstring& path, const wchar_t* key) {
    FILE* f = _wfopen(path.c_str(), L"r, ccs=UTF-8"); if (!f) return L"";
    wchar_t b[1024]; std::wstring k = std::wstring(key) + L"=", out;
    while (fgetws(b, 1023, f)) {
        std::wstring s = b;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
        if (!s.empty() && s[0] == 0xfeff) s.erase(0, 1);
        if (s.rfind(k, 0) == 0) { out = s.substr(k.size()); break; }
    }
    fclose(f); return out;
}
// mp=block mods are unloaded (loader "stop X") while we are in a room and run again after,
// so they never take part in a match and are not reported to the room.
static std::vector<std::wstring> g_blockedMods;
const std::string& LocalMods() {
    static std::string list; static bool done;
    if (done) return list;
    done = true;
    auto h = uex::Host(); if (!h) return list;
    std::wstring dir = h->dir;
    FILE* f = _wfopen((dir + L"autorun.txt").c_str(), L"r, ccs=UTF-8"); if (!f) return list;
    wchar_t b[512];
    while (fgetws(b, 511, f)) {
        std::wstring id = b;
        while (!id.empty() && (id.back() == L'\n' || id.back() == L'\r' || id.back() == L' ' || id.back() == L'\t')) id.pop_back();
        while (!id.empty() && (id[0] == L' ' || id[0] == 0xfeff)) id.erase(0, 1);
        if (id.empty() || id[0] == L'#' || id == L"nosave") continue;  // nosave: test-instance helper
        std::wstring info = dir + id + L".txt";
        if (GetFileAttributesW(info.c_str()) == INVALID_FILE_ATTRIBUTES) info = dir + id + L"\\" + id + L".txt";
        if (InfoVal(info, L"mp") == L"block") { g_blockedMods.push_back(id); continue; }
        std::string e = uex::Utf8(id) + ":" + uex::Utf8(InfoVal(info, L"version")) + ":a";
        for (char& c : e) if (c == ';' || c == ' ') c = '_';
        if (list.size() + e.size() + 1 > 200) break;  // the server's field limit
        if (!list.empty()) list += ';';
        list += e;
    }
    fclose(f);
    return list;
}
static void SuspendBlocked(bool on) {
    static bool suspended;
    LocalMods();
    if (on == suspended || g_blockedMods.empty()) return;
    suspended = on;
    auto h = uex::Host(); if (!h) return;
    FILE* f = _wfopen((std::wstring(h->dir) + L"cmd.txt").c_str(), L"a, ccs=UTF-8"); if (!f) return;
    for (auto& id : g_blockedMods) {
        fwprintf(f, L"\n%ls %ls\n", on ? L"stop" : L"run", id.c_str());
        h->log((std::wstring(on ? L"mp: room, unloading " : L"mp: left room, running ") + id).c_str());
    }
    fclose(f);
}
// "" = fine; else who runs a blocked mod, or whose mod versions differ (a warning only)
std::wstring ModProblem(const net::Room& r, bool* block) {
    *block = false;
    std::map<std::string, std::string> ver;  // id -> first version seen
    std::wstring warn, bad;
    for (auto& m : r.members) {
        size_t a = 0;
        while (a < m.mods.size()) {
            size_t e = m.mods.find(';', a); if (e == std::string::npos) e = m.mods.size();
            std::string it = m.mods.substr(a, e - a); a = e + 1;
            size_t c1 = it.find(':'), c2 = c1 == std::string::npos ? c1 : it.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            std::string id = it.substr(0, c1), v = it.substr(c1 + 1, c2 - c1 - 1);
            if (it.substr(c2 + 1) == "b") { *block = true; bad += L" " + uex::Wide(m.name) + L"（" + uex::Wide(id) + L"）"; }
            auto [p, fresh] = ver.emplace(id, v);
            if (!fresh && p->second != v && warn.find(uex::Wide(id)) == std::wstring::npos) warn += L" " + uex::Wide(id);
        }
    }
    if (!bad.empty()) return L"有人启用了禁止联机的 Mod，不能开始:" + bad;
    if (!warn.empty()) return L"提示: 双方 Mod 版本不同:" + warn;
    return L"";
}

bool AllReady(const net::Room& r) {
    if (r.members.size() < 2) return false;
    for (auto& m : r.members) if (m.slot != r.host && !m.ready) return false;  // the server's rule: every guest ready, connected or not
    return true;
}

// a card row: selection frame and gold name on the pick; the pick's page is shown (also on a remote echo)
void UpdateCards(Cards& cs, int sel, const wchar_t* pageId) {
    int n = int(cs.card.size());
    if (sel != cs.shown) {
        for (int i = 0; i < n && i < int(cs.frame.size()) && i < int(cs.name.size()); ++i) {
            Vis(cs.frame[i], i == sel ? kHitTestInvisible : kCollapsed);
            TextColor(cs.name[i], i == sel ? kGold : kDim);
        }
        cs.shown = sel;
        if (sel >= 0 && sel < n) cs.page = sel / kCardsPerPage;
    }
    if (cs.page != cs.pageShown) {
        for (int i = 0; i < n; ++i) Vis(cs.card[i], i / kCardsPerPage == cs.page ? kVisible : kCollapsed);
        uex::SetText(g.text[pageId], std::to_wstring(cs.page + 1) + L" / " + std::to_wstring(Pages(n)));
        cs.pageShown = cs.page;
    }
}

void UpdateLobby(const net::Room& r) {
    uex::SetText(g.text[L"code"], L"房间码  " + uex::Wide(r.code));
    std::wstring s;
    for (auto& m : r.members) {
        wchar_t b[512];  // name 24 chars + weapon / edge labels (an unknown id is shown raw, up to 200 bytes each)
        std::wstring edge = m.edge.empty() ? L"" : L"  杀器 " + EdgeLabel(m.edge);
        swprintf(b, 512, L"%s%s    %s%s    %s    胜场 %d%s\n", m.slot == r.host ? L"【房主】" : L"",
                 uex::Wide(m.name).c_str(), WeaponLabel(m.weapon).c_str(), edge.c_str(), m.ready ? L"已准备" : L"未准备", m.score,
                 m.slot == r.you ? L"  （你）" : (m.conn ? L"" : L"  （掉线）"));
        s += b;
    }
    if (!s.empty()) s.pop_back();
    uex::SetText(g.text[L"members"], s);
    auto me = Find(r, r.you);
    auto& es = fight::Edges();
    uex::SetText(g.btn[L"ready"].label, me && me->ready ? L"取消准备" : L"准备");
    uex::SetText(g.btn[L"weapon"].label, L"武器: " + WeaponLabel(me ? me->weapon : fight::Weapons()[g.weapon].id) + L"  >");
    uex::SetText(g.btn[L"edge"].label, L"杀器: " + EdgeLabel(me ? me->edge : g.edge < int(es.size()) ? es[g.edge].id : "") + L"  >");
    // cards: the local pick for 2 s after a click (server echo may lag), else the server's
    UpdateCards(k.wc, me && net::Now() - g.weaponAt > 2 ? WeaponIndex(me->weapon) : g.weapon, L"wpage");
    UpdateCards(k.ec, me && net::Now() - g.edgeAt > 2 ? EdgeIndex(me->edge) : g.edge, L"epage");
    UpdateCards(k.oc, g.outfit, L"opage");
    if (k.tab != k.tabShown) {  // one card row at a time
        Vis(g.text[L"wrow"], k.tab == 0 ? kSelfHitTestInvisible : kCollapsed);
        Vis(g.text[L"erow"], k.tab == 1 ? kSelfHitTestInvisible : kCollapsed);
        Vis(g.text[L"orow"], k.tab == 2 ? kSelfHitTestInvisible : kCollapsed);
        TextColor(g.btn[L"wtab"].label, k.tab == 0 ? kGold : kDim);
        TextColor(g.btn[L"etab"].label, k.tab == 1 ? kGold : kDim);
        TextColor(g.btn[L"otab"].label, k.tab == 2 ? kGold : kDim);
        k.tabShown = k.tab;
    }
    int mi = r.map >= 0 && r.map < arena::MapCount() ? r.map : 0;
    if (mi != k.mapShown) {
        for (int i = 0; i < int(k.mapSel.size()); ++i) {
            Vis(k.mapSel[i], i == mi ? kHitTestInvisible : kCollapsed);
            TextColor(k.mapName[i], i == mi ? kGold : kDim);
        }
        k.mapShown = mi;
    }
    uex::SetText(g.btn[L"rounds"].label, std::to_wstring(r.rounds) + L" 回合");
    uex::SetText(g.btn[L"time"].label, TimeLabel(r.timeSec));
    uex::SetText(g.btn[L"dmg"].label, std::to_wstring(r.dmg) + L"%");
    bool host = r.you == r.host;
    Vis(g.text[L"hostrow"], host ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.btn[L"start"].o, host ? kVisible : kCollapsed);
    if (me && me->mods != LocalMods() && net::Now() - g.modsAt > 3) { net::SetMods(LocalMods()); g.modsAt = net::Now(); }
    bool modBlock; std::wstring modNote = ModProblem(r, &modBlock);
    bool canStart = host && AllReady(r) && !modBlock;
    Call1<bool>(g.btn[L"start"].o, L"SetIsEnabled", L"bInIsEnabled", canStart);
    uex::SetText(g.text[L"settings"], L"地图 " + std::wstring(arena::Maps()[mi].name) + L"  回合 " + std::to_wstring(r.rounds) + L"  时限 " + TimeLabel(r.timeSec) +
                 L"  伤害 " + std::to_wstring(r.dmg) + L"%" + (host ? (canStart ? L"" : L"   (全员准备后可开始)") : L"   (房主设置)"));
    std::wstring st = r.msg.empty() ? g.status : uex::Wide(r.msg);
    if (!modNote.empty()) st = modNote;
    if (auto e = net::LastError(); !e.empty()) st = L"错误: " + ErrText(e);
    uex::SetText(g.text[L"lstatus"], st); g.lobbyStatus = st;
}

// round wins as filled / empty diamonds: a best-of-N needs N/2+1
std::wstring Pips(const net::Room& r, int score) {
    int need = r.rounds / 2 + 1; std::wstring p;
    for (int i = 0; i < need; ++i) p += i < score ? L"◆ " : L"◇ ";
    return p;
}
void UpdateFight(const net::Room& r) {
    std::wstring score;
    for (auto& m : r.members) {
        if (!score.empty()) score += L"  :  ";
        score += uex::Wide(m.name) + L" " + std::to_wstring(m.score);
    }
    int left = r.leftMs ? int(r.leftMs - (net::Now() - r.recvAt) * 1000) : -1;
    if (left < 0 && r.leftMs) left = 0;
    auto pct = [](UObject* pb, float f) {
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        if (auto it = g.pct.find(pb); it != g.pct.end() && std::abs(it->second - f) < 0.002f) return;
        g.pct[pb] = f; Call1<float>(pb, L"SetPercent", L"InPercent", f);
    };
    if (auto me = uex::Me()) {
        float mx = uex::Attr(me, uex::attr::HealthMax);
        float f = mx > 0 ? uex::Attr(me, uex::attr::Health) / mx : 0;
        if (fight::LocalDead()) f = 0;
        pct(g.myBar, f);
        uex::SetText(g.text[L"myhp"], std::to_wstring(int(f * 100 + 0.5f)) + L"%");
    }
    if (auto m = Find(r, r.you)) { uex::SetText(g.text[L"myname"], uex::Wide(m->name)); uex::SetText(g.text[L"mypips"], Pips(r, m->score)); }
    for (auto& m : r.members) {
        if (m.slot == r.you) continue;
        net::State st;
        int hp = net::Sample(m.slot, st) ? int(st.hp * 100 + 0.5f) : -1;
        if (hp >= 0) pct(g.oppBar, st.hp);
        uex::SetText(g.text[L"oppname"], uex::Wide(m.name)); uex::SetText(g.text[L"opppips"], Pips(r, m.score));
        uex::SetText(g.text[L"opphp"], hp >= 0 ? std::to_wstring(hp) + L"%" : std::wstring(L"--"));
        break;  // 2 players: the bar shows the one opponent
    }
    // plaque: "第 N 回合" over the time left (∞ without a limit), or the phase
    uex::SetText(g.text[L"hudround"], L"第 " + std::to_wstring(r.round) + L" / " + std::to_wstring(r.rounds) + L" 回合");
    std::wstring h = left >= 0 ? std::to_wstring((left + 999) / 1000) : std::wstring(L"∞");
    if (r.phase == "load") h = L"载入中";
    if (r.phase == "count") h = L"准备 " + std::to_wstring(left >= 0 ? (left + 999) / 1000 : 3);
    if (r.phase == "end" || r.phase == "over") h = L"—";
    uex::SetText(g.text[L"hud"], h);
    TextColor(g.text[L"hud"], r.phase == "fight" && left >= 0 && left < 10000 ? kRed : kInk);
    {   // load / count notes: the Phantom Edge pick needs one in the save's slots (EquipEdge swaps into an
        // occupied one only); a DLC / preorder outfit is worn only when this account owns it
        auto m = Find(r, r.you);
        bool early = r.phase == "load" || r.phase == "count";
        std::wstring note;
        if (early && m && !m->edge.empty() && fight::NoEdgeEquipped()) note = L"你的存档没有装备杀器，所选的「" + EdgeLabel(m->edge) + L"」本局不生效";
        if (early && fight::outfits::PickDenied())
            note += (note.empty() ? L"" : L"\n") + std::wstring(L"未拥有所选外观「") + OutfitLabel(fight::outfits::Pick()) + L"」，本局使用你当前的外观";
        bool on = !note.empty();
        if (on) uex::SetText(g.text[L"hudnote"], note);
        if (on != g.edgeNote) { Vis(g.text[L"hudnotebox"], on ? kHitTestInvisible : kCollapsed); g.edgeNote = on; }
    }
    if (r.phase == "end" || r.phase == "over") {
        std::wstring w = r.winner < 0 ? L"平局" : (r.winner == r.you ? L"你赢了" : NameOfSlot(r, r.winner) + L" 获胜");
        uex::SetText(g.text[L"result"], (r.phase == "over" ? L"比赛结束  " : L"本回合  ") + w);
        uex::SetText(g.text[L"result2"], score + (left >= 0 ? L"    " + std::to_wstring((left + 999) / 1000) + L" 秒后" + (r.phase == "over" ? L"返回房间" : L"下一回合") : L""));
    }
}

void PollButtons() {
    for (auto& [id, b] : g.btn) {
        bool p = CallB(b.o, L"IsPressed");
        if (b.was && !p && CallB(b.o, L"IsHovered")) OnClick(id);
        b.was = p;
    }
}

// ---- test hook ----------------------------------------------------------------------------
bool MenuEntryOk();
void Dump(FILE* f) {
    auto vis = [](UObject* w) { if (!w) return -1; return int(At<uint8_t>(w, 0xdd)); };
    fwprintf(f, L"open=%d uiMode=%d root=%p inViewport=%d sections corner=%d connect=%d lobby=%d hud=%d result=%d\n",
             g.open, g.uiMode, g.root, g.root ? int(CallB(g.root, L"IsInViewport")) : 0,
             vis(g.corner), vis(g.connect), vis(g.lobby), vis(g.hud), vis(g.result));
    for (auto& [id, t] : g.text) {
        fwprintf(f, L"text %s vis=%d\n", id.c_str(), vis(t));
    }
    for (auto& [id, b] : g.btn) fwprintf(f, L"button %s vis=%d enabled=%d\n", id.c_str(), vis(b.o), int(At<uint8_t>(b.o, 0xd9) & 4) ? 1 : 0);
    for (auto& [id, e] : g.edit) fwprintf(f, L"edit %s = '%s'\n", id.c_str(), ReadEdit(id.c_str()).c_str());
    net::Room r;
    if (net::GetRoom(r)) {
        fwprintf(f, L"room code=%S phase=%S you=%d host=%d rounds=%d time=%d dmg=%d round=%d winner=%d map=%d\n", r.code.c_str(), r.phase.c_str(),
                 r.you, r.host, r.rounds, r.timeSec, r.dmg, r.round, r.winner, r.map);
        for (auto& m : r.members) fwprintf(f, L"  m%d %s weapon='%S' edge='%S' ready=%d score=%d\n", m.slot, uex::Wide(m.name).c_str(), m.weapon.c_str(),
                                           m.edge.c_str(), m.ready, m.score), fwprintf(f, L"  m%d mods='%S'\n", m.slot, m.mods.c_str());
        fwprintf(f, L"lobby tab=%d weapon page=%d/%d sel=%d edge page=%d/%d sel=%d (%d Phantom Edges)\n", k.tab, k.wc.page + 1, Pages(int(k.wc.card.size())),
                 k.wc.shown, k.ec.page + 1, Pages(int(k.ec.card.size())), k.ec.shown, int(fight::Edges().size()) - 1);
        fwprintf(f, L"lobby status='%s' edgenote=%d\n", g.lobbyStatus.c_str(), int(g.edgeNote));
        fwprintf(f, L"lobby outfit page=%d/%d sel=%d pick='%S' denied=%d (%d outfits)\n", k.oc.page + 1, Pages(int(k.oc.card.size())), k.oc.shown,
                 fight::outfits::Pick().c_str(), int(fight::outfits::PickDenied()), int(fight::outfits::List().size()) - 1);
    } else fwprintf(f, L"no room, status=%d err=%S\n", int(net::GetStatus()), net::LastError().c_str());
    fwprintf(f, L"ui status='%s' failed=%d\n", g.status.c_str(), int(g.failed));
    fwprintf(f, L"steam mode=%d avail=%d net=%d persona=%s\n", int(g.steamMode), int(net::SteamAvailable()), int(net::SteamMode()),
             uex::Wide(net::SteamPersona()).c_str());
    fwprintf(f, L"menu entry=%d %p\n", int(MenuEntryOk()), g.menuBtn);
}
UObject* FindUserWidget(const std::wstring& clsSub) {  // first live UserWidget whose class name contains clsSub
    auto uw = UmgClass(L"UserWidget"); int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || (o->flags & 0x10) || !IsA(o, uw) || o == g.root) continue;
        if (NameOf(o->cls).find(clsSub) != std::wstring::npos && At<UObject*>(o, 0x2c0)) return o;
    }
    return nullptr;
}

// ---- title menu entry ---------------------------------------------------------------------
// A button of the game's own menu button class goes into WB_StartMenu_V2's button column, just
// above 退出游戏: QuitGame is taken out and added back after ours, keeping its slot settings.
struct ChildSize { float v; uint8_t rule, pad[3]; };
struct VSlot { ChildSize size; Margin pad; uint8_t h, v; };
VSlot ReadSlot(UObject* s) { return {At<ChildSize>(s, 0x38), At<Margin>(s, 0x40), At<uint8_t>(s, 0x50), At<uint8_t>(s, 0x51)}; }
void WriteSlot(UObject* s, const VSlot& r) {
    if (!s) return;
    Call1(s, L"SetSize", L"InSize", r.size); Call1(s, L"SetPadding", L"InPadding", r.pad);
    Call1<uint8_t>(s, L"SetHorizontalAlignment", L"InHorizontalAlignment", r.h);
    Call1<uint8_t>(s, L"SetVerticalAlignment", L"InVerticalAlignment", r.v);
}
bool MenuEntryOk() { return g.menuBtn && uex::Alive(g.menuBtn) && uex::Alive(g.menu) && At<UObject*>(g.menuBtn, 0x30); }
// The hidden input icon inside a menu button still takes width in its row. A button made at runtime
// keeps the class default icon size (32 instead of the menu's 29), which pushes the label right by
// the difference; RefreshSize recomputes it like the menu's own buttons.
void RefreshActionIcon() {
    auto tr = g.menuBtn ? At<UObject*>(g.menuBtn, 0x2c0) : nullptr;
    for (int i = 0, n = NumObjects(); tr && i < n; ++i) { auto x = ObjectAt(i);
        if (x && x->outer == tr && !(x->flags & 0x10) && NameOf(x) == L"InputActionWidget") { Params q(uex::Fn(x, L"RefreshSize")); q.Call(x); return; } }
}
void MenuEntry() {
    if (MenuEntryOk() || g.tick % 30) return;
    g.menuBtn = nullptr;
    auto menu = FindUserWidget(L"WB_StartMenu_V2"); if (!menu) return;
    auto tree = At<UObject*>(menu, 0x2c0); UObject *quit = nullptr, *ng = nullptr;
    for (int i = 0, n = NumObjects(); i < n && !(quit && ng); ++i) {
        auto o = ObjectAt(i);
        if (!o || o->outer != tree || (o->flags & 0x10)) continue;
        auto nm = NameOf(o);
        if (nm == L"QuitGame") quit = o; else if (nm == L"NewGame") ng = o;
    }
    if (!quit || !ng) return;
    auto qs = At<UObject*>(quit, 0x30), ns = At<UObject*>(ng, 0x30);
    auto box = qs ? At<UObject*>(qs, 0x28) : nullptr;
    if (!box || !ns || At<UObject*>(ns, 0x28) != box) return;
    auto btn = uex::CreateWidget(quit->cls); if (!btn) return;
    { Params t(uex::Fn(btn, L"SetButtonText")); t.Set(L"InText", uex::Text(L"多人游戏")); t.Call(btn); }
    // the menu sets these on its own buttons in the designer; a fresh button has the class defaults
    // (UnselectedOpacity 0.6 = the grey text, no text decorator change on select)
    Put<uint8_t>(btn, 0x1948, At<uint8_t>(ng, 0x1948));   // ChangeTextDecoratorOnSelected
    Put<double>(btn, 0x1950, At<double>(ng, 0x1950));     // SelectedOpacity
    Put<double>(btn, 0x1958, At<double>(ng, 0x1958));     // UnselectedOpacity
    VSlot qv = ReadSlot(qs), nv = ReadSlot(ns);
    Call1(box, L"RemoveChild", L"Content", quit);
    WriteSlot(Add(box, btn, L"AddChildToVerticalBox"), nv);
    WriteSlot(Add(box, quit, L"AddChildToVerticalBox"), qv);
    // join the menu's button group (+0x780) so selecting another button deselects ours
    g.menuInGroup = false;
    if (auto grp = At<UObject*>(menu, 0x780)) { Call1(grp, L"AddWidget", L"InWidget", btn); g.menuInGroup = true; } else uex::Log(L"ui: start menu has no button group yet");
    g.menu = menu; g.menuBtn = btn; g.menuFont = nullptr; g.menuWas = false; g.menuFix = 0;
    RefreshActionIcon();
    uex::Log(L"ui: title menu entry added %s", NameOf(btn).c_str());
}
void PollMenuEntry() {
    if (!MenuEntryOk()) return;
    // a button made after the menu opened keeps the faded text of its intro animation: match the
    // others (text opacity 1); its deselect animation fades it again, so keep checking
    { if (auto tb = At<UObject*>(g.menuBtn, 0x1670)) if (At<float>(tb, 0xe0) < 0.99f) Call1<float>(tb, L"SetRenderOpacity", L"InOpacity", 1.f); }
    // the selected look switches the label to a subset of the same typeface that only has the glyphs
    // of the game's own menu ("多" is missing). Keep the full font (the unselected one), size untouched.
    if (auto tb = At<UObject*>(g.menuBtn, 0x1670)) {
        auto fo = At<UObject*>(tb, 0x1e0);
        if (!g.menuFont && fo && !CallB(g.menuBtn, L"GetSelected")) g.menuFont = fo;
        if (g.menuFont && fo && fo != g.menuFont && uex::Alive(g.menuFont)) {
            Params p(uex::Fn(tb, L"SetFont")); uint8_t fi[0x60]; std::memcpy(fi, reinterpret_cast<uint8_t*>(tb) + 0x1e0, sizeof fi);
            *reinterpret_cast<UObject**>(fi) = g.menuFont; p.Set(L"InFontInfo", fi); p.Call(tb);
        }
    }
    // the menu's button group may not exist yet when our button goes in: join it once it does
    if (!g.menuInGroup) if (auto grp = At<UObject*>(g.menu, 0x780)) {
        Call1(grp, L"AddWidget", L"InWidget", g.menuBtn); g.menuInGroup = true; uex::Log(L"ui: title entry joined the button group");
    }
    // stay lit only while hovered or focused (another button got the pick: the group may not tell us)
    if (CallB(g.menuBtn, L"GetSelected") && !CallB(g.menuBtn, L"IsHovered") && !CallB(g.menuBtn, L"HasAnyUserFocus") && !CallB(g.menuBtn, L"HasFocusedDescendants")) {
        Params q(uex::Fn(g.menuBtn, L"ClearSelection")); q.Call(g.menuBtn);
    }
    bool p = CallB(g.menuBtn, L"IsPressed");
    if (g.menuWas && !p && CallB(g.menuBtn, L"IsHovered")) { uex::Log(L"ui: click title entry"); SetOpen(true); }
    g.menuWas = p;
}
void DumpProps(FILE* f, UObject* cls) {
    for (auto c = cls; c && c != UmgClass(L"UserWidget"); c = At<UObject*>(c, kUStruct_Super)) {
        fwprintf(f, L"class %s\n", NameOf(c).c_str());
        for (auto p = At<const uint8_t*>(c, kUStruct_ChildProps); p; p = At<const uint8_t*>(p, kFField_Next))
            fwprintf(f, L"  +%x %s\n", At<int32_t>(p, kProp_Offset), NameOf(*reinterpret_cast<const FName*>(p + kFField_Name)).c_str());
        for (auto fn = At<UObject*>(c, kUStruct_Children); fn; fn = At<UObject*>(fn, kUField_Next))
            fwprintf(f, L"  fn %s\n", NameOf(fn).c_str());
    }
}
// offset of the "Padding" property of a panel slot object (it differs between slot classes); 0 if none
int InnerPadOff(UObject* slot) {
    for (auto c = slot ? slot->cls : nullptr; c; c = At<UObject*>(c, kUStruct_Super))
        for (auto p = At<const uint8_t*>(c, kUStruct_ChildProps); p; p = At<const uint8_t*>(p, kFField_Next))
            if (NameOf(*reinterpret_cast<const FName*>(p + kFField_Name)) == L"Padding") return At<int32_t>(p, kProp_Offset);
    return 0;
}
void DumpTree(FILE* f, UObject* w, int d) {
    if (!w || d > 14) return;
    auto uw = UmgClass(L"UserWidget"), panel = UmgClass(L"PanelWidget"), tb = UmgClass(L"TextBlock");
    std::wstring extra;
    if (IsA(w, tb)) { auto t = At<uint8_t*>(w, 0x1a0) ; (void)t; extra = L" font=" + std::to_wstring(At<float>(w, 0x1e0 + 0x48)); }
    fwprintf(f, L"%*s%s : %s vis=%d%s\n", d * 2, L"", NameOf(w).c_str(), NameOf(w->cls).c_str(), int(At<uint8_t>(w, 0xdd)), extra.c_str());
    if (IsA(w, uw)) { if (auto t = At<UObject*>(w, 0x2c0)) DumpTree(f, At<UObject*>(t, 0x30), d + 1); return; }
    if (IsA(w, panel)) {
        Params c(uex::Fn(w, L"GetChildrenCount")); c.Call(w); int n = c.Get<int32_t>(L"ReturnValue");
        for (int i = 0; i < n; ++i) { Params p(uex::Fn(w, L"GetChildAt")); p.Set<int32_t>(L"Index", i); p.Call(w); DumpTree(f, p.Get<UObject*>(L"ReturnValue"), d + 1); }
    }
}
void TestHook() {
    auto h = uex::Host(); if (!h) return;
    std::wstring dir = h->dir, in = dir + L"ui_cmd.txt";
    FILE* f = _wfopen(in.c_str(), L"r, ccs=UTF-8"); if (!f) return;
    std::vector<std::wstring> lines; wchar_t ln[512];
    while (fgetws(ln, 511, f)) { std::wstring s(ln); while (!s.empty() && (s.back() == 10 || s.back() == 13)) s.pop_back(); if (!s.empty()) lines.push_back(s); }
    fclose(f); _wremove(in.c_str());
    FILE* o = _wfopen((dir + L"ui_test.txt").c_str(), L"a, ccs=UTF-8"); if (!o) return;
    for (auto& s : lines) {
        fwprintf(o, L"> %s\n", s.c_str());
        auto sp = s.find(L' '); std::wstring c = s.substr(0, sp), a = sp == std::wstring::npos ? L"" : s.substr(sp + 1);
        if (c == L"open") SetOpen(true);
        else if (c == L"close") SetOpen(false);
        else if (c == L"click") { if (g.btn.count(a)) OnClick(a); else fwprintf(o, L"no button %s\n", a.c_str()); }
        else if (c == L"set") { auto p = a.find(L' '); WriteEdit(a.substr(0, p).c_str(), p == std::wstring::npos ? L"" : a.substr(p + 1)); }
        else if (c == L"dump") Dump(o);
        else if (c == L"tree") { if (auto w = FindUserWidget(a)) DumpTree(o, w, 0); else fwprintf(o, L"no widget %s\n", a.c_str()); }
        else if (c == L"cls") {  // cls <full class name>: props + template WidgetTree of a BP widget class
            auto k = uex::Class(a.c_str());
            if (!k && a.find(L' ') != std::wstring::npos) k = uex::LoadAsset(a.substr(a.find(L' ') + 1).c_str());
            fwprintf(o, L"class %p\n", k);
            if (k) {
                DumpProps(o, k);
                for (auto p = At<const uint8_t*>(k->cls, kUStruct_ChildProps); p; p = At<const uint8_t*>(p, kFField_Next))
                    fwprintf(o, L" meta +%x %s\n", At<int32_t>(p, kProp_Offset), NameOf(*reinterpret_cast<const FName*>(p + kFField_Name)).c_str());
                auto wt = FindProp(k->cls, L"WidgetTree");
                if (wt.offset >= 0) if (auto t = At<UObject*>(k, wt.offset)) DumpTree(o, At<UObject*>(t, 0x30), 1);
            }
        }
        else if (c == L"sig") {  // sig <full class name>|<fn>
            auto p = a.find(L'|'); auto k = uex::Class(a.substr(0, p).c_str());
            auto fn = k && p != std::wstring::npos ? FindFunction(k, a.substr(p + 1)) : nullptr;
            fwprintf(o, L"fn %p\n", fn);
            if (fn) for (auto q = At<const uint8_t*>(fn, kUStruct_ChildProps); q; q = At<const uint8_t*>(q, kFField_Next))
                fwprintf(o, L"  +%x %d %s\n", At<int32_t>(q, kProp_Offset), At<int32_t>(q, kProp_ElemSize), NameOf(*reinterpret_cast<const FName*>(q + kFField_Name)).c_str());
        }
        else if (c == L"classes") {  // classes <substr>: loaded widget classes / assets whose path contains substr
            auto wbgc = uex::Class(L"Class /Script/UMG.WidgetBlueprintGeneratedClass"); int n = NumObjects();
            for (int i = 0; i < n; ++i) { auto x = ObjectAt(i);
                if (x && x->cls == wbgc && PathOf(x).find(a) != std::wstring::npos) fwprintf(o, L"  %s\n", PathOf(x).c_str()); }
        }
        else if (c == L"live") {  // live: every live UserWidget that is in the viewport or has a parent, with its class
            auto uw = UmgClass(L"UserWidget"); int n = NumObjects();
            for (int i = 0; i < n; ++i) { auto x = ObjectAt(i);
                if (!x || (x->flags & 0x10) || !IsA(x, uw) || x->cls == uw) continue;
                if (a.empty() || NameOf(x->cls).find(a) != std::wstring::npos)
                    fwprintf(o, L"  %s : %s vp=%d vis=%d\n", NameOf(x).c_str(), NameOf(x->cls).c_str(), int(CallB(x, L"IsInViewport")), int(At<uint8_t>(x, 0xdd))); }
        }
        else if (c == L"find") {  // find <class substr>|<path substr>
            auto p = a.find(L'|'); std::wstring cs = a.substr(0, p), ps = p == std::wstring::npos ? L"" : a.substr(p + 1);
            int n = NumObjects(), k = 0;
            for (int i = 0; i < n && k < 300; ++i) { auto x = ObjectAt(i);
                if (!x || !x->cls || NameOf(x->cls).find(cs) == std::wstring::npos) continue;
                auto path = PathOf(x); if (path.find(ps) == std::wstring::npos) continue;
                fwprintf(o, L"  %s : %s\n", path.c_str(), NameOf(x->cls).c_str()); ++k; }
        }
        else if (c == L"stacks") {
            auto sc = uex::Class(L"Class /Script/CommonUI.CommonActivatableWidgetStack"); int n = NumObjects();
            for (int i = 0; i < n; ++i) { auto x = ObjectAt(i);
                if (!x || x->cls != sc || (x->flags & 0x10) || PathOf(x).find(L"Transient") == std::wstring::npos) continue;
                Params p(uex::Fn(x, L"GetActiveWidget")); p.Call(x); auto w = p.Get<UObject*>(L"ReturnValue");
                fwprintf(o, L"  %p %s list=%d active=%s\n", x, NameOf(x).c_str(), At<int32_t>(x, 0x1a8 + 8), w ? (NameOf(w) + L" : " + NameOf(w->cls)).c_str() : L"-"); }
        }
        else if (c == L"fnfind") {  // fnfind <substr>: every UFunction whose name contains substr
            auto fc = uex::Class(L"Class /CoreUObject.Function"); int n = NumObjects(), k = 0;
            for (int i = 0; i < n && k < 200; ++i) { auto x = ObjectAt(i);
                if (!x || !x->cls || NameOf(x->cls).find(L"Function") == std::wstring::npos) continue;
                if (NameOf(x).find(a) == std::wstring::npos) continue;
                fwprintf(o, L"  %s\n", PathOf(x).c_str()); ++k; }
            (void)fc;
        }
        else if (c == L"props") { if (auto w = FindUserWidget(a)) DumpProps(o, w->cls); else fwprintf(o, L"no widget %s\n", a.c_str()); }
        else if (c == L"rebuild") g.root = nullptr;
        else if (c == L"grp") { auto m = FindUserWidget(L"WB_StartMenu_V2"); auto gr = m ? At<UObject*>(m, 0x780) : nullptr; int n = -1;
            if (gr) { Params p(uex::Fn(gr, L"GetButtonCount")); p.Call(gr); n = p.Get<int32_t>(L"ReturnValue"); }
            int idx = -9;
            if (gr && g.menuBtn) { Params q(uex::Fn(gr, L"FindButtonIndex")); q.Set(L"ButtonToFind", g.menuBtn); q.Call(gr); idx = q.Get<int32_t>(L"ReturnValue"); }
            fwprintf(o, L"group %p buttons %d ours at %d\n", gr, n, idx);
            if (gr && !a.empty()) {  // grp <i>: select button i through the group, report ours
                Params s(uex::Fn(gr, L"SelectButtonAtIndex")); s.Set<int32_t>(L"ButtonIndex", _wtoi(a.c_str())); s.Call(gr);
                fwprintf(o, L"selected %d, ours selected=%d\n", _wtoi(a.c_str()), int(CallB(g.menuBtn, L"GetSelected"))); } }
        else if (c == L"mdiff") {  // 4-byte words that differ between NewGame and our entry in 0x15c8..0x1960
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? At<UObject*>(menu, 0x6c8) : nullptr;
            if (ng && g.menuBtn) for (int off = 0x15c8; off < 0x1960; off += 4) {
                uint32_t x = At<uint32_t>(ng, off), y = At<uint32_t>(g.menuBtn, off);
                if (x != y) fwprintf(o, L"  +%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(ng, off), y, At<float>(g.menuBtn, off)); }
            // the text blocks (+0x1670) and their slots: what lays out the label inside the button
            UObject *ta = ng ? At<UObject*>(ng, 0x1670) : nullptr, *tb2 = g.menuBtn ? At<UObject*>(g.menuBtn, 0x1670) : nullptr;
            if (ta && tb2) {
                fwprintf(o, L"  text %s / %s\n", NameOf(ta->cls).c_str(), NameOf(tb2->cls).c_str());
                for (int off = 0x28; off < 0x400; off += 4) { uint32_t x = At<uint32_t>(ta, off), y = At<uint32_t>(tb2, off);
                    if (x != y) fwprintf(o, L"  t+%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(ta, off), y, At<float>(tb2, off)); }
                UObject *sa = At<UObject*>(ta, 0x30), *sb = At<UObject*>(tb2, 0x30);
                if (sa && sb) { fwprintf(o, L"  tslot %s\n", NameOf(sa->cls).c_str());
                    for (int off = 0x28; off < 0x80; off += 4) { uint32_t x = At<uint32_t>(sa, off), y = At<uint32_t>(sb, off);
                        if (x != y) fwprintf(o, L"  s+%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(sa, off), y, At<float>(sb, off)); } }
            }
        }
        else if (c == L"mtree" && g.menuBtn) {  // inner widgets of NewGame vs ours: render offset, opacity, slot padding
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? At<UObject*>(menu, 0x6c8) : nullptr;
            for (UObject* b : { ng, g.menuBtn }) { if (!b) continue; auto tr = At<UObject*>(b, 0x2c0);
                fwprintf(o, L" [%s]\n", NameOf(b).c_str());
                for (int i = 0, n = NumObjects(); tr && i < n; ++i) { auto x = ObjectAt(i);
                    if (!x || x->outer != tr || (x->flags & 0x10)) continue;
                    auto sl = At<UObject*>(x, 0x30); if (!sl) continue;
                    int po = InnerPadOff(sl); Margin pd = po ? At<Margin>(sl, po) : Margin{};
                    struct V2 { double x, y; }; Params q(uex::Fn(x, L"GetDesiredSize")); q.Call(x); auto ds = q.Get<V2>(L"ReturnValue");
                    fwprintf(o, L"  %s:%s vis=%d clip=%d ds=%.1f,%.1f pad=%.1f,%.1f,%.1f,%.1f\n", NameOf(x).c_str(), NameOf(x->cls).c_str(),
                             int(At<uint8_t>(x, 0xdd)), int(At<uint8_t>(x, 0xdc)), ds.x, ds.y, pd.l, pd.t, pd.r, pd.b); } } }
        else if (c == L"miaw" && g.menuBtn) {  // InputActionWidget of NewGame vs ours: differing words
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? At<UObject*>(menu, 0x6c8) : nullptr;
            auto find = [](UObject* b) -> UObject* { auto tr = At<UObject*>(b, 0x2c0);
                for (int i = 0, n = NumObjects(); tr && i < n; ++i) { auto x = ObjectAt(i);
                    if (x && x->outer == tr && !(x->flags & 0x10) && NameOf(x) == L"InputActionWidget") return x; } return nullptr; };
            UObject *ia = ng ? find(ng) : nullptr, *ib = find(g.menuBtn);
            if (ib && !a.empty()) { Params q(uex::Fn(ib, a.c_str())); q.Call(ib); fwprintf(o, L"called %s\n", a.c_str()); }
            else if (ia && ib) { DumpProps(o, ia->cls);
                for (int off = 0x28; off < 0x600; off += 4) { uint32_t x = At<uint32_t>(ia, off), y = At<uint32_t>(ib, off);
                    if (x != y) fwprintf(o, L"  +%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(ia, off), y, At<float>(ib, off)); } } }
        else if ((c == L"mcall" || c == L"ncall") && g.menuBtn) {  // mcall/ncall <fn>: no-arg fn on ours / NewGame
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* t = c == L"mcall" ? g.menuBtn : menu ? At<UObject*>(menu, 0x6c8) : nullptr;
            if (auto f = t ? uex::Fn(t, a.c_str()) : nullptr) { Params q(f); q.Call(t); fwprintf(o, L"called %s\n", a.c_str()); } else fwprintf(o, L"no fn\n"); }
        else if (c == L"mfont" && g.menuBtn) {  // font of the label of NewGame vs ours (object, typeface, size, style classes)
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? At<UObject*>(menu, 0x6c8) : nullptr;
            for (UObject* b : { ng, g.menuBtn }) { auto tb = b ? At<UObject*>(b, 0x1670) : nullptr; if (!tb) continue;
                auto fo = At<UObject*>(tb, 0x1e0); auto st = At<UObject*>(tb, 0x340), ld = At<UObject*>(tb, 0x3a0);
                fwprintf(o, L"  %s sel=%d font=%s face=%s size=%.1f style=%s deco=%s decoOn=%d\n", NameOf(b).c_str(), int(CallB(b, L"GetSelected")),
                         fo ? PathOf(fo).c_str() : L"-", NameOf(*reinterpret_cast<const FName*>(reinterpret_cast<const uint8_t*>(tb) + 0x220)).c_str(), At<float>(tb, 0x228),
                         st ? NameOf(st).c_str() : L"-", ld ? NameOf(ld).c_str() : L"-", int(At<uint8_t>(b, 0x1948))); } }
        else if (c == L"mtext" && g.menuBtn) {  // mtext <text>: relabel our title entry (layout checks)
            Params t(uex::Fn(g.menuBtn, L"SetButtonText")); t.Set(L"InText", uex::Text(a.c_str())); t.Call(g.menuBtn); }
        else if (c == L"mbtn") {  // menu buttons of the title menu: opacity, enabled flags, text opacity
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); auto tree = menu ? At<UObject*>(menu, 0x2c0) : nullptr;
            for (int i = 0, n = NumObjects(); tree && i < n; ++i) { auto x = ObjectAt(i);
                if (!x || (x->flags & 0x10) || x->cls != (g.menuBtn ? g.menuBtn->cls : nullptr)) continue;
                if (x->outer != tree && x != g.menuBtn) continue;
                auto tb = At<UObject*>(x, 0x1670);
                fwprintf(o, L"  %s op=%.2f d9=%02x sel=%.2f unsel=%.2f dis=%d tbop=%.2f\n", NameOf(x).c_str(), At<float>(x, 0xe0), At<uint8_t>(x, 0xd9),
                         At<float>(x, 0x1950), At<float>(x, 0x1958), int(At<uint8_t>(x, 0x1932)), tb ? At<float>(tb, 0xe0) : -1.f);
                // render translation (+0x90) of the button and its text, slot padding, parent class
                auto sl = At<UObject*>(x, 0x30); auto par = sl ? At<UObject*>(sl, 0x28) : nullptr; Margin pd = sl ? At<Margin>(sl, 0x40) : Margin{};
                fwprintf(o, L"     tr=%.1f,%.1f sc=%.2f tbtr=%.1f,%.1f tbsc=%.2f pad=%.0f,%.0f,%.0f,%.0f parent=%s\n", At<double>(x, 0x90), At<double>(x, 0x98), At<double>(x, 0xa0),
                         tb ? At<double>(tb, 0x90) : 0, tb ? At<double>(tb, 0x98) : 0, tb ? At<double>(tb, 0xa0) : 0, pd.l, pd.t, pd.r, pd.b, par ? NameOf(par->cls).c_str() : L"-"); }
        }
        else if (c == L"focus") { if (auto e = g.edit[a]) { Params q(uex::Fn(e, L"SetKeyboardFocus")); q.Call(e); } }  // focus <edit id>
        else if (c == L"foc") {  // who holds focus: our edit boxes, the title menu and its buttons
            fwprintf(o, L"menuOff=%p refocus=%d\n", g.menuOff, g.refocus);
            for (auto& [id, e] : g.edit) fwprintf(o, L"edit %s kb=%d any=%d\n", id.c_str(), int(CallB(e, L"HasKeyboardFocus")), int(CallB(e, L"HasAnyUserFocus")));
            auto uw = UmgClass(L"UserWidget"); int n = NumObjects();
            for (int i = 0; i < n; ++i) { auto x = ObjectAt(i);
                if (!x || (x->flags & 0x10) || !IsA(x, uw) || x->cls == uw || x == g.root) continue;
                bool kb = CallB(x, L"HasKeyboardFocus"), any = CallB(x, L"HasAnyUserFocus"), desc = CallB(x, L"HasFocusedDescendants");
                if (kb || any || desc) fwprintf(o, L"  focus %s : %s kb=%d any=%d desc=%d\n", NameOf(x).c_str(), NameOf(x->cls).c_str(), int(kb), int(any), int(desc)); }
        }
        else if (c == L"esc") SetEsc(a != L"0");  // esc 1|0: the in-match menu (key presses are never simulated)
        else if (c == L"keys") {  // keys: the player's live enhanced-input mappings for the menu keys (and F24)
            struct A { uint8_t* d; int32_t n, m; };
            auto pi = uex::PC() ? At<UObject*>(uex::PC(), 0x428) : nullptr;
            auto m = pi ? *reinterpret_cast<A*>(reinterpret_cast<uint8_t*>(pi) + 0x628) : A{};
            fwprintf(o, L"player input %p, %d mappings\n", pi, m.n);
            for (int i = 0; i < m.n && i < 1024; ++i) {
                auto e = m.d + i * 0x50; auto act = At<UObject*>(e, 0x20); auto key = NameOf(*reinterpret_cast<FName*>(e + 0x28));
                if (key == L"Escape" || key == L"F24" || key == L"M" || key == L"Tab" || key == L"I" || key == L"Gamepad_Special_Right")
                    fwprintf(o, L"  %s -> %s\n", key.c_str(), act ? NameOf(act).c_str() : L"-");
            }
        }
        else if (c == L"hudtest") g.hudTest = a != L"0";  // hudtest 1|0: show the match HUD with a fake room (layout checks)
        else if (c == L"dead") { net::Room r; if (net::GetRoom(r)) net::Dead(r.round, _wtoi(a.c_str())); }
        else if (c == L"console") uex::Console(a.c_str());
        else if (c == L"equip") { fight::Equip(uex::Utf8(a)); fwprintf(o, L"equip %s -> %S key %s\n", a.c_str(), fight::CurrentWeaponPath(uex::Me()).c_str(), fight::AbilityKey(uex::Me()).c_str()); }  // equip <id> mid-match
        else if (c == L"ability") fwprintf(o, L"ability %s -> %d\n", a.c_str(), int(fight::TestAbility(a.c_str())));
        else if (c == L"edges") {  // edges: the Phantom Edge list (card id, ItemId, label, config, icon)
            auto& es = fight::Edges();
            fwprintf(o, L"%d Phantom Edges\n", int(es.size()) - 1);
            for (int i = 0; i < int(es.size()); ++i)
                fwprintf(o, L"  e%d %S %s cfg=%S icon=%S\n", i, es[i].id.c_str(), es[i].label.c_str(), fight::EdgeConfigPath(es[i].id).c_str(),
                         fight::EdgeIconPath(es[i].id).c_str());
        }
        else if (c == L"edge") { fight::EquipEdge(uex::Utf8(a)); fwprintf(o, L"edge %s\n%s\n", a.c_str(), fight::LoadoutReport().c_str()); }  // edge <ItemId>: equip now
        else if (c == L"loadout") fwprintf(o, L"%s\n", fight::LoadoutReport().c_str());
        else if (c == L"restore") { fight::RestoreWeapons(); fwprintf(o, L"restore\n%s\n", fight::LoadoutReport().c_str()); }
        else if (c == L"ghostedge") {  // ghostedge <ItemId>: ghosts show it ("ghostedge" alone: back to the remote's own)
            fight::TestGhostEdge(uex::Utf8(a).c_str());
            fwprintf(o, L"ghostedge %s -> %S\n", a.c_str(), fight::EdgeConfigPath(uex::Utf8(a)).c_str());
        }
        else if (c == L"outfits") { std::wstring t; int n = fight::outfits::Probe(&t); fwprintf(o, L"outfits %d\n%s", n, t.c_str()); }  // the appearance table
        else if (c == L"wear") {  // wear <ItemId>: on the local player now ("wear" alone: its own look)
            fight::outfits::Wear(uex::Me(), a); std::wstring t; fight::outfits::Probe(&t);
            fwprintf(o, L"wear %s event '%S'\n%s\n", a.c_str(), fight::outfits::Event().c_str(), t.substr(0, t.find(L"\ntable")).c_str());
        }
        else if (c == L"unwear") { fight::outfits::Restore(); std::wstring t; fight::outfits::Probe(&t); fwprintf(o, L"unwear\n%s\n", t.substr(0, t.find(L"\ntable")).c_str()); }
    }
    fclose(o);
}
}  // namespace

void KeepEditFocus() {
    // the title menu: the title entry's, else a scan (slow: every 30 ticks, only while the panel is
    // open at the title). Kept as a checked Ref: a freed widget's address gets reused
    bool title = g.open && !uex::Me();
    UObject* menu = uex::Get(g.kefMenu);
    if (title && !menu) {
        menu = MenuEntryOk() ? g.menu : g.tick % 30 == 0 ? FindUserWidget(L"WB_StartMenu_V2") : nullptr;
        if (menu) g.kefMenu = uex::Hold(menu);
    }
    bool want = title && menu;
    if (want && g.menuOff != menu) { g.menuVis = At<uint8_t>(menu, 0xdd); Vis(menu, kHidden); g.menuOff = menu; g.menuOffRef = uex::Hold(menu); }  // a hidden menu cannot hold or take focus
    if (!want && g.menuOff) { if (uex::Get(g.menuOffRef) == g.menuOff) Vis(g.menuOff, g.menuVis); g.menuOff = nullptr; }
    if (!g.open) { g.lastEdit = nullptr; return; }
    UObject* now = nullptr;
    for (auto& [id, e] : g.edit) if (e && CallB(e, L"HasKeyboardFocus")) { now = e; break; }
    if (now) { g.lastEdit = now; return; }
    // focus left our edit box: give it back only if a game widget took it (a click on our own
    // buttons or on empty space is left alone)
    if (g.lastEdit && uex::Alive(g.lastEdit)) {
        bool stolen = (menu && CallB(menu, L"HasFocusedDescendants"));
        if (stolen) { Params q(uex::Fn(g.lastEdit, L"SetKeyboardFocus")); q.Call(g.lastEdit); ++g.refocus; }
        else g.lastEdit = nullptr;
    }
}

void SessionTick(bool match);
void Tick(float) {
    if (!uex::PC() || !uex::World()) return;
    if (!Valid()) {
        static double retryAt;
        if (net::Now() < retryAt) return;
        if (g.root) uex::Log(L"ui: world changed, rebuilding");
        g.uiMode = false;
        try { Build(); }
        catch (...) {  // half built: drop it and try again in a second, not every frame
            auto w = uex::ExcWhere(); uex::Log(L"ui: build exception %s", w.c_str());
            try { if (g.root && uex::Alive(g.root)) uex::RemoveWidget(g.root); } catch (...) {}
            g.root = nullptr;
        }
        if (!g.root) { retryAt = net::Now() + 1; return; }
    }
    if (++g.tick % 30 == 0) TestHook();
    if (!Valid()) return;

    net::Room r; bool inRoom = net::GetRoom(r);
    if (g.hudTest && !inRoom) {  // fake 2-player room in its fight phase
        r.you = 0; r.host = 0; r.phase = "fight"; r.round = 2; r.rounds = 3; r.leftMs = 47000; r.recvAt = net::Now();
        r.members = {{0, "甲A", "", 1}, {1, "乙B", "", 0}};
        inRoom = true;
    }
    auto st = net::GetStatus();
    bool pawn = uex::Me() != nullptr;
    bool match = inRoom && r.phase != "lobby";
    if (inRoom && r.phase != g.lastPhase) {
        if (r.phase == "lobby" && !g.lastPhase.empty()) SetOpen(true);  // back from a match
        g.lastPhase = r.phase;
    }
    SuspendBlocked(inRoom || st == net::Status::Connecting);
    if (!inRoom) {
        // the room was lost mid-match (server gone, host left): the panel shows why
        if (!g.lastPhase.empty() && g.lastPhase != "lobby" && st == net::Status::Failed && !g.open) SetOpen(true);
        g.lastPhase.clear();
    }
    if (match && g.open) SetOpen(false);  // the match owns input; any phase after lobby
    if (st == net::Status::Failed && !g.failed) { g.failed = true; g.status = L"失败: " + ErrText(net::LastError()); }
    if (st == net::Status::InRoom && !g.status.empty() && !g.failed) g.status.clear();
    if (st == net::Status::InRoom) g.failed = false;

    // F7 toggles the panel in the world (works while the game owns input)
    if (pawn && !g.open && !match) {
        auto pc = uex::PC(); Params p(uex::Fn(pc, L"WasInputKeyJustPressed"));
        p.Set(L"Key", FKey{uex::Name(L"F7"), {}}); p.Call(pc);
        if (p.Get<bool>(L"ReturnValue")) SetOpen(true);
    }

    // Esc closes the panel: key state is only read, and only while a window of ours is in front
    {
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        bool esc = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
        if (esc && !g.escWas) {
            if (g.open) OnClick(L"close");
            else if (match) SetEsc(!g.escMenu);  // in-match menu
        }
        g.escWas = esc;
        // gamepad: the menu button (its game action is blocked during a match) toggles it too
        if (match && pawn && !g.open) {
            auto pc = uex::PC(); Params p(uex::Fn(pc, L"WasInputKeyJustPressed"));
            p.Set(L"Key", FKey{uex::Name(L"Gamepad_Special_Right"), {}}); p.Call(pc);
            if (p.Get<bool>(L"ReturnValue")) SetEsc(!g.escMenu);
        }
    }
    if (!match && g.escMenu) SetEsc(false);
    ApplyCursor();  // a pawn that showed up while a menu was open (title -> world, rebuilt UI)
    uex::BlockMenuKeys(match);  // the game's own pause/system menu and map stay shut during a match

    if (!pawn) MenuEntry();
    // the corner button is only a fallback for when the title menu entry could not be added
    Vis(g.corner, !g.open && !pawn && !match && !MenuEntryOk() ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.connect, g.open && !inRoom ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.lobby, g.open && inRoom && !match ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.hud, match ? kHitTestInvisible : kCollapsed);
    Vis(g.result, match && (r.phase == "end" || r.phase == "over") ? kHitTestInvisible : kCollapsed);
    Vis(g.esc, g.escMenu ? kSelfHitTestInvisible : kCollapsed);
    if (g.escMenu) uex::SetText(g.text[L"escinfo"], L"房间 " + uex::Wide(r.code) + L"    第 " + std::to_wstring(r.round) + L" / " + std::to_wstring(r.rounds) + L" 回合");

    if (g.open && !inRoom) {
        std::wstring s = st == net::Status::Connecting ? (g.steamMode ? L"连接中（Steam 找房间最多需要 25 秒）..." : L"连接中...") : g.status;
        uex::SetText(g.text[L"cstatus"], s);
        uex::SetText(g.btn[L"mode"].label, g.steamMode ? L"Steam 联机" : L"中转服务器");
        Vis(g.text[L"iprow"], g.steamMode ? kCollapsed : kSelfHitTestInvisible);
        Vis(g.text[L"portrow"], g.steamMode ? kCollapsed : kSelfHitTestInvisible);
        Vis(g.text[L"pwrow"], g.steamMode ? kCollapsed : kSelfHitTestInvisible);
        Vis(g.text[L"steamrow"], g.steamMode ? kHitTestInvisible : kCollapsed);
        if (g.steamMode) uex::SetText(g.text[L"steaminfo"], net::SteamAvailable()
            ? L"房主创建房间后把房间码发给 Steam 好友，对方输入房间码加入" : L"Steam 不可用");
    }
    if (inRoom && !match && g.open) UpdateLobby(r);
    if (match) UpdateFight(r);
    SessionTick(match);
    KeepEditFocus();
    if (g.open || g.escMenu || !pawn) PollButtons();
    if (!pawn && !g.open) PollMenuEntry();
}

// crash rejoin: during a match <mod dir>mp_session.txt holds the connection, room code and token
// (rewritten every 5 s with the time). A restarted game that finds a fresh one resumes the match.
std::wstring SessionPath() { auto h = uex::Host(); return h ? std::wstring(h->dir) + L"mp_session.txt" : L""; }
void SessionTick(bool match) {
    static bool tried;
    if (!tried) {  // first tick: resume a match the last run was in (crash, killed process)
        tried = true;
        auto p = SessionPath(); FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"r, ccs=UTF-8");
        if (f) {
            std::map<std::wstring, std::wstring> kv; wchar_t line[512];
            while (fgetws(line, 512, f)) {
                std::wstring s = line; while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
                if (auto eq = s.find(L'='); eq != std::wstring::npos) kv[s.substr(0, eq)] = s.substr(eq + 1);
            }
            fclose(f);
            long long age = _time64(nullptr) - _wtoi64(kv[L"t"].c_str());
            uint32_t token = (uint32_t)wcstoul(kv[L"token"].c_str(), nullptr, 10);
            if (token && age >= 0 && age < 110 && !kv[L"code"].empty()) {
                uex::Log(L"ui: resuming room %s (session %llds old)", kv[L"code"].c_str(), age);
                if (kv[L"mode"] == L"steam") { if (net::SteamAvailable()) net::UseSteam(); else token = 0; }
                else net::SetServer(uex::Utf8(kv[L"ip"]), _wtoi(kv[L"port"].c_str()));
                if (token) {
                    net::Resume(uex::Utf8(kv[L"code"]), token, _wtoi(kv[L"slot"].c_str()));
                    g.status = L"正在重新连接上一局..."; g.failed = false; SetOpen(true);
                }
            }
            _wremove(p.c_str());
        }
    }
    std::string code; uint32_t token; int slot;
    if (match && net::Session(code, token, slot)) {
        if (net::Now() - g_sessionAt < 5) return;
        g_sessionAt = net::Now();
        auto p = SessionPath(); FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"w, ccs=UTF-8");
        if (!f) return;
        std::string host; int port = 0; net::GetServer(host, port);
        fwprintf(f, L"mode=%s\nip=%s\nport=%d\ncode=%s\ntoken=%u\nslot=%d\nt=%lld\n", net::SteamMode() ? L"steam" : L"relay",
                 uex::Wide(host).c_str(), port, uex::Wide(code).c_str(), token, slot, (long long)_time64(nullptr));
        fclose(f); g_sessionFile = true;
    } else if (g_sessionFile && !net::Resuming()) {
        _wremove(SessionPath().c_str()); g_sessionFile = false; g_sessionAt = -100;
    }
}

void Shutdown() {
    if (g.menuOff && uex::Get(g.menuOffRef) == g.menuOff) Vis(g.menuOff, g.menuVis);
    if (g_sessionFile) { _wremove(SessionPath().c_str()); g_sessionFile = false; }  // left the room: nothing to resume
    if (MenuEntryOk()) {
        if (auto grp = At<UObject*>(g.menu, 0x780)) Call1(grp, L"RemoveWidget", L"InWidget", g.menuBtn);
        uex::RemoveWidget(g.menuBtn);
    }
    if (Valid()) uex::RemoveWidget(g.root);
    if (g.uiMode && uex::Me()) uex::GameMode();
    uex::BlockMenuKeys(false);
    g = G{};
}

bool InMenu() { return g.open && g.root != nullptr; }
}  // namespace ui

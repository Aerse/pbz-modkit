// ui: multiplayer menus built at runtime from plain UMG widgets (UserWidget + WidgetTree, children
// created with GameplayStatics::SpawnObject). One root UserWidget holds every section:
//   corner button "多人游戏" (title screen, no pawn; F7 opens the panel in the world), connect panel,
//   lobby panel, HUD (hp bars, round, score, timer), big count, result.
// Buttons are polled (IsPressed falling edge while hovered / focused). Text is set through
// uex::SetText (FText cached per string, skips unchanged). Edit boxes are only read on a click
// (GetText/Conv_TextToString leak a little, never per frame).
// Esc (our window in front, read with GetAsyncKeyState, never sent) closes the panel.
// Test hook: <loader root>\ui_cmd.txt lines "open" "close" "set <id> <text>" "click <id>" "dump";
// results go to ui_test.txt. Checked every 30 ticks, file deleted after reading.
// Room settings (panel "cfg"; opened by "cfg" in the lobby (host) / "hostcfg" on the connect panel (kept for
// the next create), closed by "cfgback"; pages "pgbasic" / "pgmore"). Every option <id> has the value
// button <id> (click = next) and the arrows <id>- / <id>+; "set <id> <value>" sets it directly.
//   基本: gmode (duel|ffa|kill) players (2..Room::cap) rounds time (seconds, 0 = none) mapsel (map index | rand)
//         dmg (10..500) hp (50..500) preset (0..2 | 标准|快速|硬核)
//   更多: sd sr ult (0..300) heal (0..2) cimu (0|1) acc (0..9) edgeon (0|1) wrand (0|1) stun (0 | 3..10) cdmg (0|1)
//         respawn kills (kill mode only); weapon bans wb<i> (toggle, bit i = fight::Weapons()[i + 1]),
//         "wbclear"; "set wban <mask>". Texts: cfgtitle cfgsum cfgnote, rows row_<id>.
// Lobby: "cfg" button, member texts mem0..mem7, "cardnote" (bans / random weapon), "settings" (summary line).
// HUD: "hudtop" (score / survivors / my kills and deaths), "rankbox" (ranktitle, rank0..rank7), head bars
// (dump "head" lines), result "resultlist". "hudtest duel|ffa|kill|0" shows the HUD with a fake room.
// dump adds: cfg room/saved (every setting + rules text + summary), cfg open/page/note/violation, hud, head, result.
#include "ui.h"
#include <windows.h>
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include "uex.h"
#include "../net/net.h"
#include "../net/rules.h"
#include "../fight/houserules.h"
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
constexpr int kMaxRows = net::kMaxPlayers;  // lobby member texts
constexpr int kHeads = net::kMaxPlayers - 1;  // head hp bars (every opponent)

// reflected fields (by name; the number is this build's offset, used only if the field is gone)
constexpr const wchar_t* kWidget = L"Class /Script/UMG.Widget";
uex::Prop kSlot{kWidget, L"Slot", 0x30}, kVis{kWidget, L"Visibility", 0xdd}, kEnabled{kWidget, L"bIsEnabled", 0xd9, 0x4},
    kClip{kWidget, L"Clipping", 0xdc}, kOpacity{kWidget, L"RenderOpacity", 0xe0}, kXform{kWidget, L"RenderTransform", 0x90};
uex::Prop kXfTrans{L"ScriptStruct /Script/UMG.WidgetTransform", L"Translation", 0},
    kXfScale{L"ScriptStruct /Script/UMG.WidgetTransform", L"Scale", 0x10};
uex::Prop kJustify{L"Class /Script/UMG.TextLayoutWidget", L"Justification", 0x17b};
constexpr const wchar_t* kTb = L"Class /Script/UMG.TextBlock";
uex::Prop kTbColor{kTb, L"ColorAndOpacity", 0x1b8}, kTbMinW{kTb, L"MinDesiredWidth", 0x1dc}, kTbFont{kTb, L"Font", 0x1e0},
    kTbShadow{kTb, L"ShadowOffset", 0x2f0}, kTbShadowColor{kTb, L"ShadowColorAndOpacity", 0x300};
uex::Prop kColorRule{L"ScriptStruct /Script/SlateCore.SlateColor", L"ColorUseRule", 0x10};
constexpr const wchar_t* kFont = L"ScriptStruct /Script/SlateCore.SlateFontInfo";
uex::Prop kFontOutline{kFont, L"OutlineSettings", 0x10}, kFontFace{kFont, L"TypefaceFontName", 0x40},
    kFontSize{kFont, L"Size", 0x48}, kFontSpacing{kFont, L"LetterSpacing", 0x4c};
uex::Prop kImgBrush{L"Class /Script/UMG.Image", L"Brush", 0x180}, kImgColor{L"Class /Script/UMG.Image", L"ColorAndOpacity", 0x240};
constexpr const wchar_t* kBrush = L"ScriptStruct /Script/SlateCore.SlateBrush";
uex::Prop kBrushTint{kBrush, L"TintColor", 0x8}, kBrushDrawAs{kBrush, L"DrawAs", 0x1c}, kBrushImgSize{kBrush, L"ImageSize", 0x20},
    kBrushMargin{kBrush, L"Margin", 0x28};
uex::Prop kBtnStyle{L"Class /Script/UMG.Button", L"WidgetStyle", 0x190}, kBtnBg{L"Class /Script/UMG.Button", L"BackgroundColor", 0x530};
constexpr const wchar_t* kBs = L"ScriptStruct /Script/SlateCore.ButtonStyle";
uex::Prop kBsNormal{kBs, L"Normal", 0x10}, kBsHovered{kBs, L"Hovered", 0xc0}, kBsPressed{kBs, L"Pressed", 0x170}, kBsDisabled{kBs, L"Disabled", 0x220};
constexpr const wchar_t* kPb = L"Class /Script/UMG.ProgressBar";
uex::Prop kBarStyle{kPb, L"WidgetStyle", 0x180}, kBarFill{kPb, L"BarFillType", 0x3b4}, kBarPad{kPb, L"BorderPadding", 0x3b8};
uex::Prop kPbBack{L"ScriptStruct /Script/SlateCore.ProgressBarStyle", L"BackgroundImage", 0x10},
    kPbFill{L"ScriptStruct /Script/SlateCore.ProgressBarStyle", L"FillImage", 0xc0};
uex::Prop kWidgetTree{L"Class /Script/UMG.UserWidget", L"WidgetTree", 0x2c0}, kRootWidget{L"Class /Script/UMG.WidgetTree", L"RootWidget", 0x30};
uex::Prop kEditStyle{L"Class /Script/UMG.EditableTextBox", L"WidgetStyle", 0x1a0},
    kEditMinW{L"Class /Script/UMG.EditableTextBox", L"MinimumDesiredWidth", 0xe44};
constexpr const wchar_t* kEs = L"ScriptStruct /Script/SlateCore.EditableTextBoxStyle";
uex::Prop kEsNormal{kEs, L"BackgroundImageNormal", 0x10}, kEsHovered{kEs, L"BackgroundImageHovered", 0xc0},
    kEsFocused{kEs, L"BackgroundImageFocused", 0x170}, kEsPadding{kEs, L"Padding", 0x2d0}, kEsText{kEs, L"Textstyle", 0x2e0},
    kEsFg{kEs, L"ForegroundColor", 0x5c0}, kEsFocusFg{kEs, L"FocusedForegroundColor", 0x5fc};
uex::Prop kTsFont{L"ScriptStruct /Script/SlateCore.TextBlockStyle", L"Font", 0x8};
uex::Prop kBorderBg{L"Class /Script/UMG.Border", L"Background", 0x1d0};
uex::Prop kSlotParent{L"Class /Script/UMG.PanelSlot", L"Parent", 0x28};
constexpr const wchar_t* kVs = L"Class /Script/UMG.VerticalBoxSlot";
uex::Prop kVsSize{kVs, L"Size", 0x38}, kVsPad{kVs, L"Padding", 0x40}, kVsH{kVs, L"HorizontalAlignment", 0x50}, kVsV{kVs, L"VerticalAlignment", 0x51};
uex::Prop kBtnText{L"Class /Script/ProjectPBZ.PBZButtonBase", L"ButtonTextBlock", 0x1670};
uex::Prop kCtStyle{L"Class /Script/CommonUI.CommonTextBlock", L"Style", 0x340};
uex::Prop kLocDeco{L"Class /Script/ProjectPBZ.PBZCommonLocText", L"LanguageTextDecoratorClass", 0x3a0};
uex::Prop kWidgetList{L"Class /Script/CommonUI.CommonActivatableWidgetContainerBase", L"WidgetList", 0x1a8};
uex::Prop kPlayerInput{L"Class /Script/Engine.PlayerController", L"PlayerInput", 0x428};
uex::Prop kActionMappings{L"Class /Script/EnhancedInput.EnhancedPlayerInput", L"EnhancedActionMappings", 0x628};
constexpr const wchar_t* kMapping = L"ScriptStruct /Script/EnhancedInput.EnhancedActionKeyMapping";
uex::Prop kMapAction{kMapping, L"Action", 0x20}, kMapKey{kMapping, L"Key", 0x28};
int MappingSize() { return uex::StructSize(kMapping, 0x50); }
// FSlateFontInfo: the CompositeFont shared pointer (native, not a property) sits between OutlineSettings and TypefaceFontName
int FontShared() { return kFontOutline + uex::StructSize(L"ScriptStruct /Script/SlateCore.FontOutlineSettings", 0x20); }
// fixed buffer sizes (FSlateFontInfo, FSlateBrush in this build): a runtime size cannot size an array
constexpr int kFontInfoBytes = 0x60, kBrushBytes = 0xb0;
constexpr int kArrNum = 0x8;  // TArray: data +0, num +8
// Blueprint variables, by name on the object's own class (blueprint classes load late and have no
// native path): WB_StartMenu_V2 ButtonGroup / NewGame, and on its buttons (WB_PBZMenuButton_V2)
// ChangeTextDecoratorOnSelected (bool), SelectedOpacity / UnselectedOpacity (BP float = double), IsDisabled (bool)
template <class T> T BpVar(UObject* o, const wchar_t* n) { auto p = o ? uex::Field<T>(o, n) : nullptr; return p ? *p : T{}; }
template <class T> void BpSet(UObject* o, const wchar_t* n, T v) { if (auto p = o ? uex::Field<T>(o, n) : nullptr) *p = v; }
UObject* MenuGroup(UObject* menu) { return BpVar<UObject*>(menu, L"ButtonGroup"); }
UObject* MenuNewGame(UObject* menu) { return BpVar<UObject*>(menu, L"NewGame"); }

const int kRounds[] = {1, 3, 5, 7, 9};
const int kTimes[] = {0, 60, 90, 120, 180, 300};
const int kStuns[] = {0, 3, 4, 5, 6, 7, 8, 9, 10};
const int kRespawns[] = {3, 5, 8, 10, 15};
const int kKills[] = {0, 5, 10, 15, 20, 30};

// palette
constexpr Color kInk{0.93f, 0.9f, 0.84f, 1}, kDim{0.62f, 0.6f, 0.56f, 1}, kGold{0.98f, 0.78f, 0.38f, 1},
    kRed{0.92f, 0.3f, 0.25f, 1}, kGreen{0.45f, 0.85f, 0.45f, 1}, kWarn{1, 0.68f, 0.35f, 1};

struct Head {  // an opponent's name + hp bar over its head (fight::GhostAnchor projected to the screen)
    UObject *box = nullptr, *name = nullptr, *bar = nullptr;
    int slot = -1; bool shown = false; double x = 0, y = 0; float hp = -1;
};
struct G {
    UObject* world = nullptr;
    Head head[kHeads];
    std::wstring hudTop, rankText, resultList;  // HUD state as last shown (test hook dump)
    UObject* root = nullptr;  // UserWidget
    UObject* tree = nullptr;
    UObject *canvas = nullptr, *promo = nullptr, *corner = nullptr, *connect = nullptr, *lobby = nullptr, *hud = nullptr, *result = nullptr,
            *count = nullptr;
    UObject *myBar = nullptr, *oppBar = nullptr;
    UObject* rows[kMaxRows]{};  // lobby member texts mem<i>
    UObject *cfg = nullptr, *heads = nullptr;  // room settings panel, head bar canvas
    bool cfgOpen = false; int cfgPage = 0;
    std::wstring cfgNote;       // last refusal on the settings panel
    std::wstring violation; double violAt = -10, unreadyAt = -10;  // houserules::Violation, checked once a second
    std::map<std::wstring, Btn> btn;
    std::map<std::wstring, UObject*> edit, text;
    std::map<UObject*, float> pct;    // last SetPercent
    std::map<UObject*, Color> color;  // last SetColorAndOpacity
    std::map<UObject*, uint8_t> vis;  // last SetVisibility
    uint8_t font[kFontInfoBytes]{}; bool haveFont = false;
    bool open = false, uiMode = false, hudTest = false; std::string hudMode = "duel";
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

// ---- files ----------------------------------------------------------------------------------
// Mod-owned files (maps\, mp_settings.txt, mp_session.txt) live in the mod folder PbzRun got
// (<root>\mp\, or the root itself for a flat install). Loader / test files (autorun.txt, cmd.txt,
// ui_cmd.txt, ui_test.txt) stay in the root (host->dir).
static std::wstring g_modDir;
std::wstring RootDir() { auto h = uex::Host(); return h && h->dir ? h->dir : L""; }
std::wstring ModDir() { return g_modDir.empty() ? RootDir() : g_modDir; }
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
// where to read a mod file from: the mod folder, else the root if an old flat install left it there
std::wstring ModFileIn(const std::wstring& name) {
    std::wstring p = ModDir() + name, old = RootDir() + name;
    return Exists(p) || p == old || !Exists(old) ? p : old;
}

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

// copy the font (not the CompositeFont shared pointer after OutlineSettings) of a live game TextBlock
void FindFont() {
    auto tb = UmgClass(L"TextBlock"); if (!tb) return;
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || (o->flags & 0x10) || !IsA(o, tb)) continue;
        auto f = kTbFont.Of<UObject*>(o);  // FontObject
        if (!f || NameOf(f).find(L"Roboto") != std::wstring::npos) continue;
        memcpy(g.font, reinterpret_cast<uint8_t*>(o) + kTbFont, kFontInfoBytes);
        memset(g.font + FontShared(), 0, kFontFace - FontShared());
        g.haveFont = true;
        uex::Log(L"ui: font %s from %s", PathOf(f).c_str(), PathOf(o).c_str());
        return;
    }
    uex::Log(L"ui: no game font found, using default");
}
void ApplyFont(uint8_t* fi, float size) {  // fi: an FSlateFontInfo inside a widget not built yet
    if (g.haveFont) { memcpy(fi, g.font, FontShared()); memcpy(fi + kFontFace, g.font + kFontFace, kFontInfoBytes - kFontFace); }
    *reinterpret_cast<float*>(fi + kFontSize) = size;
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
    uint8_t blank[kBrushBytes]{};
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
    if (!k.scratch) { k.scratch = New(L"Image", g.tree); if (!k.scratch) return; memcpy(k.blank, reinterpret_cast<uint8_t*>(k.scratch) + kImgBrush, kBrushBytes); }
    memcpy(reinterpret_cast<uint8_t*>(k.scratch) + kImgBrush, k.blank, kBrushBytes);
    if (t) {  // bMatchSize: ImageSize = texture size unless w/h given
        Params p(uex::Fn(k.scratch, L"SetBrushFromTexture")); p.Set(L"Texture", t); p.Set<bool>(L"bMatchSize", true); p.Call(k.scratch);
    }
    memcpy(out, reinterpret_cast<uint8_t*>(k.scratch) + kImgBrush, kBrushBytes);
    out[kBrushDrawAs] = drawAs; *reinterpret_cast<Color*>(out + kBrushTint) = tint; out[kBrushTint + kColorRule] = 0;
    *reinterpret_cast<Margin*>(out + kBrushMargin) = m;
    if (w > 0) { reinterpret_cast<float*>(out + kBrushImgSize)[0] = w; reinterpret_cast<float*>(out + kBrushImgSize)[1] = h; }
}
void MakeBrush(uint8_t* out, const wchar_t* tex, uint8_t drawAs, Color tint, Margin m = {}, float w = 0, float h = 0) {
    auto t = tex ? uex::LoadAsset(tex) : nullptr;
    if (tex && !t) uex::Log(L"ui: no texture %s", tex);
    MakeBrushTex(out, t, drawAs, tint, m, w, h);
}
// a PNG shipped in the mod folder (<mod dir>maps\<file>, else <root>maps\<file> from an old flat
// install) loaded as a transient texture; the brush keeps it referenced
UObject* MkFileImg(const std::wstring& file, float w, float h) {
    struct FS { const wchar_t* d; int32_t n, m; };
    std::wstring path = ModFileIn(L"maps\\" + file);
    UObject* t = nullptr;
    if (auto lib = uex::Lib(L"KismetRenderingLibrary /Script/Engine.Default__KismetRenderingLibrary")) {
        Params p(uex::Fn(lib, L"ImportFileAsTexture2D"));
        p.Set(L"WorldContextObject", uex::World());
        p.Set(L"Filename", FS{path.c_str(), int32_t(path.size() + 1), int32_t(path.size() + 1)});
        p.Call(lib); t = p.Get<UObject*>(L"ReturnValue");
    }
    if (!t) uex::Log(L"ui: no preview %s", path.c_str());
    auto i = New(L"Image", g.tree); if (!i) return nullptr;
    MakeBrushTex(reinterpret_cast<uint8_t*>(i) + kImgBrush, t, 3, {1, 1, 1, 1}, {}, w, h);
    Put(i, kImgColor, Color{1, 1, 1, 1}); Put<uint8_t>(i, kVis, kHitTestInvisible);
    return i;
}
// plain rectangular outline (Border draw, no texture): t px thick on a w x h box
UObject* MkRect(float w, float h, float t, Color c) {  // overlay of four solid bars along the edges
    auto ov = New(L"Overlay", g.tree); if (!ov) return nullptr;
    struct E { float w, h; uint8_t ha, va; } es[4] = {{w, t, 0, 1}, {w, t, 0, 3}, {t, h, 1, 0}, {t, h, 3, 0}};
    for (auto& e : es) {
        auto i = New(L"Image", g.tree); if (!i) continue;
        MakeBrush(reinterpret_cast<uint8_t*>(i) + kImgBrush, L"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture", 3, {1, 1, 1, 1}, {}, e.w, e.h);
        Put(i, kImgColor, c); Put<uint8_t>(i, kVis, kHitTestInvisible);
        auto s = Add(ov, i, L"AddChildToOverlay");
        Call1<uint8_t>(s, L"SetHorizontalAlignment", L"InHorizontalAlignment", e.ha);
        Call1<uint8_t>(s, L"SetVerticalAlignment", L"InVerticalAlignment", e.va);
    }
    Put<uint8_t>(ov, kVis, kHitTestInvisible);
    return ov;
}
UObject* MkImg(const wchar_t* tex, float w, float h, Color tint = {1, 1, 1, 1}) {
    auto i = New(L"Image", g.tree); if (!i) return nullptr;
    MakeBrush(reinterpret_cast<uint8_t*>(i) + kImgBrush, tex, 3, {1, 1, 1, 1}, {}, w, h);
    Put(i, kImgColor, tint); Put<uint8_t>(i, kVis, kHitTestInvisible);
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
    ApplyFont(reinterpret_cast<uint8_t*>(t) + kTbFont, size);
    Put(t, kTbColor, c); Put<uint8_t>(t, kTbColor + kColorRule, 0);
    Put(t, kTbShadow, V2{1.5, 1.5}); Put(t, kTbShadowColor, Color{0, 0, 0, 0.85f});  // drop shadow, like the game's hud text
    Put<uint8_t>(t, kVis, kHitTestInvisible);
    uex::SetText(t, s);
    return t;
}
UObject* Title(UObject* t, int spacing = 60) {  // the confirm dialog / button typeface
    if (t && k.titleFont) { Put(t, kTbFont, k.titleFont); Put<int32_t>(t, kTbFont + kFontSpacing, spacing); }
    return t;
}
UObject* Center(UObject* t, float minW = 0) { if (t) { Put<uint8_t>(t, kJustify, 1); if (minW > 0) Put<float>(t, kTbMinW, minW); } return t; }
UObject* MkText(const wchar_t* id, const std::wstring& s, float size, Color c = {0.92f, 0.9f, 0.85f, 1}) {
    auto t = MkText(s, size, c); g.text[id] = t; return t;
}
// Button skin: WidgetStyle (ButtonStyle) brushes Normal, Hovered, Pressed, Disabled; BackgroundColor
void SkinButton(UObject* b, const wchar_t* normal, const wchar_t* hover, Color nTint = {1, 1, 1, 1}, Color hTint = {1, 1, 1, 1}) {
    auto p = reinterpret_cast<uint8_t*>(b) + kBtnStyle;
    MakeBrush(p + kBsNormal, normal, normal ? 3 : 0, nTint);
    MakeBrush(p + kBsHovered, hover, 3, hTint);
    MakeBrush(p + kBsPressed, hover, 3, {hTint.r * 0.8f, hTint.g * 0.8f, hTint.b * 0.8f, hTint.a});
    MakeBrush(p + kBsDisabled, normal, normal ? 3 : 0, {nTint.r, nTint.g, nTint.b, nTint.a * 0.45f});
    Put(b, kBtnBg, Color{1, 1, 1, 1});
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
    // WidgetStyle (EditableTextBoxStyle): text style font, brushes Normal/Hovered/Focused, Padding,
    // ForegroundColor, FocusedForegroundColor (SlateColor)
    auto p = reinterpret_cast<uint8_t*>(e) + kEditStyle;
    ApplyFont(p + kEsText + kTsFont, 24);
    Put<float>(e, kEditMinW, width);
    MakeBrush(p + kEsNormal, art::kSoft, 1, {1, 1, 1, 0.9f}, {0.3f, 0.3f, 0.3f, 0.3f});
    MakeBrush(p + kEsHovered, art::kSoft, 1, {1.15f, 1.1f, 1.f, 1}, {0.3f, 0.3f, 0.3f, 0.3f});
    MakeBrush(p + kEsFocused, art::kSoft, 1, {1.3f, 1.2f, 1.05f, 1}, {0.3f, 0.3f, 0.3f, 0.3f});
    Put(e, kEditStyle + kEsPadding, Margin{14, 8, 14, 8});
    for (int off : {int(kEsFg), int(kEsFocusFg)}) { Put(e, kEditStyle + off, kInk); Put<uint8_t>(e, kEditStyle + off + kColorRule, 0); }
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
    auto h = HBox(); auto l = MkText(label, 26, kDim); Put<float>(l, kTbMinW, 130);
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
    if (b) MakeBrush(reinterpret_cast<uint8_t*>(b) + kBorderBg, ink ? art::kPanel : art::kSoft, ink ? 3 : 1, {1, 1, 1, 1},
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

static std::wstring g_conn[4];  // ip, port, name, pw as last saved (a settings change rewrites the file with them)
// ---- room settings ------------------------------------------------------------------------
// Cfg is everything the host sets; every change goes out as one net::Conf(ConfArgs) with the rules
// text from rules::Format. The host's last settings are kept in mp_settings.txt (room= / rules=)
// and sent once after a create.
struct Cfg {
    std::string mode = "duel";
    int max = 2, rounds = 3, time = 120, map = 0, dmg = 100, respawn = 5, kills = 10;
    bool rand = false;  // maps = every map
    rules::Rules rr;
};
static Cfg g_saved;           // the host's last settings (mp_settings.txt), also edited before a create
static bool g_applyRoom;      // a create is pending: send g_saved once we host the room
struct Preset { const wchar_t* name; int rounds, time, dmg, hp, sd, sr, ult, heal, cimu, acc, stun, cdmg; };
const Preset kPresets[] = {
    {L"标准", 3, 120, 100, 300, 100, 100, 100, 2, 0, 9, 6, 1},
    {L"快速", 3, 60, 150, 150, 100, 150, 150, 1, 0, 9, 5, 1},
    {L"硬核", 5, 90, 100, 100, 100, 100, 100, 0, 1, 3, 0, 0}};
int PresetOf(const Cfg& c) {
    for (int i = 0; i < 3; ++i) {
        auto& p = kPresets[i]; auto& R = c.rr;
        if (c.rounds == p.rounds && c.time == p.time && c.dmg == p.dmg && R.hp == p.hp && R.sd == p.sd && R.sr == p.sr && R.ult == p.ult &&
            R.heal == p.heal && R.cimu == p.cimu && R.acc == p.acc && R.stun == p.stun && R.cdmg == p.cdmg) return i;
    }
    return -1;
}
void ApplyPreset(Cfg& c, int i) {
    auto& p = kPresets[i]; auto& R = c.rr;
    c.rounds = p.rounds; c.time = p.time; c.dmg = p.dmg; R.hp = p.hp; R.sd = p.sd; R.sr = p.sr; R.ult = p.ult;
    R.heal = p.heal; R.cimu = p.cimu; R.acc = p.acc; R.stun = p.stun; R.cdmg = p.cdmg;
}
int AllMaps() { return (1 << arena::MapCount()) - 1; }
Cfg FromRoom(const net::Room& r) {
    Cfg c; c.mode = r.mode; c.max = r.max; c.rounds = r.rounds; c.time = r.timeSec; c.map = r.map; c.rand = r.maps != 0;
    c.dmg = r.dmg; c.respawn = r.respawn; c.kills = r.kills; c.rr = rules::Parse(r.rules);
    return c;
}
void SendCfg(const Cfg& c) {
    net::ConfArgs a;
    a.mode = c.mode; a.max = c.mode == "duel" ? 2 : c.max; a.rounds = c.rounds; a.timeSec = c.time; a.dmg = c.dmg;
    a.map = c.map; a.maps = c.rand ? AllMaps() : 0; a.respawn = c.respawn; a.kills = c.kills;
    a.setRules = true; a.rules = rules::Format(c.rr);
    net::Conf(a);
}
std::wstring ModeLabel(const std::string& m) { return m == "ffa" ? L"混战" : m == "kill" ? L"击杀赛" : L"对决"; }
bool Banned(const rules::Rules& R, int weapon) { return weapon >= 1 && weapon <= 30 && (R.wban >> (weapon - 1)) & 1; }
const wchar_t* kBasicOpts[] = {L"gmode", L"players", L"rounds", L"time", L"mapsel", L"dmg", L"hp", L"preset"};
const wchar_t* kMoreOpts[] = {L"sd", L"sr", L"ult", L"heal", L"cimu", L"acc", L"edgeon", L"wrand", L"stun", L"cdmg", L"respawn", L"kills"};
template <size_t N> int Step(const int (&a)[N], int cur, int dir) {
    if (dir > 0) { for (size_t i = 0; i < N; ++i) if (a[i] > cur) return a[i]; return a[0]; }
    for (size_t i = N; i-- > 0;) if (a[i] < cur) return a[i]; return a[N - 1];
}
int Wrap(int v, int lo, int hi, int) { return v > hi ? lo : v < lo ? hi : v; }  // arrows wrap around
// one arrow step (dir +1 / -1) on option `id`; cap = the server's player limit. false = unknown id
bool CfgStep(Cfg& c, const std::wstring& id, int dir, int cap) {
    auto& R = c.rr;
    if (id == L"gmode") { const char* m[] = {"duel", "ffa", "kill"}; int i = c.mode == "ffa" ? 1 : c.mode == "kill" ? 2 : 0;
        c.mode = m[(i + dir + 3) % 3]; if (c.mode == "duel") c.max = 2; else if (c.max < 3 && cap >= 3) c.max = (std::min)(cap, 4); }
    else if (id == L"players") { if (c.mode != "duel") c.max = Wrap(c.max + dir, 2, (std::max)(2, cap), 1); }
    else if (id == L"rounds") c.rounds = Step(kRounds, c.rounds, dir);
    else if (id == L"time") c.time = Step(kTimes, c.time, dir);
    else if (id == L"mapsel") { int n = arena::MapCount(), i = c.rand ? n : c.map; i = (i + dir + n + 1) % (n + 1);
        c.rand = i == n; if (!c.rand) c.map = i; }
    else if (id == L"dmg") c.dmg = Wrap(c.dmg + 10 * dir, 10, 500, 10);
    else if (id == L"hp") R.hp = Wrap(R.hp + 10 * dir, 50, 500, 10);
    else if (id == L"preset") { int p = PresetOf(c); ApplyPreset(c, p < 0 ? (dir > 0 ? 0 : 2) : (p + dir + 3) % 3); }
    else if (id == L"sd") R.sd = Wrap(R.sd + 10 * dir, 0, 300, 10);
    else if (id == L"sr") R.sr = Wrap(R.sr + 10 * dir, 0, 300, 10);
    else if (id == L"ult") R.ult = Wrap(R.ult + 10 * dir, 0, 300, 10);
    else if (id == L"heal") R.heal = (R.heal + dir + 3) % 3;
    else if (id == L"cimu") R.cimu ^= 1;
    else if (id == L"acc") R.acc = Wrap(R.acc + dir, 0, 9, 1);
    else if (id == L"edgeon") R.edge ^= 1;
    else if (id == L"wrand") R.wrand ^= 1;
    else if (id == L"stun") R.stun = Step(kStuns, R.stun, dir);
    else if (id == L"cdmg") R.cdmg ^= 1;
    else if (id == L"respawn") c.respawn = Step(kRespawns, c.respawn, dir);
    else if (id == L"kills") c.kills = Step(kKills, c.kills, dir);
    else return false;
    return true;
}
// "set <id> <value>" on an option: the value as shown (numbers without "%"/"秒"), see the file header
bool CfgSet(Cfg& c, const std::wstring& id, const std::wstring& v, int cap) {
    auto& R = c.rr; long n = wcstol(v.c_str(), nullptr, 10);
    auto num = [&](int lo, int hi, int step) { int x = rules::Clamp(n, lo, hi); return step > 1 ? (x - lo) / step * step + lo : x; };
    if (id == L"gmode") { std::string m = uex::Utf8(v); if (m != "duel" && m != "ffa" && m != "kill") return false;
        c.mode = m; if (m == "duel") c.max = 2; else if (c.max < 3 && cap >= 3) c.max = (std::min)(cap, 4); }
    else if (id == L"players") { if (c.mode != "duel") c.max = num(2, (std::max)(2, cap), 1); }
    else if (id == L"rounds") c.rounds = num(1, 9, 1);
    else if (id == L"time") c.time = num(0, 600, 1);
    else if (id == L"mapsel") { if (v == L"rand") c.rand = true; else { c.rand = false; c.map = num(0, arena::MapCount() - 1, 1); } }
    else if (id == L"dmg") c.dmg = num(10, 500, 10);
    else if (id == L"hp") R.hp = num(50, 500, 10);
    else if (id == L"preset") { int p = v == L"标准" ? 0 : v == L"快速" ? 1 : v == L"硬核" ? 2 : num(0, 2, 1); ApplyPreset(c, p); }
    else if (id == L"sd") R.sd = num(0, 300, 10);
    else if (id == L"sr") R.sr = num(0, 300, 10);
    else if (id == L"ult") R.ult = num(0, 300, 10);
    else if (id == L"heal") R.heal = num(0, 2, 1);
    else if (id == L"cimu") R.cimu = num(0, 1, 1);
    else if (id == L"acc") R.acc = num(0, 9, 1);
    else if (id == L"edgeon") R.edge = num(0, 1, 1);
    else if (id == L"wrand") R.wrand = num(0, 1, 1);
    else if (id == L"stun") { R.stun = num(0, 10, 1); if (R.stun && R.stun < 3) R.stun = 3; }
    else if (id == L"cdmg") R.cdmg = num(0, 1, 1);
    else if (id == L"respawn") c.respawn = num(1, 60, 1);
    else if (id == L"kills") c.kills = num(0, 99, 1);
    else if (id == L"wban") R.wban = (uint32_t)wcstoul(v.c_str(), nullptr, 10) & 0x3FFFFFFFu;
    else return false;
    return true;
}
std::wstring Pct(int v) { return std::to_wstring(v) + L"%"; }
std::wstring OnOff(int v) { return v ? L"开" : L"关"; }
std::wstring CfgValue(const Cfg& c, const std::wstring& id) {
    auto& R = c.rr;
    if (id == L"gmode") return ModeLabel(c.mode);
    if (id == L"players") return std::to_wstring(c.mode == "duel" ? 2 : c.max) + L" 人";
    if (id == L"rounds") return c.mode == "kill" ? L"1 回合" : std::to_wstring(c.rounds) + L" 回合";
    if (id == L"time") return c.time ? std::to_wstring(c.time) + L" 秒" : L"不限";
    if (id == L"mapsel") return c.rand ? L"随机" : c.map >= 0 && c.map < arena::MapCount() ? arena::Maps()[c.map].name : L"?";
    if (id == L"dmg") return Pct(c.dmg);
    if (id == L"hp") return Pct(R.hp);
    if (id == L"preset") { int p = PresetOf(c); return p < 0 ? L"自定义" : kPresets[p].name; }
    if (id == L"sd") return Pct(R.sd);
    if (id == L"sr") return Pct(R.sr);
    if (id == L"ult") return Pct(R.ult);
    if (id == L"heal") return R.heal == 0 ? L"全禁" : R.heal == 1 ? L"只留酒葫芦" : L"不限";
    if (id == L"cimu") return R.cimu ? L"禁用" : L"允许";
    if (id == L"acc") return R.acc >= 9 ? L"不限" : std::to_wstring(R.acc) + L" 件";
    if (id == L"edgeon") return OnOff(R.edge);
    if (id == L"wrand") return OnOff(R.wrand);
    if (id == L"stun") return R.stun ? L"第 " + std::to_wstring(R.stun) + L" 下后" : L"关";
    if (id == L"cdmg") return OnOff(R.cdmg);
    if (id == L"respawn") return std::to_wstring(c.respawn) + L" 秒";
    if (id == L"kills") return c.kills ? std::to_wstring(c.kills) + L" 杀" : L"不限";
    return L"";
}
// one line for everyone (lobby): mode, players, map, the numbers that differ from the defaults
std::wstring CfgSummary(const Cfg& c) {
    rules::Rules d; auto& R = c.rr;
    std::wstring s = ModeLabel(c.mode) + L"  " + CfgValue(c, L"players") + L"  地图 " + CfgValue(c, L"mapsel");
    if (c.mode == "kill") s += L"  复活 " + CfgValue(c, L"respawn") + L"  目标 " + CfgValue(c, L"kills");
    else s += L"  " + CfgValue(c, L"rounds");
    s += L"  时限 " + CfgValue(c, L"time") + L"  伤害 " + Pct(c.dmg) + L"  血量 " + Pct(R.hp);
    if (R.sd != d.sd) s += L"  杀器伤害 " + Pct(R.sd);
    if (R.sr != d.sr) s += L"  杀气回复 " + Pct(R.sr);
    if (R.ult != d.ult) s += L"  大招 " + Pct(R.ult);
    if (R.heal != d.heal) s += L"  回血 " + CfgValue(c, L"heal");
    if (R.cimu) s += L"  禁慈母";
    if (R.acc < 9) s += L"  饰品≤" + std::to_wstring(R.acc);
    if (!R.edge) s += L"  无杀器";
    if (R.wban) { int n = 0; for (uint32_t b = R.wban; b; b &= b - 1) ++n; s += L"  禁用武器 " + std::to_wstring(n); }
    if (R.wrand) s += L"  随机武器";
    if (R.stun != d.stun) s += L"  硬直递减 " + CfgValue(c, L"stun");
    if (R.cdmg != d.cdmg) s += L"  连段递减 " + OnOff(R.cdmg);
    int p = PresetOf(c); if (p >= 0) s += L"  [" + std::wstring(kPresets[p].name) + L"]";
    return s;
}

// connect settings remembered across runs: <mod dir>mp_settings.txt, UTF-8 "key=value" lines
// (ip, port, name, pw, mode). Read when the panel is built (from the root if an old flat install
// left it there), written on create/join (always to the mod folder).
void WriteEdit(const wchar_t* id, const std::wstring& s);
std::wstring SettingsPath() { auto d = ModDir(); return d.empty() ? L"" : d + L"mp_settings.txt"; }
void LoadSettings() {
    if (SettingsPath().empty()) return;
    auto p = ModFileIn(L"mp_settings.txt");
    FILE* f = _wfopen(p.c_str(), L"r, ccs=UTF-8"); if (!f) return;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        auto eq = s.find(L'='); if (eq == std::wstring::npos) continue;
        std::wstring k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == L"mode") g.steamMode = v == L"steam";
        else if (k == L"room") {  // mode,max,rounds,time,map,rand,dmg,respawn,kills
            wchar_t m[16]{}; int x[8]{};
            if (swscanf(v.c_str(), L"%15[a-z],%d,%d,%d,%d,%d,%d,%d,%d", m, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6, x + 7) == 9) {
                std::string ms = uex::Utf8(m); if (ms != "ffa" && ms != "kill") ms = "duel";
                g_saved.mode = ms; g_saved.max = rules::Clamp(x[0], 2, net::kMaxPlayers); g_saved.rounds = rules::Clamp(x[1], 1, 9);
                g_saved.time = rules::Clamp(x[2], 0, 600); g_saved.map = rules::Clamp(x[3], 0, arena::MapCount() - 1); g_saved.rand = x[4] != 0;
                g_saved.dmg = rules::Clamp(x[5] / 10 * 10, 10, 500); g_saved.respawn = rules::Clamp(x[6], 1, 60); g_saved.kills = rules::Clamp(x[7], 0, 99);
                if (ms == "duel") g_saved.max = 2;
            }
        }
        else if (k == L"rules") g_saved.rr = rules::Parse(uex::Utf8(v));
        else if ((k == L"ip" || k == L"port" || k == L"name" || k == L"pw") && !v.empty() && v.size() < 128) {
            WriteEdit(k.c_str(), v);
            g_conn[k == L"ip" ? 0 : k == L"port" ? 1 : k == L"name" ? 2 : 3] = v;
        }
    }
    fclose(f);
}
void SaveSettings(const std::wstring& ip, const std::wstring& port, const std::wstring& name, const std::wstring& pw) {
    g_conn[0] = ip; g_conn[1] = port; g_conn[2] = name; g_conn[3] = pw;
    auto p = SettingsPath(); if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"w, ccs=UTF-8"); if (!f) return;
    fwprintf(f, L"ip=%s\nport=%s\nname=%s\npw=%s\nmode=%s\n", ip.c_str(), port.c_str(), name.c_str(), pw.c_str(), g.steamMode ? L"steam" : L"relay");
    auto& c = g_saved;
    fwprintf(f, L"room=%S,%d,%d,%d,%d,%d,%d,%d,%d\nrules=%S\n", c.mode.c_str(), c.max, c.rounds, c.time, c.map, int(c.rand), c.dmg, c.respawn, c.kills,
             rules::Format(c.rr).c_str());
    fclose(f);
}
void SaveCfg(const Cfg& c) { g_saved = c; if (!g_conn[0].empty()) SaveSettings(g_conn[0], g_conn[1], g_conn[2], g_conn[3]); }

void Build() {
    g = G{.world = uex::World(), .open = g.open, .weapon = g.weapon, .edge = g.edge, .outfit = g.outfit, .status = g.status};
    uex::ForgetTexts();  // new widgets can come at old addresses
    auto cls = UmgClass(L"UserWidget");
    g.root = uex::CreateWidget(cls);
    if (!g.root) { uex::Log(L"ui: CreateWidget failed"); return; }
    g.tree = kWidgetTree.Of<UObject*>(g.root);
    if (!g.tree) { g.tree = New(L"WidgetTree", g.root); Put(g.root, kWidgetTree, g.tree); }
    g.canvas = New(L"CanvasPanel", g.tree);
    if (!g.tree || !g.canvas) { uex::Log(L"ui: tree/canvas failed"); g.root = nullptr; return; }
    Put(g.tree, kRootWidget, g.canvas);
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
    { auto h = HBox(); auto l = MkText(L"连接方式", 26, kDim); Put<float>(l, kTbMinW, 130); HAdd(h, l, {0, 0, 16, 0});
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
    { auto h = HBox(); HAdd(h, MkButton(L"create", L"创建房间"), {0, 0, 12, 0}); HAdd(h, MkButton(L"hostcfg", L"房间设置", 24), {0, 0, 0, 0});
      VAdd(v, h, {0, 16, 0, 8}, 2); }
    { auto h = HBox(); auto l = MkText(L"房间码", 26, kDim); Put<float>(l, kTbMinW, 130); HAdd(h, l, {0, 0, 16, 0});
      HAdd(h, MkEdit(L"code", L"", 160), {0, 0, 16, 0}); HAdd(h, MkButton(L"join", L"加入房间")); VAdd(v, h); }
    VAdd(v, Center(MkText(L"cstatus", L"", 24, kWarn)), {0, 10, 0, 4}, 2);
    VAdd(v, Line(560), {0, 6, 0, 6}, 2);
    VAdd(v, MkButton(L"close", L"关闭", 24), {0, 4, 0, 0}, 2);

    // promo card, top right of the room page: QR code (maps\qr.png) and two lines
    g.promo = Frame({1, 0}, {1, 0}, {-30, 30}, &v, {0, 0, 0, 0.9f});  // ink panel, as the room page
    Call1(g.promo, L"SetPadding", L"InPadding", Margin{18, 14, 18, 14});
    if (auto ps = uex::Field<UObject*>(g.promo, L"Slot"); ps && *ps) Call1<int32_t>(*ps, L"SetZOrder", L"InZOrder", 10);  // above the room panel
    VAdd(v, Center(MkFileImg(L"qr.png", 104, 104)), {0, 0, 0, 6}, 2);
    VAdd(v, Center(MkText(L"promo1", L"SoonPlay 轻游平台", 19, kGold)), {0, 0, 0, 2}, 2);
    VAdd(v, Center(MkText(L"promo2", L"一段话、一张图，就能变成游戏", 16, kInk)), {0, 0, 0, 1}, 2);
    VAdd(v, Center(MkText(L"promo3", L"扫码创作，拉上伙伴一起玩", 16, kDim)), {0, 0, 0, 0}, 2);

    // lobby panel
    g.lobby = Frame({0.5, 0.5}, {0.5, 0.5}, {0, 0}, &v);
    VAdd(v, Heading(L"code", L"房间", 44), {0, 0, 0, 4}, 2);
    VAdd(v, Line(900), {0, 0, 0, 8}, 2);
    {   // one text per member (8 at most), two columns so 8 fit at 1280x720
        auto h = HBox(); UObject* col[2] = {VBox(), VBox()};
        for (int i = 0; i < kMaxRows; ++i) {
            g.rows[i] = MkText((L"mem" + std::to_wstring(i)).c_str(), L"", 22);
            VAdd(col[i / 4], g.rows[i], {0, 1, 0, 1});
        }
        HAdd(h, col[0], {0, 0, 40, 0}); HAdd(h, col[1], {0, 0, 0, 0});
        VAdd(v, h, {0, 4, 0, 8}, 2);
    }
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
            auto sel = MkImg(art::kCardSel, 132, 132, {1, 0.92f, 0.75f, 1}); layer(sel, 0); Put<uint8_t>(sel, kVis, kCollapsed);
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
    if (auto e = g.text[L"erow"]) Put<uint8_t>(e, kVis, kCollapsed);  // the weapon tab is shown first (UpdateLobby)
    if (auto o = g.text[L"orow"]) Put<uint8_t>(o, kVis, kCollapsed);
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
            auto sel = MkRect(256, 144, 3.f, {0.95f, 0.8f, 0.5f, 1}); layer(sel, 0); Put<uint8_t>(sel, kVis, kCollapsed);
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
        for (auto id : {L"map", L"map+", L"map-"}) { auto w = MkButton(id, L"地图", 22); VAdd(v, w, {0, 0, 0, 0}, 2); Put<uint8_t>(w, kVis, kCollapsed); }
    }
    // "weapon" / "edge" cycle to the next card (test hook / e2e); the cards replace them on screen
    { auto w = MkButton(L"weapon", L"武器", 22); VAdd(v, w, {0, 0, 0, 0}, 2); Put<uint8_t>(w, kVis, kCollapsed); }
    { auto w = MkButton(L"edge", L"杀器", 22); VAdd(v, w, {0, 0, 0, 0}, 2); Put<uint8_t>(w, kVis, kCollapsed); }
    VAdd(v, Center(MkText(L"settings", L"", 24, kDim), 900), {0, 4, 0, 4}, 2);
    VAdd(v, Center(MkText(L"cardnote", L"", 22, kWarn)), {0, 0, 0, 0}, 2);
    VAdd(v, Line(900), {0, 6, 0, 6}, 2);
    { auto h = HBox(); HAdd(h, MkButton(L"ready", L"准备"), {0, 0, 12, 0}); HAdd(h, MkButton(L"start", L"开始"), {0, 0, 12, 0});
      HAdd(h, MkButton(L"cfg", L"房间设置"), {0, 0, 12, 0});
      HAdd(h, MkButton(L"leave", L"离开"), {0, 0, 12, 0}); HAdd(h, MkButton(L"hide", L"隐藏"), {0, 0, 0, 0});
      VAdd(v, h, {0, 4, 0, 4}, 2); }
    VAdd(v, Center(MkText(L"lstatus", L"", 24, kWarn)), {0, 4, 0, 0}, 2);

    // room settings panel (host; also before a create from the connect panel): pages 基本 / 更多,
    // rows of the settings menu (T_SettingsDefaultBar) with the start menu arrows
    g.cfg = Frame({0.5, 0.5}, {0.5, 0.5}, {0, 0}, &v);
    VAdd(v, Heading(L"cfgtitle", L"房间设置", 40), {0, 0, 0, 4}, 2);
    { auto h = HBox(); HAdd(h, MkButton(L"pgbasic", L"基本", 24), {0, 0, 12, 0}); HAdd(h, MkButton(L"pgmore", L"更多", 24), {0, 0, 0, 0});
      VAdd(v, h, {0, 0, 0, 6}, 2); }
    VAdd(v, Line(760), {0, 0, 0, 6}, 2);
    {
        UObject* rows = nullptr;
        auto opt = [&](const wchar_t* id, const wchar_t* label) {
            auto bar = New(L"Border", g.tree);
            MakeBrush(reinterpret_cast<uint8_t*>(bar) + kBorderBg, art::kBar, 3, {1, 1, 1, 1});
            Call1(bar, L"SetPadding", L"InPadding", Margin{40, 2, 40, 2});
            auto h = HBox(); auto l = MkText(label, 26, kInk); Put<float>(l, kTbMinW, 300); HAdd(h, l, {0, 0, 20, 0});
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
            VAdd(rows, bar, {0, 1, 0, 1}, 2);
            g.text[std::wstring(L"row_") + id] = bar;
        };
        const wchar_t* basic[][2] = {{L"gmode", L"模式"}, {L"players", L"人数"}, {L"rounds", L"回合数"}, {L"time", L"限时"},
                                     {L"mapsel", L"地图"}, {L"dmg", L"伤害倍率"}, {L"hp", L"血量倍率"}, {L"preset", L"预设"}};
        const wchar_t* more[][2] = {{L"sd", L"杀器伤害"}, {L"sr", L"杀气回复"}, {L"ult", L"大招充能"}, {L"heal", L"回血道具"},
                                    {L"cimu", L"慈母护符"}, {L"acc", L"饰品上限"}, {L"edgeon", L"杀器"}, {L"wrand", L"随机武器"},
                                    {L"stun", L"硬直递减"}, {L"cdmg", L"连段伤害递减"}, {L"respawn", L"复活等待"}, {L"kills", L"目标击杀"}};
        rows = VBox(); for (auto& o : basic) opt(o[0], o[1]);
        VAdd(v, rows, {0, 2, 0, 2}, 2); g.text[L"pagebasic"] = rows;
        auto page2 = VBox();
        auto two = HBox();
        rows = VBox(); for (int i = 0; i < 6; ++i) opt(more[i][0], more[i][1]); HAdd(two, rows, {0, 0, 12, 0});
        rows = VBox(); for (int i = 6; i < 12; ++i) opt(more[i][0], more[i][1]); HAdd(two, rows, {0, 0, 0, 0});
        VAdd(page2, two, {0, 0, 0, 4}, 2);
        // weapon bans: a toggle per weapon (wb<i>, bit i = Weapons()[i + 1]), 7 per line
        VAdd(page2, Center(MkText(L"wbtitle", L"禁用武器（点击切换）", 22, kDim)), {0, 2, 0, 2}, 2);
        auto& ws = fight::Weapons(); UObject* line = nullptr;
        for (int i = 0; i + 1 < int(ws.size()) && i < 30; ++i) {
            if (i % 7 == 0) { line = HBox(); VAdd(page2, line, {0, 1, 0, 1}, 2); }
            auto b = New(L"Button", g.tree); if (!b) break;
            SkinButton(b, art::kBar, art::kBarHi, {1, 1, 1, 0.8f}, {1, 1, 1, 0.6f});
            auto t = Center(MkText(ws[i + 1].label, 20, kInk), 120);
            if (auto s = Add(b, t)) Call1(s, L"SetPadding", L"InPadding", Margin{6, 3, 6, 3});
            g.btn[L"wb" + std::to_wstring(i)] = {b, t, false}; HAdd(line, b, {2, 0, 2, 0});
        }
        VAdd(page2, MkButton(L"wbclear", L"全部解禁", 20), {0, 4, 0, 0}, 2);
        VAdd(v, page2, {0, 2, 0, 2}, 2); g.text[L"pagemore"] = page2;
    }
    VAdd(v, Center(MkText(L"cfgsum", L"", 20, kDim), 760), {0, 4, 0, 2}, 2);
    VAdd(v, Center(MkText(L"cfgnote", L"", 22, kWarn)), {0, 2, 0, 2}, 2);
    VAdd(v, MkButton(L"cfgback", L"返回", 24), {0, 4, 0, 0}, 2);

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
        auto st = reinterpret_cast<uint8_t*>(bar) + kBarStyle;
        MakeBrush(st + kPbBack, art::kHpBack, 3, {0.25f, 0.22f, 0.2f, 0.85f});
        MakeBrush(st + kPbFill, art::kHpFill, 3, {1, 1, 1, 1});
        Put<uint8_t>(bar, kBarFill, right ? 1 : 0);  // BarFillType: LeftToRight / RightToLeft
        Put(bar, kBarPad, V2{0, 0});
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
        g.text[L"hudnotebox"] = nf; Put<uint8_t>(nf, kVis, kCollapsed);
        // ffa / kill: the top line (survivors, or my kills / deaths) and the kill ranking on the right
        VAdd(pv, Center(MkText(L"hudtop", L"", 22, kGold), 180), {0, 2, 0, 0}, 2);
        UObject* rv; auto rb = Frame({1, 0}, {1, 0}, {-40, 140}, &rv, {0, 0, 0, 0.45f}, g.hud);
        Call1(rb, L"SetPadding", L"InPadding", Margin{20, 8, 20, 10});
        VAdd(rv, Title(MkText(L"ranktitle", L"击杀排行", 24, kGold), 40), {0, 0, 0, 4}, 1);
        for (int i = 0; i < net::kMaxPlayers; ++i) VAdd(rv, MkText((L"rank" + std::to_wstring(i)).c_str(), L"", 22, kInk), {0, 0, 0, 0}, 1);
        g.text[L"rankbox"] = rb; Put<uint8_t>(rb, kVis, kCollapsed);
    }
    // head bars: one per opponent on a full-screen canvas, moved every frame to the projected GhostAnchor
    g.heads = Canvas(g.canvas, {0, 0}, {1, 1}, {0, 0}, Margin{0, 0, 0, 0});
    for (int i = 0; i < kHeads; ++i) {
        auto& hd = g.head[i];
        auto col = VBox();
        hd.name = Center(MkText(L"", 20, kInk), 160); VAdd(col, hd.name, {0, 0, 0, 2}, 2);
        auto sb = New(L"SizeBox", g.tree); auto bar = New(L"ProgressBar", g.tree);
        Call1<float>(sb, L"SetWidthOverride", L"InWidthOverride", 140.f);
        Call1<float>(sb, L"SetHeightOverride", L"InHeightOverride", 10.f);
        auto st = reinterpret_cast<uint8_t*>(bar) + kBarStyle;
        MakeBrush(st + kPbBack, art::kHpBack, 3, {0.25f, 0.22f, 0.2f, 0.85f});
        MakeBrush(st + kPbFill, art::kHpFill, 3, {1, 1, 1, 1});
        Put(bar, kBarPad, V2{0, 0});
        Call1(bar, L"SetFillColorAndOpacity", L"InColor", Color{0.86f, 0.18f, 0.14f, 1});
        Call1<float>(bar, L"SetPercent", L"InPercent", 1.f);
        Add(sb, bar); VAdd(col, sb, {0, 0, 0, 0}, 2); hd.bar = bar;
        hd.box = Canvas(g.heads, {0, 0}, {0, 0}, {0.5, 1}, Margin{0, 0, 0, 0}, col, true);
        Put<uint8_t>(col, kVis, kCollapsed);
    }
    Put<uint8_t>(g.heads, kVis, kHitTestInvisible);
    g.result = Frame({0.5, 0.4}, {0.5, 0.5}, {0, 0}, &v, {0, 0, 0, 0.7f});
    VAdd(v, MkText(L"result", L"", 34, {1, 0.85f, 0.5f, 1}), {0, 0, 0, 0}, 2);
    VAdd(v, MkText(L"result2", L"", 20), {0, 6, 0, 0}, 2);
    VAdd(v, MkText(L"resultlist", L"", 24, kInk), {0, 8, 0, 0}, 2);
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
    for (auto w : {g.corner, g.connect, g.lobby, g.cfg, g.promo}) Vis(w, kCollapsed);
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

bool CfgClick(const net::Room& r, bool inRoom, const std::wstring& id);
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
        if (id == L"create") {
            int n = g_saved.mode == "duel" ? 2 : std::clamp(g_saved.max, 2, net::kMaxPlayers);
            net::Create(uex::Utf8(name), n); g_applyRoom = true; g.status = L"正在创建房间..."; g.failed = false;
        }
        else if (code.empty()) { g.status = L"请输入房间码"; g.failed = true; }
        else { net::Join(uex::Utf8(code), uex::Utf8(name)); g.status = L"正在加入房间 " + code + L"..."; g.failed = false; }
    } else if (id == L"hostcfg") { if (!inRoom) { g.cfgOpen = true; g.cfgNote.clear(); } }
    else if (id == L"cfgback") g.cfgOpen = false;
    else if (id == L"pgbasic" || id == L"pgmore") g.cfgPage = id == L"pgmore";
    else if (g.cfgOpen && (!inRoom || r.you == r.host) && CfgClick(r, inRoom, id)) {}
    else if (!inRoom) return;
    else if (id == L"leave") { net::Leave(); g.status.clear(); }
    else if (id == L"ready") {
        if (auto m = Find(r, r.you)) {
            if (!m->ready && !g.violation.empty()) { g.status = L"不能准备: " + g.violation; return; }
            net::SetReady(!m->ready);
        }
    }
    else if (id == L"cfg") { if (r.you == r.host) { g.cfgOpen = true; g.cfgNote.clear(); } }
    else if (id == L"weapon") {
        auto m = Find(r, r.you); int i = m && net::Now() - g.weaponAt > 2 ? WeaponIndex(m->weapon) : g.weapon; g.weaponAt = net::Now();
        auto R = rules::Parse(r.rules); int n = int(fight::Weapons().size());
        if (R.wrand) return;
        for (int s = 0; s < n; ++s) { i = (i + 1) % n; if (!Banned(R, i)) break; }
        g.weapon = i;
        net::SetWeapon(fight::Weapons()[g.weapon].id);
    } else if (id == L"wpg-" || id == L"wpg+") {
        int n = Pages(int(k.wc.card.size())); k.wc.page = (k.wc.page + (id == L"wpg+" ? 1 : n - 1)) % n;
    } else if (id.size() >= 2 && id[0] == L'w' && iswdigit(id[1])) {  // weapon card w<i>
        int i = _wtoi(id.c_str() + 1);
        if (i < 0 || i >= int(fight::Weapons().size())) return;
        auto R = rules::Parse(r.rules);
        if (R.wrand || Banned(R, i)) { g.status = R.wrand ? L"本房间每回合随机武器" : L"这把武器被房主禁用了"; return; }
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
        if (i >= 0 && i < arena::MapCount()) { Cfg c = FromRoom(r); c.map = i; c.rand = false; SendCfg(c); SaveCfg(c); }
    }
    else if (id == L"start") net::Start();
}
// an option button / arrow / weapon ban on the settings panel. In the room: sent to the server
// (the lobby shows the echo) and remembered; before a create: only remembered (sent after the create)
bool CfgClick(const net::Room& r, bool inRoom, const std::wstring& id) {
    Cfg c = inRoom ? FromRoom(r) : g_saved;
    int cap = inRoom ? r.cap : net::kMaxPlayers;
    std::wstring base = id; int dir = 1;
    if (!base.empty() && (base.back() == L'+' || base.back() == L'-')) { dir = base.back() == L'-' ? -1 : 1; base.pop_back(); }
    if (base.size() >= 3 && base[0] == L'w' && base[1] == L'b' && iswdigit(base[2])) {
        int i = _wtoi(base.c_str() + 2); if (i < 0 || i >= 30) return true;
        c.rr.wban ^= 1u << i;
        if (int(fight::Weapons().size()) > 1) {  // keep one weapon pickable
            uint32_t all = (int(fight::Weapons().size()) - 1 >= 30) ? 0x3FFFFFFFu : (1u << (int(fight::Weapons().size()) - 1)) - 1;
            if ((c.rr.wban & all) == all) { c.rr.wban ^= 1u << i; g.cfgNote = L"至少要留一把武器"; return true; }
        }
    } else if (base == L"wbclear") c.rr.wban = 0;
    else if (!CfgStep(c, base, dir, cap)) return false;
    if (inRoom && c.mode == "duel" && r.members.size() > 2) { g.cfgNote = L"房间里超过 2 人，不能改成对决"; return true; }
    if (inRoom && c.mode != "duel") for (auto& m : r.members) if (m.slot >= c.max) { g.cfgNote = L"有玩家的位置超出人数，不能再减"; return true; }
    if (c.mode == "kill" && c.time == 0 && c.kills == 0) { g.cfgNote = L"击杀赛要有限时或目标击杀"; return true; }
    g.cfgNote.clear();
    if (inRoom) SendCfg(c);
    SaveCfg(c);
    return true;
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
    std::wstring dir = RootDir(); if (dir.empty()) return list;
    FILE* f = _wfopen((dir + L"autorun.txt").c_str(), L"r, ccs=UTF-8"); if (!f) return list;
    wchar_t b[512];
    while (fgetws(b, 511, f)) {
        std::wstring id = b;
        while (!id.empty() && (id.back() == L'\n' || id.back() == L'\r' || id.back() == L' ' || id.back() == L'\t')) id.pop_back();
        while (!id.empty() && (id[0] == L' ' || id[0] == 0xfeff)) id.erase(0, 1);
        if (id.empty() || id[0] == L'#' || id == L"nosave") continue;  // nosave: test-instance helper
        std::wstring info = dir + id + L"\\" + id + L".txt";  // <root>\<id>\<id>.txt, else legacy flat <root>\<id>.txt
        if (!Exists(info)) info = dir + id + L".txt";
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
    uex::SetText(g.text[L"code"], L"房间码  " + uex::Wide(r.code) + L"    " + std::to_wstring(r.members.size()) + L" / " + std::to_wstring(r.max) + L" 人");
    auto R = rules::Parse(r.rules);
    for (int i = 0; i < kMaxRows; ++i) {
        const net::Member* m = i < int(r.members.size()) ? &r.members[i] : nullptr;
        std::wstring s;
        if (m) {
            wchar_t b[512];  // name 24 chars + weapon / edge labels (an unknown id is shown raw, up to 200 bytes each)
            std::wstring edge = m->edge.empty() || !R.edge ? L"" : L"  杀器 " + EdgeLabel(m->edge);
            swprintf(b, 512, L"%s%s  %s%s  %s%s", m->slot == r.host ? L"【房主】" : L"", uex::Wide(m->name).c_str(),
                     R.wrand ? L"随机武器" : WeaponLabel(m->weapon).c_str(), edge.c_str(), m->ready ? L"已准备" : L"未准备",
                     m->slot == r.you ? L"（你）" : (m->conn ? L"" : L"（掉线）"));
            s = b;
        }
        uex::SetText(g.rows[i], s);
        Vis(g.rows[i], i < r.max || m ? kHitTestInvisible : kCollapsed);
        if (m) TextColor(g.rows[i], m->ready || m->slot == r.host ? kInk : kDim);
    }
    auto me = Find(r, r.you);
    // weapon cards: banned ones disabled and dimmed; random weapons: one note, cards off
    for (int i = 0; i < int(k.wc.card.size()); ++i) {
        bool ok = !R.wrand && !Banned(R, i);
        Call1<bool>(k.wc.card[i], L"SetIsEnabled", L"bInIsEnabled", ok);
    }
    std::wstring cardNote = R.wrand ? L"随机武器：每回合由系统分配武器" : R.wban ? L"灰色武器被房主禁用" : L"";
    if (me && !R.wrand && Banned(R, WeaponIndex(me->weapon))) cardNote = L"你选的武器被禁用了，请换一把";
    uex::SetText(g.text[L"cardnote"], cardNote);
    // houserules check of the loadout, once a second: refuses ready, and takes it back if already ready
    if (net::Now() - g.violAt > 1) {
        g.violAt = net::Now();
        g.violation = houserules::Violation(R, uex::Me());
        if (me && !R.wrand && Banned(R, WeaponIndex(me->weapon))) g.violation = L"所选武器被禁用";
        if (me && me->ready && !g.violation.empty() && r.you != r.host && net::Now() - g.unreadyAt > 3) { net::SetReady(false); g.unreadyAt = net::Now(); }
    }
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
    int mi = r.maps ? -1 : r.map >= 0 && r.map < arena::MapCount() ? r.map : 0;  // random: no card picked
    if (mi != k.mapShown) {
        for (int i = 0; i < int(k.mapSel.size()); ++i) {
            Vis(k.mapSel[i], i == mi ? kHitTestInvisible : kCollapsed);
            TextColor(k.mapName[i], i == mi ? kGold : kDim);
        }
        k.mapShown = mi;
    }
    bool host = r.you == r.host;
    Vis(g.btn[L"start"].o, host ? kVisible : kCollapsed);
    Vis(g.btn[L"cfg"].o, host ? kVisible : kCollapsed);
    if (!host) g.cfgOpen = false;
    Call1<bool>(g.btn[L"ready"].o, L"SetIsEnabled", L"bInIsEnabled", g.violation.empty() || (me && me->ready));
    if (me && me->mods != LocalMods() && net::Now() - g.modsAt > 3) { net::SetMods(LocalMods()); g.modsAt = net::Now(); }
    bool modBlock; std::wstring modNote = ModProblem(r, &modBlock);
    bool canStart = host && AllReady(r) && !modBlock;
    Call1<bool>(g.btn[L"start"].o, L"SetIsEnabled", L"bInIsEnabled", canStart);
    uex::SetText(g.text[L"settings"], CfgSummary(FromRoom(r)) + (host ? (canStart ? L"" : L"   (全员准备后可开始)") : L"   (房主设置)"));
    std::wstring st = r.msg.empty() ? g.status : uex::Wide(r.msg);
    if (!modNote.empty()) st = modNote;
    if (!g.violation.empty()) st = L"不能准备: " + g.violation;
    if (auto e = net::LastError(); !e.empty()) st = L"错误: " + ErrText(e);
    uex::SetText(g.text[L"lstatus"], st); g.lobbyStatus = st;
}

// settings panel: values of the shown page; rows that do not apply are hidden (duel: players fixed,
// kill: rounds unused, respawn / kills only in kill mode)
void UpdateCfg(const net::Room& r, bool inRoom) {
    Cfg c = inRoom ? FromRoom(r) : g_saved;
    for (auto id : kBasicOpts) uex::SetText(g.btn[id].label, CfgValue(c, id));
    for (auto id : kMoreOpts) uex::SetText(g.btn[id].label, CfgValue(c, id));
    Vis(g.text[L"pagebasic"], g.cfgPage == 0 ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.text[L"pagemore"], g.cfgPage == 1 ? kSelfHitTestInvisible : kCollapsed);
    TextColor(g.btn[L"pgbasic"].label, g.cfgPage == 0 ? kGold : kDim);
    TextColor(g.btn[L"pgmore"].label, g.cfgPage == 1 ? kGold : kDim);
    bool kill = c.mode == "kill";
    Vis(g.text[L"row_rounds"], kill ? kCollapsed : kSelfHitTestInvisible);
    Vis(g.text[L"row_respawn"], kill ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.text[L"row_kills"], kill ? kSelfHitTestInvisible : kCollapsed);
    for (auto id : {L"players-", L"players+", L"players"}) Call1<bool>(g.btn[id].o, L"SetIsEnabled", L"bInIsEnabled", c.mode != "duel");
    for (int i = 0; i < 30; ++i) {
        auto it = g.btn.find(L"wb" + std::to_wstring(i)); if (it == g.btn.end()) break;
        bool ban = (c.rr.wban >> i) & 1;
        TextColor(it->second.label, ban ? kRed : kInk);
        Call1<float>(it->second.o, L"SetRenderOpacity", L"InOpacity", ban ? 1.f : 0.75f);
    }
    uex::SetText(g.text[L"cfgtitle"], inRoom ? L"房间设置" : L"房间设置（创建后生效）");
    uex::SetText(g.text[L"cfgsum"], CfgSummary(c));
    uex::SetText(g.text[L"cfgnote"], g.cfgNote);
}

// round wins as filled / empty diamonds: a best-of-N needs N/2+1
std::wstring Pips(const net::Room& r, int score) {
    int need = r.rounds / 2 + 1; std::wstring p;
    for (int i = 0; i < need; ++i) p += i < score ? L"◆ " : L"◇ ";
    return p;
}
// world cm -> slate units of our root canvas (PlayerController::ProjectWorldLocationToScreen gives
// viewport pixels; WidgetLayoutLibrary::GetViewportScale converts). false = behind the camera
bool Project(const float w[3], double& x, double& y) {
    auto pc = uex::PC(); if (!pc) return false;
    auto f = uex::Fn(pc, L"ProjectWorldLocationToScreen"); if (!f) return false;
    Params p(f); p.Set(L"WorldLocation", uex::Vec{w[0], w[1], w[2]}); p.Set<bool>(L"bPlayerViewportRelative", true); p.Call(pc);
    if (!p.Get<bool>(L"ReturnValue")) return false;
    auto s = p.Get<V2>(L"ScreenLocation");
    double scale = 1;
    if (auto lib = uex::Lib(L"WidgetLayoutLibrary /Script/UMG.Default__WidgetLayoutLibrary")) {
        Params q(uex::Fn(lib, L"GetViewportScale")); q.Set(L"WorldContextObject", uex::World()); q.Call(lib);
        float v = q.Get<float>(L"ReturnValue"); if (v > 0.05f) scale = v;
    }
    x = s.x / scale; y = s.y / scale; return true;
}
// members ordered for the ranking / result: score high to low, then fewer deaths, then slot
std::vector<const net::Member*> Ranked(const net::Room& r) {
    std::vector<const net::Member*> v; for (auto& m : r.members) v.push_back(&m);
    std::stable_sort(v.begin(), v.end(), [](auto a, auto b) { return a->score != b->score ? a->score > b->score : a->deaths < b->deaths; });
    return v;
}
void UpdateHeads(const net::Room& r) {
    int n = 0;
    for (auto& m : r.members) {
        if (m.slot == r.you || n >= kHeads) continue;
        auto& hd = g.head[n++];
        float w[3]; double x, y; net::State st;
        bool have = fight::GhostAnchor(m.slot, w) && Project(w, x, y);
        if (g.hudTest && !have) { have = true; x = 500 + 220 * n; y = 420; st.hp = 1.f - 0.15f * n; }
        else if (!net::Sample(m.slot, st)) st.hp = m.alive ? 1.f : 0.f;
        bool show = have && (m.alive || g.hudTest);
        if (show != hd.shown) { Vis(hd.box, show ? kHitTestInvisible : kCollapsed); hd.shown = show; }
        hd.slot = m.slot;
        if (!show) continue;
        uex::SetText(hd.name, uex::Wide(m.name));
        if (std::abs(hd.x - x) > 0.5 || std::abs(hd.y - y) > 0.5) {
            if (auto s = kSlot.Of<UObject*>(hd.box)) Call1(s, L"SetPosition", L"InPosition", V2{x, y});
            hd.x = x; hd.y = y;
        }
        float f = std::clamp(st.hp, 0.f, 1.f);
        if (std::abs(hd.hp - f) > 0.002f) { Call1<float>(hd.bar, L"SetPercent", L"InPercent", f); hd.hp = f; }
    }
    for (; n < kHeads; ++n) if (g.head[n].shown) { Vis(g.head[n].box, kCollapsed); g.head[n].shown = false; g.head[n].slot = -1; }
}
void UpdateFight(const net::Room& r) {
    std::wstring score;
    for (auto m : Ranked(r)) {
        if (!score.empty()) score += L"  :  ";
        score += uex::Wide(m->name) + L" " + std::to_wstring(m->score);
    }
    bool duel = r.mode == "duel", kill = r.mode == "kill";
    Vis(g.heads, r.phase == "fight" || r.phase == "count" ? kHitTestInvisible : kCollapsed);
    UpdateHeads(r);
    {   // top line by mode: duel the score, ffa the survivors, kill my kills / deaths and the ranking
        auto me = Find(r, r.you);
        std::wstring top;
        if (duel) top = score;
        else if (!kill) { int a = 0; for (auto& m : r.members) a += m.alive; top = L"存活 " + std::to_wstring(a) + L" / " + std::to_wstring(r.members.size()); }
        else top = L"击杀 " + std::to_wstring(me ? me->score : 0) + L"  死亡 " + std::to_wstring(me ? me->deaths : 0) +
                   (r.kills ? L"  目标 " + std::to_wstring(r.kills) : L"");
        uex::SetText(g.text[L"hudtop"], top); g.hudTop = top;
        Vis(g.text[L"rankbox"], !duel ? kHitTestInvisible : kCollapsed);
        uex::SetText(g.text[L"ranktitle"], kill ? L"击杀排行" : L"比分");
        auto rk = Ranked(r); std::wstring all;
        for (int i = 0; i < net::kMaxPlayers; ++i) {
            std::wstring s;
            if (i < int(rk.size())) {
                auto m = rk[i];
                s = std::to_wstring(i + 1) + L". " + uex::Wide(m->name) + L"  " + std::to_wstring(m->score) +
                    (kill ? L" 杀 " + std::to_wstring(m->deaths) + L" 死" : L" 分") + (m->alive ? L"" : L"  ✕");
                TextColor(g.text[L"rank" + std::to_wstring(i)], m->slot == r.you ? kGold : m->alive ? kInk : kDim);
            }
            uex::SetText(g.text[L"rank" + std::to_wstring(i)], s);
            if (!s.empty()) all += s + L"; ";
        }
        g.rankText = all;
        Vis(g.text[L"opppips"], duel ? kHitTestInvisible : kCollapsed);
        Vis(g.text[L"mypips"], duel ? kHitTestInvisible : kCollapsed);
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
        break;  // the big right bar: the first opponent (duel); every opponent also has a head bar
    }
    // plaque: "第 N 回合" over the time left (∞ without a limit), or the phase
    uex::SetText(g.text[L"hudround"], kill ? std::wstring(L"击杀赛") : ModeLabel(r.mode) + L"  第 " + std::to_wstring(r.round) + L" / " + std::to_wstring(r.rounds) + L" 回合");
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
        uex::SetText(g.text[L"result2"], left >= 0 ? std::to_wstring((left + 999) / 1000) + L" 秒后" + (r.phase == "over" ? L"返回房间" : L"下一回合") : L"");
        std::wstring list; int i = 0;
        for (auto m : Ranked(r)) {
            list += std::to_wstring(++i) + L".  " + uex::Wide(m->name) + L"    " + std::to_wstring(m->score) + (kill ? L" 杀  " + std::to_wstring(m->deaths) + L" 死" : L" 分") +
                    (m->slot == r.winner ? L"  ★" : L"") + (m->slot == r.you ? L"  （你）" : L"") + L"\n";
        }
        if (!list.empty()) list.pop_back();
        uex::SetText(g.text[L"resultlist"], list); g.resultList = list;
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
    auto vis = [](UObject* w) { if (!w) return -1; return int(kVis.Of<uint8_t>(w)); };
    fwprintf(f, L"open=%d uiMode=%d root=%p inViewport=%d sections corner=%d connect=%d lobby=%d hud=%d result=%d\n",
             g.open, g.uiMode, g.root, g.root ? int(CallB(g.root, L"IsInViewport")) : 0,
             vis(g.corner), vis(g.connect), vis(g.lobby), vis(g.hud), vis(g.result));
    for (auto& [id, t] : g.text) {
        fwprintf(f, L"text %s vis=%d\n", id.c_str(), vis(t));
    }
    for (auto& [id, b] : g.btn) fwprintf(f, L"button %s vis=%d enabled=%d\n", id.c_str(), vis(b.o), kEnabled.Bit(b.o) ? 1 : 0);
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
    {   // settings: the room's (in a room) and the remembered ones
        auto dumpCfg = [&](const wchar_t* tag, const Cfg& c) {
            fwprintf(f, L"cfg %s", tag);
            for (auto id : kBasicOpts) fwprintf(f, L" %s=%s", id, CfgValue(c, id).c_str());
            for (auto id : kMoreOpts) fwprintf(f, L" %s=%s", id, CfgValue(c, id).c_str());
            fwprintf(f, L" wban=%u maps=%d rules='%S'\n", c.rr.wban, c.rand ? AllMaps() : 0, rules::Format(c.rr).c_str());
            fwprintf(f, L"cfg %s summary='%s'\n", tag, CfgSummary(c).c_str());
        };
        net::Room r2;
        if (net::GetRoom(r2)) {
            dumpCfg(L"room", FromRoom(r2));
            fwprintf(f, L"room mode=%S max=%d cap=%d maps=%d respawn=%d kills=%d\n", r2.mode.c_str(), r2.max, r2.cap, r2.maps, r2.respawn, r2.kills);
            for (auto& m : r2.members) fwprintf(f, L"  m%d alive=%d life=%d deaths=%d\n", m.slot, int(m.alive), m.life, m.deaths);
        }
        dumpCfg(L"saved", g_saved);
        fwprintf(f, L"cfg open=%d page=%d note='%s' violation='%s'\n", int(g.cfgOpen), g.cfgPage, g.cfgNote.c_str(), g.violation.c_str());
        fwprintf(f, L"hud top='%s' rank='%s'\n", g.hudTop.c_str(), g.rankText.c_str());
        for (auto& hd : g.head) if (hd.slot >= 0) fwprintf(f, L"head slot=%d shown=%d x=%.0f y=%.0f hp=%.2f\n", hd.slot, int(hd.shown), hd.x, hd.y, hd.hp);
        fwprintf(f, L"result list='%s'\n", g.resultList.c_str());
    }
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
        if (NameOf(o->cls).find(clsSub) != std::wstring::npos && kWidgetTree.Of<UObject*>(o)) return o;
    }
    return nullptr;
}

// ---- title menu entry ---------------------------------------------------------------------
// A button of the game's own menu button class goes into WB_StartMenu_V2's button column, just
// above 退出游戏: QuitGame is taken out and added back after ours, keeping its slot settings.
struct ChildSize { float v; uint8_t rule, pad[3]; };
struct VSlot { ChildSize size; Margin pad; uint8_t h, v; };
VSlot ReadSlot(UObject* s) { return {kVsSize.Of<ChildSize>(s), kVsPad.Of<Margin>(s), kVsH.Of<uint8_t>(s), kVsV.Of<uint8_t>(s)}; }
void WriteSlot(UObject* s, const VSlot& r) {
    if (!s) return;
    Call1(s, L"SetSize", L"InSize", r.size); Call1(s, L"SetPadding", L"InPadding", r.pad);
    Call1<uint8_t>(s, L"SetHorizontalAlignment", L"InHorizontalAlignment", r.h);
    Call1<uint8_t>(s, L"SetVerticalAlignment", L"InVerticalAlignment", r.v);
}
bool MenuEntryOk() { return g.menuBtn && uex::Alive(g.menuBtn) && uex::Alive(g.menu) && kSlot.Of<UObject*>(g.menuBtn); }
// The hidden input icon inside a menu button still takes width in its row. A button made at runtime
// keeps the class default icon size (32 instead of the menu's 29), which pushes the label right by
// the difference; RefreshSize recomputes it like the menu's own buttons.
void RefreshActionIcon() {
    auto tr = g.menuBtn ? kWidgetTree.Of<UObject*>(g.menuBtn) : nullptr;
    for (int i = 0, n = NumObjects(); tr && i < n; ++i) { auto x = ObjectAt(i);
        if (x && x->outer == tr && !(x->flags & 0x10) && NameOf(x) == L"InputActionWidget") { Params q(uex::Fn(x, L"RefreshSize")); q.Call(x); return; } }
}
void MenuEntry() {
    if (MenuEntryOk() || g.tick % 30) return;
    g.menuBtn = nullptr;
    auto menu = FindUserWidget(L"WB_StartMenu_V2"); if (!menu) return;
    auto tree = kWidgetTree.Of<UObject*>(menu); UObject *quit = nullptr, *ng = nullptr;
    for (int i = 0, n = NumObjects(); i < n && !(quit && ng); ++i) {
        auto o = ObjectAt(i);
        if (!o || o->outer != tree || (o->flags & 0x10)) continue;
        auto nm = NameOf(o);
        if (nm == L"QuitGame") quit = o; else if (nm == L"NewGame") ng = o;
    }
    if (!quit || !ng) return;
    auto qs = kSlot.Of<UObject*>(quit), ns = kSlot.Of<UObject*>(ng);
    auto box = qs ? kSlotParent.Of<UObject*>(qs) : nullptr;
    if (!box || !ns || kSlotParent.Of<UObject*>(ns) != box) return;
    auto btn = uex::CreateWidget(quit->cls); if (!btn) return;
    { Params t(uex::Fn(btn, L"SetButtonText")); t.Set(L"InText", uex::Text(L"多人游戏")); t.Call(btn); }
    // the menu sets these on its own buttons in the designer; a fresh button has the class defaults
    // (UnselectedOpacity 0.6 = the grey text, no text decorator change on select)
    for (auto n : {L"ChangeTextDecoratorOnSelected"}) BpSet<uint8_t>(btn, n, BpVar<uint8_t>(ng, n));
    for (auto n : {L"SelectedOpacity", L"UnselectedOpacity"}) BpSet<double>(btn, n, BpVar<double>(ng, n));
    if (!uex::Field<double>(btn, L"UnselectedOpacity")) uex::Log(L"ui: menu button has no UnselectedOpacity (game update?), entry may look grey");
    VSlot qv = ReadSlot(qs), nv = ReadSlot(ns);
    Call1(box, L"RemoveChild", L"Content", quit);
    WriteSlot(Add(box, btn, L"AddChildToVerticalBox"), nv);
    WriteSlot(Add(box, quit, L"AddChildToVerticalBox"), qv);
    // join the menu's button group so selecting another button deselects ours
    g.menuInGroup = false;
    if (auto grp = MenuGroup(menu)) { Call1(grp, L"AddWidget", L"InWidget", btn); g.menuInGroup = true; } else uex::Log(L"ui: start menu has no button group yet");
    g.menu = menu; g.menuBtn = btn; g.menuFont = nullptr; g.menuWas = false; g.menuFix = 0;
    RefreshActionIcon();
    uex::Log(L"ui: title menu entry added %s", NameOf(btn).c_str());
}
void PollMenuEntry() {
    if (!MenuEntryOk()) return;
    // a button made after the menu opened keeps the faded text of its intro animation: match the
    // others (text opacity 1); its deselect animation fades it again, so keep checking
    { if (auto tb = kBtnText.Of<UObject*>(g.menuBtn)) if (kOpacity.Of<float>(tb) < 0.99f) Call1<float>(tb, L"SetRenderOpacity", L"InOpacity", 1.f); }
    // the selected look switches the label to a subset of the same typeface that only has the glyphs
    // of the game's own menu ("多" is missing). Keep the full font (the unselected one), size untouched.
    if (auto tb = kBtnText.Of<UObject*>(g.menuBtn)) {
        auto fo = kTbFont.Of<UObject*>(tb);
        if (!g.menuFont && fo && !CallB(g.menuBtn, L"GetSelected")) g.menuFont = fo;
        if (g.menuFont && fo && fo != g.menuFont && uex::Alive(g.menuFont)) {
            Params p(uex::Fn(tb, L"SetFont")); uint8_t fi[kFontInfoBytes]; std::memcpy(fi, reinterpret_cast<uint8_t*>(tb) + kTbFont, sizeof fi);
            *reinterpret_cast<UObject**>(fi) = g.menuFont; p.Set(L"InFontInfo", fi); p.Call(tb);
        }
    }
    // the menu's button group may not exist yet when our button goes in: join it once it does
    if (!g.menuInGroup) if (auto grp = MenuGroup(g.menu)) {
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
    if (IsA(w, tb)) { extra = L" font=" + std::to_wstring(At<float>(w, kTbFont + kFontSize)); }
    fwprintf(f, L"%*s%s : %s vis=%d%s\n", d * 2, L"", NameOf(w).c_str(), NameOf(w->cls).c_str(), int(kVis.Of<uint8_t>(w)), extra.c_str());
    if (IsA(w, uw)) { if (auto t = kWidgetTree.Of<UObject*>(w)) DumpTree(f, kRootWidget.Of<UObject*>(t), d + 1); return; }
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
        else if (c == L"set") {
            auto p = a.find(L' '); std::wstring id = a.substr(0, p), v = p == std::wstring::npos ? L"" : a.substr(p + 1);
            if (g.edit.count(id)) WriteEdit(id.c_str(), v);
            else {  // a room setting: in the room the host sends it, else it is remembered for the next create
                net::Room r; bool in = net::GetRoom(r);
                Cfg cf = in ? FromRoom(r) : g_saved;
                if (in && r.you != r.host) fwprintf(o, L"not host\n");
                else if (!CfgSet(cf, id, v, in ? r.cap : net::kMaxPlayers)) fwprintf(o, L"no setting %s\n", id.c_str());
                else { if (in) SendCfg(cf); SaveCfg(cf); fwprintf(o, L"set %s = %s\n", id.c_str(), CfgValue(cf, id).c_str()); }
            }
        }
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
                if (wt.offset >= 0) if (auto t = At<UObject*>(k, wt.offset)) DumpTree(o, kRootWidget.Of<UObject*>(t), 1);
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
                    fwprintf(o, L"  %s : %s vp=%d vis=%d\n", NameOf(x).c_str(), NameOf(x->cls).c_str(), int(CallB(x, L"IsInViewport")), int(kVis.Of<uint8_t>(x))); }
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
                fwprintf(o, L"  %p %s list=%d active=%s\n", x, NameOf(x).c_str(), At<int32_t>(x, kWidgetList + kArrNum), w ? (NameOf(w) + L" : " + NameOf(w->cls)).c_str() : L"-"); }
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
        else if (c == L"grp") { auto m = FindUserWidget(L"WB_StartMenu_V2"); auto gr = m ? MenuGroup(m) : nullptr; int n = -1;
            if (gr) { Params p(uex::Fn(gr, L"GetButtonCount")); p.Call(gr); n = p.Get<int32_t>(L"ReturnValue"); }
            int idx = -9;
            if (gr && g.menuBtn) { Params q(uex::Fn(gr, L"FindButtonIndex")); q.Set(L"ButtonToFind", g.menuBtn); q.Call(gr); idx = q.Get<int32_t>(L"ReturnValue"); }
            fwprintf(o, L"group %p buttons %d ours at %d\n", gr, n, idx);
            if (gr && !a.empty()) {  // grp <i>: select button i through the group, report ours
                Params s(uex::Fn(gr, L"SelectButtonAtIndex")); s.Set<int32_t>(L"ButtonIndex", _wtoi(a.c_str())); s.Call(gr);
                fwprintf(o, L"selected %d, ours selected=%d\n", _wtoi(a.c_str()), int(CallB(g.menuBtn, L"GetSelected"))); } }
        else if (c == L"mdiff") {  // 4-byte words that differ between NewGame and our entry in 0x15c8..0x1960
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? MenuNewGame(menu) : nullptr;
            if (ng && g.menuBtn) for (int off = 0x15c8; off < 0x1960; off += 4) {
                uint32_t x = At<uint32_t>(ng, off), y = At<uint32_t>(g.menuBtn, off);
                if (x != y) fwprintf(o, L"  +%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(ng, off), y, At<float>(g.menuBtn, off)); }
            // the text blocks (ButtonTextBlock) and their slots: what lays out the label inside the button
            UObject *ta = ng ? kBtnText.Of<UObject*>(ng) : nullptr, *tb2 = g.menuBtn ? kBtnText.Of<UObject*>(g.menuBtn) : nullptr;
            if (ta && tb2) {
                fwprintf(o, L"  text %s / %s\n", NameOf(ta->cls).c_str(), NameOf(tb2->cls).c_str());
                for (int off = 0x28; off < 0x400; off += 4) { uint32_t x = At<uint32_t>(ta, off), y = At<uint32_t>(tb2, off);
                    if (x != y) fwprintf(o, L"  t+%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(ta, off), y, At<float>(tb2, off)); }
                UObject *sa = kSlot.Of<UObject*>(ta), *sb = kSlot.Of<UObject*>(tb2);
                if (sa && sb) { fwprintf(o, L"  tslot %s\n", NameOf(sa->cls).c_str());
                    for (int off = 0x28; off < 0x80; off += 4) { uint32_t x = At<uint32_t>(sa, off), y = At<uint32_t>(sb, off);
                        if (x != y) fwprintf(o, L"  s+%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(sa, off), y, At<float>(sb, off)); } }
            }
        }
        else if (c == L"mtree" && g.menuBtn) {  // inner widgets of NewGame vs ours: render offset, opacity, slot padding
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? MenuNewGame(menu) : nullptr;
            for (UObject* b : { ng, g.menuBtn }) { if (!b) continue; auto tr = kWidgetTree.Of<UObject*>(b);
                fwprintf(o, L" [%s]\n", NameOf(b).c_str());
                for (int i = 0, n = NumObjects(); tr && i < n; ++i) { auto x = ObjectAt(i);
                    if (!x || x->outer != tr || (x->flags & 0x10)) continue;
                    auto sl = kSlot.Of<UObject*>(x); if (!sl) continue;
                    int po = InnerPadOff(sl); Margin pd = po ? At<Margin>(sl, po) : Margin{};
                    struct V2 { double x, y; }; Params q(uex::Fn(x, L"GetDesiredSize")); q.Call(x); auto ds = q.Get<V2>(L"ReturnValue");
                    fwprintf(o, L"  %s:%s vis=%d clip=%d ds=%.1f,%.1f pad=%.1f,%.1f,%.1f,%.1f\n", NameOf(x).c_str(), NameOf(x->cls).c_str(),
                             int(kVis.Of<uint8_t>(x)), int(kClip.Of<uint8_t>(x)), ds.x, ds.y, pd.l, pd.t, pd.r, pd.b); } } }
        else if (c == L"miaw" && g.menuBtn) {  // InputActionWidget of NewGame vs ours: differing words
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? MenuNewGame(menu) : nullptr;
            auto find = [](UObject* b) -> UObject* { auto tr = kWidgetTree.Of<UObject*>(b);
                for (int i = 0, n = NumObjects(); tr && i < n; ++i) { auto x = ObjectAt(i);
                    if (x && x->outer == tr && !(x->flags & 0x10) && NameOf(x) == L"InputActionWidget") return x; } return nullptr; };
            UObject *ia = ng ? find(ng) : nullptr, *ib = find(g.menuBtn);
            if (ib && !a.empty()) { Params q(uex::Fn(ib, a.c_str())); q.Call(ib); fwprintf(o, L"called %s\n", a.c_str()); }
            else if (ia && ib) { DumpProps(o, ia->cls);
                for (int off = 0x28; off < 0x600; off += 4) { uint32_t x = At<uint32_t>(ia, off), y = At<uint32_t>(ib, off);
                    if (x != y) fwprintf(o, L"  +%x ng=%08x (%.2f) ours=%08x (%.2f)\n", off, x, At<float>(ia, off), y, At<float>(ib, off)); } } }
        else if ((c == L"mcall" || c == L"ncall") && g.menuBtn) {  // mcall/ncall <fn>: no-arg fn on ours / NewGame
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* t = c == L"mcall" ? g.menuBtn : menu ? MenuNewGame(menu) : nullptr;
            if (auto f = t ? uex::Fn(t, a.c_str()) : nullptr) { Params q(f); q.Call(t); fwprintf(o, L"called %s\n", a.c_str()); } else fwprintf(o, L"no fn\n"); }
        else if (c == L"mfont" && g.menuBtn) {  // font of the label of NewGame vs ours (object, typeface, size, style classes)
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); UObject* ng = menu ? MenuNewGame(menu) : nullptr;
            for (UObject* b : { ng, g.menuBtn }) { auto tb = b ? kBtnText.Of<UObject*>(b) : nullptr; if (!tb) continue;
                auto fo = kTbFont.Of<UObject*>(tb); auto st = kCtStyle.Of<UObject*>(tb), ld = kLocDeco.Of<UObject*>(tb);
                fwprintf(o, L"  %s sel=%d font=%s face=%s size=%.1f style=%s deco=%s decoOn=%d\n", NameOf(b).c_str(), int(CallB(b, L"GetSelected")),
                         fo ? PathOf(fo).c_str() : L"-", NameOf(*reinterpret_cast<const FName*>(reinterpret_cast<const uint8_t*>(tb) + kTbFont + kFontFace)).c_str(), At<float>(tb, kTbFont + kFontSize),
                         st ? NameOf(st).c_str() : L"-", ld ? NameOf(ld).c_str() : L"-", int(BpVar<uint8_t>(b, L"ChangeTextDecoratorOnSelected"))); } }
        else if (c == L"mtext" && g.menuBtn) {  // mtext <text>: relabel our title entry (layout checks)
            Params t(uex::Fn(g.menuBtn, L"SetButtonText")); t.Set(L"InText", uex::Text(a.c_str())); t.Call(g.menuBtn); }
        else if (c == L"mbtn") {  // menu buttons of the title menu: opacity, enabled flags, text opacity
            auto menu = FindUserWidget(L"WB_StartMenu_V2"); auto tree = menu ? kWidgetTree.Of<UObject*>(menu) : nullptr;
            for (int i = 0, n = NumObjects(); tree && i < n; ++i) { auto x = ObjectAt(i);
                if (!x || (x->flags & 0x10) || x->cls != (g.menuBtn ? g.menuBtn->cls : nullptr)) continue;
                if (x->outer != tree && x != g.menuBtn) continue;
                auto tb = kBtnText.Of<UObject*>(x);
                fwprintf(o, L"  %s op=%.2f d9=%02x sel=%.2f unsel=%.2f dis=%d tbop=%.2f\n", NameOf(x).c_str(), kOpacity.Of<float>(x), At<uint8_t>(x, kEnabled),
                         BpVar<double>(x, L"SelectedOpacity"), BpVar<double>(x, L"UnselectedOpacity"), int(BpVar<uint8_t>(x, L"IsDisabled")), tb ? kOpacity.Of<float>(tb) : -1.f);
                // render translation (RenderTransform) of the button and its text, slot padding, parent class
                auto sl = kSlot.Of<UObject*>(x); auto par = sl ? kSlotParent.Of<UObject*>(sl) : nullptr; Margin pd = sl ? kVsPad.Of<Margin>(sl) : Margin{};
                int tr = kXform + kXfTrans, sc = kXform + kXfScale;  // Translation x, y (doubles), Scale x
                fwprintf(o, L"     tr=%.1f,%.1f sc=%.2f tbtr=%.1f,%.1f tbsc=%.2f pad=%.0f,%.0f,%.0f,%.0f parent=%s\n", At<double>(x, tr), At<double>(x, tr + 8), At<double>(x, sc),
                         tb ? At<double>(tb, tr) : 0, tb ? At<double>(tb, tr + 8) : 0, tb ? At<double>(tb, sc) : 0, pd.l, pd.t, pd.r, pd.b, par ? NameOf(par->cls).c_str() : L"-"); }
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
            auto pi = uex::PC() ? kPlayerInput.Of<UObject*>(uex::PC()) : nullptr;
            auto m = pi ? kActionMappings.Of<A>(pi) : A{};
            fwprintf(o, L"player input %p, %d mappings\n", pi, m.n);
            for (int i = 0; i < m.n && i < 1024; ++i) {
                auto e = m.d + i * MappingSize(); auto act = kMapAction.Of<UObject*>(e); auto key = NameOf(kMapKey.Of<FName>(e));
                if (key == L"Escape" || key == L"F24" || key == L"M" || key == L"Tab" || key == L"I" || key == L"Gamepad_Special_Right")
                    fwprintf(o, L"  %s -> %s\n", key.c_str(), act ? NameOf(act).c_str() : L"-");
            }
        }
        else if (c == L"hudtest") { g.hudTest = a != L"0"; g.hudMode = a == L"ffa" || a == L"kill" ? uex::Utf8(a) : "duel"; }  // fake room HUD
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
    if (want && g.menuOff != menu) { g.menuVis = kVis.Of<uint8_t>(menu); Vis(menu, kHidden); g.menuOff = menu; g.menuOffRef = uex::Hold(menu); }  // a hidden menu cannot hold or take focus
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
    if (g.hudTest && !inRoom) {  // fake room in its fight phase (hudtest duel|ffa|kill)
        r.you = 0; r.host = 0; r.phase = "fight"; r.round = 2; r.rounds = 3; r.leftMs = 47000; r.recvAt = net::Now();
        r.mode = g.hudMode; r.kills = 10;
        int n = r.mode == "duel" ? 2 : 6;
        const char* names[] = {"甲A", "乙B", "丙C", "丁D", "戊E", "己F", "庚G", "辛H"};
        for (int i = 0; i < n; ++i) { net::Member m; m.slot = i; m.name = names[i]; m.score = (i * 3 + 1) % 5; m.deaths = i % 3; m.alive = i != 3; m.conn = true; r.members.push_back(m); }
        r.max = n; inRoom = true;
    }
    if (inRoom && g_applyRoom && r.you >= 0) {  // our create came through: the host's remembered settings
        g_applyRoom = false;
        if (r.you == r.host && r.phase == "lobby") { Cfg c = g_saved; if (c.mode != "duel") c.max = std::clamp(c.max, 2, (std::max)(2, r.cap)); SendCfg(c); }
    }
    if (!inRoom && net::GetStatus() != net::Status::Connecting) g_applyRoom = false;
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
    if (match) g.cfgOpen = false;
    bool cfg = g.open && g.cfgOpen;
    Vis(g.connect, g.open && !inRoom && !cfg ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.lobby, g.open && inRoom && !match && !cfg ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.cfg, cfg ? kSelfHitTestInvisible : kCollapsed);
    Vis(g.promo, g.open && inRoom && !match && !cfg ? kHitTestInvisible : kCollapsed);
    if (cfg) UpdateCfg(r, inRoom);
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
// (rewritten every 5 s with the time). A restarted game that finds a fresh one resumes the match
// (one in the root, left by an old flat install, is read and removed the same way).
std::wstring SessionPath() { auto d = ModDir(); return d.empty() ? L"" : d + L"mp_session.txt"; }
void SessionTick(bool match) {
    static bool tried;
    if (!tried) {  // first tick: resume a match the last run was in (crash, killed process)
        tried = true;
        auto p = SessionPath(); if (!p.empty()) p = ModFileIn(L"mp_session.txt");
        FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"r, ccs=UTF-8");
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
        if (auto grp = MenuGroup(g.menu)) Call1(grp, L"RemoveWidget", L"InWidget", g.menuBtn);
        uex::RemoveWidget(g.menuBtn);
    }
    if (Valid()) uex::RemoveWidget(g.root);
    if (g.uiMode && uex::Me()) uex::GameMode();
    uex::BlockMenuKeys(false);
    g = G{};
}

bool InMenu() { return g.open && g.root != nullptr; }
void SetModDir(const wchar_t* dir) {
    g_modDir = dir ? dir : L"";
    if (!g_modDir.empty() && g_modDir.back() != L'\\' && g_modDir.back() != L'/') g_modDir += L'\\';
}
}  // namespace ui

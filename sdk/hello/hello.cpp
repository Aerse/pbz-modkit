// hello.cpp: a mod written with the framework (framework/uex.h). Build with build.bat, copy
// hello.dll and hello.txt into <game>\platform\steam\pbzmp\hello\, tick it in Mod管理器.
// In game, Ctrl+F10 logs the player's health, posture, position and walk speed to loader.log.
// Shows: no memory addresses anywhere. Engine globals come from the loader, fields are looked up
// by name through reflection, functions are called by name. A game update needs no change here.
#include <windows.h>
#include <cmath>
#include "uex.h"

using ue::UObject;
using ue::Params;

static const PbzHost* g_host;
static int g_hot;

// A field of a native class, found by name the first time it is used. The number is only a
// fallback for the build this was written for; if the game moves the field, loader.log says
// "field Character.CharacterMovement moved: 0x.. (was 0x338)" and the mod keeps working.
static uex::Prop kMove{L"Class /Script/Engine.Character", L"CharacterMovement", 0x338};

static void OnHotkey(void*) {
    UObject* me = uex::Me();  // the local player's pawn, null in menus and loading screens
    if (!me) { uex::Log(L"no player yet"); return; }

    // attributes (BasicAttributeSet) by name: Health, Shachi (posture), Ult, ...
    float hp = uex::Attr(me, uex::attr::Health), hpMax = uex::Attr(me, uex::attr::HealthMax);
    float po = uex::Attr(me, uex::attr::Shachi), poMax = uex::Attr(me, uex::attr::ShachiMax);

    // a UFunction by name: parameters are set and read by name too
    uex::Vec at = uex::Loc(me);  // K2_GetActorLocation inside
    Params p(uex::Fn(me, L"GetVelocity"));
    p.Call(me);
    auto v = p.Get<uex::Vec>(L"ReturnValue");

    // a field through a Prop, and one on any object by name (uex::Field, cached per class)
    UObject* cm = kMove.Of<UObject*>(me);
    float* walk = cm ? uex::Field<float>(cm, L"MaxWalkSpeed") : nullptr;

    uex::Log(L"%s hp %.0f/%.0f posture %.0f/%.0f at (%.0f, %.0f, %.0f) speed %.0f max walk %.0f",
             ue::NameOf(me).c_str(), hp, hpMax, po, poMax, at.x, at.y, at.z, std::hypot(v.x, v.y),
             walk ? *walk : -1.f);
}

extern "C" __declspec(dllexport) void PbzInit(const PbzHost* host) {
    g_host = host;
    uex::SetHost(host);
    uex::SetLogName(L"hello");
    if (host->version < 3) { host->log(L"hello: needs loader api 3"); return; }
    g_hot = host->addHotkey(VK_F10, kPbzHotCtrl, OnHotkey, nullptr);
    if (!g_hot) uex::Log(L"Ctrl+F10 belongs to another mod");
}

// dir: this mod's own folder (pbzmp\hello\), for its settings and resources
extern "C" __declspec(dllexport) void PbzRun(const wchar_t* dir) { uex::Log(L"loaded from %s", dir); }

// nothing to undo: the hotkey is removed by the loader
extern "C" __declspec(dllexport) void PbzUnload() { uex::Log(L"unloaded"); }

// uex: shared UE helpers for mp.dll (ui/, arena/, fight/, flow/). Game thread only.
// Offsets are tied to the current game build. Rules: nothing here allocates in the game heap per frame except
// Text() on a new string (cached) and console commands; never call FString-returning UE
// functions per frame (their results leak: our param buffers run no destructors).
#pragma once
#include <cstdint>
#include <string>
#include "ue.h"
#include "ue_call.h"
#include "../loader/host.h"

namespace uex {
using ue::UObject;
struct Vec { double x = 0, y = 0, z = 0; };
struct Rot { double pitch = 0, yaw = 0, roll = 0; };
struct FText { uint8_t b[16]; };  // opaque, owned by the game

void SetHost(const PbzHost* h);
const PbzHost* Host();
void Log(const wchar_t* fmt, ...);  // loader.log, prefixed "mp: "

// static library objects ("Default__X"), cached
UObject* Lib(const wchar_t* fullName);
UObject* GameplayStatics();
UObject* SystemLib();
UObject* Class(const wchar_t* fullName);  // e.g. L"Class /Script/Engine.Actor"

// world / player, cheap (pointer chasing only)
UObject* Engine();
UObject* GameInstance();
UObject* World();
UObject* PC();      // first local PlayerController
UObject* Me();      // its pawn (PBZCharacter_Player), null in menus / loading
bool Alive(UObject* o);  // still in the object array and not being destroyed
// a kept object pointer that remembers which object it was: freed objects' addresses and array
// slots are reused, so a raw pointer can later point at a different (live) object
struct Ref { UObject* o = nullptr; int32_t index = -1; uint32_t name = 0, number = 0; UObject* cls = nullptr; };
Ref Hold(UObject* o);
UObject* Get(const Ref& r);  // the object while it is still that one and alive, else null

// generic call: fn found up the class chain, cached per (class, name)
UObject* Fn(UObject* self, const wchar_t* name);

// diagnostics: inside this scope an access violation (any structured exception) becomes a C++
// exception that records where it happened; catch(...) as before, then log ExcWhere()
struct SehScope { void* prev; SehScope(); ~SehScope(); SehScope(const SehScope&) = delete; };
std::wstring ExcWhere();  // last one caught: "seh <code> at module+rva < frames", "c++" for a throw; resets

// actors
UObject* Spawn(UObject* cls, Vec loc, double yaw, UObject* owner = nullptr);
void Destroy(UObject* actor);
Vec Loc(UObject* actor);
Rot GetRot(UObject* actor);
bool Teleport(UObject* actor, Vec loc, double yaw);  // K2_SetActorLocationAndRotation, teleport
void SetControlYaw(double yaw);                      // player's camera/controller
UObject* Comp(UObject* actor, UObject* compCls);     // GetComponentByClass
bool Trace(UObject* ctx, Vec a, Vec b, Vec* hit);     // Visibility line trace, ignores ctx

// attributes (BasicAttributeSet at PBZCharacter +0x758; offset = attribute struct offset)
namespace attr { constexpr int Health = 0x58, HealthMax = 0x68, HealthMin = 0xd78, Shield = 0x88,
    Attack = 0xc8, Shachi = 0x638, ShachiMax = 0x658, Ult = 0x898, UltMax = 0x8a8; }
float Attr(UObject* ch, int off);               // current value
void SetAttr(UObject* ch, int off, float v);    // base and current

// assets, console
UObject* LoadAsset(const wchar_t* path);  // "/Game/.../X.X" soft path, blocking, cached
void Console(const wchar_t* cmd);

// strings
FText Text(const std::wstring& s);   // cached per string
ue::FName Name(const std::wstring& s);  // cached
std::string Utf8(const std::wstring& s);
std::wstring Wide(const std::string& s);

// widgets
UObject* CreateWidget(UObject* cls);
void AddToViewport(UObject* w, int z);
void RemoveWidget(UObject* w);
void SetText(UObject* textBlock, const std::wstring& s);  // skips if unchanged
void ForgetTexts();  // drop SetText's cache (rebuilt widgets can reuse old addresses)
void SetVisible(UObject* w, bool v);  // Visible / Collapsed
void UiMode(UObject* focus);  // UIOnlyEx + cursor
void GameMode();              // GameOnly, no cursor

// game systems
void SaveVeto(bool on, const wchar_t* source = L"pbzmp");  // PBZSaveLoadSubsystem, reason ModeGate; one veto per source
void Hostile(UObject* a, UObject* b, bool on); // faction override both ways / clear
bool Ability(UObject* ch, UObject* abilityCls);
void IgnoreInput(bool move, bool look);       // idempotent: tracks our own state
UObject* Movement(UObject* ch);                // CharacterMovementComponent
void SetMoveMode(UObject* ch, int mode);       // 0 none (frozen, no gravity), 1 walking, 3 falling
// on: the game's pause/system menu (Esc, I, gamepad menu) and map (M, Tab) keys do nothing (rescans
// every few seconds for contexts loaded later); off: puts them back. Idempotent.
void BlockMenuKeys(bool on);
}  // namespace uex

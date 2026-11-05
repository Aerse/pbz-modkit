// uex: shared UE helpers for mods (pvp, sniper, your own). Game thread only.
// Fields are found by name through reflection (Prop, Field below), so a game update that moves them
// needs no code change; the numbers next to the names are only fallbacks. Rules: nothing here allocates in the game heap per frame except
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
void Log(const wchar_t* fmt, ...);  // one line in loader.log, prefixed "<name>: " (default "mp: ")
void SetLogName(const wchar_t* name);  // your mod's log prefix, e.g. L"hello"

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

// ---- reflection: fields by name ----------------------------------------------------------
// A Prop is a field of a native class or struct ("Class /Script/Engine.Controller",
// "ScriptStruct /Script/Engine.HitResult"), looked up by name on first use and cached. The fallback
// is the offset in the build the mod was written for: used, and logged once, only when the field is
// gone. A changed offset is logged once too ("field X.Y moved"), handy after a game update.
//   static uex::Prop kPawn{L"Class /Script/Engine.Controller", L"Pawn", 0x2f0};
//   UObject* pawn = kPawn.Of<UObject*>(pc);
// BoolProperty: give the fallback mask (4th value); Mask() then is the field's bit.
// Every Prop is also checked once right after the mod loads (on the first engine tick after
// SetHost); loader.log then has one line: "fields <mod>: 120 checked, 0 moved, 0 missing".
struct Prop {
    enum State : uint8_t { kUnchecked, kOk, kMoved, kMissing, kNoClass };
    const wchar_t* owner; const wchar_t* name; int fallback; uint8_t fallbackMask;
    int off = -1; uint8_t mask = 0; bool done = false; State state = kUnchecked; uint64_t retryAt = 0;
    Prop(const wchar_t* owner, const wchar_t* name, int fallback, uint8_t fallbackMask = 0);
    Prop(const Prop&) = delete;
    Prop& operator=(const Prop&) = delete;
    int Resolve();
    operator int() { return done ? off : Resolve(); }
    uint8_t Mask() { if (!done) Resolve(); return mask; }
    template <class T> T& Of(const void* o) {
        return *reinterpret_cast<T*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(o)) + int(*this));
    }
    bool Bit(const void* o) { int k = *this; return (static_cast<const uint8_t*>(o)[k] & mask) != 0; }
    void SetBit(void* o, bool v) { int k = *this; auto& b = static_cast<uint8_t*>(o)[k]; b = v ? (b | mask) : (b & ~mask); }
};
int StructSize(const wchar_t* owner, int fallback);  // PropertiesSize of a native struct, cached
// Resolves every Prop of this mod made so far (those at namespace scope, and function-local
// statics already reached). Runs by itself after SetHost; call it again to re-check.
struct FieldStats { int checked, moved, missing, noClass; };
FieldStats CheckFields(bool log = true);
// any object's field by name (its own class, up the Super chain), cached per class; -1 / null if none
int OffsetOf(UObject* cls, const wchar_t* prop);
template <class T> T* Field(UObject* o, const wchar_t* prop) {
    int k = o ? OffsetOf(o->cls, prop) : -1;
    return k < 0 ? nullptr : reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(o) + k);
}

// ---- engine containers -------------------------------------------------------------------
// Native layouts (fixed by the engine version, not by game updates), in one place.
// TSet / TMap: a TSparseArray of elements (data, num slots, max) plus allocation bits; a TMap<K, V>
// element is the pair then two int32 hash links: MapStride(sizeof K, sizeof V) bytes.
namespace layout {
inline constexpr int kSetNum = 0x8, kSetMax = 0xc, kSetBitsInline = 0x10, kSetBitsHeap = 0x20, kSetNumBits = 0x28, kSetNumFree = 0x34;
inline constexpr int kSoftPkg = 0x8, kSoftAsset = 0x10;  // TSoftObjectPtr: weak ptr, then FSoftObjectPath package / asset FName
}
constexpr int MapStride(int key, int value) { return ((key + value + 7) & ~7) + 8; }
// f(element) for each live element (stride bytes each) of the TSet / TMap at `set`; false (and no
// calls) if it does not look like one. limit: the most slots it will believe in.
template <class F> bool EachSetElem(const void* set, int stride, F f, int limit = 65536) {
    using namespace layout;
    auto m = static_cast<const uint8_t*>(set);
    auto data = ue::At<uint8_t*>(m, 0);
    int num = ue::At<int32_t>(m, kSetNum), max = ue::At<int32_t>(m, kSetMax), nbits = ue::At<int32_t>(m, kSetNumBits),
        nfree = ue::At<int32_t>(m, kSetNumFree);
    if (num < 0 || num > limit || max < num || (num && !data) || nfree < 0 || nfree > num || (nfree && nbits < num)) return false;
    auto bits = ue::At<const uint32_t*>(m, kSetBitsHeap);
    if (!bits) bits = reinterpret_cast<const uint32_t*>(m + kSetBitsInline);
    for (int k = 0; k < num; ++k)
        if (!nfree || ((bits[k >> 5] >> (k & 31)) & 1)) f(data + size_t(k) * stride);  // skip holes
    return true;
}
// a DataTable's rows: f(const FName& rowName, uint8_t* row). RowMap (native TMap<FName, uint8*>)
// sits right after the reflected RowStruct.
template <class F> bool EachRow(UObject* dt, F f) {
    static Prop kRowStruct{L"Class /Script/Engine.DataTable", L"RowStruct", 0x28};
    if (!dt) return false;
    return EachSetElem(reinterpret_cast<uint8_t*>(dt) + kRowStruct + 8, MapStride(8, 8), [&](uint8_t* e) {
        if (auto row = ue::At<uint8_t*>(e, 8)) f(*reinterpret_cast<const ue::FName*>(e), row);
    });
}
// a TSoftObjectPtr / TSoftClassPtr field -> "/Pkg/Path.Asset", "" if unset
std::wstring SoftPath(const void* field);

// attributes: BasicAttributeSet fields (FGameplayAttributeData: base +8, current +0xC), by name.
// Use them where an int offset goes: Attr(ch, attr::Health), attr::Shachi + 0x10 (not constexpr).
namespace attr {
inline constexpr const wchar_t* kSet = L"Class /Script/CharacterBasics.BasicAttributeSet";
inline Prop Level{kSet, L"Level", 0x48}, Health{kSet, L"Health", 0x58}, HealthMax{kSet, L"Health_Max", 0x68},
    HealthMin{kSet, L"Health_Min", 0xd78}, Shield{kSet, L"Shield", 0x88}, Attack{kSet, L"Attack", 0xc8},
    Shachi{kSet, L"Shachi", 0x638}, ShachiMin{kSet, L"Shachi_Min", 0x648}, ShachiMax{kSet, L"Shachi_Max", 0x658},
    ShachiRegen{kSet, L"ShachiRegen", 0x678}, ShachiRegenCooldown{kSet, L"ShachiRegenCooldown", 0x688},
    Ult{kSet, L"UltimateCharge", 0x898}, UltMax{kSet, L"UltimateCharge_Max", 0x8a8},
    Spirit{kSet, L"SpiritShadow", 0x8f8}, SpiritMax{kSet, L"SpiritShadow_Max", 0x908},
    AccFirst{kSet, L"Ignite_Accumulation", 0x918}, AccLast{kSet, L"Water_Accumulation", 0x988},  // *_Accumulation, 0x10 apart
    FinalWalkSpeed{kSet, L"FinalWalkSpeed", 0xdc8};
}
uint8_t* AttrSet(UObject* ch);                  // PBZCharacter.AttributeSetBase, null if none
float Attr(UObject* ch, int off);               // current value
void SetAttr(UObject* ch, int off, float v);    // base and current
UObject* ASC(UObject* ch);                      // PBZCharacter.AbilitySystemComponent
// gameplay tags the character owns (its ASC's GameplayTagCountContainer.ExplicitTags), game memory:
// read it right away, it changes as tags come and go
struct TagList { const ue::FName* d = nullptr; int n = 0; };
TagList OwnedTags(UObject* ch);
bool HasTag(UObject* ch, const wchar_t* tag);   // exact match, e.g. L"CharacterState.Stand.Break"

// assets, console
UObject* LoadAsset(const wchar_t* path);  // "/Game/.../X.X" soft path, blocking, cached
void Console(const wchar_t* cmd);
void OpenLevel(const wchar_t* level);

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
// an InputMappingContext's key mappings (DefaultKeyMappings.Mappings), by reflection. Setting an
// entry's action to null and rebuilding disables that key; put the action back to restore it.
int KeyMappingCount(UObject* imc);
UObject*& KeyMappingAction(UObject* imc, int i);  // i < KeyMappingCount
ue::FName KeyMappingKey(UObject* imc, int i);      // FKey's name ("LeftMouseButton", "R", ...)
void RebuildMappings(UObject* imc);                // apply changes now
}  // namespace uex

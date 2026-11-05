// Minimal UE5.7 runtime access for PBZ. The engine globals (object array, name pool, ProcessEvent
// slot) come from the loader, which finds them at every start (host.h v3), so they follow game
// updates. The UObject / UStruct / FProperty layout below is UE5.7's and only changes with the engine.
#pragma once
#include <cstdint>
#include <string>

namespace ue {
// Where the engine globals are. Filled when the module loads: from the loader's PbzHost (found
// through the PBZMP_HOST variable, so also for modules that never get PbzInit), else the values of
// the 2026-11-04 build.
struct Globals {
    uintptr_t objects;      // FUObjectItem** chunk table (GUObjectArray.Objects)
    uintptr_t numElements;  // int32
    uintptr_t namePool;     // FNamePool
    int processEvent;       // UObject::ProcessEvent vtable index
    bool fromLoader;        // false: built-in values (old loader)
};
Globals LoadGlobals();
inline const Globals globals = LoadGlobals();
constexpr int kItemSize = 24, kItemObjOff = 8, kChunk = 65536;

struct FString { wchar_t* data; int32_t num; int32_t max; };
struct FName { uint32_t comparison; uint32_t number; };

struct UObject {
    void** vtable;   // 0x00
    int32_t flags;   // 0x08
    int32_t index;   // 0x0C
    UObject* cls;    // 0x10
    FName name;      // 0x18
    UObject* outer;  // 0x20
};

// UField / UStruct (layout reordered vs stock UE)
constexpr int kUField_Next          = 0x28;
constexpr int kUStruct_Super        = 0x40;
constexpr int kUStruct_Children     = 0x48;  // UField* (functions)
constexpr int kUStruct_ChildProps   = 0x50;  // FField*
constexpr int kUStruct_PropsSize    = 0xA8;  // int32
constexpr int kUStruct_MinAlign     = 0xAC;  // int16

// FField / FProperty
constexpr int kFField_Class  = 0x08;  // FFieldClass*
constexpr int kFField_Next   = 0x10;
constexpr int kFField_Owner  = 0x18;  // tagged pointer (bit0 = UObject owner)
constexpr int kFField_Name   = 0x20;
constexpr int kFField_Flags  = 0x28;
constexpr int kProp_ArrayDim = 0x30;
constexpr int kProp_ElemSize = 0x34;
constexpr int kProp_Flags    = 0x38;  // uint64
constexpr int kProp_Offset   = 0x44;  // int32
constexpr int kProp_Extra    = 0x70;  // first subclass field (PropertyClass / Struct / Inner / bool masks)

template <class T> inline T At(const void* p, int off) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(p) + off);
}

inline int32_t NumObjects() { return *reinterpret_cast<int32_t*>(globals.numElements); }
inline UObject* ObjectAt(int32_t i) {
    auto chunks = *reinterpret_cast<uint8_t***>(globals.objects);
    uint8_t* c = chunks[i / kChunk];
    if (!c) return nullptr;
    return *reinterpret_cast<UObject**>(c + (i % kChunk) * kItemSize + kItemObjOff);
}

std::wstring NameOf(const FName& n);
bool NameIs(const FName& n, const std::wstring& s);  // no allocation for numberless names
std::wstring NameOf(const UObject* o);
std::wstring PathOf(const UObject* o);   // Outer.Outer.Name
std::wstring FullName(const UObject* o); // ClassName Path
UObject* FindObject(const std::wstring& fullName);
}  // namespace ue

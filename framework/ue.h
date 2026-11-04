// Minimal UE5.7 runtime access for PBZ (CL-301524_JKS-17219). Offsets are tied to this game build.
#pragma once
#include <cstdint>
#include <string>

namespace ue {
constexpr uintptr_t kObjObjects    = 0x14ddb3c58;  // FUObjectItem** chunk table
constexpr uintptr_t kNumElements   = 0x14ddb3c60;  // int32
constexpr uintptr_t kFNameToString = 0x14144d2c0;  // void(const FName*, FString*); allocates, don't use
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

inline int32_t NumObjects() { return *reinterpret_cast<int32_t*>(kNumElements); }
inline UObject* ObjectAt(int32_t i) {
    auto chunks = *reinterpret_cast<uint8_t***>(kObjObjects);
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

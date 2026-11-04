// Reflection-driven function calls through UObject::ProcessEvent.
#pragma once
#include <cstring>
#include <string>
#include <vector>
#include "ue.h"

namespace ue {
constexpr int kVT_ProcessEvent = 0x4c;        // vtable index (found by scanning for FUNC_Native test)
constexpr int kUFunction_Flags = 0x100;

using ProcessEventFn = void(__fastcall*)(UObject*, UObject*, void*);

inline bool IsA(const UObject* o, const UObject* cls) {
    for (auto c = o ? o->cls : nullptr; c; c = At<UObject*>(c, kUStruct_Super))
        if (c == cls) return true;
    return false;
}

// Walks Children (UFunctions) up the Super chain.
inline UObject* FindFunction(const UObject* cls, const std::wstring& name) {
    for (auto c = cls; c; c = At<UObject*>(c, kUStruct_Super))
        for (auto f = At<UObject*>(c, kUStruct_Children); f; f = At<UObject*>(f, kUField_Next))
            if (NameIs(f->name, name)) return f;
    return nullptr;
}

struct PropInfo { int offset = -1; int size = 0; };

inline PropInfo FindProp(const UObject* st, const std::wstring& name) {
    for (auto c = st; c; c = At<UObject*>(c, kUStruct_Super))
        for (auto p = At<const uint8_t*>(c, kUStruct_ChildProps); p; p = At<const uint8_t*>(p, kFField_Next))
            if (NameIs(*reinterpret_cast<const FName*>(p + kFField_Name), name))
                return {At<int32_t>(p, kProp_Offset), At<int32_t>(p, kProp_ElemSize) * At<int32_t>(p, kProp_ArrayDim)};
    return {};
}

// Parameter block sized from the function's own properties.
class Params {
public:
    explicit Params(UObject* fn) : fn_(fn) {
        int end = 0;
        for (auto p = fn ? At<const uint8_t*>(fn, kUStruct_ChildProps) : nullptr; p; p = At<const uint8_t*>(p, kFField_Next)) {
            int e = At<int32_t>(p, kProp_Offset) + At<int32_t>(p, kProp_ElemSize) * At<int32_t>(p, kProp_ArrayDim);
            if (e > end) end = e;
        }
        buf_.assign((end + 15) & ~15, 0);
    }
    template <class T> bool Set(const std::wstring& name, const T& v) {
        auto pi = FindProp(fn_, name);
        if (pi.offset < 0) return false;
        std::memcpy(buf_.data() + pi.offset, &v, sizeof(T) < size_t(pi.size) ? sizeof(T) : pi.size);
        return true;
    }
    template <class T> T Get(const std::wstring& name) const {
        T v{};
        auto pi = FindProp(fn_, name);
        if (pi.offset >= 0) std::memcpy(&v, buf_.data() + pi.offset, sizeof(T));
        return v;
    }
    void Call(UObject* self) {
        if (!fn_ || !self) return;  // missing function or object: nothing to call
        auto pe = reinterpret_cast<ProcessEventFn>(self->vtable[kVT_ProcessEvent]);
        pe(self, fn_, buf_.data());
    }
    int Size() const { return int(buf_.size()); }

private:
    UObject* fn_;
    std::vector<uint8_t> buf_;
};
}  // namespace ue

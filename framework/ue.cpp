#include "ue.h"
#include <windows.h>
#include <unordered_map>

namespace ue {
// Names are read straight from the game's FNamePool, so nothing is allocated in the game's heap.
// (Calling FName::ToString returned a game-allocated FString per call; never freeing it leaked
// tens of MB per FindObject, about 220 GB in an hour of tests.)
// Pool layout: +0x08 current block (u32), +0x10 block pointers. An entry is 2-byte aligned:
// u16 header (length << 6 | wide flag in bit 0), then the characters.
constexpr uintptr_t kNamePool = 0x14dd11ac0;

static const uint8_t* Entry(uint32_t id) {
    auto pool = reinterpret_cast<const uint8_t*>(kNamePool);
    uint32_t block = id >> 16;
    if (block > *reinterpret_cast<const uint32_t*>(pool + 8)) return nullptr;
    auto base = *reinterpret_cast<const uint8_t* const*>(pool + 0x10 + 8 * size_t(block));
    return base ? base + 2 * size_t(id & 0xffff) : nullptr;
}

std::wstring NameOf(const FName& n) {
    std::wstring s;
    auto e = Entry(n.comparison);
    if (!e) return s;
    uint16_t h = *reinterpret_cast<const uint16_t*>(e);
    size_t len = h >> 6;
    if (h & 1) s.assign(reinterpret_cast<const wchar_t*>(e + 2), len);
    else { s.resize(len); for (size_t i = 0; i < len; ++i) s[i] = wchar_t(e[2 + i]); }
    if (n.number) { s += L'_'; s += std::to_wstring(n.number - 1); }
    return s;
}

bool NameIs(const FName& n, const std::wstring& str) {
    if (n.number) return NameOf(n) == str;
    auto e = Entry(n.comparison);
    if (!e) return false;
    uint16_t h = *reinterpret_cast<const uint16_t*>(e);
    size_t len = h >> 6;
    if (len != str.size()) return false;
    if (h & 1) return wmemcmp(reinterpret_cast<const wchar_t*>(e + 2), str.data(), len) == 0;
    for (size_t i = 0; i < len; ++i) if (wchar_t(e[2 + i]) != str[i]) return false;
    return true;
}

std::wstring NameOf(const UObject* o) { return o ? NameOf(o->name) : L"None"; }

std::wstring PathOf(const UObject* o) {
    if (!o) return L"None";
    std::wstring path = NameOf(o);
    for (auto p = o->outer; p; p = p->outer) path = NameOf(p) + L"." + path;
    return path;
}

std::wstring FullName(const UObject* o) {
    if (!o) return L"None";
    return NameOf(o->cls) + L" " + PathOf(o);
}

// Cached by full name; an entry is checked (same slot, same name) before use, so an unloaded
// object is never returned. Misses are not cached: an asset may load later.
UObject* FindObject(const std::wstring& fullName) {
    static std::unordered_map<std::wstring, std::pair<UObject*, int32_t>> cache;
    if (auto it = cache.find(fullName); it != cache.end()) {
        auto [o, i] = it->second;
        if (i < NumObjects() && ObjectAt(i) == o && FullName(o) == fullName) return o;
        cache.erase(it);
    }
    size_t sp = fullName.find(L' ');
    if (sp == std::wstring::npos) return nullptr;
    size_t dot = fullName.find_last_of(L'.');
    std::wstring shortName = fullName.substr(dot == std::wstring::npos || dot < sp ? sp + 1 : dot + 1);
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || !NameIs(o->name, shortName)) continue;
        if (FullName(o) == fullName) { cache[fullName] = {o, i}; return o; }
    }
    return nullptr;
}
}  // namespace ue

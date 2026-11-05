#include "discover.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace disc {
namespace {
struct Sec { uintptr_t lo, hi; DWORD ch; };
struct Image { uintptr_t base = 0, end = 0; std::vector<Sec> secs; IMAGE_DATA_DIRECTORY pdata{}; };

const Image& Img() {
    static Image im = [] {
        Image r;
        r.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(r.base + reinterpret_cast<IMAGE_DOS_HEADER*>(r.base)->e_lfanew);
        r.end = r.base + nt->OptionalHeader.SizeOfImage;
        r.pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        auto s = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            r.secs.push_back({r.base + s[i].VirtualAddress, r.base + s[i].VirtualAddress + s[i].Misc.VirtualSize, s[i].Characteristics});
        return r;
    }();
    return im;
}
bool InImage(uintptr_t p) { return p >= Img().base && p < Img().end; }
bool Exec(const Sec& s) { return s.ch & IMAGE_SCN_MEM_EXECUTE; }
bool Data(const Sec& s) { return (s.ch & IMAGE_SCN_MEM_WRITE) && !Exec(s) && s.hi - s.lo < 0x10000000; }
bool RoData(const Sec& s) { return (s.ch & IMAGE_SCN_MEM_READ) && !(s.ch & IMAGE_SCN_MEM_WRITE) && !Exec(s); }

bool Copy(void* dst, uintptr_t src, size_t n) {
    __try { memcpy(dst, reinterpret_cast<const void*>(src), n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> bool Rd(uintptr_t a, T& v) { return a > 0x10000 && a < 0x7fffffffffffull && Copy(&v, a, sizeof v); }
uintptr_t Q(uintptr_t a) { uintptr_t v = 0; Rd(a, v); return v; }

// calls f(lo, hi) for each committed, readable piece of [lo, hi)
template <class F> void Readable(uintptr_t lo, uintptr_t hi, F f) {
    for (uintptr_t p = lo; p < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mi, sizeof mi)) break;
        uintptr_t e = (std::min)(hi, reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize);
        bool ok = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mi.Protect != 0;
        if (ok) f(p, e);
        p = e;
    }
}

// the object array: {Objects, Max?...} as in the stock FChunkedFixedUObjectArray:
// Objects** +0, PreAllocated +8?  This build: Objects +0, NumElements +8, MaxElements +0xC (MaxChunks after).
bool ObjArrayAt(uintptr_t a) {
    uintptr_t objs = 0; int32_t num = 0, mx = 0;
    if (!Rd(a, objs) || !Rd(a + 8, num) || !Rd(a + 12, mx)) return false;
    if (!(objs > 0x10000 && objs < 0x7fffffffffffull) || !(1000 < num && num <= mx) || mx % 0x10000) return false;
    uintptr_t c0 = Q(objs);
    if (!c0) return false;
    for (int i : {0, 1, 2, 3, 100, 1000}) {
        uintptr_t ob = Q(c0 + 24 * size_t(i) + 8);
        int32_t idx = -1;
        if (!ob || !InImage(Q(ob)) || !Rd(ob + 0xC, idx) || idx != i) return false;
    }
    return true;
}
// FNamePool: +8 current block (u32), +0x10 block pointers; entry 0 is "None" (header len 4, ansi)
bool NamePoolAt(uintptr_t a) {
    uint32_t nb = 0; uintptr_t p = 0; uint8_t h[6];
    if (!Rd(a + 8, nb) || nb < 1 || nb >= 0x2000 || !Rd(a + 0x10, p) || !Copy(h, p, 6)) return false;
    uint16_t hd = uint16_t(h[0] | h[1] << 8);
    return (hd >> 6) == 4 && !(hd & 1) && memcmp(h + 2, "None", 4) == 0;
}

template <class Pred> std::vector<uintptr_t> ScanData(Pred pred) {
    std::vector<uintptr_t> out;
    for (auto& s : Img().secs) {
        if (!Data(s)) continue;
        Readable(s.lo, s.hi, [&](uintptr_t lo, uintptr_t hi) {
            for (uintptr_t a = (lo + 7) & ~uintptr_t(7); a + 0x18 <= hi; a += 8) if (pred(a)) out.push_back(a);
        });
    }
    return out;
}
// cheap prefilters run inline, the full check through the guarded reads
bool ObjArrayQuick(uintptr_t a) {
    auto p = reinterpret_cast<const int32_t*>(a + 8);
    return p[0] > 1000 && p[0] <= p[1] && (p[1] & 0xffff) == 0 && ObjArrayAt(a);
}
bool NamePoolQuick(uintptr_t a) {
    uint32_t nb = *reinterpret_cast<const uint32_t*>(a + 8);
    uintptr_t p = *reinterpret_cast<const uintptr_t*>(a + 0x10);
    return nb >= 1 && nb < 0x2000 && p > 0x10000 && p < 0x7fffffffffffull && (p & 1) == 0 && NamePoolAt(a);
}

// ---- names and objects (own minimal readers: the framework is not set up yet) ----
bool NameEq(uintptr_t pool, uint32_t id, const char* s) {
    uintptr_t blk = 0; uint32_t nb = 0;
    if (!Rd(pool + 8, nb) || (id >> 16) > nb || !Rd(pool + 0x10 + 8 * size_t(id >> 16), blk) || !blk) return false;
    uintptr_t e = blk + 2 * size_t(id & 0xffff);
    uint16_t hd = 0;
    if (!Rd(e, hd) || (hd & 1)) return false;
    size_t n = strlen(s);
    if ((hd >> 6) != n) return false;
    char b[64];
    return n < sizeof b && Copy(b, e + 2, n) && memcmp(b, s, n) == 0;
}
bool NamePrefix(uintptr_t pool, uint32_t id, const char* s) {
    uintptr_t blk = 0;
    if (!Rd(pool + 0x10 + 8 * size_t(id >> 16), blk) || !blk) return false;
    uintptr_t e = blk + 2 * size_t(id & 0xffff);
    uint16_t hd = 0; size_t n = strlen(s); char b[64];
    if (!Rd(e, hd) || (hd & 1) || (hd >> 6) < n || n >= sizeof b || !Copy(b, e + 2, n)) return false;
    return memcmp(b, s, n) == 0;
}
uintptr_t ObjectAt(const Result& r, int32_t i) {
    uintptr_t c = Q(Q(r.objects) + 8 * size_t(i >> 16));
    return c ? Q(c + 24 * size_t(i & 0xffff) + 8) : 0;
}
uint32_t NameId(uintptr_t obj) { uint32_t v = 0; Rd(obj + 0x18, v); return v; }
// first object (not a CDO) whose class or a super class (UStruct::Super at +0x40) is named cls
uintptr_t FindInstance(const Result& r, const char* cls, bool cdo) {
    int32_t n = 0; Rd(r.numObjects, n);
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(r, i);
        if (!o) continue;
        uintptr_t c = Q(o + 0x10);
        bool hit = false;
        for (int d = 0; c && d < 8 && !hit; ++d, c = Q(c + 0x40)) hit = NameEq(r.namePool, NameId(c), cls);
        if (hit && NamePrefix(r.namePool, NameId(o), "Default__") == cdo) return o;
    }
    return 0;
}

uintptr_t FindNamed(const Result& r, const char* name) {
    int32_t n = 0; Rd(r.numObjects, n);
    for (int32_t i = 0; i < n; ++i)
        if (uintptr_t o = ObjectAt(r, i); o && NameEq(r.namePool, NameId(o), name)) return o;
    return 0;
}

// ---- code ----
// start of the function containing a (follows chained unwind info back to the primary entry)
uintptr_t FuncStart(uintptr_t a, uintptr_t* primaryEnd) {
    auto& im = Img();
    if (!im.pdata.VirtualAddress) return 0;
    auto rf = reinterpret_cast<const RUNTIME_FUNCTION*>(im.base + im.pdata.VirtualAddress);
    size_t lo = 0, hi = im.pdata.Size / sizeof(RUNTIME_FUNCTION);
    DWORD rva = DWORD(a - im.base);
    while (lo < hi) { size_t m = (lo + hi) / 2; if (rf[m].BeginAddress <= rva) lo = m + 1; else hi = m; }
    if (!lo) return 0;
    RUNTIME_FUNCTION f = rf[lo - 1];
    if (rva >= f.EndAddress) return 0;
    for (int d = 0; d < 32; ++d) {
        uint8_t ui[4];
        if (!Copy(ui, im.base + f.UnwindData, 4) || !((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        if (!Copy(&f, im.base + f.UnwindData + 4 + 2 * ((ui[2] + 1) & ~1), sizeof f)) return 0;
    }
    if (primaryEnd) *primaryEnd = im.base + f.EndAddress;
    return im.base + f.BeginAddress;
}
// lea r64, [rip+disp32] (48/4c 8d modrm=00 reg 101) in [lo, hi) pointing at target
void LeaRefs(uintptr_t lo, uintptr_t hi, uintptr_t target, std::vector<uintptr_t>& out) {
    Readable(lo, hi, [&](uintptr_t a0, uintptr_t a1) {
        auto p = reinterpret_cast<const uint8_t*>(a0);
        size_t n = a1 - a0;
        for (size_t i = 0; i + 7 <= n; ++i) {
            if (p[i + 1] != 0x8d || (p[i] | 4) != 0x4c || (p[i + 2] & 0xc7) != 0x05) continue;
            int32_t d; memcpy(&d, p + i + 3, 4);
            if (a0 + i + 7 + intptr_t(d) == target) out.push_back(a0 + i);
        }
    });
}
// UTF-16 string start (the wchar before it is 0) in the read-only data sections
std::vector<uintptr_t> FindWide(const wchar_t* s) {
    std::vector<uintptr_t> out;
    size_t n = (wcslen(s) + 1) * 2;
    for (auto& sec : Img().secs) {
        if (!RoData(sec)) continue;
        Readable(sec.lo, sec.hi, [&](uintptr_t lo, uintptr_t hi) {
            auto p = reinterpret_cast<const uint8_t*>(lo);
            for (size_t i = 2; i + n <= hi - lo; i += 2)
                if (p[i] == uint8_t(s[0]) && p[i - 2] == 0 && p[i - 1] == 0 && memcmp(p + i, s, n) == 0) out.push_back(lo + i);
        });
    }
    return out;
}
// test dword ptr [reg+0x100], 0x400 (FUNC_Native on UFunction::FunctionFlags) in the first bytes
bool TestsNative(uintptr_t fn) {
    uint8_t b[0x300];
    if (!InImage(fn) || !Copy(b, fn, sizeof b)) return false;
    static const uint8_t tail[] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00};
    for (size_t i = 0; i + 11 <= sizeof b; ++i) {
        if (b[i] != 0xf7) continue;
        uint8_t m = b[i + 1];
        if ((m & 0xf8) != 0x80) continue;                      // mod 10, reg 000 (test)
        size_t k = (m & 7) == 4 ? i + 3 : i + 2;               // SIB byte
        if (k + 8 <= sizeof b && memcmp(b + k, tail, 8) == 0) return true;
    }
    return false;
}
}  // namespace

std::wstring Hex(uintptr_t a) {
    wchar_t b[40];
    if (InImage(a)) swprintf_s(b, L"exe+0x%llx", (unsigned long long)(a - Img().base));
    else swprintf_s(b, L"0x%llx", (unsigned long long)a);
    return b;
}

bool FindObjectArray(Result& r, LogFn log) {
    if (r.objects && ObjArrayAt(r.objects)) { r.numObjects = r.objects + 8; return true; }
    auto c = ScanData(ObjArrayQuick);
    static bool warned;
    if (c.size() != 1) {
        if (c.size() > 1 && !warned) { warned = true; log(L"discover: " + std::to_wstring(c.size()) + L" object array candidates"); }
        return false;
    }
    r.objects = c[0]; r.numObjects = c[0] + 8;
    r.how += L" objects " + Hex(r.objects) + L" (scan)";
    return true;
}

bool FindNamePool(Result& r, LogFn log) {
    if (r.namePool && NamePoolAt(r.namePool)) return true;
    auto c = ScanData(NamePoolQuick);
    static bool warned;
    if (c.size() != 1) {
        if (!warned) { warned = true; log(L"discover: " + std::to_wstring(c.size()) + L" name pool candidates"); }
        return false;
    }
    r.namePool = c[0];
    r.how += L" names " + Hex(r.namePool) + L" (scan)";
    return true;
}

bool FindEngine(Result& r, LogFn log) {
    uintptr_t eng = FindInstance(r, "GameEngine", false);
    if (!eng) return false;
    uintptr_t vt = Q(eng);
    if (!InImage(vt)) return false;
    bool cached = r.engineVt == vt && r.tickSlot >= 0 && r.processEvent >= 0;
    r.engineVt = vt;
    if (!cached) {
        r.tickSlot = r.processEvent = -1;
        // Tick: the vtable slot whose function references "UGameEngine::Tick.ViewportClosed"
        std::vector<uintptr_t> fns;
        for (uintptr_t s : FindWide(L"UGameEngine::Tick.ViewportClosed")) {
            std::vector<uintptr_t> refs;
            // fast path: the primary body of each vtable function
            for (int i = 0; i < 0x200 && refs.empty(); ++i) {
                uintptr_t f = Q(vt + 8 * size_t(i)), end = 0;
                if (InImage(f) && FuncStart(f, &end) == f) LeaRefs(f, end, s, refs);
            }
            if (refs.empty())
                for (auto& sec : Img().secs) if (Exec(sec)) LeaRefs(sec.lo, sec.hi, s, refs);
            for (uintptr_t a : refs) if (uintptr_t f = FuncStart(a, nullptr)) fns.push_back(f);
        }
        for (int i = 0; i < 0x200 && r.tickSlot < 0; ++i)
            for (uintptr_t f : fns) if (Q(vt + 8 * size_t(i)) == f) { r.tickSlot = i; break; }
        if (r.tickSlot < 0) { log(L"discover: Tick not found in the GameEngine vtable " + Hex(vt)); return false; }
        // ProcessEvent: the UObject vtable slot that tests FUNC_Native at UFunction+0x100
        uintptr_t cdo = FindNamed(r, "Default__Object");
        uintptr_t ovt = cdo ? Q(cdo) : 0;
        int hits = 0;
        for (int i = 0x20; ovt && i < 0x80; ++i)
            if (TestsNative(Q(ovt + 8 * size_t(i)))) { if (!hits++) r.processEvent = i; }
        if (hits != 1) { log(L"discover: ProcessEvent: " + std::to_wstring(hits) + L" candidates"); r.processEvent = -1; return false; }
        wchar_t b[96];
        swprintf_s(b, L" tick slot 0x%x, ProcessEvent slot 0x%x (scan)", r.tickSlot, r.processEvent);
        r.how += b;
    }
    return true;
}
}  // namespace disc

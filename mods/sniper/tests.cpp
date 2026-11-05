// tests.cpp: test commands from <mod dir>sniper.cmd (one per line; read and deleted every tick) and the
// aim helper that turns the camera onto an enemy. Weapon state commands go to MainCmd (sniper.cpp) first.
#include "common.h"
#include "overlay.h"
#include <cstdarg>

TestState g_test;

static double Wrap(double a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static UObject* FindMontage(const std::wstring& nm) {
    auto mcls = uex::Class(L"Class /Script/Engine.AnimMontage"); if (!mcls) return nullptr;
    for (int k = 0, n = ue::NumObjects(); k < n; ++k) { auto o = ue::ObjectAt(k); if (o && o->cls == mcls && ue::NameOf(o) == nm) return o; }
    return nullptr;
}

// ---- reflection probes (posture break research): a property's type and value, read from memory ----
static std::wstring PType(const uint8_t* p) {
    auto fc = ue::At<const uint8_t*>(p, ue::kFField_Class);
    std::wstring t = ue::NameOf(*reinterpret_cast<const ue::FName*>(fc + ue::kFFieldClass_Name));
    if (t == L"ObjectProperty" || t == L"ClassProperty" || t == L"StructProperty" || t == L"SoftClassProperty" ||
        t == L"SoftObjectProperty" || t == L"InterfaceProperty" || t == L"WeakObjectProperty")
        if (auto x = ue::At<UObject*>(p, ue::kProp_Extra)) t += L"<" + ue::NameOf(x) + L">";
    return t;
}
static std::wstring PName(const uint8_t* p) { return ue::NameOf(*reinterpret_cast<const ue::FName*>(p + ue::kFField_Name)); }
static std::wstring PVal(const uint8_t* p, const uint8_t* v, int depth = 0);
static std::wstring StructVal(UObject* st, const uint8_t* v, int depth) {
    std::wstring s = L"{";
    for (auto c = st; c; c = ue::At<UObject*>(c, ue::kUStruct_Super))
        for (auto q = ue::At<const uint8_t*>(c, ue::kUStruct_ChildProps); q; q = ue::At<const uint8_t*>(q, ue::kFField_Next))
            s += PName(q) + L"=" + PVal(q, v + ue::At<int32_t>(q, ue::kProp_Offset), depth + 1) + L" ";
    return s + L"}";
}
static std::wstring PVal(const uint8_t* p, const uint8_t* v, int depth) {
    auto t = PType(p); wchar_t b[64];
    auto obj = [](UObject* o) { return o ? ue::NameOf(o) : std::wstring(L"null"); };
    if (t.rfind(L"ObjectProperty", 0) == 0 || t.rfind(L"ClassProperty", 0) == 0 || t.rfind(L"InterfaceProperty", 0) == 0) return obj(*reinterpret_cast<UObject* const*>(v));
    if (t == L"StructProperty<GameplayTag>" || t == L"NameProperty") return ue::NameOf(*reinterpret_cast<const ue::FName*>(v));
    if (t == L"StructProperty<GameplayTagContainer>") {
        struct C { ue::FName* d; int n, m; } c = *reinterpret_cast<const C*>(v); std::wstring s = L"[";
        for (int i = 0; i < c.n && i < 16; ++i) s += ue::NameOf(c.d[i]) + L" ";
        return s + L"]";
    }
    if (t == L"FloatProperty") { swprintf(b, 64, L"%g", *reinterpret_cast<const float*>(v)); return b; }
    if (t == L"DoubleProperty") { swprintf(b, 64, L"%g", *reinterpret_cast<const double*>(v)); return b; }
    if (t == L"IntProperty") { swprintf(b, 64, L"%d", *reinterpret_cast<const int32_t*>(v)); return b; }
    if (t == L"BoolProperty") { auto off = ue::At<uint8_t>(p, ue::kProp_Extra + 1), m = ue::At<uint8_t>(p, ue::kProp_Extra + 2); return (v[off] & m) ? L"true" : L"false"; }
    if (t == L"ByteProperty" || t == L"EnumProperty") { swprintf(b, 64, L"%u", v[0]); return b; }
    if (t == L"ArrayProperty") {
        struct A { const uint8_t* d; int n, m; } a = *reinterpret_cast<const A*>(v);
        auto in = ue::At<const uint8_t*>(p, ue::kProp_Extra); int es = in ? ue::At<int32_t>(in, ue::kProp_ElemSize) : 0;
        std::wstring s = L"#" + std::to_wstring(a.n) + L"[";
        for (int i = 0; i < a.n && i < 12 && in && depth < 3; ++i) s += PVal(in, a.d + i * es, depth + 1) + L" ";
        return s + L"]";
    }
    if (t == L"MapProperty") {  // TSparseArray: data, alloc flags (inline 4 words, ptr, num, max), first free, num free
        auto kp = ue::At<const uint8_t*>(p, ue::kProp_Extra), vp = ue::At<const uint8_t*>(p, ue::kProp_Extra + 8);
        int voff = ue::At<int32_t>(p, ue::kProp_Extra + 0x10), stride = ue::At<int32_t>(p, ue::kProp_Extra + 0x24);
        auto d = *reinterpret_cast<const uint8_t* const*>(v); int n = *reinterpret_cast<const int32_t*>(v + 8);
        int maxBits = *reinterpret_cast<const int32_t*>(v + 0x2c);
        auto bits = maxBits > 128 ? *reinterpret_cast<const uint32_t* const*>(v + 0x20) : reinterpret_cast<const uint32_t*>(v + 0x10);
        std::wstring s = L"#" + std::to_wstring(n) + L"{";
        if (stride <= 0 || stride > 0x400) { swprintf(b, 64, L"stride? %d voff %d}", stride, voff); return s + b; }
        for (int i = 0; i < n && i < 24 && d && depth < 3; ++i) {
            if (!(bits[i >> 5] & (1u << (i & 31)))) continue;
            s += PVal(kp, d + i * stride, depth + 1) + L": " + PVal(vp, d + i * stride + voff, depth + 1) + L"; ";
        }
        return s + L"}";
    }
    if (t.rfind(L"StructProperty", 0) == 0 && depth < 2) return StructVal(ue::At<UObject*>(p, ue::kProp_Extra), v, depth);
    uint64_t x = 0; memcpy(&x, v, std::min(8, std::max(0, ue::At<int32_t>(p, ue::kProp_ElemSize)))); swprintf(b, 64, L"%llx", x); return b;
}
// a live object by name (exact name first, else the first containing it; "#<index>": by object index)
static UObject* FindByName(const std::wstring& key) {
    if (!key.empty() && key[0] == L'#') return ue::ObjectAt(_wtoi(key.c_str() + 1));
    UObject* part = nullptr;
    for (int k = 0, N = ue::NumObjects(); k < N; ++k) {
        auto o = ue::ObjectAt(k); if (!o || (o->flags & 0x10)) continue;
        auto nm = ue::NameOf(o); if (nm == key) return o;
        if (!part && nm.find(key) != std::wstring::npos) part = o;
    }
    return part;
}
// all properties of an object (up its class chain, to UObject), optionally only those whose name has `filter`
static void DumpProps(UObject* o, const std::wstring& filter) {
    if (!o) { uex::Log(L"oprops: none"); return; }
    uex::Log(L"oprops %s (%s) #%d outer %s", ue::NameOf(o).c_str(), ue::NameOf(o->cls).c_str(), o->index, o->outer ? ue::NameOf(o->outer).c_str() : L"-");
    for (auto c = o->cls; c; c = ue::At<UObject*>(c, ue::kUStruct_Super)) {
        if (ue::NameOf(c) == L"Object") break;
        for (auto p = ue::At<const uint8_t*>(c, ue::kUStruct_ChildProps); p; p = ue::At<const uint8_t*>(p, ue::kFField_Next)) {
            auto nm = PName(p); if (!filter.empty() && nm.find(filter) == std::wstring::npos) continue;
            int off = ue::At<int32_t>(p, ue::kProp_Offset); std::wstring val;
            try { val = PVal(p, reinterpret_cast<const uint8_t*>(o) + off); } catch (...) { val = L"?"; }
            auto line = L"+" + std::to_wstring(off) + L" " + PType(p) + L" " + nm + L" = " + val;
            for (size_t q = 0; q < line.size(); q += 500) uex::Log(L" %s%s", q ? L"   " : L"", line.substr(q, 500).c_str());
        }
    }
}

// targets spawned with `still`: their AI brain is stopped (again every second: a hit wakes it up)
struct Still { UObject* a; ULONGLONG at; };
static std::vector<Still> g_still;
static void StillTick() {
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_still.size();) {
        auto& t = g_still[i];
        if (!Live(t.a)) { g_still.erase(g_still.begin() + i); continue; }
        if (now >= t.at) {
            t.at = now + 1000;
            ue::Params q(uex::Fn(t.a, L"GetController")); q.Call(t.a);
            if (auto c = q.Get<UObject*>(L"ReturnValue")) {
                auto off = ue::FindProp(c->cls, L"BrainComponent").offset;
                if (auto b = off >= 0 ? ue::At<UObject*>(c, off) : nullptr) {
                    ue::Params s(uex::Fn(b, L"StopLogic")); s.Call(b);
                }
                ue::Params m(uex::Fn(c, L"StopMovement")); m.Call(c);
            }
        }
        ++i;
    }
}
// stand cm from e, facing it; turn: degrees around e from the side we are on
static void StandOff(UObject* me, UObject* e, double cm, double turn = 0) {
    if (!e) return;
    ik::V p = ik::FromU(uex::Loc(me)), q = ik::FromU(uex::Loc(e)), d = ik::Norm(ik::V{p.x - q.x, p.y - q.y, 0});
    if (ik::Len(d) < .5) d = {1, 0, 0};
    if (turn) { double a = turn * 0.0174532925, c = cos(a), s = sin(a); d = ik::V{d.x * c - d.y * s, d.x * s + d.y * c, 0}; }
    ik::V t = q + d * cm; t.z = q.z + 5;
    double yaw = atan2(-d.y, -d.x) / 0.0174532925;
    uex::Teleport(me, ik::ToU(t), yaw);
    auto r = CtrlRot(); r.yaw = yaw; r.pitch = 0; SetCtrlRot(r);
}

// ---- selftest: every weapon against one still enemy; damage and posture against the formulas, then the
// posture break through GE_GrabHit and through the ChangeHitState fallback. Results: the log ("selftest PASS
// ..." / "selftest FAIL ...") and <mod>selftest.txt. The target gets 9000 Health meanwhile, put back after.
struct StCase { const wchar_t* name; int wpn; bool head; int brk; };  // brk: 1 posture break, 2 ChangeHitState only
static const StCase kStCases[] = {{L"awp body", 0, false, 0}, {L"awp head", 0, true, 0}, {L"ak47 body", 1, false, 0},
                                  {L"m4a4 body", 2, false, 0}, {L"break", 0, false, 1}, {L"break direct", 0, false, 2}};
constexpr int kStN = int(sizeof(kStCases) / sizeof(kStCases[0]));
constexpr double kStDist = 800;  // cm
enum { kStPrep, kStAim, kStFire };
struct SelfTest {
    bool on = false, fired = false, dbg0 = false; int i = 0, phase = 0, tries = 0, pass = 0, fail = 0, n0 = 0;
    ULONGLONG until = 0, wait = 0, fireBy = 0, brkSince = 0; UObject* t = nullptr; float hp0 = 0, hpMax0 = 0, sc0 = 0, hp0x = 0; std::wstring out;
};
static SelfTest g_st;
static void StLine(int ok, const wchar_t* fmt, ...) {  // ok: 1 PASS, 0 FAIL, -1 plain line
    wchar_t b[512]; va_list a; va_start(a, fmt); _vsnwprintf_s(b, 512, _TRUNCATE, fmt, a); va_end(a);
    std::wstring s = std::wstring(ok > 0 ? L"selftest PASS " : ok == 0 ? L"selftest FAIL " : L"selftest: ") + b;
    uex::Log(L"%s", s.c_str()); g_st.out += s + L"\n";
}
static void StWrite() {
    std::wstring p = g_modDir + L"selftest.txt";
    if (FILE* f = _wfopen(p.c_str(), L"w, ccs=UTF-8")) { fputws(g_st.out.c_str(), f); fclose(f); }
}
void SelfTestStart(UObject* me) {
    if (g_st.on) { uex::Log(L"selftest: already running"); return; }
    g_st = SelfTest{}; _wremove((g_modDir + L"selftest.txt").c_str());
    auto e = NearestEnemy(me);
    if (!e) { StLine(0, L"no enemy nearby"); StLine(-1, L"0 passed, 1 failed"); StWrite(); return; }
    g_st.on = true; g_st.t = e; g_st.dbg0 = g_dbg; g_dbg = true;
    g_st.hp0 = uex::Attr(e, uex::attr::Health); g_st.hpMax0 = uex::Attr(e, uex::attr::HealthMax);
    g_still.push_back({e, GetTickCount64()});
    if (CurWpn() < 0) MainCmd(me, L"equip 0");
    StLine(-1, L"target %s hp %.0f/%.0f shachi max %.0f", ue::NameOf(e).c_str(), g_st.hp0, g_st.hpMax0, uex::Attr(e, uex::attr::ShachiMax));
}
static void StEnd() {
    auto e = g_st.t;
    if (Live(e)) {
        uex::SetAttr(e, uex::attr::HealthMax, g_st.hpMax0); uex::SetAttr(e, uex::attr::Health, g_st.hp0);
        if (InBreakOf(e)) SetHitState(e, L"None");  // the direct break has no animation to end it
    }
    g_breakDirect = false; g_test.aimSteps = 0; g_test.fire = false; g_dbg = g_st.dbg0;
    MainCmd(uex::Me(), L"zoom 0");
    StLine(-1, L"%d passed, %d failed", g_st.pass, g_st.fail); StWrite();
    g_st.on = false;
}
static void StNext(bool ok) { (ok ? g_st.pass : g_st.fail)++; ++g_st.i; g_st.phase = kStPrep; g_st.tries = 0; g_st.wait = GetTickCount64() + 400; }
// the shot just fired: damage and posture against the formulas
static void StCheck(UObject* me, const StCase& k) {
    auto e = g_st.t; const Wpn& w = g_wpn[k.wpn]; auto r = g_lastShot;
    if (r.victim != e || (k.head && r.kind != 1 && r.kind != 3) || (!k.head && !k.brk && r.kind == 1)) {  // a break case takes any hit
        { ik::V eye; uex::Rot cr; CamView(&eye, &cr);
          StLine(-1, L"%s: try %d hit %s kind %d at %.1f m (target %.1f m)", k.name, g_st.tries, r.victim ? ue::NameOf(r.victim).c_str() : L"nothing", r.kind,
                 ik::Len(r.end - eye) / 100, ik::Len(ik::FromU(uex::Loc(e)) - eye) / 100); }
        if (++g_st.tries >= 3) { StLine(0, L"%s: no %s hit in 3 shots", k.name, k.head ? L"head" : L"body"); StNext(false); }
        else { g_st.phase = kStPrep; g_st.wait = GetTickCount64() + 1600; }
        return;
    }
    float hp = uex::Attr(e, uex::attr::Health), sc = uex::Attr(e, uex::attr::Shachi);
    float smin = uex::Attr(e, uex::attr::ShachiMin), smx = uex::Attr(e, uex::attr::ShachiMax);
    ik::V eye; uex::Rot cr; CamView(&eye, &cr);
    double units = ik::Len(r.end - eye) / 2.54;
    double want = w.damage * kDmgScale * pow(w.rangeMod, units / 500) * (r.kind == 1 || r.kind == 3 ? w.head : 1);
    bool okDmg = fabs(r.dmg - want) <= want * .03 + 1 && fabs((g_st.hp0x - hp) - r.dmg) <= 1.5;
    if (!k.brk) {
        double drop = std::min(double(w.posture * (k.head ? kPostureHead : 1) * smx), double(g_st.sc0 - smin));
        bool okPos = fabs((g_st.sc0 - sc) - drop) <= smx * .02 + 1;
        StLine(okDmg && okPos, L"%s: %.0f m dmg %d (want %.0f), hp %.0f -> %.0f, posture %.0f -> %.0f (want -%.0f)", k.name,
               units * 2.54 / 100, r.dmg, want, g_st.hp0x, hp, g_st.sc0, sc, drop);
        StNext(okDmg && okPos);
        return;
    }
    bool brk = InBreakOf(e), path = k.brk == 2 ? g_lastBreak == 2 : g_lastBreak != 0;
    StLine(okDmg && brk && path && sc <= smin + .5f, L"%s: dmg %d (want %.0f), posture %.0f -> %.0f (min %.0f), in break %d via %s", k.name,
           r.dmg, want, g_st.sc0, sc, smin, brk, g_lastBreak == 1 ? L"GE_GrabHit" : g_lastBreak == 2 ? L"ChangeHitState" : L"-");
    StNext(okDmg && brk && path && sc <= smin + .5f);
}
static void SelfTestTick(UObject* me) {
    if (!g_st.on) return;
    ULONGLONG now = GetTickCount64();
    if (!g_st.until) g_st.until = now + 120000;
    if (!Live(g_st.t) || uex::Attr(g_st.t, uex::attr::Health) <= 0) { StLine(0, L"target gone"); g_st.fail += kStN - g_st.i - 1; StEnd(); return; }
    if (now > g_st.until) { StLine(0, L"timeout at %s", g_st.i < kStN ? kStCases[g_st.i].name : L"end"); StEnd(); return; }
    if (g_st.i >= kStN) { StEnd(); return; }
    if (now < g_st.wait) return;
    auto& k = kStCases[g_st.i]; auto e = g_st.t;
    switch (g_st.phase) {
    case kStPrep: {
        if (k.brk && InBreakOf(e)) {  // the last break has to wear off first; one set directly never does (forced out after 3 s)
            if (!g_st.brkSince) g_st.brkSince = now;
            if (now - g_st.brkSince > 3000) SetHitState(e, L"None");
            g_st.wait = now + 500; return;
        }
        g_st.brkSince = 0;
        if (CurWpn() != k.wpn) { MainCmd(me, std::wstring(L"wpn ") + std::to_wstring(k.wpn)); g_st.wait = now + 600; return; }
        MainCmd(me, g_wpn[k.wpn].zoomLevels ? L"zoom 1" : L"zoom 0");
        StandOff(me, e, kStDist, g_st.tries * 90);  // a miss: something in the way, try another side
        MainCmd(me, L"ammo " + std::to_wstring(g_wpn[k.wpn].clip));  // never an automatic reload mid-test
        uex::SetAttr(me, uex::attr::Health, uex::Attr(me, uex::attr::HealthMax));
        uex::SetAttr(e, uex::attr::HealthMax, 9000); uex::SetAttr(e, uex::attr::Health, 9000);
        float smin = uex::Attr(e, uex::attr::ShachiMin), smx = uex::Attr(e, uex::attr::ShachiMax);
        uex::SetAttr(e, uex::attr::Shachi, k.brk ? smin + .1f * smx : smx);  // a break case: one shot empties the bar
        g_breakDirect = k.brk == 2; g_lastBreak = 0;
        g_test.aimAt = e; g_test.aimHead = k.head; g_test.aimOff = 30; g_test.aimSteps = 40;
        g_st.phase = kStAim; g_st.wait = now + 300; g_st.fireBy = now + 5000;
        return;
    }
    case kStAim:
        if (g_test.aimSteps > 0) return;
        if (!WpnSteady()) {  // still landing from the teleport, or the bolt is not home yet: aim again
            if (now > g_st.fireBy) { StLine(0, L"%s: never steady to fire", k.name); StNext(false); return; }
            g_test.aimSteps = 10; return;
        }
        g_st.hp0x = uex::Attr(e, uex::attr::Health); g_st.sc0 = uex::Attr(e, uex::attr::Shachi);
        g_st.n0 = g_shotN; g_test.fire = true; g_st.phase = kStFire; g_st.wait = 0; g_st.fireBy = now + 3000;
        return;
    case kStFire:
        if (g_shotN != g_st.n0) { g_breakDirect = false; StCheck(me, k); return; }
        if (now > g_st.fireBy) { StLine(0, L"%s: the weapon did not fire", k.name); g_test.fire = false; StNext(false); }
        return;
    }
}
void TestCmds(UObject* me) {
    StillTick();
    SelfTestTick(me);
    std::wstring path = g_modDir + L"sniper.cmd";
    FILE* f = _wfopen(path.c_str(), L"r, ccs=UTF-8"); if (!f) return;
    wchar_t line[256]; std::vector<std::wstring> cmds;
    while (fgetws(line, 256, f)) {
        std::wstring c(line);
        while (!c.empty() && (c.back() == '\n' || c.back() == '\r' || c.back() == ' ')) c.pop_back();
        if (!c.empty()) cmds.push_back(c);
    }
    fclose(f); _wremove(path.c_str());
    for (auto& c : cmds) {
        uex::Log(L"test %s", c.c_str());
        auto starts = [&](const wchar_t* p) { return c.rfind(p, 0) == 0; };
        auto arg = [&](size_t n) { return c.size() > n ? c.c_str() + n : L""; };
        double v[4]{};
        if (MainCmd(me, c)) continue;
        if (c == L"fire") g_test.fire = true;
        else if (c == L"equip") g_test.equip = true;
        else if (starts(L"skip ")) g_test.skip = _wtoi(arg(5));
        else if (c == L"interact") g_test.interact = true;
        else if (starts(L"icheck")) {  // the player's interact component: why the prompt is off
            auto off = ue::FindProp(me->cls, L"PBZInteract").offset; auto ic = off >= 0 ? ue::At<UObject*>(me, off) : nullptr;
            if (!ic) for (int k = 0, N = ue::NumObjects(); k < N && !ic; ++k) { auto o = ue::ObjectAt(k); if (o && o->outer == me && ue::NameOf(o->cls) == L"PBZInteractComponent") ic = o; }
            if (!ic) { uex::Log(L"icheck: no component"); continue; }
            if (c == L"icheck search") {  // SearchActors / InteractActors (TMap<AActor*, ...>): the actors
                for (auto pn : {L"SearchActors", L"InteractActors"}) {
                    auto mp = ue::FindPropPtr(ic->cls, pn); if (!mp) { uex::Log(L"iset %s not found", pn); continue; }
                    // FMapProperty: KeyProp, ValueProp; element = key, value, two hash links
                    auto kp = ue::At<const uint8_t*>(mp, ue::kProp_Extra), vp = ue::At<const uint8_t*>(mp, ue::kProp_Extra + 8);
                    if (!kp || !vp) continue;
                    int off = ue::At<int32_t>(mp, ue::kProp_Offset), stride = uex::MapStride(ue::At<int32_t>(kp, ue::kProp_ElemSize), ue::At<int32_t>(vp, ue::kProp_ElemSize));
                    auto base = reinterpret_cast<uint8_t*>(ic) + off; int n = ue::At<int32_t>(base, uex::layout::kSetNum), i = 0;
                    // the actor: the key if it is an object, else the value (after the key, 8-aligned)
                    bool inKey = PType(kp).find(L"ObjectProperty") != std::wstring::npos;
                    const uint8_t* ap = inKey ? kp : vp; int aoff = inKey ? 0 : (ue::At<int32_t>(kp, ue::kProp_ElemSize) + 7) & ~7;
                    bool weak = PType(ap).rfind(L"WeakObjectProperty", 0) == 0;
                    uex::Log(L"iset %s +%x n %d key %s value %s stride 0x%x", pn, off, n, PType(kp).c_str(), PType(vp).c_str(), stride);
                    uex::EachSetElem(base, stride, [&](uint8_t* e) {
                        // a weak key is {object index, serial}: the object at that index
                        UObject* a = weak ? ue::ObjectAt(ue::At<int32_t>(e, aoff)) : ue::At<UObject*>(e, aoff);
                        if (i < 32) uex::Log(L"iset  %d %p %s", i, a, a && Live(a) ? ue::NameOf(a).c_str() : L"?"); ++i; });
                }
                continue;
            }
            if (c == L"icheck tags") {  // the player's owned gameplay tags
                auto tc = uex::OwnedTags(me);
                for (int i = 0; i < tc.n && i < 200; ++i) uex::Log(L"itag %s", ue::NameOf(tc.d[i]).c_str());
                uex::Log(L"itags %d", tc.n);
                continue;
            }
            if (c == L"icheck props") {
                for (auto cc = ic->cls; cc; cc = ue::At<UObject*>(cc, ue::kUStruct_Super)) {
                    if (ue::NameOf(cc) == L"ActorComponent") break;
                    for (auto p = ue::At<const uint8_t*>(cc, ue::kUStruct_ChildProps); p; p = ue::At<const uint8_t*>(p, ue::kFField_Next)) {
                        int o = ue::At<int32_t>(p, ue::kProp_Offset), sz = ue::At<int32_t>(p, ue::kProp_ElemSize);
                        auto base = reinterpret_cast<const uint8_t*>(ic) + o; uint64_t v = 0; memcpy(&v, base, sz < 8 ? sz : 8);
                        int n = sz == 16 ? *reinterpret_cast<const int32_t*>(base + 8) : -1;
                        uex::Log(L"iprop %s +%x sz %d = %llx n %d", ue::NameOf(*reinterpret_cast<const ue::FName*>(p + ue::kFField_Name)).c_str(), o, sz, v, n);
                    }
                }
                continue;
            }
            for (auto fnn : {L"SetCurrentInteractActor", L"CheckInteractActors", L"CheckInteractActors_Internal", L"DoInteract", L"StartInteract", L"OpenInteractTick", L"CanTriggerInteract", L"IsSpecialMove", L"GetInteractActor", L"GetInteractingActor", L"IsValidInteract", L"IsPlayerFaceToActor", L"GetValidDistance"}) {
                auto fn = uex::Fn(ic, fnn); if (!fn) continue; std::wstring ps;
                for (auto p = ue::At<const uint8_t*>(fn, ue::kUStruct_ChildProps); p; p = ue::At<const uint8_t*>(p, ue::kFField_Next))
                    ps += ue::NameOf(*reinterpret_cast<const ue::FName*>(p + ue::kFField_Name)) + L" ";
                ue::Params q(fn); static UObject* last; auto ia = [&]{ ue::Params g(uex::Fn(ic, L"GetInteractActor")); g.Call(ic); return g.Get<UObject*>(L"ReturnValue"); }();
                if (ia) last = ia; else if (Live(last)) ia = last;
                q.Set(L"Actor", ia); q.Set(L"InActor", ia); q.Set(L"NewActor", ia); q.Set(L"TargetActor", ia); q.Set(L"SearchActor", ia); q.Set(L"Player", me);
                bool doCall = wcsncmp(fnn, L"Set", 3) && wcsncmp(fnn, L"Check", 5) && wcsncmp(fnn, L"Do", 2) && wcsncmp(fnn, L"Start", 5) && wcsncmp(fnn, L"Open", 4);
                if (doCall || (c.size() > 7 && c.substr(7) == fnn)) q.Call(ic);
                if (!wcscmp(fnn, L"IsValidInteract"))
                    uex::Log(L"icheck actor %s tags block %d sprint %d air %d", ia ? ue::NameOf(ia).c_str() : L"-", q.Get<bool>(L"bHasBlockTag"), q.Get<bool>(L"bHasSprintTag"), q.Get<bool>(L"bHasAirTag"));
                uex::Log(L"icheck %s(%s) -> u8 %d ptr %s", fnn, ps.c_str(), q.Get<uint8_t>(L"ReturnValue"), q.Get<UObject*>(L"ReturnValue") && (wcsncmp(fnn, L"GetInteract", 11) == 0) ? ue::NameOf(q.Get<UObject*>(L"ReturnValue")).c_str() : L"");
            }
        }
        else if (starts(L"ofind ")) {  // ofind <substr>: live objects whose class name has it, with their functions
            auto key = c.substr(6); int n = 0;
            for (int k = 0, N = ue::NumObjects(); k < N && n < 40; ++k) {
                auto o = ue::ObjectAt(k); if (!o || !o->cls || (o->flags & 0x10)) continue;
                auto cn = ue::NameOf(o->cls); if (cn.find(key) == std::wstring::npos) continue;
                ++n; std::wstring fns;
                for (auto cc = o->cls; cc; cc = ue::At<UObject*>(cc, ue::kUStruct_Super)) {
                    auto nm = ue::NameOf(cc); if (nm == L"Object" || nm == L"ActorComponent" || nm == L"SceneComponent") break;
                    for (auto f = ue::At<UObject*>(cc, ue::kUStruct_Children); f; f = ue::At<UObject*>(f, ue::kUField_Next)) fns += ue::NameOf(f) + L" ";
                }
                uex::Log(L"ofind %s (%s) outer %s", ue::NameOf(o).c_str(), cn.c_str(), o->outer ? ue::NameOf(o->outer).c_str() : L"-");
                for (size_t q = 0; q < fns.size(); q += 400) uex::Log(L"  %s", fns.substr(q, 400).c_str());
            }
        }
        else if (starts(L"oprops ")) {  // oprops <object name|#index> [property name part]
            auto rest = c.substr(7); auto sp = rest.find(L' ');
            DumpProps(FindByName(rest.substr(0, sp)), sp == std::wstring::npos ? L"" : rest.substr(sp + 1));
        }
        else if (starts(L"cdo ")) {  // cdo <class name> [property part]: its default object, and the objects inside it (GE components)
            auto rest = c.substr(4); auto sp = rest.find(L' '); auto dn = L"Default__" + rest.substr(0, sp);
            auto filt = sp == std::wstring::npos ? L"" : rest.substr(sp + 1); UObject* d = nullptr;
            for (int k = 0, N = ue::NumObjects(); k < N && !d; ++k) { auto o = ue::ObjectAt(k); if (o && ue::NameOf(o) == dn) d = o; }
            DumpProps(d, filt);
            for (int k = 0, N = ue::NumObjects(); d && k < N; ++k) { auto o = ue::ObjectAt(k); if (o && o->outer == d) DumpProps(o, filt); }
        }
        else if (starts(L"gehit ")) {  // gehit <GE class name> [shachi first]: that effect from the player's ASC onto the aimed (or nearest) enemy
            wchar_t cn[160]{}; float pre = -1; swscanf(arg(6), L"%159s %f", cn, &pre);
            auto e = g_test.aimAt && Live(g_test.aimAt) ? g_test.aimAt : NearestEnemy(me); auto ge = FindByName(cn);
            auto src = uex::ASC(me), dst = e ? uex::ASC(e) : nullptr;
            if (!e || !ge || !src || !dst) { uex::Log(L"gehit: missing %d %d %d %d", !!e, !!ge, !!src, !!dst); continue; }
            if (pre >= 0) uex::SetAttr(e, uex::attr::Shachi, pre);
            float hp = uex::Attr(e, uex::attr::Health), sc = uex::Attr(e, uex::attr::Shachi);
            ue::Params p(uex::Fn(src, L"BP_ApplyGameplayEffectToTarget"));  // an empty context: the ASC makes one (source: the player)
            p.Set(L"GameplayEffectClass", ge); p.Set(L"Target", dst); p.Set<float>(L"Level", 1.f); p.Call(src);
            uex::Log(L"gehit %s -> %s hp %.0f -> %.0f shachi %.0f -> %.0f", cn, ue::NameOf(e).c_str(), hp, uex::Attr(e, uex::attr::Health), sc, uex::Attr(e, uex::attr::Shachi));
            BreakInfo(e);
        }
        else if (c == L"break" || starts(L"break ")) BreakInfo(g_test.aimAt && Live(g_test.aimAt) ? g_test.aimAt : NearestEnemy(me));
        else if (starts(L"eprops ")) {  // eprops <component class part|self> [property name part]: on the aimed (or nearest) enemy
            auto e = g_test.aimAt && Live(g_test.aimAt) ? g_test.aimAt : NearestEnemy(me); if (!e) continue;
            auto rest = c.substr(7); auto sp = rest.find(L' '); auto key = rest.substr(0, sp);
            auto filt = sp == std::wstring::npos ? L"" : rest.substr(sp + 1);
            if (key == L"self") { DumpProps(e, filt); continue; }
            for (int k = 0, N = ue::NumObjects(); k < N; ++k) {  // its subobjects (components, ability instances) whose class has key
                auto o = ue::ObjectAt(k); if (!o || o->outer != e || (o->flags & 0x10)) continue;
                if (ue::NameOf(o->cls).find(key) != std::wstring::npos) DumpProps(o, filt);
            }
        }
        else if (starts(L"hitstate")) {  // hitstate [tag]: the aimed (or nearest) enemy's hit state, set to tag if given ("none": empty)
            auto e = g_test.aimAt && Live(g_test.aimAt) ? g_test.aimAt : NearestEnemy(me); if (!e) continue;
            std::wstring t = c.size() > 9 ? c.substr(9) : L""; bool ok = false;
            if (!t.empty()) ok = SetHitState(e, t == L"none" ? L"None" : t.c_str());
            uex::Log(L"hitstate %s %s%s", ue::NameOf(e).c_str(), HitStateOf(e).c_str(), t.empty() ? L"" : ok ? L" (set)" : L" (refused)");
        }
        else if (starts(L"fnsig ")) {  // fnsig <object name|#index> <function>: its parameters
            auto rest = c.substr(6); auto sp = rest.find(L' '); if (sp == std::wstring::npos) continue;
            auto o = FindByName(rest.substr(0, sp)); auto fn = o ? uex::Fn(o, rest.substr(sp + 1).c_str()) : nullptr;
            std::wstring s;
            for (auto p = fn ? ue::At<const uint8_t*>(fn, ue::kUStruct_ChildProps) : nullptr; p; p = ue::At<const uint8_t*>(p, ue::kFField_Next))
                s += PType(p) + L" " + PName(p) + L", ";
            uex::Log(L"fnsig %s.%s (%s)", o ? ue::NameOf(o->cls).c_str() : L"-", rest.substr(sp + 1).c_str(), fn ? s.c_str() : L"not found");
        }
        else if (starts(L"oname ")) {  // oname <part> [class part]: live objects (classes, assets, instances) whose name has it
            auto rest = c.substr(6); auto sp = rest.find(L' '); auto key = rest.substr(0, sp);
            auto ck = sp == std::wstring::npos ? std::wstring() : rest.substr(sp + 1); int n = 0;
            for (int k = 0, N = ue::NumObjects(); k < N && n < 80; ++k) {
                auto o = ue::ObjectAt(k); if (!o || (o->flags & 0x10)) continue;
                auto nm = ue::NameOf(o); if (nm.find(key) == std::wstring::npos || nm.rfind(L"Default__", 0) == 0) continue;
                if (!ck.empty() && ue::NameOf(o->cls).find(ck) == std::wstring::npos) continue;
                ++n; uex::Log(L"oname #%d %s (%s) outer %s", k, nm.c_str(), ue::NameOf(o->cls).c_str(), o->outer ? ue::NameOf(o->outer).c_str() : L"-");
            }
        }
        else if (starts(L"ocls ")) {  // ocls <class part> <property> <value part>: objects of that class whose property's value has it
            wchar_t cp[128]{}, pp[128]{}, vp[128]{}; swscanf(arg(5), L"%127s %127s %127s", cp, pp, vp); int n = 0;
            for (int k = 0, N = ue::NumObjects(); k < N && n < 60; ++k) {
                auto o = ue::ObjectAt(k); if (!o || !o->cls || ue::NameOf(o->cls).find(cp) == std::wstring::npos) continue;
                for (auto cc = o->cls; cc; cc = ue::At<UObject*>(cc, ue::kUStruct_Super))
                    for (auto p = ue::At<const uint8_t*>(cc, ue::kUStruct_ChildProps); p; p = ue::At<const uint8_t*>(p, ue::kFField_Next)) {
                        if (PName(p) != pp) continue;
                        std::wstring v; try { v = PVal(p, reinterpret_cast<const uint8_t*>(o) + ue::At<int32_t>(p, ue::kProp_Offset)); } catch (...) {}
                        if (v.find(vp) == std::wstring::npos) continue;
                        ++n; uex::Log(L"ocls #%d %s outer %s %s = %s", k, ue::NameOf(o).c_str(), o->outer ? ue::PathOf(o->outer).c_str() : L"-", pp, v.substr(0, 300).c_str());
                    }
            }
            uex::Log(L"ocls %d", n);
        }
        else if (starts(L"burst ")) g_test.burst = _wtoi(arg(6));
        else if (c == L"hold") g_test.hold = !g_test.hold;
        else if (c == L"ads") g_test.ads = !g_test.ads;  // rifles: as if RMB were held
        else if (starts(L"grip ") || starts(L"muzzle ")) {
            int i = CurWpn(); bool g = starts(L"grip ");
            swscanf(arg(g ? 5 : 7), L"%lf %lf %lf", v, v + 1, v + 2);
            if (i >= 0) (g ? g_wpn[i].grip : g_wpn[i].muzzle) = {v[0], v[1], v[2]};
        }
        else if (starts(L"hl ")) { swscanf(arg(3), L"%lf %lf %lf", v, v + 1, v + 2); g_hgL = {v[0], v[1], v[2]}; }
        else if (starts(L"hg ")) { swscanf(arg(3), L"%lf %lf %lf", v, v + 1, v + 2); g_hgR = {v[0], v[1], v[2]}; }
        else if (starts(L"wo ")) { swscanf(arg(3), L"%lf %lf %lf", v, v + 1, v + 2); g_wpnOff = {v[0], v[1], v[2]}; }
        else if (starts(L"poseat ")) { g_poseAt = _wtof(arg(7)); PoseRefreeze(); }
        else if (starts(L"face ")) g_faceOff = _wtof(arg(5));
        else if (c == L"noface") g_noFace = !g_noFace;
        else if (c == L"refreeze") PoseRefreeze();
        else if (starts(L"fx ")) swscanf(arg(3), L"%lf %lf %lf %lf", g_fxK, g_fxK + 1, g_fxK + 2, g_fxK + 3);
        else if (starts(L"headr ")) g_headR = _wtof(arg(6));
        else if (starts(L"margin ")) g_bodyMargin = _wtof(arg(7));
        else if (c == L"snap" || starts(L"snap ")) {
            std::wstring fn = g_modDir + (c.size() > 5 ? c.substr(5) : std::wstring(L"hud.bmp"));
            uex::Log(L"test: snap %s %d", fn.c_str(), overlay::Snap(fn.c_str()));
        }
        else if (c == L"dbg") { g_dbg = !g_dbg; uex::Log(L"dbg %d", g_dbg); }
        else if (starts(L"camyaw ") || starts(L"campitch ")) {
            auto r = CtrlRot(); bool y = starts(L"camyaw ");
            (y ? r.yaw : r.pitch) += _wtof(arg(y ? 7 : 9)); SetCtrlRot(r);
        }
        else if (c == L"aim" || c == L"aim head" || starts(L"aim ")) {
            g_test.aimAt = NearestEnemy(me); g_test.aimHead = c == L"aim head"; g_test.aimSteps = 40;
            g_test.aimOff = (starts(L"aim ") && !g_test.aimHead) ? _wtof(arg(4)) : 30;
            if (g_test.aimAt) uex::Log(L"test: aim at %s hp %.0f, %.0f m", ue::NameOf(g_test.aimAt).c_str(),
                uex::Attr(g_test.aimAt, uex::attr::Health), ik::Len(ik::FromU(uex::Loc(g_test.aimAt)) - ik::FromU(uex::Loc(me))) / 100);
            else uex::Log(L"test: no enemy");
        }
        else if (starts(L"aimn ")) {  // aimn <name part>: the camera onto that character, dead or alive
            auto key = c.substr(5); auto chCls = uex::Class(L"Class /Script/ProjectPBZ.PBZCharacter"); g_test.aimAt = nullptr;
            for (int k = 0, n = ue::NumObjects(); chCls && k < n && !g_test.aimAt; ++k) {
                auto o = ue::ObjectAt(k);
                if (o && !(o->flags & 0x30) && ue::IsA(o, chCls) && ue::NameOf(o).find(key) != std::wstring::npos) g_test.aimAt = o;
            }
            g_test.aimHead = false; g_test.aimSteps = 40; g_test.aimOff = 0;
            auto m = g_test.aimAt ? MeshOf(g_test.aimAt) : nullptr; bool phys = false, vis = false;
            if (m) { { ue::Params q(uex::Fn(m, L"IsSimulatingPhysics")); q.Call(m); phys = q.Get<bool>(L"ReturnValue"); }
                     { ue::Params q(uex::Fn(m, L"IsVisible")); q.Call(m); vis = q.Get<bool>(L"ReturnValue"); } }
            uex::Log(L"aimn %s mesh visible %d physics %d", g_test.aimAt ? ue::NameOf(g_test.aimAt).c_str() : L"-", vis, phys);
        }
        else if (starts(L"goto ")) StandOff(me, NearestEnemy(me), _wtof(arg(5)));  // stand d cm from the nearest enemy, facing it
        else if (c == L"selftest") SelfTestStart(me);
        else if (c == L"heal") uex::SetAttr(me, uex::attr::Health, uex::Attr(me, uex::attr::HealthMax));
        else if (c == L"posture") PostureInfo(g_test.aimAt && Live(g_test.aimAt) ? g_test.aimAt : NearestEnemy(me));  // the aimed (or nearest) enemy
        else if (c == L"codex" || starts(L"codex ")) {  // codex [bosses|<part>]: enemy codex rows (DT_CodexEnemyDataTable): class path, boss flag
            auto key = c.size() > 6 ? c.substr(6) : std::wstring(); bool bosses = key == L"bosses";
            auto dt = uex::LoadAsset(L"/Game/PBZContent/DataTable/DT_CodexEnemyDataTable.DT_CodexEnemyDataTable");
            if (!dt) { uex::Log(L"codex: no table"); continue; }
            static uex::Prop kBoss{L"ScriptStruct /Script/ProjectPBZ.CodexEnemyRow", L"bIsBoss", 0xb1, 0xff},
                kEnemyClass{L"ScriptStruct /Script/ProjectPBZ.CodexEnemyRow", L"EnemyClass", 0x8};
            int n = 0, shown = 0;
            uex::EachRow(dt, [&](const ue::FName& k, uint8_t* r) {
                ++n; bool boss = kBoss.Bit(r); auto nm = ue::NameOf(k);
                if (shown >= 120 || (bosses ? !boss : (!key.empty() && nm.find(key) == std::wstring::npos))) return;
                uex::Log(L"codex %s boss %d %s", nm.c_str(), boss, uex::SoftPath(r + kEnemyClass).c_str()); ++shown;
            });
            uex::Log(L"codex %d rows", n);
        }
        else if (c == L"posture tags") {  // the aimed (or nearest) enemy's owned gameplay tags
            auto e = g_test.aimAt && Live(g_test.aimAt) ? g_test.aimAt : NearestEnemy(me); if (!e) continue;
            auto tc = uex::OwnedTags(e);
            for (int i = 0; i < tc.n && i < 200; ++i) uex::Log(L"etag %s", ue::NameOf(tc.d[i]).c_str());
            uex::Log(L"etags %s %d", ue::NameOf(e).c_str(), tc.n);
        }
        else if (starts(L"estat ")) {  // estat <hp> <shachi>: set the nearest enemy's Health and Shachi (negative = keep)
            auto e = NearestEnemy(me); float h = -1, sh = -1; swscanf(arg(6), L"%f %f", &h, &sh); if (!e) continue;
            if (h >= 0) uex::SetAttr(e, uex::attr::Health, h); if (sh >= 0) uex::SetAttr(e, uex::attr::Shachi, sh); PostureInfo(e);
        }
        else if (c == L"posture full") { auto e = NearestEnemy(me); if (e) uex::SetAttr(e, uex::attr::Shachi, uex::Attr(e, uex::attr::ShachiMax)); PostureInfo(e); }
        else if (c == L"pos") { auto l = uex::Loc(me); uex::Log(L"pos %.0f %.0f %.0f yaw %.1f", l.x, l.y, l.z, uex::GetRot(me).yaw); }
        else if (starts(L"tp ")) { swscanf(arg(3), L"%lf %lf %lf", v, v + 1, v + 2); uex::Teleport(me, {v[0], v[1], v[2]}, uex::GetRot(me).yaw); }
        else if (c == L"spawners" || starts(L"spawners ")) {  // NPC spawners: object index, class, distance, spawned actor
            auto key = c.size() > 9 ? c.substr(9) : std::wstring(); auto sc = uex::Class(L"Class /Script/ProjectPBZ.NPCSpawner"); int n = 0;
            ik::V pl = ik::FromU(uex::Loc(me));
            for (int k = 0, N = ue::NumObjects(); sc && k < N && n < 60; ++k) {
                auto o = ue::ObjectAt(k); if (!o || (o->flags & 0x30) || !ue::IsA(o, sc)) continue;
                UObject* a = nullptr; { ue::Params q(uex::Fn(o, L"GetSpawnedNPCActor")); q.Call(o); a = q.Get<UObject*>(L"ReturnValue"); }
                auto nm = ue::NameOf(o->cls) + L" " + ue::NameOf(o) + L" " + (a ? ue::NameOf(a) : std::wstring(L"-"));
                if (!key.empty() && nm.find(key) == std::wstring::npos) continue;
                auto l = uex::Loc(o); double d = ik::Len(ik::FromU(l) - pl) / 100;
                uex::Log(L"spawner #%d %s at %.0f %.0f %.0f (%.0f m) hp %.0f", k, nm.c_str(), l.x, l.y, l.z, d, a && uex::AttrSet(a) ? uex::Attr(a, uex::attr::Health) : -1.f);
                ++n;
            }
        }
        else if (starts(L"spawn ")) {  // spawn <object index>: that spawner, forced
            auto o = ue::ObjectAt(_wtoi(arg(6)));
            if (o) { ue::Params q(uex::Fn(o, L"Spawn")); q.Set<bool>(L"IsForce", true); q.Call(o); uex::Log(L"spawn %s", ue::NameOf(o).c_str()); }
        }
        else if (starts(L"mspawn ")) {  // mspawn <class name> [cm ahead]: a loaded character class next to the player
            wchar_t cn[200]{}; double d = 400, side = 0; int still = 0; swscanf(arg(7), L"%199s %lf %lf %d", cn, &d, &side, &still);
            UObject* cls = nullptr; std::wstring dn = std::wstring(L"Default__") + cn;
            for (int k = 0, N = ue::NumObjects(); k < N && !cls; ++k) { auto o = ue::ObjectAt(k); if (o && ue::NameOf(o) == dn) cls = o->cls; }
            if (!cls && wcschr(cn, L'/')) cls = uex::LoadAsset(cn);  // a class asset path: /Pkg/.../BP_X.BP_X_C
            auto r = CtrlRot(); r.pitch = r.roll = 0; ik::V f, rt, up; ik::Axes(r, f, rt, up);
            ik::V at = ik::FromU(uex::Loc(me)) + f * d + rt * side;
            {   // drop onto the ground below (the terrain isn't flat)
                auto kl = uex::SystemLib();
                ue::Params t(uex::Fn(kl, L"LineTraceSingle"));
                t.Set(L"WorldContextObject", me); t.Set(L"Start", ik::ToU(at + ik::V{0, 0, 600})); t.Set(L"End", ik::ToU(at - ik::V{0, 0, 1500}));
                t.Set<uint8_t>(L"TraceChannel", 0); t.Set<bool>(L"bTraceComplex", false); t.Set<uint8_t>(L"DrawDebugType", 0); t.Set<bool>(L"bIgnoreSelf", true); t.Set<float>(L"DrawTime", 0.f);
                t.Call(kl);
                if (t.Get<bool>(L"ReturnValue")) {
                    struct HB { uint8_t b[0x100]; } hb = t.Get<HB>(L"OutHit");
                    auto gs = uex::GameplayStatics(); ue::Params b(uex::Fn(gs, L"BreakHitResult")); b.Set(L"Hit", hb); b.Call(gs);
                    at = ik::FromU(b.Get<uex::Vec>(L"ImpactPoint")) + ik::V{0, 0, 100};
                }
            }
            auto a = cls ? uex::Spawn(cls, ik::ToU(at), r.yaw + 180) : nullptr;
            if (a) { ue::Params q(uex::Fn(a, L"SpawnDefaultController")); q.Call(a); }
            if (a && still) g_still.push_back({a, GetTickCount64() + 800});  // stand still: stop the AI's brain once it's up
            uex::Log(L"mspawn %s -> %s", cn, a ? ue::NameOf(a).c_str() : L"-");
        }
        else if (starts(L"kill ")) {  // kill <mode>: the nearest enemy, one death path (shoot.cpp KillTest)
            auto e = NearestEnemy(me);
            if (e) KillTest(e, _wtoi(arg(5)), ik::Norm(ik::FromU(uex::Loc(e)) - ik::FromU(uex::Loc(me))));
            else uex::Log(L"kill: no enemy");
        }
        else if (starts(L"find ")) {
            auto key = c.substr(5); auto chCls = uex::Class(L"Class /Script/ProjectPBZ.PBZCharacter");
            for (int k = 0, n = ue::NumObjects(); chCls && k < n; ++k) {
                auto o = ue::ObjectAt(k);
                if (!o || !ue::IsA(o, chCls) || ue::NameOf(o).find(key) == std::wstring::npos) continue;
                auto l = (o->flags & 0x10) ? uex::Vec{} : uex::Loc(o); bool dead = false;
                if (auto lib = uex::Lib(L"SGameAIStateUtils /Script/SGameAI.Default__SGameAIStateUtils"); lib && !(o->flags & 0x10)) {
                    ue::Params q(uex::Fn(lib, L"IsDead")); q.Set(L"Pawn", o); q.Call(lib); dead = q.Get<bool>(L"ReturnValue"); }
                auto ai = (o->flags & 0x10) ? nullptr : AnimOf(o); UObject* mt = nullptr;
                if (ai) { ue::Params q(uex::Fn(ai, L"GetCurrentActiveMontage")); q.Call(ai); mt = q.Get<UObject*>(L"ReturnValue"); }
                uex::Log(L"find %s flags %x hp %.0f alive %d dead %d at %.0f %.0f %.0f mont %s", ue::NameOf(o).c_str(), o->flags,
                    uex::AttrSet(o) ? uex::Attr(o, uex::attr::Health) : -1.f, uex::Alive(o), dead, l.x, l.y, l.z, mt ? ue::NameOf(mt).c_str() : L"-");
            }
        }
        else if (starts(L"lsmont ")) {
            auto key = c.substr(7); auto mcls = uex::Class(L"Class /Script/Engine.AnimMontage"); int n = 0;
            for (int k = 0, N = ue::NumObjects(); mcls && k < N && n < 80; ++k) {
                auto o = ue::ObjectAt(k);
                if (o && o->cls == mcls && ue::NameOf(o).find(key) != std::wstring::npos) { uex::Log(L"mont %s", ue::NameOf(o).c_str()); ++n; }
            }
        }
        else if (starts(L"pmont ")) {  // pmont <montage> [rate]: play on the player
            auto sp = c.find(L' ', 6); auto m = FindMontage(c.substr(6, sp - 6));
            float rate = sp == std::wstring::npos ? 1.f : float(_wtof(c.c_str() + sp + 1)), len = -1;
            if (auto ai = AnimOf(me); m && ai) {
                ue::Params p(uex::Fn(ai, L"Montage_Play")); p.Set(L"MontageToPlay", m); p.Set<float>(L"InPlayRate", rate);
                p.Set<uint8_t>(L"ReturnValueType", 0); p.Set<float>(L"InTimeToStartMontageAt", 0.f); p.Set<bool>(L"bStopAllMontages", true);
                p.Call(ai); len = p.Get<float>(L"ReturnValue");
            }
            uex::Log(L"pmont %p len %.2f", m, len);
        }
        else if (starts(L"pseq ")) {  // pseq <asset path> [rate] [freeze at s, <0 plays] [slot]: hold with another animation
            wchar_t pth[400]{}, slot[64]{}; double rate = 1, at = -1;
            swscanf(arg(5), L"%399s %lf %lf %63s", pth, &rate, &at, slot);
            auto a = uex::LoadAsset(pth);
            uex::Log(L"pseq %s -> %p %s", pth, a, a ? ue::FullName(a->cls).c_str() : L"-");
            if (a) PoseUse(me, a, rate, at, slot);
        }
        else if (c == L"pbow") PoseUse(me, nullptr, 1.5, .675, L"DefaultSlot");
        else if (starts(L"skel ")) {  // skel <asset path>: its skeleton next to the player mesh's
            auto skelOf = [](UObject* o) -> UObject* { auto pi = ue::FindProp(o ? o->cls : nullptr, L"Skeleton"); return pi.offset >= 0 ? ue::At<UObject*>(o, pi.offset) : nullptr; };
            auto a = uex::LoadAsset(arg(5)); auto m = MeshOf(me);
            auto pi = ue::FindProp(m ? m->cls : nullptr, L"SkeletalMeshAsset"); UObject* sm = pi.offset >= 0 ? ue::At<UObject*>(m, pi.offset) : nullptr;
            auto s1 = skelOf(a), s2 = skelOf(sm);
            uex::Log(L"skel asset %s: %s / player mesh %s: %s", a ? ue::NameOf(a).c_str() : L"-", s1 ? ue::PathOf(s1).c_str() : L"-",
                sm ? ue::NameOf(sm).c_str() : L"-", s2 ? ue::PathOf(s2).c_str() : L"-");
        }
        else if (c == L"mhide" || c == L"mshow") ModelHide(c == L"mhide");
        else if (c == L"bones") {  // key bones in the body frame (cm forward / right / up from the actor)
            auto m = MeshOf(me); if (!m) continue;
            uex::Rot br = uex::GetRot(me); br.pitch = br.roll = 0; ik::V f, rt, up; ik::Axes(br, f, rt, up);
            ik::V o = ik::FromU(uex::Loc(me));
            for (auto bn : {L"head", L"neck_01", L"clavicle_r", L"upperarm_r", L"lowerarm_r", L"hand_r", L"clavicle_l", L"lowerarm_l", L"hand_l", L"spine_03", L"pelvis"}) {
                ue::Params q(uex::Fn(m, L"GetBoneIndex")); q.Set(L"BoneName", uex::Name(bn)); q.Call(m);
                if (q.Get<int32_t>(L"ReturnValue") < 0) { uex::Log(L"bone %s missing", bn); continue; }
                ik::V d = Sock(m, bn) - o;
                uex::Log(L"bone %-10s %6.1f %6.1f %6.1f", bn, ik::Dot(d, f), ik::Dot(d, rt), ik::Dot(d, up));
            }
        }
        else if (starts(L"mslot ")) {  // mslot <montage path>: its slot names and length
            auto a = uex::LoadAsset(arg(6)); if (!a) { uex::Log(L"mslot no asset"); continue; }
            auto pi = ue::FindProp(a->cls, L"SlotAnimTracks");
            if (pi.offset >= 0) { auto t = ue::At<TA>(a, pi.offset); auto stride = 0x18;
                for (int i = 0; i < t.n; ++i) uex::Log(L"mslot %s slot %d %s", ue::NameOf(a).c_str(), i, ue::NameOf(*reinterpret_cast<ue::FName*>(static_cast<uint8_t*>(t.d) + i * stride)).c_str()); }
            ue::Params q(uex::Fn(a, L"GetPlayLength")); q.Call(a); uex::Log(L"mslot len %.2f", q.Get<float>(L"ReturnValue"));
        }
        else if (starts(L"aimp ")) {  // aimp <pitch> <alpha>: the anim instance's combat aim offset (CombatAimAnimInterface)
            swscanf(arg(5), L"%lf %lf", v, v + 1); auto ai = AnimOf(me);
            auto fp = ai ? uex::Fn(ai, L"SetCombatAimPitchOffset") : nullptr, fa = ai ? uex::Fn(ai, L"SetCombatAimAlpha") : nullptr;
            if (fa) { ue::Params q(fa); q.Set<float>(L"Alpha", float(v[1])); q.Call(ai); }
            if (fp) { ue::Params q(fp); q.Set<float>(L"PitchOffset", float(v[0])); q.Call(ai); }
            float got = -999; if (auto fg = ai ? uex::Fn(ai, L"GetCombatAimPitchOffset") : nullptr) { ue::Params q(fg); q.Call(ai); got = q.Get<float>(L"ReturnValue"); }
            uex::Log(L"aimp anim %s fn %p %p pitch now %.1f", ai ? ue::NameOf(ai->cls).c_str() : L"-", fp, fa, got);
        }
        else if (c == L"shot") uex::Console(L"HighResShot 1");
        else if (c == L"mdbg") ModelDebug();
        else if (starts(L"walk ")) { swscanf(arg(5), L"%lf %lf %lf", &g_test.walkF, &g_test.walkR, &g_test.walkT); }
        else if (c == L"crouch" || c == L"uncrouch") { ue::Params q(uex::Fn(me, c == L"crouch" ? L"Crouch" : L"UnCrouch")); q.Set<bool>(L"bClientSimulation", false); q.Call(me); }
        else if (c == L"jump") { ue::Params q(uex::Fn(me, L"Jump")); q.Call(me); }
        else if (c == L"fxlist" || c == L"fxkill") FxList(me, c == L"fxkill");
        else uex::Log(L"test: unknown command");
    }
}

// one step per tick: turn the controller by the angle between the camera's view and the target (chest,
// or the head sphere's centre); the spring arm moves the camera as it turns, so it converges over ticks
void AimStep() {
    if (g_test.aimSteps <= 0) return;
    --g_test.aimSteps;
    auto t = g_test.aimAt;
    if (!Live(t)) { g_test.aimSteps = 0; return; }
    ik::V p = ik::FromU(uex::Loc(t)); p.z += g_test.aimOff;
    if (g_test.aimHead) { ik::V h; if (HeadPos(t, &h)) p = h + ik::V{0, 0, 5}; }
    ik::V eye; uex::Rot cam; if (!CamView(&eye, &cam)) return;
    uex::Rot want = ik::RotOf(p - eye);
    double ep = Wrap(want.pitch - cam.pitch), ey = Wrap(want.yaw - cam.yaw);
    auto r = CtrlRot(); r.pitch += ep; r.yaw += ey; SetCtrlRot(r);
    if (g_test.aimSteps == 0 || (fabs(ep) < .03 && fabs(ey) < .03)) {
        uex::Log(L"test: aim error %.3f %.3f deg, %.1f m", ep, ey, ik::Len(p - eye) / 100);
        g_test.aimSteps = 0;
    }
}

void WalkStep(UObject* me, double dt) {
    if (g_test.walkT <= 0 || !me) return;
    g_test.walkT -= dt;
    auto r = CtrlRot(); r.pitch = 0; r.roll = 0;
    ik::V f, rt, up; ik::Axes(r, f, rt, up);
    ik::V d = f * g_test.walkF + rt * g_test.walkR;
    ue::Params q(uex::Fn(me, L"AddMovementInput")); q.Set(L"WorldDirection", ik::ToU(d)); q.Set<float>(L"ScaleValue", 1.f); q.Set<bool>(L"bForce", true); q.Call(me);
}

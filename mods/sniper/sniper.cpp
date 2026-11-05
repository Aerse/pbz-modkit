// sniper.cpp: CS rifles in PBZ (local mod). AWP, AK-47 and M4A4 with CS2 handling: accuracy, spray /
// recoil, damage falloff, reload and bolt timings, zoom, and the sounds; models, hold pose (model.cpp),
// bullets and effects (shoot.cpp), HUD and scope (overlay.cpp), sounds (audio.cpp).
//   F6        draw / put away (refills the ammo)
//   1 / 2 / 3 AWP / AK-47 / M4A4
//   LMB       fire (AK / M4 automatic)
//   RMB       AWP zoom: 40 -> 15 -> off; AK-47 / M4A4: hold to aim (view zooms in)
//   R / T     reload
#include "common.h"
#include "overlay.h"
#include <algorithm>

const PbzHost* g_h;
std::wstring g_modDir, g_cacheDir;
bool g_dbg;
double g_now;
static int g_tick;

constexpr double kDeg = 0.0174532925;
enum { kIdle, kBolt, kReload, kDeploy };
struct Slot { int ammo = 0, reserve = 0; Acc acc; };
static Slot g_slot[kWpnN];
static bool g_on;
static int g_cur;                  // selected weapon
static int g_zoom;                 // 0 off, 1 / 2 zoom level (AWP)
static int g_act; static double g_actT, g_actLen; static int g_sndNext;
static double g_nextShot;          // g_now when the next shot may fire
static bool g_rezoom;              // AWP: zoom back in when the bolt is home
static bool g_trigPrev;
static int g_shots;                // tracer count
static double g_helpUntil, g_hideAt;
static double g_camApplied[2];     // camera punch now added to the control rotation: up, right (deg)
static double g_baseFov = 90; static bool g_ctrlInit;
static UObject* g_me;
// rifles (no scope): RMB held raises the sights, the view narrows to kAdsFov x the normal field of view
constexpr double kAdsFov = .6, kAdsTime = .15;  // share, s to full
static double g_ads;               // 0..1

static double Wrap(double a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
int CurWpn() { return g_on ? g_cur : -1; }

// LMB / RMB / keys count only with the game in front, no menu (cursor) and not paused
static bool InputOk() {
    if (!Front()) return false;
    auto pc = uex::PC(); if (!pc) return false;
    uint8_t m = 0; auto b = BoolAt(pc, L"bShowMouseCursor", &m);
    if (b && (*b & m)) return false;
    if (auto gs = uex::GameplayStatics()) {
        ue::Params q(uex::Fn(gs, L"IsGamePaused")); q.Set(L"WorldContextObject", uex::World()); q.Call(gs);
        if (q.Get<bool>(L"ReturnValue")) return false;
    }
    return true;
}
static MoveSt Move(UObject* me) {
    MoveSt m{0, true, false};
    { ue::Params q(uex::Fn(me, L"GetVelocity")); q.Call(me); auto v = q.Get<uex::Vec>(L"ReturnValue"); m.speed = hypot(v.x, v.y) / 2.54; }
    if (auto cm = uex::Movement(me)) { ue::Params q(uex::Fn(cm, L"IsFalling")); q.Call(cm); m.grounded = !q.Get<bool>(L"ReturnValue"); }
    static UObject* who; static uint8_t* cb; static uint8_t mk;
    if (who != me) { who = me; cb = BoolAt(me, L"bIsCrouched", &mk); }
    if (cb) m.crouch = (*cb & mk) != 0;
    return m;
}
static float CamFov() {
    auto cm = CamMgr(); if (!cm) return 90;
    ue::Params f(uex::Fn(cm, L"GetFOVAngle")); f.Call(cm); float v = f.Get<float>(L"ReturnValue");
    return v > 1 && v < 170 ? v : 90;
}

// the next shot would be a clean one: no bolt / reload, cycle done, standing still on the ground (selftest)
bool WpnSteady() {
    if (!g_on || !g_me || g_act != kIdle || g_now < g_nextShot) return false;
    MoveSt m = Move(g_me); return m.grounded && m.speed < 5;
}
static void SetZoom(int level, bool sound) {
    const Wpn& w = g_wpn[g_cur];
    if (level > w.zoomLevels) level = 0;
    if (level == g_zoom) return;
    if (sound) audio::Play(level ? L"zoom" : L"awp_zoom_out", .8f);
    g_zoom = level;
    SetFov(level ? w.zoomFov[level - 1] : 0.f);
    ModelHide(level > 0);
}
static void StartAct(int act, double len) { g_act = act; g_actT = 0; g_actLen = len; g_sndNext = 0; }
static void Select(UObject* me, int i) {
    SetZoom(0, false); g_rezoom = false;
    if (g_ads > 0) { g_ads = 0; SetFov(0); }
    g_cur = i;
    if (!ModelShow(me, i)) uex::Log(L"%s model missing", g_wpn[i].id);
    StartAct(kDeploy, std::min(g_wpn[i].deploy, .25f));  // quick swap
    g_slot[i].acc = Acc{};
    audio::Play(g_wpn[i].draw, .8f);
    uex::Log(L"draw %s (%d / %d)", g_wpn[i].id, g_slot[i].ammo, g_slot[i].reserve);
}
static void Equip(UObject* me, int i) {
    if (!g_on) {
        g_on = true; g_me = me; g_ctrlInit = false; g_camApplied[0] = g_camApplied[1] = 0;
        for (int k = 0; k < kWpnN; ++k) { g_slot[k].ammo = g_wpn[k].clip; g_slot[k].reserve = g_wpn[k].reserve; }
        g_helpUntil = g_now + 8; g_hideAt = 0;
        audio::Init();
    }
    Select(me, i);
}
// scoped: the third-person camera looks past the player, so the player's own body is hidden meanwhile
static UObject* g_bodyHidden;  // the pawn we hid
static void BodyHide(UObject* me, bool h) {
    if (h ? g_bodyHidden == me : !g_bodyHidden) return;
    auto set = [](UObject* o, bool v) { ue::Params q(uex::Fn(o, L"SetActorHiddenInGame")); q.Set<bool>(L"bNewHidden", v); q.Call(o); };
    if (g_bodyHidden && Live(g_bodyHidden)) set(g_bodyHidden, false);
    g_bodyHidden = nullptr;
    if (h && Live(me)) { set(me, true); g_bodyHidden = me; }
}
static void Unequip() {
    auto me = uex::Me();
    if (g_zoom || g_ads > 0) { g_zoom = 0; g_ads = 0; SetFov(0); }
    ModelHide(false); BodyHide(nullptr, false);
    HoldOff(Live(g_me) ? g_me : nullptr);
    ModelDrop(); HideWeapons(nullptr, false); BlockKeys(false);
    overlay::Set(overlay::HudState{});
    g_on = false; g_act = kIdle; g_rezoom = false; g_test = TestState{};
    uex::Log(L"put away");
    (void)me;
}
static void StartReload() {
    const Wpn& w = g_wpn[g_cur]; Slot& s = g_slot[g_cur];
    if (s.ammo >= w.clip || s.reserve <= 0) return;
    SetZoom(0, false); g_rezoom = false;
    StartAct(kReload, w.reload);
    uex::Log(L"reload %s %d + %d", w.id, s.ammo, s.reserve);
}

// one bullet: CS eye angles = the camera without the punch we added; bullet = eye + 2 x punch, then the
// inaccuracy + spread cone. Effects from the muzzle (none through the scope), feedback from the result.
static int g_rezoomLevel; static double g_unzoomAt;
ShotRes g_lastShot{-1, 0, nullptr, {}}; int g_shotN;
static void Fire(UObject* me) {
    const Wpn& w = g_wpn[g_cur]; Slot& s = g_slot[g_cur];
    int alt = g_zoom > 0 ? 1 : 0;
    double inacc = AccInacc(w, alt, s.acc, Move(me)), before[2];
    AccFire(s.acc, w, alt, g_now, before);
    ik::V eye; uex::Rot view; if (!CamView(&eye, &view)) return;
    uex::Rot r{view.pitch - g_camApplied[0] + 2 * before[0], view.yaw - g_camApplied[1] + 2 * before[1], 0};
    ik::V dir = ShotDir(r, inacc, w.st[alt].spread);
    ik::V mz, mdir; bool hasMz = MuzzlePos(w, &mz, &mdir);
    if (!hasMz) mz = ik::FromU(uex::Loc(me)) + ik::V{0, 0, 50};
    bool tracer = !alt && (++g_shots % w.tracerFreq) == 0;
    audio::Play(w.shot[int(Rand() * w.nShot) % w.nShot], w.shotVol);
    if (hasMz && !alt) MuzzleFx(w, mz, dir);
    ShotRes res = Shoot(me, w, eye, dir, mz, tracer);
    g_lastShot = res; ++g_shotN;
    if (res.kind >= 0) overlay::Hit(res.kind, res.dmg);
    --s.ammo;
    g_nextShot = g_now - g_nextShot < .05 ? g_nextShot + w.cycle : g_now + w.cycle;  // keeps the cyclic rate exact
    if (!w.autoFire && s.ammo > 0 && w.boltSnd[0].snd) {  // bolt action: out of the scope, back in when the bolt is home
        StartAct(kBolt, w.cycle);
        if (g_zoom) { g_rezoomLevel = g_zoom; g_rezoom = true; g_unzoomAt = g_now + .12; }
    } else if (g_zoom && s.ammo == 0) g_unzoomAt = g_now + .12;
    if (g_dbg) uex::Log(L"shot %s inacc %.4f punch %.2f %.2f -> %.2f %.2f kind %d dmg %d ammo %d", w.id, inacc,
        before[0], before[1], s.acc.punch[0], s.acc.punch[1], res.kind, res.dmg, s.ammo);
}

// action timers: the timed sounds of the reload / bolt lists, the result when the time is up
static void Acts(double dt) {
    if (g_unzoomAt > 0 && g_now >= g_unzoomAt) { g_unzoomAt = 0; SetZoom(0, false); }
    if (g_act == kIdle) return;
    const Wpn& w = g_wpn[g_cur]; Slot& s = g_slot[g_cur];
    g_actT += dt;
    const SndAt* list = g_act == kReload ? w.reloadSnd : g_act == kBolt ? w.boltSnd : nullptr;
    int n = g_act == kReload ? 5 : 3;
    while (list && g_sndNext < n && list[g_sndNext].t > 0 && list[g_sndNext].snd && g_actT >= list[g_sndNext].t)
        audio::Play(list[g_sndNext++].snd, .85f);
    if (g_actT < g_actLen) return;
    if (g_act == kReload) {
        int take = std::min(w.clip - s.ammo, s.reserve);
        s.ammo += take;  // reserve is unlimited
        uex::Log(L"reloaded %s %d / %d", w.id, s.ammo, s.reserve);
    }
    if (g_act == kBolt && g_rezoom) { g_rezoom = false; if (g_zoom == 0) SetZoom(g_rezoomLevel, false); }
    g_act = kIdle;
}

// scoped: the mouse turns slower in proportion to the zoom (CS zoom_sensitivity_ratio 1)
static double AdsFov() { return g_baseFov * (1 - (1 - kAdsFov) * g_ads); }
static void ZoomSens() {
    auto cur = CtrlRot();
    if ((g_zoom || g_ads > 0) && g_ctrlInit) {
        double fov = g_zoom ? g_wpn[g_cur].zoomFov[g_zoom - 1] : AdsFov();
        double k = tan(fov * kDeg / 2) / tan(g_baseFov * kDeg / 2);
        uex::Rot r = g_ctrlLast;
        r.pitch += Wrap(cur.pitch - g_ctrlLast.pitch) * k; r.yaw += Wrap(cur.yaw - g_ctrlLast.yaw) * k; r.roll = cur.roll;
        SetCtrlRot(r);
    }
}
// camera punch: the view shows 0.9 x the aim punch (CS view_recoil_tracking .45 of the 2x bullet offset),
// added to the control rotation as the change since last frame so the player's own mouse input stays
static void CamPunch(const Acc& a) {
    double want[2] = {a.punch[0] * .9, a.punch[1] * .9};
    double d0 = want[0] - g_camApplied[0], d1 = want[1] - g_camApplied[1];
    if (fabs(d0) < 1e-6 && fabs(d1) < 1e-6) return;
    auto r = CtrlRot(); r.pitch += d0; r.yaw += d1; SetCtrlRot(r);
    g_camApplied[0] = want[0]; g_camApplied[1] = want[1];
}

static void Hud(const MoveSt& mv) {
    const Wpn& w = g_wpn[g_cur]; const Slot& s = g_slot[g_cur];
    overlay::HudState h;
    h.on = true; h.scope = g_zoom > 0; h.help = g_now < g_helpUntil; h.label = w.label;
    for (int i = 0; i < kWpnN; ++i) h.slots[i] = g_wpn[i].label;
    h.ammo = s.ammo; h.clip = w.clip; h.reserve = s.reserve; h.sel = g_cur;
    h.kind = g_act; h.prog = g_act ? float(g_actT / (g_actLen > 0 ? g_actLen : 1)) : 0;
    h.xmode = w.autoFire ? 1 : 0;  // CS: no crosshair on the AWP
    h.xgap = float(AccInacc(w, 0, s.acc, mv) + w.st[0].spread);
    h.fov = CamFov();
    overlay::Set(h);
}

// E with the rifle out: the game's interaction scan finds nothing while our pose plays, so drop the pose,
// let the scan pick the target up and start the interaction ourselves; the pose comes back when it's over
static UObject* InteractComp(UObject* me) {
    static UObject *who, *ic;
    if (who != me || !Live(ic)) {
        who = me; ic = nullptr;
        for (int k = 0, N = ue::NumObjects(); k < N && !ic; ++k) { auto o = ue::ObjectAt(k); if (o && o->outer == me && ue::NameOf(o->cls) == L"PBZInteractComponent") ic = o; }
    }
    return ic;
}
static UObject* IcGet(UObject* ic, const wchar_t* fn) { ue::Params g(uex::Fn(ic, fn)); g.Call(ic); return g.Get<UObject*>(L"ReturnValue"); }
// interactables in range: the classes the component searches (LevelA/B/C, TArray<UClass*> at +0x110/+0x120/+0x130),
// in its range and triggerable. The game's own check refuses while our pose montage plays, so near one the pose drops
// and the game shows the E prompt and runs the interaction itself.
struct CA { UObject** d; int n, m; };
static bool NearInteract(UObject* me, UObject* ic) {
    static std::vector<UObject*> cands; static double listAt, checkAt; static bool isNear;
    if (g_now >= listAt) {
        listAt = g_now + 2; cands.clear();
        // actors close by that the component would take (its class lists are soft refs, so test them all)
        static UObject* actorCls = uex::Class(L"Class /Script/Engine.Actor");
        ik::V at = ik::FromU(uex::Loc(me));
        for (int k = 0, N = ue::NumObjects(); k < N && actorCls; ++k) {
            auto o = ue::ObjectAt(k); if (!o || o == me || !o->cls || (o->flags & 0x30) || !ue::IsA(o, actorCls)) continue;
            if (ik::Len(ik::FromU(uex::Loc(o)) - at) > 800) continue;
            ue::Params t(uex::Fn(ic, L"CanTriggerInteract")); t.Set(L"Actor", o); t.Call(ic);
            if (t.Get<bool>(L"ReturnValue")) cands.push_back(o);
        }
        if (g_dbg) for (auto c : cands) uex::Log(L"  cand %s", ue::NameOf(c).c_str());
    }
    if (g_now < checkAt) return isNear;
    checkAt = g_now + .2; isNear = false;
    for (auto a : cands) {
        if (!Live(a)) continue;
        ue::Params d(uex::Fn(ic, L"GetValidDistance")); d.Set(L"SearchActor", a); d.Set(L"Player", me); d.Call(ic);
        if (!d.Get<bool>(L"ReturnValue")) continue;
        ue::Params t(uex::Fn(ic, L"CanTriggerInteract")); t.Set(L"Actor", a); t.Call(ic);
        if (!t.Get<bool>(L"ReturnValue")) continue;
        ue::Params f(uex::Fn(ic, L"IsPlayerFaceToActor")); f.Set(L"Player", me); f.Set(L"SearchActor", a); f.Call(ic);
        if (f.Get<bool>(L"ReturnValue")) { isNear = true; break; }
    }
    return isNear;
}
static bool InteractStep(UObject* me) {  // true: leave the body to the game this frame
    auto ic = InteractComp(me); if (!ic) return false;
    bool on = NearInteract(me, ic) || IcGet(ic, L"GetInteractingActor");
    static bool was; if (on != was) { was = on; uex::Log(L"interactable %s", on ? L"near: rifle down" : L"gone"); if (on) HoldOff(me); }
    return on;
}
static void Tick(void*, float dtf) {
    double dt = dtf > .1f ? .1 : dtf < 0 ? 0 : dtf;  // hitches: one long frame is not a long recovery
    g_now += dt;
    auto me = uex::Me();
    bool f6 = Pressed(VK_F6), k1 = Pressed('1'), k2 = Pressed('2'), k3 = Pressed('3'), rmb = Pressed(VK_RBUTTON);
    bool rk = Pressed('R'), tk = Pressed('T'), ek = Pressed('E');
    if (!me) { if (g_on) Unequip(); return; }
    bool in = InputOk();
    if ((f6 && in) || g_test.equip) { g_test.equip = false; if (g_on) Unequip(); else Equip(me, g_cur); }
    TestCmds(me);
    AimStep();
    FxTick(dt);
    DeathTick();
    WalkStep(me, dt);
    if (!g_on) return;
    if (me != g_me) {  // pawn changed (travel / respawn): rebuild on the new one
        g_me = me; HideWeapons(nullptr, false); g_hideAt = 0;
        if (!ModelShow(me, g_cur)) uex::Log(L"model lost");
    }
    const Wpn& w = g_wpn[g_cur]; Slot& s = g_slot[g_cur];
    if (g_zoom || g_ads > 0) ZoomSens(); else g_baseFov = CamFov();
    if (!(g_test.skip & 1)) BlockKeys(true); else BlockKeys(false);
    if (!(g_test.skip & 2) && g_now >= g_hideAt) { g_hideAt = g_now + 1; HideWeapons(me, true); }
    if (in) {
        int sel = k1 ? 0 : k2 ? 1 : k3 ? 2 : -1;
        if (sel >= 0) { if (sel != g_cur) Select(me, sel); }
        else if ((rk || tk) && g_act == kIdle) StartReload();
        else if (rmb && w.zoomLevels && g_act == kIdle && g_unzoomAt == 0) SetZoom((g_zoom + 1) % (w.zoomLevels + 1), true);
    }
    const Wpn& cw = g_wpn[g_cur]; Slot& cs = g_slot[g_cur];  // the selection may have changed
    {   // rifles: hold RMB to aim (not while reloading or drawing)
        bool want = !cw.zoomLevels && g_act == kIdle && ((in && Down(VK_RBUTTON)) || g_test.ads);
        double was = g_ads;
        g_ads = std::clamp(g_ads + (want ? dt : -dt) / kAdsTime, 0.0, 1.0);
        if (g_ads > 0) SetFov(float(AdsFov()));
        else if (was > 0) SetFov(0);
    }
    (void)w; (void)s;
    Acts(dt);
    MoveSt mv = Move(me);
    AccRecover(cs.acc, cw, dt, mv.crouch, g_now);
    bool trig = (in && Down(VK_LBUTTON)) || g_test.hold || g_test.burst > 0;
    bool edge = trig && !g_trigPrev; g_trigPrev = trig;
    if (g_test.fire) edge = true;
    bool ready = g_act == kIdle && g_now >= g_nextShot;
    if (ready && cs.ammo > 0 && ((cw.autoFire ? trig : edge) || g_test.fire)) {
        Fire(me);
        g_test.fire = false;
        if (g_test.burst > 0) --g_test.burst;
    } else if (ready && cs.ammo == 0 && cs.reserve > 0) StartReload();  // empty: reload on its own
    if (g_test.fire && !ready && g_act == kIdle && cs.ammo == 0) g_test.fire = false;
    CamPunch(cs.acc);
    if (InteractStep(me)) {} else if (!(g_test.skip & 4)) Hold(me, float(dt), cw); else if (g_test.skip & 8) HoldOff(me);
    g_test.interact = false;
    BodyHide(me, g_zoom > 0);
    ik::V cl; uex::Rot cr;
    if (CamView(&cl, &cr)) { ik::V f, rt, up; ik::Axes(cr, f, rt, up); audio::Listener(cl, rt); }
    if (!(g_test.skip & 16)) Hud(mv);
    g_ctrlLast = CtrlRot(); g_ctrlInit = true;
}

static int WpnId(const std::wstring& a) {
    for (int i = 0; i < kWpnN; ++i) if (a == g_wpn[i].id || a == std::to_wstring(i)) return i;
    return -1;
}
bool MainCmd(UObject* me, const std::wstring& c) {
    auto arg = c.find(L' ') == std::wstring::npos ? std::wstring() : c.substr(c.find(L' ') + 1);
    auto cmd = c.substr(0, c.find(L' '));
    if (cmd == L"equip") { int i = WpnId(arg); Equip(me, i >= 0 ? i : g_cur); }
    else if (cmd == L"unequip") { if (g_on) Unequip(); }
    else if (cmd == L"wpn") { int i = WpnId(arg); if (g_on && i >= 0) Select(me, i); }
    else if (cmd == L"reload") { if (g_on) StartReload(); }
    else if (cmd == L"zoom") { if (g_on) SetZoom(arg.empty() ? (g_zoom + 1) % (g_wpn[g_cur].zoomLevels + 1) : _wtoi(arg.c_str()), true); }
    else if (cmd == L"ammo") { if (g_on) g_slot[g_cur].ammo = _wtoi(arg.c_str()); }
    else if (cmd == L"info") {
        if (!g_on) { uex::Log(L"info: off"); return true; }
        auto& s = g_slot[g_cur]; auto mv = Move(me); auto r = CtrlRot();
        uex::Log(L"info %s ammo %d/%d act %d %.2f/%.2f zoom %d punch %.2f %.2f idx %.1f pen %.4f inacc %.4f speed %.0f ground %d crouch %d fov %.0f ctrl %.1f %.1f cam %.2f %.2f",
            g_wpn[g_cur].id, s.ammo, s.reserve, g_act, g_actT, g_actLen, g_zoom, s.acc.punch[0], s.acc.punch[1], s.acc.index, s.acc.penalty,
            AccInacc(g_wpn[g_cur], g_zoom ? 1 : 0, s.acc, mv), mv.speed, mv.grounded, mv.crouch, CamFov(), r.pitch, r.yaw, g_camApplied[0], g_camApplied[1]);
    }
    else return false;
    return true;
}

extern "C" __declspec(dllexport) void PbzInit(const PbzHost* h) { g_h = h; uex::SetLogName(L"sniper"); uex::SetHost(h); }
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
extern "C" __declspec(dllexport) void PbzRun(const wchar_t* dir) {
    g_modDir = dir && *dir ? dir : g_h->dir;
    if (g_modDir.back() != L'\\' && g_modDir.back() != L'/') g_modDir += L'\\';
    // assets: <mod>\cache\, else an older layout's <mod>\sniper_cache\ or <root>\sniper_cache\ (flat install)
    std::wstring root = g_h->dir;
    g_cacheDir = g_modDir + L"cache\\";
    for (auto& c : {g_modDir + L"cache\\", g_modDir + L"sniper_cache\\", root + L"sniper_cache\\"})
        if (IsDir(c)) { g_cacheDir = c; break; }
    uex::Log(L"assets in %s", g_cacheDir.c_str());
    WpnInit();
    overlay::Start();
    g_tick = g_h->addTick(Tick, nullptr);
    uex::Log(L"loaded (F6 draw / put away, 1 AWP 2 AK-47 3 M4A4, LMB fire, RMB zoom (AWP) / hold to aim (rifles), R reload)");
}
extern "C" __declspec(dllexport) void PbzUnload() {
    if (g_tick > 0) g_h->removeTick(g_tick);
    g_tick = 0;
    if (g_on) Unequip();
    FxClear();
    overlay::Stop();
    audio::Shutdown();
}

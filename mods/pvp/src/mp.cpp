// mp.dll: PvP mod entry. The room phase from the server drives the flow; ui owns the menus.
//   lobby -> load (open the map's level if needed, fix the pawn, build arena, teleport to spawn,
//   fight::Begin, ArenaReady) -> count (frozen) -> fight -> end (next round: load again)
//   -> over (stay in arena) -> lobby (back home)
// Players enter 多人游戏 from the title screen, no save is loaded: the first load opens the map's
// level (arena::Maps), with the save veto and fileguard's save-file block on from before the open
// until mp unloads. "Home" is then the title screen. A match started from inside a loaded save
// on a map without a level keeps the old behaviour: home is the position before the first round.
// Leaving the room at any point also returns the player home and undoes everything.
#include "uex.h"
#include "net/net.h"
#include "ui/ui.h"
#include "arena/arena.h"
#include "fight/fight.h"
#include "fileguard.h"
static const PbzHost* g_h; static int g_tick = -1;
static std::string g_phase; static int g_round = -1; static bool g_veto;

// where the player stood before the first round; restored when the match is left
static bool g_haveHome; static uex::Vec g_home; static double g_homeYaw;
// after returning home the level under the player may still be streaming in: hold the player
// with movement disabled until a downward trace finds ground (or give up after kLandWait)
static bool g_landing; static double g_landUntil;
constexpr double kLandWait = 15;

// level travel: g_opened = mp has opened a level this session (home is the title from then on).
// g_travel: an open we issued has not settled yet (a world change then is ours, not a crash);
// g_travelAt: when it was issued (retried after kTravelWait); g_arrivedAt: when the new world's
// pawn first showed up (round setup waits kSettle for the level to spawn its actors)
static const wchar_t* kTitle = L"/Game/PBZContent/Maps/InGame/StartMenu_CE/PBZ_StartMenu_CE_WP";
static bool g_guarded;  // a level was opened by mp in this process: the save block outlives mp (PbzUnload)
static bool g_opened, g_travel; static double g_travelAt = -100, g_arrivedAt = -1, g_tidyAt;
static int g_travelMap = -1;
constexpr double kTravelWait = 45, kSettle = 1.5;

static std::wstring WorldPkg() { auto w = uex::World(); return w && w->outer ? ue::PathOf(w->outer) : L""; }
static bool AtTitle() { return WorldPkg() == kTitle; }
static void Open(const wchar_t* level) {
    uex::SaveVeto(true); fileguard::On();  // before the open: the level writes the save right away
    uex::Log(L"open %s (from %s)", level, WorldPkg().c_str());
    uex::OpenLevel(level);
    g_opened = g_guarded = true; g_travel = true; g_travelAt = net::Now(); g_arrivedAt = -1;
}
static void GoTitle() {
    g_travelMap = -1;
    if (!g_opened || AtTitle()) { g_travel = false; return; }
    Open(kTitle);
}
// a level opened by mp: its pawn arrives at hp 0 behind a black camera fade, falling from far off
static void FixPawn(ue::UObject* me) {
    if (uex::Attr(me, uex::attr::Health) < uex::Attr(me, uex::attr::HealthMax))
        uex::SetAttr(me, uex::attr::Health, uex::Attr(me, uex::attr::HealthMax));
    auto gs = uex::GameplayStatics(); auto pc = uex::PC();
    if (!gs || !pc) return;
    ue::Params g(uex::Fn(gs, L"GetPlayerCameraManager"));
    g.Set(L"WorldContextObject", uex::World()); g.Set<int32_t>(L"PlayerIndex", 0); g.Call(gs);
    if (auto cm = g.Get<ue::UObject*>(L"ReturnValue")) {
        { ue::Params p(uex::Fn(cm, L"StopCameraFade")); p.Call(cm); }
        struct LC { float r, g, b, a; };
        ue::Params p(uex::Fn(cm, L"SetManualCameraFade"));
        p.Set<float>(L"InFadeAmount", 0.f); p.Set(L"Color", LC{0, 0, 0, 1}); p.Set<bool>(L"bInFadeAudio", false); p.Call(cm);
    }
    ue::Params v(uex::Fn(pc, L"SetViewTargetWithBlend")); v.Set(L"NewViewTarget", me); v.Set<float>(L"BlendTime", 0.f); v.Call(pc);
}

static void GoHome() {
    if (g_opened) { g_haveHome = false; g_landing = false; GoTitle(); return; }
    auto me = uex::Me();
    if (!g_haveHome || !me) { g_haveHome = false; return; }
    uex::SetMoveMode(me, 0);
    uex::Teleport(me, g_home, g_homeYaw); uex::SetControlYaw(g_homeYaw);
    g_landing = true; g_landUntil = net::Now() + kLandWait; g_haveHome = false;
    uex::Log(L"home %.0f %.0f %.0f, waiting for ground", g_home.x, g_home.y, g_home.z);
}
static void Landing() {
    if (!g_landing) return;
    auto me = uex::Me(); if (!me) return;
    uex::Vec p = uex::Loc(me), hit;
    bool ground = uex::Trace(me, {p.x, p.y, p.z + 50}, {p.x, p.y, p.z - 3000}, &hit);
    if (!ground && net::Now() < g_landUntil) { uex::SetMoveMode(me, 0); return; }
    if (ground && p.z - hit.z > 200) uex::Teleport(me, {p.x, p.y, hit.z + 100}, uex::GetRot(me).yaw);
    uex::SetMoveMode(me, 1);
    g_landing = false;
    uex::Log(ground ? L"landed (ground at %.0f)" : L"landing timed out at %.0f", ground ? hit.z : p.z);
}

static void LeaveMatch(bool home) {
    fight::MatchOver();  // End + restore hp/posture/weapons saved at the first Begin
    g_round = -1;
    uex::IgnoreInput(false, false);
    arena::Clear();
    if (home) GoHome();
}
// in a level we opened while no match needs it (room left, back in the lobby): go to the title
static bool InMapLevel() {
    auto p = WorldPkg();
    for (int i = 0; i < arena::MapCount(); ++i) if (arena::Maps()[i].level && p == arena::Maps()[i].level) return true;
    return false;
}
static bool Stray() { return g_opened && !g_travel && uex::World() && !AtTitle() && InMapLevel(); }

static void Flow() {
    fileguard::Tick();
    Landing();
    net::Room r;
    if (!net::GetRoom(r)) {
        if (!uex::World()) return;  // loading: an open issued now would do nothing
        // our trip home is over, or a trip to the match map is moot now (or never got anywhere):
        // without this Stray() stays false and the player is left frozen in the arena level
        if (g_travel && (AtTitle() || g_travelMap != -1 || net::Now() - g_travelAt > kTravelWait)) g_travel = false;
        if (g_round != -1 || g_haveHome || Stray()) LeaveMatch(true);
        // back at the title: a save the player loads from here is their own world, not a stray arena
        if (!g_travel && g_round == -1 && AtTitle()) g_opened = false;
        if (g_veto) { uex::SaveVeto(false); g_veto = false; }  // kept on by uex while fileguard is on
        g_phase.clear(); return;
    }
    if (!g_veto) { uex::SaveVeto(true); g_veto = true; }
    bool inMatch = r.phase == "load" || r.phase == "count" || r.phase == "fight" || r.phase == "end";
    // world reloaded mid-match (death screen, level change): the arena and our pawn are gone,
    // set the round up again in the new world; the old home position means nothing there
    static ue::UObject* world;
    if (uex::World() != world) {
        if (world && g_round >= 0) {
            uex::Log(g_travel ? L"world changed by our travel in round %d" : L"world changed in round %d", g_round);
            fight::End(); g_round = -2; g_haveHome = false; g_landing = false;
            if (!g_travel) g_travelMap = -1;  // not ours: open the map again right away
        }
        world = uex::World(); g_arrivedAt = -1;
        if (g_travel) uex::Log(L"travel: now in %s", WorldPkg().c_str());
    }
    if (!world) return;  // loading
    // the player went back to the title mid-match (pause menu): that is leaving the match. Without
    // this the room stays in "fight" and the panel can never open again at the title.
    if (inMatch && !g_travel && AtTitle() && g_round != -1) {  // -1: the first load has not left the title yet
        uex::Log(L"back at the title in phase %S round %d: leaving the room", r.phase.c_str(), r.round);
        net::Leave(); LeaveMatch(false); g_travelMap = -1; g_phase.clear();
        return;
    }
    int mi = r.map >= 0 && r.map < arena::MapCount() ? r.map : 0;
    if (inMatch) arena::SetMap(mi);
    if (inMatch && !arena::InLevel(mi)) {  // first load of the match: open the map's level (retry when stuck)
        // also later phases while we never got there (resumed after a crash, or the load timed out)
        if ((r.phase == "load" || g_round < 0) && (g_travelMap != mi || net::Now() - g_travelAt > kTravelWait)) {
            if (g_round >= 0) fight::End();
            g_round = -1; g_haveHome = false; g_landing = false; uex::IgnoreInput(false, false);
            arena::Clear(); g_travelMap = mi; Open(arena::Maps()[mi].level);
        }
        if (r.phase != g_phase) { uex::Log(L"phase %S round %d (travelling)", r.phase.c_str(), r.round); g_phase = r.phase; }
        return;
    }
    if (inMatch && g_travel) {  // arrived: wait for the pawn, then let the level settle
        auto me = uex::Me(); if (!me) return;
        if (g_arrivedAt < 0) { g_arrivedAt = net::Now(); uex::Log(L"travel: pawn in %s", WorldPkg().c_str()); }
        uex::SetMoveMode(me, 0);
        if (net::Now() - g_arrivedAt < kSettle) return;
        // a World Partition level can hand over the pawn before its attributes / backpack are set
        // up (HealthMax 0): the round would start dead and without a weapon. Wait up to 30 s.
        if (uex::Attr(me, uex::attr::HealthMax) <= 0 && net::Now() - g_arrivedAt < 30) return;
        g_travel = false;
    }
    if (!inMatch && g_travel && (AtTitle() || g_travelMap != -1)) g_travel = false;  // back at the title, or the match ended before we got there
    if (!inMatch && !g_travel && g_round == -1 && AtTitle()) g_opened = false;
    if (inMatch && r.round != g_round && r.phase != "end") {
        auto me = uex::Me();
        if (me && !g_haveHome && !g_opened) {  // before Build, which can move the pawn
            g_home = uex::Loc(me); g_homeYaw = uex::GetRot(me).yaw; g_haveHome = true;
            g_landing = false;
        }
        if (me && g_opened) { FixPawn(me); arena::ClearLevel(); g_tidyAt = net::Now(); }
        if (me && arena::Build()) {  // no pawn yet (loading): try again next frame
            if (g_round >= 0) fight::End();
            double yaw; auto p = arena::SpawnPoint(r.you, &yaw);
            uex::SetMoveMode(me, 1);
            uex::Teleport(me, p, yaw); uex::SetControlYaw(yaw);
            fight::Begin(r.round); g_round = r.round; net::ArenaReady(r.round);
            uex::Log(L"round %d on map %d at %.0f %.0f %.0f", r.round, mi, p.x, p.y, p.z);
        }
    }
    if (inMatch && g_round >= 0) {
        uex::IgnoreInput(r.phase == "load" || r.phase == "count" || fight::LocalDead(), false);
        // the level's spawners may bring its cast back; a trigger may fade the camera out again
        if (g_opened && net::Now() - g_tidyAt > 3) {
            g_tidyAt = net::Now(); arena::ClearLevel();
            if (auto me = uex::Me(); me && r.phase != "fight") FixPawn(me);
        }
        arena::Tick(0); fight::Tick(0);
    } else if (r.phase == "over" && g_round >= 0) {
        fight::End(); g_round = -1; uex::IgnoreInput(false, false);  // stay in the arena until lobby
    } else if (r.phase == "lobby" && (g_round != -1 || g_haveHome || Stray())) {
        LeaveMatch(true);
    }
    if (r.phase != g_phase) { uex::Log(L"phase %S round %d", r.phase.c_str(), r.round); g_phase = r.phase; }
}
static void Caught(const wchar_t* what, int& n) {
    auto w = uex::ExcWhere();
    if (n++ < 5) uex::Log(L"%s exception %s", what, w.c_str());
}
static void Tick(void*, float dt) {
    uex::SehScope seh;  // access violations below get logged with their location
    try { net::SteamPump(); } catch (...) { static int n; Caught(L"steam", n); }
    try { Flow(); } catch (...) { static int n; Caught(L"flow", n); }
    try { ui::Tick(dt); } catch (...) { static int n; Caught(L"ui", n); }
}
extern "C" __declspec(dllexport) void PbzInit(const PbzHost* h) { g_h = h; uex::SetHost(h); }
extern "C" __declspec(dllexport) void PbzRun(const wchar_t*) { if (g_tick < 0) g_tick = g_h->addTick(Tick, nullptr); uex::Log(L"mp loaded"); }
extern "C" __declspec(dllexport) void PbzUnload() {
    if (g_tick >= 0) g_h->removeTick(g_tick);
    g_tick = -1;
    try { LeaveMatch(true); if (g_landing) { if (auto me = uex::Me()) uex::SetMoveMode(me, 1); g_landing = false; } } catch (...) {}
    try { fight::Shutdown(); } catch (...) {}
    try { ui::Shutdown(); } catch (...) {}
    try { if (g_veto) uex::SaveVeto(false); g_veto = false; } catch (...) {}
    net::Shutdown();
    // the save block must outlive a level we opened: unloaded anywhere but the title (the open
    // issued by LeaveMatch above has not happened yet), keep the hooks and pin this DLL in memory
    // so the loader's FreeLibrary leaves them valid. The veto is never exited (uex::SaveVeto).
    if (g_guarded && !AtTitle()) fileguard::Pin(); else fileguard::Off();
}

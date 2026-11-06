// fight: remote ghosts, hostility, hit/damage exchange, local state publishing, death reporting.
// Ghost = a PBZCharacter_Player of the local player's class, set up like the old dummy
// (spawntest/fixclone/addlock/clonecam): camera rig stripped, NB lock-on point, death events
// unbound, Health_Min = 1 so it never dies locally. It walks by itself (AI controller +
// AddMovementInput, mirror.cpp "var 5") so the game's motion matching animates it; during a
// montage it is placed exactly on the remote path with root motion off.
// Damage: the attacker's game computes damage on the ghost; we read the drop of Health+Shield,
// scale it by the room's dmg%, and send "hit <victim> <amount> <attacker> <frac> <round> <posture>
// <attacker yaw>" (SURE).
// The victim takes it from its own Shield, then Health; during a round the local Health_Min is
// 1, reaching it means "dead". Hits of another round or outside phase "fight" are dropped.
// Victim-side resolution: the victim checks its own state when the hit arrives. Dodge i-frames
// (CharacterState.Stand.Invulnerable.Dodge[.Perfect]) take nothing; holding a deflect
// (Ability.Action.HoldDeflect) takes posture only; otherwise Shield/Health drop and a hit
// reaction for the attacker's side (front/back) plays. Dodge/block answer the attacker with
// "res <attacker> <victim> dodge|pdodge|block <round>" (SURE) so its ghost drops the hit reaction
// its game gave it; on "block" the attacker plays AM_Hun_<weapon>_HD_AttackerFailed (recoil). "hit" carries the attacker's yaw as a 7th field (older senders: 6 fields).
// Death: Health_Min stays 1; the dead player plays AM_Hun_Dead_F/B (by the last hit's side) and
// holds its last pose; the montage reaches the ghosts like any other and is held there too.
// Weapons: "W <config path>" (SURE, again on change, QUICK every 2 s) -> ghost SetSlotWeapon.
// Phantom Edge (杀器): "E <config path>" (same schedule) -> ghost Weapon.Slot.Shachi; only Phantom Edge configs (IsEdgeConfig) are loaded.
// ext: [0]='S', [1..2] shield permille.
// Alert VFX: the attack warnings (NS_AttackAlert*, e.g. the blue light of a Phantom-Edge-charged
// heavy) come from the ability, not the montage, so a ghost never shows them. A new one attached
// under our mesh sends "V <asset>|<parent actor class>|<parent component>|<socket>|x y z|p y r"
// (SURE); the receiver spawns it on the ghost's matching component (else its mesh).
// Room rules (rules.h, read at Begin and every Tick): hp% scales our own Health_Max (base saved
// at the first Begin, put back by End / MatchOver); a ghost's damage base is our base x hp%, so a
// hit's frac is the share of the victim's scaled max. Combo protection (victim side, ApplyHit):
// hits less than 0.8 s apart are one chain; from hit 4 cdmg scales the damage (ComboScale), after
// hit "stun" no hit reaction / hit-stop plays. wban / wrand pick the weapon at Begin.
// More than 2 players: a ghost's drop is measured against the value we wrote last frame (the
// remote's own hp changes never count) and is reported only within kOwnHitWindow of a move of
// ours (attack, skill montage); dead's "by" is the last hitter.
// Kill mode: dead carries our life number; when the server re-alives us with a bigger life we
// respawn at a free spawn point with 1.5 s of spawn protection (hits answered "res .. dodge").
#include "fight.h"
#include "weapons.h"
#include "outfits.h"
#include "houserules.h"
#include "../arena/arena.h"
#include <algorithm>
#include <set>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "uex.h"
#include "../net/net.h"

using namespace ue;
using uex::Vec; using uex::Rot;

namespace fight {
// test hooks (declared by the test module, not in fight.h)
static bool g_acceptAllHits;   // loopback: our own echoed "hit 1 ..." is for us
static Vec g_testOff;          // ghost world offset (loopback: keep the echo beside us)
static std::string g_testWeapon, g_testGhostWeapon;  // loopback: own weapon id (no room) / ghost weapon id override
void TestAcceptAllHits(bool on) { g_acceptAllHits = on; }
void TestOffset(double x, double y) { g_testOff = {x, y, 0}; }
void TestWeapons(const char* mine, const char* ghost) { g_testWeapon = mine ? mine : ""; g_testGhostWeapon = ghost ? ghost : ""; }
static std::string g_testGhostEdge;  // ghost Phantom Edge id override (ui_cmd "ghostedge")
void TestGhostEdge(const char* id) { g_testGhostEdge = id ? id : ""; }

namespace {
constexpr int kAnimOff[7] = {0x82ee, 0x6a3e, 0x8111, 0x6b24, 0x82e9, 0x82f0, 0x4a2};
constexpr uint8_t kFlagGround = 1, kFlagDead = 16;

struct Ghost {
    int slot = -1;
    UObject* actor = nullptr;
    int index = -1;
    uex::Ref ref;             // the actor, checked: a freed ghost's address can come back as another pawn
    bool setup = false;
    double yaw = 1e9;         // smoothed yaw
    double lastT = 0;
    bool glide = false;
    double offSince = -1;
    UObject* applied = nullptr;   // montage last started on it
    uint16_t appliedId = 0;
    float shownHp = -1;       // Health value we last wrote
    float shownShield = 0;    // Shield value we last wrote
    float shownShachi = -1;   // Shachi (posture) value we last wrote
    int chainN = 0;           // our hits on it in the current chain (log only)
    double chainAt = -100;
    float lastRemoteHp = 1;
    bool rootMotionOff = false;
    double nextLog = 0;
    std::string weapon;       // config path applied
    int armed = -1;           // SetArmed applied
    std::string edge;         // Phantom Edge config path applied
    UObject* localMont = nullptr;  // own hit reaction let through
    double localHitAt = -100;
    double resAt = -100;      // the victim said dodge/block: no local hit reaction for a moment
    double weaponRetryAt = 0; // a weapon that did not take is tried again from then
    float baseMax = -1;       // its own Health_Max before the damage-% scaling
    double spawnedAt = -100;  // the game settles a new pawn's attributes (Health can drop to its Min): no hits then
    double fxAt = -100;       // start of the current alert VFX window
    int fxN = 0;              // alert VFX spawned in that window
    std::map<int, uex::Ref> fx;  // peer's fx id -> our copy (ended by "X <id>")
    bool wasDead = false;     // last sample had the dead flag (kill mode: cleared again = respawned)
    double revivedAt = -100;  // that respawn: teleported onto the remote, no hits counted for a moment
    double notOursLog = 0;    // rate limit of the "drop without a move of ours" log
    int pendN = 0;            // hits reported since pendAt and not yet seen in the remote's hp
    float pendFrac = 0;       // their sum (killing blows as 1)
    double pendAt = -100;
    float pendHp0 = 1;        // remote hp + shield when the first of them went out
};

bool g_on;
int g_round;
bool g_dead;
int g_lastHitter = -1;
double g_lastHitAt = -100;
double g_lastHitSent = -100;  // our last "hit" event: blood on the ghost after it is ours
double g_nextHp;
// player state before the first round of the match, put back by MatchOver
struct Saved { bool valid = false; float health, healthMin, shachi, shield, ult, spirit; } g_saved;
std::map<int, std::string> g_remoteWeapon;  // slot -> weapon config path ("W" event)
std::string g_sentWeapon; double g_nextWeaponSend;
std::map<int, std::string> g_remoteEdge;  // slot -> Phantom Edge config path ("E" event)
std::string g_sentEdge; double g_nextEdgeSend;
struct RemoteOutfit { std::string id; int skin = 0, hat = 1; };
std::map<int, RemoteOutfit> g_remoteOutfit;  // slot -> its look ("O" event)
std::string g_sentOutfit; double g_nextOutfitSend;
UObject* g_me;
std::map<int, Ghost> g_ghosts;
// object caches: freed objects' addresses come back as other objects (level change, rematch), so
// every kept pointer is checked against what it was (uex::Ref, names). ForgetObjects drops them.
struct MontId { uint32_t name, number; uint16_t id; };
std::unordered_map<UObject*, MontId> g_montIds;    // local montage -> interned id
struct RemoteM { uex::Ref m; double retryAt; };
std::map<std::pair<int, uint16_t>, RemoteM> g_remoteMont;
uex::Ref g_hitMont[2];  // victim's hit reaction montage: [0] hit from the front, [1] from behind
bool g_hitMontSearched;
uex::Ref g_deathMont;   // death montage playing (held on its last pose)
float g_deathLen; UObject* g_deathLenOf;
bool g_lastHitBack;     // last hit came from behind
struct HitM { uint32_t name, number; bool hit; };
std::unordered_map<UObject*, HitM> g_isHit;
std::map<std::wstring, uex::Ref> g_montByName, g_gaCls;
UObject* g_fightWorld;            // world the ghosts were spawned in
std::map<int, double> g_spawnAt;  // slot -> next ghost spawn try
int g_deadBy = -1; double g_deadSentAt, g_countCheck; bool g_resetOk;
rules::Rules g_rules;            // the room's rules (Begin, every Tick)
float g_hpDelta;                 // hp rule: added to our Health_Max (base and current); End takes it off
float g_hpBaseWrote;             // Health_Max base value as we left it (the game rewriting it drops our delta)
int g_maxHpFixes;                // Tick had to put our scaled Health_Max back (logged a few times)
int g_chainN; double g_chainAt = -100;  // combo protection: hits taken in this chain, last one's time
float g_chainRaw; int g_chainRawN;      // unscaled damage of this chain's ordinary hits (a killing blow's estimate)
constexpr double kChainGap = 0.8;       // hits closer than this are one chain (pvp-protocol.md)
double g_lastActAt = -100;       // our own last move (attack / skill montage)
constexpr double kOwnHitWindow = 5.0;  // a ghost's drop this long after a move of ours is ours (projectiles, burns)
int g_hitsSent, g_hitsOk, g_hitsUnconfirmed, g_dropsNotOurs;  // per round, logged at End
int g_deadLife = -1;             // kill mode: our life number when we died
double g_protectUntil;           // kill mode: spawn protection after a respawn
constexpr double kSpawnProtect = 1.5;

// engine fields, by name (fallback = the 2026-11 build)
constexpr const wchar_t* kChar = L"Class /Script/Engine.Character";
constexpr const wchar_t* kCmc = L"Class /Script/Engine.CharacterMovementComponent";
constexpr const wchar_t* kGa = L"Class /Script/GameplayAbilities.GameplayAbility";
uex::Prop kMesh{kChar, L"Mesh", 0x330}, kCharMove{kChar, L"CharacterMovement", 0x338},
    kRoot{L"Class /Script/Engine.Actor", L"RootComponent", 0x1b8},
    kRelLoc{L"Class /Script/Engine.SceneComponent", L"RelativeLocation", 0x148},
    kRelRot{L"Class /Script/Engine.SceneComponent", L"RelativeRotation", 0x160},
    kVelocity{L"Class /Script/Engine.MovementComponent", L"Velocity", 0xd8},
    kAccel{kCmc, L"Acceleration", 0x338},
    kNoCtrlPhysics{kCmc, L"bRunPhysicsWithNoController", 0x2f9, 0x8},
    kUro{L"Class /Script/Engine.SkinnedMeshComponent", L"bEnableUpdateRateOptimizations", 0x7d9, 0x8},
    kController{L"Class /Script/Engine.Pawn", L"Controller", 0x2e0},
    kGaActive{kGa, L"bIsActive", 0x388, 0xff}, kGaMontage{kGa, L"CurrentMontage", 0x320};

UObject* MeshOf(UObject* ch) { return ch ? kMesh.Of<UObject*>(ch) : nullptr; }
UObject* CtrlOf(UObject* pawn) { return kController.Of<UObject*>(pawn); }
UObject* AnimOf(UObject* ch) {
    auto mesh = MeshOf(ch);
    if (!mesh) return nullptr;
    Params p(uex::Fn(mesh, L"GetAnimInstance")); p.Call(mesh);
    return p.Get<UObject*>(L"ReturnValue");
}
UObject* CM(UObject* ch) { return ch ? kCharMove.Of<UObject*>(ch) : nullptr; }
using uex::Field;
uint8_t Mode(UObject* ch) { auto cm = CM(ch); auto m = cm ? Field<uint8_t>(cm, L"MovementMode") : nullptr; return m ? *m : 0; }
void SetMode(UObject* cm, uint8_t m) { Params p(uex::Fn(cm, L"SetMovementMode")); p.Set<uint8_t>(L"NewMovementMode", m); p.Call(cm); }
Vec& VelRef(UObject* cm) { return kVelocity.Of<Vec>(cm); }
Vec RootLoc(UObject* a) { return kRelLoc.Of<Vec>(kRoot.Of<UObject*>(a)); }
double RootYaw(UObject* a) { return (&kRelRot.Of<double>(kRoot.Of<UObject*>(a)))[1]; }
double Ang(double d) { return std::fmod(d + 540.0, 360.0) - 180.0; }

UObject* CurMontage(UObject* ai) { Params g(uex::Fn(ai, L"GetCurrentActiveMontage")); g.Call(ai); return g.Get<UObject*>(L"ReturnValue"); }
float MontPos(UObject* ai, UObject* m) { Params p(uex::Fn(ai, L"Montage_GetPosition")); p.Set(L"Montage", m); p.Call(ai); return p.Get<float>(L"ReturnValue"); }
float MontRate(UObject* ai, UObject* m) { Params p(uex::Fn(ai, L"Montage_GetPlayRate")); p.Set(L"Montage", m); p.Call(ai); return p.Get<float>(L"ReturnValue"); }
void MontSetRate(UObject* ai, UObject* m, float r) { Params p(uex::Fn(ai, L"Montage_SetPlayRate")); p.Set(L"MontageToPlay", m); p.Set<float>(L"NewPlayRate", r); p.Call(ai); }
void MontStop(UObject* ai, UObject* m, float blend) { Params p(uex::Fn(ai, L"Montage_Stop")); p.Set<float>(L"InBlendOutTime", blend); p.Set(L"Montage", m); p.Call(ai); }
void MontPlay(UObject* ai, UObject* m, float rate, float at) {
    Params p(uex::Fn(ai, L"Montage_Play"));
    p.Set(L"MontageToPlay", m); p.Set<float>(L"InPlayRate", rate); p.Set<float>(L"InTimeToStartMontageAt", at);
    p.Set<bool>(L"bStopAllMontages", true); p.Call(ai);
}

std::vector<UObject*> Children(UObject* actor) {
    std::vector<UObject*> r;
    for (int i = 0, n = NumObjects(); i < n; ++i) { auto o = ObjectAt(i); if (o && o->outer == actor && !(o->flags & 0x10)) r.push_back(o); }
    return r;
}

bool Live(UObject* a) { return uex::Alive(a) && kRoot.Of<void*>(a); }
bool GhostLive(const Ghost& g) { auto a = uex::Get(g.ref); return a && a == g.actor && kRoot.Of<void*>(a); }

// ---- ghost setup (the dummy recipe) ----------------------------------------------------------
void SetupGhost(Ghost& g) {
    UObject* a = g.actor;
    auto kids = Children(a);
    // camera rig + broken LockViewTarget: lock-on would view from the ghost's camera / world origin
    for (auto c : kids) {
        auto nm = NameOf(c);
        if (nm == L"NBCamera" || nm == L"CineCamera1" || nm == L"SpringArm" || nm == L"SpotLight_CharacterOnly" || nm == L"LockViewTarget")
            if (auto f = uex::Fn(c, L"K2_DestroyComponent")) { Params p(f); p.Set(L"Object", c); p.Call(c); }
    }
    // lock-on point like enemies have
    if (auto lockCls = uex::Class(L"Class /Script/NBCamera.NBLockOnTargetLocationComponent")) {
        struct alignas(16) TF { double v[12]; } tf{{0, 0, 0, 1, 0, 0, 60, 0, 1, 1, 1, 0}};
        auto charCls = uex::Class(L"Class /Script/ProjectPBZ.PBZCharacter");
        for (int i = 0, n = NumObjects(); i < n; ++i) {
            auto o = ObjectAt(i);
            if (o && o->cls == lockCls && !(o->flags & 0x10) && o->outer && o->outer != a && IsA(o->outer, charCls)) {
                auto l = &kRelLoc.Of<double>(o);
                tf.v[4] = l[0]; tf.v[5] = l[1]; tf.v[6] = l[2]; break;
            }
        }
        Params p(uex::Fn(a, L"AddComponentByClass"));
        p.Set(L"Class", lockCls); p.Set<bool>(L"bManualAttachment", false); p.Set(L"RelativeTransform", tf); p.Set<bool>(L"bDeferredFinish", false);
        p.Call(a);
        if (!p.Get<UObject*>(L"ReturnValue")) uex::Log(L"fight: lock point add failed");
    }
    // attributes from the local player (partial copies leave taken-multipliers at 0 = no damage)
    auto dst = uex::AttrSet(a), src = uex::AttrSet(g_me);
    if (dst && src) for (int off = uex::attr::Level, end = uex::attr::FinalWalkSpeed; off < end; off += 0x10) memcpy(dst + off + 8, src + off + 8, 8);
    uex::SetAttr(a, uex::attr::HealthMin, 1.f);
    uex::SetAttr(a, uex::attr::Health, uex::Attr(a, uex::attr::HealthMax));
    g.shownHp = uex::Attr(a, uex::attr::Health);
    // its death must stay local (BP_RenewalPlayer binds these to the respawn flow)
    if (int dco = uex::OffsetOf(a->cls, L"Death Component"); dco >= 0)
        if (auto dc = At<UObject*>(a, dco))
            for (const wchar_t* nm : {L"Death Event", L"Pending Death Event"})
                if (int o = FindProp(dc->cls, nm).offset; o >= 0) *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(dc) + o + 8) = 0;
    // movement: own AI controller, walks by input; gravity even if the controller is missing
    if (!CtrlOf(a)) { Params sc(uex::Fn(a, L"SpawnDefaultController")); sc.Call(a); }
    if (auto cm = CM(a)) {
        if (auto f = Field<float>(cm, L"MaxAcceleration")) *f = 12000.f;
        if (auto f = Field<float>(cm, L"AirControl")) *f = 1.f;
        kNoCtrlPhysics.SetBit(cm, true);
        Params t(uex::Fn(cm, L"SetComponentTickEnabled")); t.Set<bool>(L"bEnabled", true); t.Call(cm);
        SetMode(cm, 1);
    }
    // pose always ticks, no update-rate skipping (choppy otherwise)
    if (auto mesh = MeshOf(a)) {
        if (auto v = Field<uint8_t>(mesh, L"VisibilityBasedAnimTickOption")) *v = 0;
        kUro.SetBit(mesh, false);  // bEnableUpdateRateOptimizations
    }
    uex::Hostile(g_me, a, true);
    g.setup = true;
    uex::Log(L"fight: ghost slot %d ready %s ctrl %p", g.slot, NameOf(a).c_str(), CtrlOf(a));
}

void DestroyActor(UObject* a);
void SpawnGhost(int slot) {
    Ghost& g = g_ghosts[slot];
    g.slot = slot;
    if (GhostLive(g)) return;
    g = Ghost{}; g.slot = slot;
    net::State s;
    Vec at; double yaw = 0;
    if (net::Sample(slot, s)) { at = {s.pos[0] + g_testOff.x, s.pos[1] + g_testOff.y, s.pos[2]}; yaw = s.yaw; }
    else { at = uex::Loc(g_me); at.x += 300; yaw = RootYaw(g_me) + 180; }
    g.actor = uex::Spawn(g_me->cls, at, yaw);
    if (!g.actor) { uex::Log(L"fight: spawn failed slot %d", slot); return; }
    g.index = g.actor->index; g.ref = uex::Hold(g.actor); g.spawnedAt = net::Now();
    try { SetupGhost(g); }
    catch (...) {  // half set up (its death would run the game's respawn): drop it, Tick tries again
        auto w = uex::ExcWhere(); uex::Log(L"fight: ghost %d setup exception %s", slot, w.c_str());
        DestroyActor(g.actor); g = Ghost{}; g.slot = slot;
    }
}

UObject* RemoteMontage(int slot, uint16_t id) {
    if (!id) return nullptr;
    auto k = std::make_pair(slot, id);
    if (auto it = g_remoteMont.find(k); it != g_remoteMont.end()) {
        if (auto m = uex::Get(it->second.m)) return m;
        if (net::Now() < it->second.retryAt) return nullptr;  // did not load: not every frame
    }
    std::string path = net::Lookup(slot, id);
    if (path.empty()) return nullptr;  // definition not yet here: retry next frame
    std::wstring w = uex::Wide(path);
    UObject* m = FindObject(L"AnimMontage " + w);
    if (!m) m = uex::LoadAsset(w.c_str());
    g_remoteMont[k] = {uex::Hold(m), net::Now() + 2};
    return m;
}

bool IsHitMont(UObject* m) {
    if (!m) return false;
    if (auto it = g_isHit.find(m); it != g_isHit.end() && it->second.name == m->name.comparison && it->second.number == m->name.number) return it->second.hit;
    auto n = NameOf(m);
    bool hit = n.find(L"_Hit") != std::wstring::npos || n.find(L"Stagger") != std::wstring::npos ||
               n.find(L"Knock") != std::wstring::npos || n.find(L"Break") != std::wstring::npos;
    g_isHit[m] = {m->name.comparison, m->name.number, hit};
    return hit;
}

void SyncMontage(Ghost& g, UObject* ai, const net::State& s) {
    UObject* want = RemoteMontage(g.slot, s.montage);
    float base = s.montRate > 0.05f ? s.montRate : 1.f;
    UObject* cur = CurMontage(ai);
    // the ghost's own hit reaction (our hit, the game's normal path) plays out at once; the
    // victim's copy of it arrives a round trip later and must not restart it
    if (cur && cur != g.applied && cur != g.localMont && IsHitMont(cur) && net::Now() - g.resAt < 0.5) { MontStop(ai, cur, 0.1f); cur = nullptr; }
    if (cur && cur != g.applied && cur != g.localMont && IsHitMont(cur)) {
        g.localMont = cur; uex::Log(L"fight: ghost %d local hit reaction %s", g.slot, NameOf(cur).c_str());
    }
    if (g.localMont) {
        if (cur == g.localMont) g.localHitAt = net::Now();
        else g.localMont = nullptr;                                         // played out
    }
    // while it plays (and shortly after), the remote's own copy of the reaction or idle is not
    // applied; anything else from the remote (an attack, a dodge) takes over at once
    bool recent = g.localMont || net::Now() - g.localHitAt < 0.6;
    if (recent && (!want || IsHitMont(want))) { g.applied = want; return; }
    if (want != g.applied || (want && cur != want)) {
        if (want && want != g.applied) uex::Log(L"fight: ghost %d montage %s", g.slot, NameOf(want).c_str());
        if (want) MontPlay(ai, want, base, s.montPos);
        else if (g.applied) MontStop(ai, g.applied, 0.2f);
        g.applied = want;
    } else if (want && (s.flags & kFlagDead)) {  // dead: hold the remote's (paused) death pose
        if (std::fabs(s.montPos - MontPos(ai, want)) > 0.05f) { Params p(uex::Fn(ai, L"Montage_SetPosition")); p.Set(L"Montage", want); p.Set<float>(L"NewPosition", s.montPos); p.Call(ai); }
        MontSetRate(ai, want, 0.f);
    } else if (want) {
        float err = s.montPos - MontPos(ai, want);
        if (std::fabs(err) > 0.3f) { Params p(uex::Fn(ai, L"Montage_SetPosition")); p.Set(L"Montage", want); p.Set<float>(L"NewPosition", s.montPos); p.Call(ai); err = 0; }
        MontSetRate(ai, want, base * (1.f + (std::max)(-0.5f, (std::min)(0.5f, err * 4.f))));
    }
    // the ghost's own local hit reactions etc.: its body only follows the remote
    if (!want && cur && cur != g.applied) MontStop(ai, cur, 0.15f);
}

void Place(UObject* a, Vec p, double yaw, bool teleport) {
    Params t(uex::Fn(a, L"K2_SetActorLocationAndRotation"));
    t.Set(L"NewLocation", p); t.Set(L"NewRotation", Rot{0, yaw, 0}); t.Set<bool>(L"bSweep", false); t.Set<bool>(L"bTeleport", teleport); t.Call(a);
}

void DriveGhost(Ghost& g, const net::State& s) {
    UObject* a = g.actor; auto cm = CM(a); if (!cm) return;
    double now = net::Now(), dt = (std::min)(0.1, g.lastT ? now - g.lastT : 0.016); g.lastT = now;
    Vec dst{s.pos[0] + g_testOff.x, s.pos[1] + g_testOff.y, s.pos[2]};
    Vec vel{s.vel[0], s.vel[1], s.vel[2]};
    bool inMont = s.montage != 0;
    // yaw: the sender's actor yaw snaps on direction changes; turn at a capped rate (exact in moves)
    if (g.yaw > 1e8 || inMont) g.yaw = s.yaw;
    else { double d = Ang(s.yaw - g.yaw), step = (540.0 + std::fabs(d) * 6.0) * dt; g.yaw += std::fabs(d) <= step ? d : (d > 0 ? step : -step); }
    Vec c = RootLoc(a);
    double dx = dst.x - c.x, dy = dst.y - c.y, dz = dst.z - c.z, dist = std::hypot(dx, dy);
    auto ai = AnimOf(a);
    if (auto rm = ai ? Field<uint8_t>(ai, L"RootMotionMode") : nullptr) {
        // moves: own root motion off, the remote path already contains it
        uint8_t want = inMont ? 1 : 3;  // IgnoreRootMotion / RootMotionFromMontagesOnly
        if (*rm != want) *rm = want;
    }
    if (dist > 600 || std::fabs(dz) > 600) { Place(a, dst, g.yaw, true); VelRef(cm) = vel; return; }
    if (inMont) {
        if (g.glide) { g.glide = false; SetMode(cm, 1); }
        Place(a, dst, g.yaw, false); VelRef(cm) = vel; return;
    }
    if (dist > 50) { if (g.offSince < 0) g.offSince = now; } else g.offSince = -1;
    bool stuck = g.offSince >= 0 && now - g.offSince > 0.25;
    if (!g.glide && ((std::fabs(dz) > 60 && s.mode != 3) || dist > 150 || stuck)) { g.glide = true; SetMode(cm, 5); }
    if (g.glide) {
        if (dist < 25 && std::fabs(dz) < 20 && s.mode != 3) { g.glide = false; SetMode(cm, 1); }
        else { Place(a, dst, g.yaw, false); VelRef(cm) = vel; return; }  // flying: exactly on the path
    }
    // movement input along the remote velocity drives the game's own locomotion (legs); the
    // position itself is put on the sampled path below
    double sp = std::hypot(vel.x, vel.y);
    if (sp > 25) {
        if (auto f = Field<float>(cm, L"MaxWalkSpeed")) *f = float((std::min)(2200.0, sp));
        Params m(uex::Fn(a, L"AddMovementInput"));
        m.Set(L"WorldDirection", Vec{vel.x / sp, vel.y / sp, 0}); m.Set<float>(L"ScaleValue", 1.f); m.Set<bool>(L"bForce", false); m.Call(a);
    }
    if (s.mode == 3 && s.vel[2] > 200 && Mode(a) == 1) {
        Params l(uex::Fn(a, L"LaunchCharacter")); l.Set(L"LaunchVelocity", vel); l.Set<bool>(L"bXYOverride", true); l.Set<bool>(L"bZOverride", true); l.Call(a);
    }
    // the walk alone lags (acceleration, braking); pull the rest of the gap closed within ~0.1 s
    // Sample() is already a smooth interpolated path: put the capsule on it (keep our own z so
    // walking follows the local floor), velocity stays for the locomotion graph
    Place(a, {dst.x, dst.y, std::fabs(dz) < 40 ? c.z : dst.z}, g.yaw, false);
    VelRef(cm) = vel;
    if (now >= g.nextLog) { g.nextLog = now + 2; auto st = net::GetStats(g.slot); uex::Log(L"fight: ghost %d err %.0f v %.0f ctrl %p rx %.0f/s rtt %d delay %.0f jit %.0f starves %d", g.slot, dist, std::hypot(vel.x, vel.y), CtrlOf(a), st.rxPerSec, st.rttMs, st.delayMs, st.jitterMs, st.starves); }
}

int DmgPct() { net::Room r; return net::GetRoom(r) && r.dmg > 0 ? r.dmg : 100; }
int MySlot() { net::Room r; return net::GetRoom(r) ? r.you : 0; }
rules::Rules RoomRules() { net::Room r; return net::GetRoom(r) ? rules::Parse(r.rules) : rules::Rules{}; }
const net::Member* Mine(const net::Room& r) {
    for (auto& m : r.members) if (m.slot == r.you) return &m;
    return nullptr;
}

// hp rule: Health_Max x hp%. The game's aggregation adds modifiers (gear) to the base, so the
// difference goes onto the base as well as the current value and survives a recalculation; a
// base the game rewrote itself has lost it. keepFrac: Health keeps its share of the max.
void ApplyHpRule(bool keepFrac) {
    using namespace uex::attr;
    auto set = g_me ? uex::AttrSet(g_me) : nullptr; if (!set) return;
    int off = HealthMax;
    float* base = reinterpret_cast<float*>(set + off + 8);
    float* cur = reinterpret_cast<float*>(set + off + 0xC);
    if (*cur <= 0) return;
    if (g_hpDelta != 0 && std::fabs(*base - g_hpBaseWrote) > 0.5f) {
        uex::Log(L"fight: hp rule: the game set Health_Max base %.0f (we left %.0f), delta %.0f dropped", *base, g_hpBaseWrote, g_hpDelta);
        g_hpDelta = 0;
    }
    float raw = *cur - g_hpDelta;
    if (raw <= 0) { raw = *cur; g_hpDelta = 0; }
    float want = raw * float(g_rules.hp) / 100.f, nd = want - raw;
    if (std::fabs(*cur - want) < 0.5f && std::fabs(nd - g_hpDelta) < 0.5f) return;
    float old = *cur, h = uex::Attr(g_me, Health);
    *base += nd - g_hpDelta; *cur = want; g_hpDelta = nd; g_hpBaseWrote = *base;
    if (keepFrac && old > 0) uex::SetAttr(g_me, Health, (std::max)(1.f, h * want / old));
    if (g_maxHpFixes++ < 20) uex::Log(L"fight: hp rule %d%%: Health_Max %.0f -> %.0f (unscaled %.0f)", g_rules.hp, old, want, raw);
}
void RemoveHpRule() {
    using namespace uex::attr;
    auto set = g_me ? uex::AttrSet(g_me) : nullptr;
    if (!set || g_hpDelta == 0) { g_hpDelta = 0; return; }
    int off = HealthMax;
    float* base = reinterpret_cast<float*>(set + off + 8);
    float* cur = reinterpret_cast<float*>(set + off + 0xC);
    float old = *cur;
    if (std::fabs(*base - g_hpBaseWrote) <= 0.5f) *base -= g_hpDelta;  // else the game's own base already
    *cur -= g_hpDelta; g_hpDelta = 0;
    if (*cur < 1) *cur = 1;
    float h = uex::Attr(g_me, Health);
    if (old > 0) uex::SetAttr(g_me, Health, (std::max)(1.f, h * *cur / old));
    uex::Log(L"fight: hp rule removed: Health_Max %.0f -> %.0f", old, *cur);
}

float ExtShield(const net::State& s) {  // remote Shield as a fraction of its Health_Max
    return s.extLen >= 3 && s.ext[0] == 'S' ? (s.ext[1] | s.ext[2] << 8) / 1000.f : 0.f;
}

// kill mode: the remote came back (dead flag gone): off the held death pose, onto its spawn point
void GhostLife(Ghost& g, const net::State& s) {
    bool dead = s.flags & kFlagDead;
    if (g.wasDead && !dead) {
        UObject* a = g.actor;
        if (auto ai = AnimOf(a)) MontStop(ai, nullptr, 0.1f);  // null = every montage: the paused death one too
        g.applied = nullptr; g.appliedId = 0; g.localMont = nullptr; g.localHitAt = -100;
        g.shownHp = -1; g.shownShachi = -1; g.pendN = 0; g.pendFrac = 0; g.chainN = 0;
        g.yaw = s.yaw; g.glide = false; g.offSince = -1;
        Place(a, {s.pos[0] + g_testOff.x, s.pos[1] + g_testOff.y, s.pos[2]}, s.yaw, true);
        g.revivedAt = net::Now();
        uex::Log(L"fight: ghost %d respawned at %.0f %.0f %.0f", g.slot, s.pos[0], s.pos[1], s.pos[2]);
    }
    g.wasDead = dead;
}

// local hits on a ghost show as a drop of its Health + Shield (and Shachi); report it, then
// show the remote's values again. The remote's own hp changes (hits by others, heals) never show
// as a drop: the drop is taken against the value written last frame. What remains is anything
// else the local game does to the ghost; only a drop within kOwnHitWindow of a move of ours is
// reported. Every report is checked against the remote's hp afterwards (confirmed / unconfirmed).
void GhostHp(Ghost& g, const net::State& s) {
    using namespace uex::attr;
    UObject* a = g.actor;
    float mx = uex::Attr(a, HealthMax); if (mx <= 0) return;  // as scaled last frame
    // the ghost's damage base is our own (hp-rule scaled) Health_Max: a hit's frac is then the
    // same share of the victim's own scaled max
    float ours = g_me ? uex::Attr(g_me, HealthMax) : 0.f;
    if (ours > 0) g.baseMax = ours; else if (g.baseMax <= 0) g.baseMax = mx;
    float h = uex::Attr(a, Health), sh = uex::Attr(a, Shield);
    float smx = uex::Attr(a, ShachiMax), sc = uex::Attr(a, Shachi);
    double now = net::Now();
    float tot = (s.flags & kFlagDead) ? 0.f : s.hp + ExtShield(s);
    if (g.pendN) {  // our reports since pendAt: has the remote's hp gone down by (part of) them?
        float want = (std::min)(g.pendFrac, g.pendHp0) * 0.3f;  // combo falloff takes up to 60% off
        if (g.pendHp0 - tot >= want && g.pendHp0 - tot > 0.0005f) { g_hitsOk += g.pendN; g.pendN = 0; g.pendFrac = 0; }
        else if (now - g.pendAt > 2.0) {
            g_hitsUnconfirmed += g.pendN;
            uex::Log(L"fight: %d hit(s) on ghost %d (%.1f%%) not seen in its hp (%.3f -> %.3f)", g.pendN, g.slot, g.pendFrac * 100, g.pendHp0, tot);
            g.pendN = 0; g.pendFrac = 0;
        }
    }
    // a drop counts only in the fight (the victim takes no other) and not right after the spawn
    net::Room room; bool fightNow = !net::GetRoom(room) || room.phase == "fight";
    if (g.shownHp >= 0 && !(s.flags & kFlagDead) && fightNow && now - g.spawnedAt > 1.0 && now - g.revivedAt > 0.5) {
        float drop = (std::max)(0.f, g.shownHp - h) + (std::max)(0.f, g.shownShield - sh);
        if (drop < 0.5f) drop = 0;  // float noise: no move does less
        float pdrop = smx > 0 && g.shownShachi >= 0 ? (std::max)(0.f, g.shownShachi - sc) / smx : 0.f;
        if (pdrop <= 0.01f) pdrop = 0;
        if (drop > 0 || pdrop > 0) {
            if (now - g_lastActAt > kOwnHitWindow) {
                ++g_dropsNotOurs;
                if (now >= g.notOursLog) {
                    g.notOursLog = now + 1;
                    uex::Log(L"fight: ghost %d drop %.1f posture %.1f%% with no move of ours for %.1f s: not reported", g.slot, drop, pdrop * 100, now - g_lastActAt);
                }
            } else {
                float hs = houserules::OutgoingScale(g_me);  // sd%: Phantom Edge moves
                drop *= hs; pdrop *= hs;
                float frac = drop / mx;     // as a fraction: the victim's max may differ
                float amt = frac * g.baseMax;
                // the ghost's Health_Min = 1 truncates the drop of a killing blow: make it lethal
                if (drop > 0.01f && h <= 1.01f && sh <= 0.01f) frac = (std::max)(frac, 1.5f);
                char b[160]; snprintf(b, sizeof b, "hit %d %.2f %d %.5f %d %.4f %.0f", g.slot, amt, MySlot(), frac, g_round, pdrop, RootYaw(g_me));
                net::Sure(b); g_lastHitSent = now; ++g_hitsSent;
                if (frac > 0.0005f) {
                    if (!g.pendN) { g.pendHp0 = tot; g.pendFrac = 0; }
                    ++g.pendN; g.pendFrac += (std::min)(frac, 1.f); g.pendAt = now;
                }
                uex::Log(L"fight: hit ghost %d dmg %.1f (%.1f%%) posture %.1f%% round %d", g.slot, amt, frac * 100, pdrop * 100, g_round);
            }
        }
    }
    // damage %: the ghost's bar is scaled by 100/% instead of the hits, so a hit that its
    // Health_Min = 1 cut short is a killing blow at every setting
    float want = g.baseMax * 100.f / DmgPct();
    if (std::fabs(mx - want) > 0.5f) { uex::SetAttr(a, HealthMax, want); mx = want; }
    float show = (std::max)(1.f, s.hp * mx), shield = ExtShield(s) * mx, shachi = s.posture * smx;
    uex::SetAttr(a, Health, show); g.shownHp = show;
    uex::SetAttr(a, Shield, shield); g.shownShield = shield;
    if (smx > 0) { uex::SetAttr(a, Shachi, shachi); g.shownShachi = shachi; }
    uex::SetAttr(a, HealthMin, 1.f);
}

void HitReaction(bool back) {
    auto ai = AnimOf(g_me); if (!ai) return;
    if (!g_hitMontSearched || (g_hitMont[0].o && !uex::Get(g_hitMont[0]))) {  // unloaded since: search again
        g_hitMontSearched = true; g_hitMont[0] = g_hitMont[1] = {};
        auto mcls = uex::Class(L"Class /Script/Engine.AnimMontage");
        int best[2] = {-1, -1};
        for (int i = 0, n = NumObjects(); i < n; ++i) {
            auto o = ObjectAt(i); if (!o || o->cls != mcls) continue;
            auto nm = NameOf(o);
            if (nm.find(L"Hun") == std::wstring::npos || nm.find(L"Hit") == std::wstring::npos || nm.find(L"Down") != std::wstring::npos ||
                nm.find(L"Launch") != std::wstring::npos || nm.find(L"Knockback") != std::wstring::npos) continue;
            for (int d = 0; d < 2; ++d) {
                int dir = nm.find(d ? L"_B" : L"_F") != std::wstring::npos;
                int sc = 1 + dir * 4 + (nm.find(L"Light") != std::wstring::npos) * 2 + (nm.find(L"Double") != std::wstring::npos);
                if (sc > best[d]) { best[d] = sc; g_hitMont[d] = uex::Hold(o); }
            }
        }
        uex::Log(L"fight: hit montage F %s B %s", g_hitMont[0].o ? NameOf(g_hitMont[0].o).c_str() : L"none", g_hitMont[1].o ? NameOf(g_hitMont[1].o).c_str() : L"none");
    }
    UObject* m = uex::Get(g_hitMont[back]); if (!m) m = uex::Get(g_hitMont[0]);
    if (m && !CurMontage(ai)) MontPlay(ai, m, 1.f, 0.f);
}

// owned gameplay tag (ASC GameplayTagCountContainer.ExplicitTags), exact match
bool HasTag(UObject* ch, const wchar_t* tag) {
    return ch && uex::HasTag(ch, tag);
}
// attacker behind us? from its ghost's position, else from its yaw (facing our way = behind)
bool FromBehind(int from, double attackerYaw) {
    double my = RootYaw(g_me), rel;
    if (auto it = g_ghosts.find(from); it != g_ghosts.end() && GhostLive(it->second)) {
        Vec a = RootLoc(it->second.actor), b = RootLoc(g_me);
        rel = Ang(std::atan2(a.y - b.y, a.x - b.x) * 180 / 3.14159265358979 - my);
    } else if (attackerYaw > -1e8) rel = Ang(attackerYaw + 180 - my);
    else return false;
    return std::fabs(rel) > 100;
}

void HitStop();
// Combo protection: hits less than kChainGap apart are one chain (any attacker). cdmg: from hit 4
// the damage is scaled (rules::ComboScale); a killing blow (frac >= 1: the attacker's ghost only
// saw that it was lethal) then counts as the chain's average hit, scaled, so falloff can save us.
// stun: from hit "stun" on no hit reaction / hit-stop of ours, and ours still playing is cut, so
// the victim can dodge out. A posture break (CharacterState.Stand.Break) is the game's own.
void ApplyHit(float amt, float frac, int from, float posture, bool back) {
    using namespace uex::attr;
    if (g_dead || !g_me) return;
    double now = net::Now();
    g_chainN = now - g_chainAt < kChainGap ? g_chainN + 1 : 1; g_chainAt = now;
    if (g_chainN == 1) { g_chainRaw = 0; g_chainRawN = 0; }
    float mx = uex::Attr(g_me, HealthMax);
    float sh = uex::Attr(g_me, Shield), h = uex::Attr(g_me, Health);
    bool lethal = frac >= 1.f;
    float raw = frac > 0 ? frac * mx : amt, d = raw;
    float scale = rules::ComboScale(g_rules, g_chainN);
    if (scale < 1.f) {
        float base = raw;
        if (lethal) base = (std::max)(h + sh, g_chainRawN ? g_chainRaw / g_chainRawN : 0.f);  // at least what was left
        d = base * scale;
    }
    if (!lethal && raw > 0) { g_chainRaw += raw; ++g_chainRawN; }
    float fromShield = (std::min)(sh, d);
    if (fromShield > 0) uex::SetAttr(g_me, Shield, sh - fromShield);
    float nh = (std::max)(1.f, h - (d - fromShield));
    uex::SetAttr(g_me, Health, nh);
    if (posture > 0) {
        float smx = uex::Attr(g_me, ShachiMax), smin = uex::Attr(g_me, ShachiMin);
        uex::SetAttr(g_me, Shachi, (std::max)(smin, uex::Attr(g_me, Shachi) - posture * smx));
    }
    g_lastHitter = from; g_lastHitAt = now;
    bool noStun = g_rules.stun > 0 && g_chainN >= g_rules.stun;
    uex::Log(L"fight: took %.1f (raw %.1f, chain %d x%.2f%s) (shield %.1f) from %d hp %.1f -> %.1f / %.0f", d, raw, g_chainN, scale,
             noStun ? L", no stun" : L"", fromShield, from, h, nh, mx);
    g_lastHitBack = back;
    if (nh > 1.f && d > 0.01f) {  // posture only (deflected): no flinch
        if (!noStun) { HitReaction(back); HitStop(); }
        else if (auto ai = AnimOf(g_me)) {
            UObject* cur = CurMontage(ai);
            if (cur && (cur == uex::Get(g_hitMont[0]) || cur == uex::Get(g_hitMont[1]))) {
                MontStop(ai, cur, 0.1f);
                uex::Log(L"fight: combo protection: hit %d of the chain (stun %d), hit reaction %s cut", g_chainN, g_rules.stun, NameOf(cur).c_str());
            }
        }
    }
    static double breakLog;
    if (posture > 0 && now > breakLog && HasTag(g_me, L"CharacterState.Stand.Break")) {
        breakLog = now + 2;
        uex::Log(L"fight: posture break (the game's own stun), chain %d", g_chainN);
    }
}

// hit-stop on the victim: the game freezes only the attacker (CustomTimeDilation ~0.05 for ~70 ms);
// a networked hit gets the same short freeze on our own pawn. Left alone if the game set one.
double g_stopUntil;
void HitStop() {
    auto d = Field<float>(g_me, L"CustomTimeDilation");
    if (!d || std::fabs(*d - 1.f) > 1e-3f) return;
    *d = 0.05f; g_stopUntil = net::Now() + 0.07;
}
void HitStopTick() {
    if (!g_stopUntil || net::Now() < g_stopUntil) return;
    g_stopUntil = 0;
    if (auto d = g_me ? Field<float>(g_me, L"CustomTimeDilation") : nullptr; d && std::fabs(*d - 0.05f) < 1e-3f) *d = 1.f;
}

// victim side: what our own state makes of an incoming hit
void ResolveHit(float amt, float frac, int from, float posture, int round, double attackerYaw) {
    if (g_dead || !g_me) return;
    const char* res = nullptr;
    if (net::Now() < g_protectUntil) {  // kill mode spawn protection: the hit is ignored (answered like a dodge)
        char b[96]; snprintf(b, sizeof b, "res %d %d dodge %d", from, MySlot(), round);
        net::Sure(b);
        uex::Log(L"fight: hit from %d (%.1f) -> ignored (spawn protection, %.2f s left)", from, amt, g_protectUntil - net::Now());
        return;
    }
    if (HasTag(g_me, L"CharacterState.Stand.Invulnerable.Dodge.Perfect")) res = "pdodge";
    else if (HasTag(g_me, L"CharacterState.Stand.Invulnerable.Dodge")) res = "dodge";
    else if (HasTag(g_me, L"Ability.Action.HoldDeflect")) res = "block";
    if (res) {
        if (res[0] == 'b' && posture > 0) {  // guard: posture only
            using namespace uex::attr;
            float smx = uex::Attr(g_me, ShachiMax), smin = uex::Attr(g_me, ShachiMin);
            uex::SetAttr(g_me, Shachi, (std::max)(smin, uex::Attr(g_me, Shachi) - posture * smx));
        }
        char b[96]; snprintf(b, sizeof b, "res %d %d %s %d", from, MySlot(), res, round);
        net::Sure(b);
        uex::Log(L"fight: hit from %d (%.1f) -> %S", from, amt, res);
        return;
    }
    ApplyHit(amt, frac, from, posture, FromBehind(from, attackerYaw));
}

// loaded montage by object name (cached, misses retried: a weapon swap may load it later)
UObject* MontByName(const std::wstring& nm) {
    if (auto it = g_montByName.find(nm); it != g_montByName.end()) if (auto o = uex::Get(it->second)) return o;
    auto mcls = uex::Class(L"Class /Script/Engine.AnimMontage");
    for (int i = 0, n = NumObjects(); i < n; ++i) { auto o = ObjectAt(i); if (o && o->cls == mcls && !(o->flags & 0x10) && NameOf(o) == nm) { g_montByName[nm] = uex::Hold(o); return o; } }
    return nullptr;
}
int CancelMoves(UObject* ch);
// our attack was deflected: the game's "attacker failed" recoil for the current weapon
void Recoil() {
    if (g_dead || !g_me) return;
    UObject* m = MontByName(L"AM_Hun_" + AbilityKey(g_me) + L"_HD_AttackerFailed");
    if (!m) m = MontByName(L"AM_Hun_Attack_Failed");
    auto ai = AnimOf(g_me);
    if (!m || !ai) { uex::Log(L"fight: recoil montage missing (%s)", AbilityKey(g_me).c_str()); return; }
    CancelMoves(g_me);  // the attack ends here
    MontPlay(ai, m, 1.f, 0.f);
    uex::Log(L"fight: recoil %s", NameOf(m).c_str());
}

// attacker side: the victim dodged / blocked; its ghost drops the hit reaction our game gave it,
// a block also throws us back (recoil montage)
void OnResult(int victim, const char* kind) {
    uex::Log(L"fight: ghost %d result %S", victim, kind);
    if (!strcmp(kind, "block")) Recoil();
    auto it = g_ghosts.find(victim);
    if (it == g_ghosts.end() || !GhostLive(it->second)) return;
    Ghost& g = it->second;
    g.resAt = net::Now();
    if (g.pendN > 0) { --g.pendN; ++g_hitsOk; }  // resolved by the victim: it did get the hit
    if (g.localMont) { if (auto ai = AnimOf(g.actor)) MontStop(ai, g.localMont, 0.1f); g.localMont = nullptr; }
    g.localHitAt = -100; g.applied = nullptr;  // the remote's own montage (dash / guard) takes over
}

// a hit only counts in the round it was dealt in, during phase "fight" (loopback: no room)
bool HitCounts(int round) {
    net::Room r;
    if (!net::GetRoom(r)) return g_acceptAllHits && round == g_round;
    return r.phase == "fight" && round == g_round && r.round == g_round;
}

void OnWeapon(int slot, const std::string& path) {
    if (path.empty() || g_remoteWeapon[slot] == path) return;
    g_remoteWeapon[slot] = path;
    uex::Log(L"fight: slot %d weapon %S", slot, path.c_str());
}
void OnEdge(int slot, const std::string& path) {
    if (path.empty() || g_remoteEdge[slot] == path) return;
    if (!IsEdgeConfig(path)) {  // only Phantom Edge configs are loaded for a ghost
        static double at = -10;
        if (net::Now() - at > 5) { at = net::Now(); uex::Log(L"fight: slot %d sent a path that is no Phantom Edge, ignored", slot); }
        return;
    }
    g_remoteEdge[slot] = path;
    uex::Log(L"fight: slot %d Phantom Edge %S", slot, path.c_str());
}
void OnOutfit(int slot, const std::string& ev) {
    RemoteOutfit o;
    if (!outfits::Parse(ev, &o.id, &o.skin, &o.hat)) {
        static double at = -10;
        if (net::Now() - at > 5) { at = net::Now(); uex::Log(L"fight: slot %d sent a bad outfit event, ignored", slot); }
        return;
    }
    auto& cur = g_remoteOutfit[slot];
    if (cur.id == o.id && cur.skin == o.skin && cur.hat == o.hat) return;
    cur = o;
    uex::Log(L"fight: slot %d outfit %S skin %d hat %d", slot, o.id.c_str(), o.skin, o.hat);
}

// ---- alert VFX ---------------------------------------------------------------------------
struct CompArr { UObject** d; int n, m; };
// components attached under c (depth-limited), c excluded
void AttachTree(UObject* c, std::vector<UObject*>& out, int depth) {
    if (!c || depth > 4) return;
    auto a = Field<CompArr>(c, L"AttachChildren");
    if (!a || !a->d || a->n <= 0 || a->n > 256) return;
    for (int i = 0; i < a->n; ++i) {
        UObject* k = a->d[i];
        if (!k || !uex::Alive(k)) continue;
        out.push_back(k);
        AttachTree(k, out, depth + 1);
    }
}
// skill VFX on our mesh / weapons: anything from the VFX pack but footsteps and the blood on a body
// (the victim's: see PublishBlood). Pooled components come back: an activation counts.
bool IsPackAsset(const std::wstring& p) {
    return p.rfind(L"/PBZVFXPack/", 0) == 0 && p.size() < 200 && p.find(L'|') == std::wstring::npos &&
           p.find(L"FootStep") == std::wstring::npos;
}
bool IsBodyBlood(const std::wstring& p) { return p.find(L"/Blood") != std::wstring::npos && p.find(L"BloodTrail") == std::wstring::npos; }
bool IsSyncAsset(const std::wstring& p) { return IsPackAsset(p) && !IsBodyBlood(p); }
UObject* NiagaraAsset(UObject* c) {
    static UObject* cls = uex::Class(L"Class /Script/Niagara.NiagaraComponent");
    if (!cls || !c || !IsA(c, cls)) return nullptr;
    auto as = Field<UObject*>(c, L"Asset");
    return as ? *as : nullptr;
}
bool CompActive(UObject* c) {
    Params p(uex::Fn(c, L"IsActive")); p.Call(c); return p.Get<bool>(L"ReturnValue");
}
// blood our hits put on a ghost's body: the victim's own game never shows it. "Y <victim>|<V fields>"
std::set<UObject*> g_bloodSeen;
void PublishBlood() {
    std::set<UObject*> now;
    for (auto& [slot, g] : g_ghosts) {
        if (!GhostLive(g)) continue;
        std::vector<UObject*> tree; AttachTree(MeshOf(g.actor), tree, 0);
        for (UObject* c : tree) {
            UObject* sys = NiagaraAsset(c); if (!sys || !CompActive(c)) continue;
            std::wstring path = PathOf(sys);
            if (!IsPackAsset(path) || !IsBodyBlood(path)) continue;
            now.insert(c);
            if (g_bloodSeen.count(c) || net::Now() - g_lastHitSent > 1.0) continue;  // only right after a hit we sent
            std::wstring sock; if (auto sn = Field<FName>(c, L"AttachSocketName")) sock = NameOf(*sn);
            if (sock == L"None") sock.clear();
            Vec l = kRelLoc.Of<Vec>(c); Rot r = kRelRot.Of<Rot>(c);
            char nums[128]; snprintf(nums, sizeof nums, "|%.1f %.1f %.1f|%.1f %.1f %.1f", l.x, l.y, l.z, r.pitch, r.yaw, r.roll);
            net::Sure("Y " + std::to_string(slot) + "|" + uex::Utf8(path) + "|||" + uex::Utf8(sock) + nums);
        }
    }
    g_bloodSeen.swap(now);
}
std::map<UObject*, int> g_fxSeen;  // active synced components under our mesh -> id sent
bool g_fxPrimed;                    // the first look only records what is already there
int g_fxNext;
void PublishFx() {
    std::vector<UObject*> tree; AttachTree(MeshOf(g_me), tree, 0);
    std::map<UObject*, int> now;
    for (UObject* c : tree) {
        UObject* sys = NiagaraAsset(c); if (!sys || !CompActive(c)) continue;
        std::wstring path = PathOf(sys);
        if (!IsSyncAsset(path)) continue;
        if (auto it = g_fxSeen.find(c); it != g_fxSeen.end()) { now[c] = it->second; continue; }
        if (!g_fxPrimed) { now[c] = 0; continue; }
        if (++g_fxNext > 30000) g_fxNext = 1;
        int id = now[c] = g_fxNext;
        UObject* par = nullptr; if (auto ap = Field<UObject*>(c, L"AttachParent")) par = *ap;
        std::wstring parCls = par && par->outer && par->outer->cls ? NameOf(par->outer->cls) : L"";
        std::wstring parName = par ? NameOf(par) : L"";
        std::wstring sock; if (auto sn = Field<FName>(c, L"AttachSocketName")) sock = NameOf(*sn);
        if (sock == L"None") sock.clear();
        Vec l = kRelLoc.Of<Vec>(c); Rot r = kRelRot.Of<Rot>(c);
        char nums[128]; snprintf(nums, sizeof nums, "|%.1f %.1f %.1f|%.1f %.1f %.1f|%d", l.x, l.y, l.z, r.pitch, r.yaw, r.roll, id);
        net::Sure("V " + uex::Utf8(path) + "|" + uex::Utf8(parCls) + "|" + uex::Utf8(parName) + "|" + uex::Utf8(sock) + nums);
        uex::Log(L"fight: fx %s on %s.%s sent (%d)", NameOf(sys).c_str(), parCls.c_str(), parName.c_str(), id);
    }
    for (auto& [c, id] : g_fxSeen)  // gone or deactivated: the copy ends too (looping buffs)
        if (id && !now.count(c)) net::Sure("X " + std::to_string(id));
    g_fxSeen.swap(now); g_fxPrimed = true;
    PublishBlood();
}
UObject* SpawnFx(const std::wstring& path, UObject* at, const std::wstring& sock, Vec l, Rot r) {
    auto lib = uex::Lib(L"NiagaraFunctionLibrary /Script/Niagara.Default__NiagaraFunctionLibrary");
    auto sys = uex::LoadAsset(path.c_str());
    if (!lib || !sys) return nullptr;
    Params p(uex::Fn(lib, L"SpawnSystemAttached"));
    p.Set(L"SystemTemplate", sys); p.Set(L"AttachToComponent", at);
    p.Set(L"AttachPointName", sock.empty() ? FName{} : uex::Name(sock));
    p.Set(L"Location", l); p.Set(L"Rotation", r);
    p.Set<uint8_t>(L"LocationType", 0);  // EAttachLocation::KeepRelativeOffset
    p.Set<bool>(L"bAutoDestroy", true); p.Set<bool>(L"bAutoActivate", true);
    p.Set<uint8_t>(L"PoolingMethod", 0); p.Set<bool>(L"bPreCullCheck", false);
    p.Call(lib);
    return p.Get<UObject*>(L"ReturnValue");
}
// fields of a V / Y event: asset|parent class|parent comp|socket|x y z|p y r[|id]
bool ParseFx(const std::string& ev, std::vector<std::string>& f, std::wstring& path, Vec& l, Rot& r) {
    size_t b = 0;
    for (size_t i = 0; i <= ev.size(); ++i) if (i == ev.size() || ev[i] == '|') { f.push_back(ev.substr(b, i - b)); b = i + 1; }
    path = f.empty() ? L"" : uex::Wide(f[0]);
    return f.size() >= 6 && f.size() <= 7 && IsPackAsset(path) && sscanf(f[4].c_str(), "%lf %lf %lf", &l.x, &l.y, &l.z) == 3 &&
           sscanf(f[5].c_str(), "%lf %lf %lf", &r.pitch, &r.yaw, &r.roll) == 3 &&
           std::isfinite(l.x + l.y + l.z + r.pitch + r.yaw + r.roll) && std::fabs(l.x) + std::fabs(l.y) + std::fabs(l.z) <= 1e4;
}
double g_bloodAt = -100; int g_bloodN;
// "Y <victim>|..": blood a peer's hit put on its copy of us, shown on our own body
void OnBlood(int slot, const std::string& ev) {
    size_t bar = ev.find('|'); if (bar == std::string::npos) return;
    std::vector<std::string> f; std::wstring path; Vec l; Rot r;
    if (atoi(ev.c_str()) != MySlot() || !g_me || !ParseFx(ev.substr(bar + 1), f, path, l, r) || !IsBodyBlood(path)) return;
    double now = net::Now();
    if (now - g_bloodAt >= 0.5) { g_bloodAt = now; g_bloodN = 0; }
    if (++g_bloodN > 6) return;
    UObject* mesh = MeshOf(g_me); if (!mesh) return;
    try { SpawnFx(path, mesh, uex::Wide(f[3]), l, r); } catch (...) { uex::Log(L"fight: blood fx crashed %s", path.c_str()); }
}
void DeactivateFx(UObject* c) { Params p(uex::Fn(c, L"Deactivate")); p.Call(c); }
void OnFx(int slot, const std::string& ev) {
    std::vector<std::string> f; std::wstring path; Vec l; Rot r;
    if (!ParseFx(ev, f, path, l, r) || !IsSyncAsset(path)) {
        static double at = -10;
        if (net::Now() - at > 5) { at = net::Now(); uex::Log(L"fight: slot %d sent a bad fx event, ignored", slot); }
        return;
    }
    int id = f.size() == 7 ? atoi(f[6].c_str()) : 0;
    auto it = g_ghosts.find(slot);
    if (it == g_ghosts.end() || !GhostLive(it->second)) return;
    Ghost& g = it->second;
    double now = net::Now();
    // a peer cannot flood the ghost with systems: 8 per 0.5 s (dual weapons send one per blade)
    if (now - g.fxAt >= 0.5) { g.fxAt = now; g.fxN = 0; }
    if (++g.fxN > 8) return;
    UObject* mesh = MeshOf(g.actor); if (!mesh) return;
    std::wstring parCls = uex::Wide(f[1]), parName = uex::Wide(f[2]), sock = uex::Wide(f[3]);
    UObject* at = mesh;
    std::vector<UObject*> tree; AttachTree(mesh, tree, 0);
    for (UObject* c : tree)
        if (NameOf(c) == parName && c->outer && c->outer->cls && NameOf(c->outer->cls) == parCls) { at = c; break; }
    if (at == mesh && !parName.empty() && parCls != NameOf(g.actor->cls)) { sock.clear(); l = {}; r = {}; }  // no such part: at the mesh root
    try {
        UObject* made = SpawnFx(path, at, sock, l, r);
        if (made && id) {
            if (g.fx.size() > 64) g.fx.clear();
            if (auto old = uex::Get(g.fx[id])) DeactivateFx(old);
            g.fx[id] = uex::Hold(made);
        }
        uex::Log(L"fight: slot %d fx %s on %s -> %s", slot, path.c_str(), NameOf(at).c_str(), made ? L"ok" : L"none");
    } catch (...) { uex::Log(L"fight: fx crashed %s", path.c_str()); }
}
void OnFxEnd(int slot, const std::string& ev) {
    auto it = g_ghosts.find(slot);
    if (it == g_ghosts.end()) return;
    auto f = it->second.fx.find(atoi(ev.c_str()));
    if (f == it->second.fx.end()) return;
    if (UObject* c = uex::Get(f->second)) DeactivateFx(c);
    it->second.fx.erase(f);
}

void Events() {
    net::Event e;
    while (net::PollEvent(e)) {
        int victim = -1, from = -1, round = -1; float amt = 0, frac = 0, posture = 0, ayaw = -1e9f; char kind[16] = "";
        if (sscanf(e.text.c_str(), "hit %d %f %d %f %d %f %f", &victim, &amt, &from, &frac, &round, &posture, &ayaw) >= 5) {
            if (!HitCounts(round)) { uex::Log(L"fight: dropped stale hit (round %d, ours %d)", round, g_round); continue; }
            // a peer's numbers are not trusted: finite and in range (a killing blow sends frac 1.5)
            if (!std::isfinite(amt) || !std::isfinite(frac) || !std::isfinite(posture) || amt < 0 || amt > 1e5f || frac < 0 || frac > 2.f) {
                uex::Log(L"fight: dropped bad hit from %d", e.slot); continue;
            }
            posture = (std::min)(1.f, (std::max)(0.f, posture));
            if (!g_acceptAllHits) from = e.slot;  // the attacker is whoever sent it
            if (victim == MySlot() || g_acceptAllHits) ResolveHit(amt, frac, from >= 0 ? from : e.slot, posture, round, ayaw);
        } else if (sscanf(e.text.c_str(), "res %d %d %15s %d", &from, &victim, kind, &round) == 4) {
            // loopback: our own echoed result is about the ghost (the sender's slot)
            if (round == g_round && (from == MySlot() || g_acceptAllHits)) OnResult(g_acceptAllHits ? e.slot : victim, kind);
        } else if (e.text.size() > 2 && e.text[0] == 'W' && e.text[1] == ' ') {
            OnWeapon(e.slot, e.text.substr(2));
        } else if (e.text.size() > 2 && e.text[0] == 'E' && e.text[1] == ' ') {
            OnEdge(e.slot, e.text.substr(2));
        } else if (e.text.size() > 2 && e.text[0] == 'O' && e.text[1] == ' ') {
            OnOutfit(e.slot, e.text);
        } else if (e.text.size() > 2 && e.text[0] == 'V' && e.text[1] == ' ') {
            OnFx(e.slot, e.text.substr(2));
        } else if (e.text.size() > 2 && e.text[0] == 'X' && e.text[1] == ' ') {
            OnFxEnd(e.slot, e.text.substr(2));
        } else if (e.text.size() > 2 && e.text[0] == 'Y' && e.text[1] == ' ') {
            OnBlood(e.slot, e.text.substr(2));
        }
    }
}
void DrainEvents() {
    net::Event e; int n = 0;
    while (net::PollEvent(e)) {
        if (e.text.rfind("W ", 0) == 0) OnWeapon(e.slot, e.text.substr(2));
        else if (e.text.rfind("E ", 0) == 0) OnEdge(e.slot, e.text.substr(2));
        else if (e.text.rfind("O ", 0) == 0) OnOutfit(e.slot, e.text);
        else if (e.text.rfind("V ", 0) == 0) {}  // a past effect
        else ++n;
    }
    if (n) uex::Log(L"fight: dropped %d queued events", n);
}

void Publish() {
    net::State s{};
    Vec p = RootLoc(g_me); s.pos[0] = float(p.x); s.pos[1] = float(p.y); s.pos[2] = float(p.z);
    auto cm = CM(g_me);
    if (cm) {
        Vec v = VelRef(cm); s.vel[0] = float(v.x); s.vel[1] = float(v.y); s.vel[2] = float(v.z);
        Vec acc = kAccel.Of<Vec>(cm);
        float ma = 1; if (auto f = Field<float>(cm, L"MaxAcceleration")) ma = (std::max)(1.f, *f);
        s.acc[0] = float((std::max)(-1.0, (std::min)(1.0, acc.x / ma)));
        s.acc[1] = float((std::max)(-1.0, (std::min)(1.0, acc.y / ma)));
    }
    s.yaw = float(RootYaw(g_me));
    s.mode = Mode(g_me);
    if (s.mode != 3) s.flags |= kFlagGround;
    if (g_dead) s.flags |= kFlagDead;
    float hm = uex::Attr(g_me, uex::attr::HealthMax), sm = uex::Attr(g_me, uex::attr::ShachiMax);
    s.hp = g_dead ? 0.f : hm > 0 ? uex::Attr(g_me, uex::attr::Health) / hm : 1.f;
    s.posture = sm > 0 ? uex::Attr(g_me, uex::attr::Shachi) / sm : 1.f;
    if (auto ai = AnimOf(g_me)) {
        if (UObject* m = CurMontage(ai)) {
            auto it = g_montIds.find(m);
            uint16_t id;
            if (it != g_montIds.end() && it->second.name == m->name.comparison && it->second.number == m->name.number) id = it->second.id;
            else { id = net::Intern(uex::Utf8(PathOf(m))); g_montIds[m] = {m->name.comparison, m->name.number, id}; }
            s.montage = id; s.montPos = MontPos(ai, m); s.montRate = MontRate(ai, m);
            if (!g_dead && !IsHitMont(m)) g_lastActAt = net::Now();  // a move of ours: drops on ghosts now are ours
        }
        for (int k = 0; k < 7; ++k) s.anim[k] = reinterpret_cast<uint8_t*>(ai)[kAnimOff[k]];
    }
    { Params a(uex::Fn(g_me, L"IsArmed")); if (a.Size()) { a.Call(g_me); s.armed = a.Get<bool>(L"ReturnValue"); } }
    if (hm > 0) {
        int sp = (std::min)(65535, int(1000 * uex::Attr(g_me, uex::attr::Shield) / hm + 0.5f));
        s.extLen = 3; s.ext[0] = 'S'; s.ext[1] = uint8_t(sp); s.ext[2] = uint8_t(sp >> 8);
    }
    net::SetLocalState(s);
    PublishFx();
    // weapon: SURE on change, QUICK every 2 s (a peer that joined late or a dropped STATE)
    double now = net::Now();
    if (now >= g_nextWeaponSend) {
        std::string w = CurrentWeaponPath(g_me);
        if (!w.empty()) {
            if (w != g_sentWeapon) { net::Sure("W " + w); g_sentWeapon = w; }
            else net::Quick("W " + w);
        }
        g_nextWeaponSend = now + (w == g_sentWeapon ? 2.0 : 0.25);
    }
    // Phantom Edge: same schedule
    if (now >= g_nextEdgeSend) {
        std::string w = CurrentEdgePath(g_me);
        if (!w.empty()) {
            if (w != g_sentEdge) { net::Sure("E " + w); g_sentEdge = w; }
            else net::Quick("E " + w);
        }
        g_nextEdgeSend = now + (w == g_sentEdge ? 2.0 : 0.25);
    }
    // outfit (the pick worn, else the own look): same schedule
    if (now >= g_nextOutfitSend) {
        std::string o = outfits::Event();
        if (!o.empty()) {
            if (o != g_sentOutfit) { net::Sure(o); g_sentOutfit = o; }
            else net::Quick(o);
        }
        g_nextOutfitSend = now + (o == g_sentOutfit ? 2.0 : 0.25);
    }
}

// ghost weapon: the remote's "W" path, else (loopback / not heard yet) the room member's id
void GhostWeapon(Ghost& g, const net::State& s) {
    std::string path;
    if (!g_testGhostWeapon.empty()) path = WeaponConfigPath(g_testGhostWeapon);
    else if (auto it = g_remoteWeapon.find(g.slot); it != g_remoteWeapon.end()) path = it->second;
    else {
        net::Room r;
        if (net::GetRoom(r)) for (auto& m : r.members) if (m.slot == g.slot && !m.weapon.empty()) path = WeaponConfigPath(m.weapon);
    }
    if (path != g.weapon && !path.empty()) {
        double now = net::Now();
        if (now < g.weaponRetryAt) return;
        g.weaponRetryAt = now + 1.0;  // not loaded / not taken yet: again in a second
        if (SetGhostWeapon(g.actor, path, s.armed != 0)) { g.weapon = path; g.armed = s.armed; }
    } else if (int(s.armed) != g.armed && !g.weapon.empty()) {
        SetGhostWeapon(g.actor, "", s.armed != 0); g.armed = s.armed;
    }
}
// ghost Phantom Edge: the remote's "E" path only (the room pick may not be equipped: no slot in
// that save); test: "ghostedge <id>"
void GhostEdge(Ghost& g) {
    std::string path;
    if (!g_testGhostEdge.empty()) path = EdgeConfigPath(g_testGhostEdge);
    else if (auto it = g_remoteEdge.find(g.slot); it != g_remoteEdge.end()) path = it->second;
    if (path.empty() || path == g.edge) return;
    g.edge = path; SetGhostEdge(g.actor, path);
}
// ghost outfit: the remote's "O" (its pick, else its own look); none heard yet: what the game gave it
void GhostOutfit(Ghost& g) {
    auto it = g_remoteOutfit.find(g.slot);
    if (it != g_remoteOutfit.end()) outfits::WearGhost(g.actor, it->second.id, it->second.skin, it->second.hat);
}


// abilities that are running a move (montage): attacks, dodges, hit reactions. Passives stay.
int CancelMoves(UObject* ch) {
    auto ga = uex::Class(L"Class /Script/GameplayAbilities.GameplayAbility");
    int n = 0;
    for (int i = 0, c = NumObjects(); i < c; ++i) {
        auto o = ObjectAt(i);
        if (!o || o->outer != ch || (o->flags & 0x10) || !IsA(o, ga)) continue;
        if (!kGaActive.Bit(o) || !kGaMontage.Of<UObject*>(o)) continue;
        if (auto f = uex::Fn(o, L"K2_CancelAbility")) { Params p(f); p.Call(o); ++n; }
    }
    if (auto ai = AnimOf(ch)) MontStop(ai, nullptr, 0.1f);  // null = every active montage
    return n;
}
// buffs/debuffs with a duration (potions, status, combat buffs); infinite ones are equipment / passives
int ClearTimedEffects(UObject* ch) {
    auto asc = uex::ASC(ch); if (!asc) return 0;
    auto lib = uex::Lib(L"AbilitySystemBlueprintLibrary /Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary");
    auto fGet = uex::Fn(asc, L"GetActiveEffects"), fRem = uex::Fn(asc, L"RemoveActiveGameplayEffect");
    auto fDur = lib ? uex::Fn(lib, L"GetActiveGameplayEffectTotalDuration") : nullptr;
    if (!fGet || !fRem || !fDur) return -1;
    Params q(fGet); q.Call(asc);  // zeroed query = all; the returned array is the game's (one small leak per round)
    struct H { int32_t h; uint8_t pass; uint8_t pad[3]; };
    struct A { H* d; int n, m; } arr = q.Get<A>(L"ReturnValue");
    std::vector<H> hs(arr.d, arr.d + (arr.d ? arr.n : 0));
    int n = 0;
    for (auto& h : hs) {
        Params d(fDur); d.Set(L"ActiveHandle", h); d.Call(lib);
        if (d.Get<float>(L"ReturnValue") <= 0) continue;  // -1 infinite, 0 instant
        Params r(fRem); r.Set(L"Handle", h); r.Set<int>(L"StacksToRemove", -1); r.Call(asc); ++n;
    }
    return n;
}

void SaveOriginal() {
    using namespace uex::attr;
    if (g_saved.valid || !g_me) return;
    g_saved = {true, uex::Attr(g_me, Health), uex::Attr(g_me, HealthMin), uex::Attr(g_me, Shachi),
               uex::Attr(g_me, Shield), uex::Attr(g_me, Ult), uex::Attr(g_me, uex::attr::Spirit)};
    uex::Log(L"fight: saved hp %.0f min %.0f shachi %.0f shield %.0f", g_saved.health, g_saved.healthMin, g_saved.shachi, g_saved.shield);
}

// round start: full hp and posture, no shield/ult, full spirit, no status build-up, no buffs,
// nothing running
void ResetLocal() {
    if (!g_me) return;
    using namespace uex::attr;
    SaveOriginal();
    int moves = CancelMoves(g_me), fx = ClearTimedEffects(g_me);
    g_rules = RoomRules();
    ApplyHpRule(false);
    uex::SetAttr(g_me, HealthMin, 1.f);
    uex::SetAttr(g_me, Health, uex::Attr(g_me, HealthMax));
    uex::SetAttr(g_me, Shachi, uex::Attr(g_me, ShachiMax));
    uex::SetAttr(g_me, Shield, 0.f);
    uex::SetAttr(g_me, Ult, 0.f);
    uex::SetAttr(g_me, uex::attr::Spirit, uex::Attr(g_me, uex::attr::SpiritMax));
    for (int off = uex::attr::AccFirst, last = uex::attr::AccLast; off <= last; off += 0x10) uex::SetAttr(g_me, off, 0.f);
    g_resetOk = uex::Attr(g_me, HealthMax) > 0;
    g_chainN = 0; g_chainAt = -100;
    uex::Log(L"fight: reset hp %.0f shachi %.0f, cancelled %d, effects removed %d", uex::Attr(g_me, Health), uex::Attr(g_me, Shachi), moves, fx);
}

// own weapon for the round: wrand picks one from (room code, round, own slot) so every client
// gets the same: FNV-1a 32 of "<code>:<round>:<slot>" modulo the count of allowed weapons
// (Weapons()[1..] without the wban bits, in list order). Without wrand a banned pick falls back
// to "" (the save's current weapon, which wban cannot ban).
std::string PickWeapon(const net::Room& r, std::string id) {
    auto& ws = Weapons();
    auto banned = [&](size_t i) { return i >= 1 && i <= 30 && (g_rules.wban >> (i - 1) & 1u); };
    if (g_rules.wrand) {
        std::vector<size_t> ok;
        for (size_t i = 1; i < ws.size(); ++i) if (!banned(i)) ok.push_back(i);
        if (ok.empty()) { uex::Log(L"fight: wrand: every weapon banned, keeping the current one"); return ""; }
        std::string key = r.code + ":" + std::to_string(g_round) + ":" + std::to_string(r.you);
        uint32_t h = 2166136261u;
        for (unsigned char c : key) { h ^= c; h *= 16777619u; }
        const std::string& w = ws[ok[h % ok.size()]].id;
        uex::Log(L"fight: wrand %S -> %S (%zu allowed)", key.c_str(), w.c_str(), ok.size());
        return w;
    }
    for (size_t i = 1; i < ws.size(); ++i)
        if (ws[i].id == id && banned(i)) { uex::Log(L"fight: weapon %S banned (wban %u), keeping the current one", id.c_str(), g_rules.wban); return ""; }
    return id;
}

// kill mode: the server re-alived us with a bigger life: full reset at a spawn point away from the
// others, spawn protection, input back (mp follows LocalDead)
void Respawn(const net::Room& r, int life) {
    using namespace uex::attr;
    double now = net::Now();
    g_dead = false; g_deathMont = {}; g_deathLenOf = nullptr; g_lastHitter = -1; g_lastHitAt = -100; g_deadBy = -1;
    ResetLocal();  // cancels the death montage with the other moves
    // the free-est of the 8 ring points: farthest from the nearest live ghost, ties at random
    int best = r.you & 7; double bestD = -1;
    int start = int(GetTickCount64() % 8);
    for (int k = 0; k < 8; ++k) {
        int sl = (start + k) % 8;
        Vec p = arena::SpawnPoint(sl, nullptr);
        double d = 1e18;
        for (auto& [slot, g] : g_ghosts)
            if (GhostLive(g)) { Vec q = RootLoc(g.actor); d = (std::min)(d, std::hypot(q.x - p.x, q.y - p.y)); }
        if (d > bestD + 1) { bestD = d; best = sl; }
    }
    double yaw = 0; Vec p = arena::SpawnPoint(best, &yaw);
    uex::SetMoveMode(g_me, 1);
    uex::Teleport(g_me, p, yaw); uex::SetControlYaw(yaw);
    g_protectUntil = now + kSpawnProtect;
    g_deadLife = -1;
    uex::IgnoreInput(false, false);
    uex::Log(L"fight: respawn life %d at point %d (%.0f %.0f %.0f), nearest ghost %.0f, protected %.1f s", life, best, p.x, p.y, p.z,
             bestD > 1e17 ? -1.0 : bestD, kSpawnProtect);
}

// Local helpers (proposed for uex): Destroy that hides + drops collision first, ability by class name.
void DestroyActor(UObject* a) {
    if (!Live(a)) return;
    { Params h(uex::Fn(a, L"SetActorHiddenInGame")); h.Set<bool>(L"bNewHidden", true); h.Call(a); }
    { Params c(uex::Fn(a, L"SetActorEnableCollision")); c.Set<bool>(L"bNewActorEnableCollision", false); c.Call(a); }
    uex::Destroy(a);
}
// GA class taken from a live ability instance (the BP class object isn't findable by name).
bool ActivateAbility(UObject* ch, const wchar_t* gaClassName) {
    UObject* cls = nullptr;
    if (auto it = g_gaCls.find(gaClassName); it != g_gaCls.end()) cls = uex::Get(it->second);
    if (!cls) {
        for (int i = 0, n = NumObjects(); i < n && !cls; ++i) { auto o = ObjectAt(i);
            if (o && o->cls && !(o->flags & 0x10) && NameOf(o->cls) == gaClassName) cls = o->cls; }
        if (cls) g_gaCls[gaClassName] = uex::Hold(cls);  // not cached when missing: a weapon swap may grant it later
    }
    return cls && uex::Ability(ch, cls);
}
void PlayMontageAt(UObject* ch, UObject* m, float pos, float rate) {
    auto mesh = MeshOf(ch); if (!mesh || !m) return;
    Params g(uex::Fn(mesh, L"GetAnimInstance")); g.Call(mesh);
    if (auto ai = g.Get<UObject*>(L"ReturnValue")) MontPlay(ai, m, rate, pos);
}

// death: the game's death montage, front or back by the last hit, paused before its blend-out
// (that would stand the body up again)
constexpr float kDeathHold = 0.4f;  // s before the end
void PlayDeath() {
    const wchar_t* n = g_lastHitBack ? L"/PBZCombatAsset/ActorData/Players/Hun/Montage/Hit/AM_Hun_Dead_B.AM_Hun_Dead_B"
                                     : L"/PBZCombatAsset/ActorData/Players/Hun/Montage/Hit/AM_Hun_Dead_F.AM_Hun_Dead_F";
    UObject* m = FindObject(std::wstring(L"AnimMontage ") + n);
    if (!m) m = uex::LoadAsset(n);
    CancelMoves(g_me);
    auto ai = AnimOf(g_me);
    if (m && ai) MontPlay(ai, m, 1.f, 0.f);
    g_deathMont = uex::Hold(m); g_deathLenOf = nullptr;
    uex::Log(L"fight: death montage %s", m ? NameOf(m).c_str() : L"none");
}
void HoldDeath() {
    auto ai = AnimOf(g_me); auto dm = uex::Get(g_deathMont); if (!ai || !dm) return;
    if (g_deathLenOf != dm) {
        g_deathLenOf = dm; g_deathLen = 1.5f;
        if (auto f = uex::Fn(dm, L"GetPlayLength")) { Params l(f); l.Call(dm); g_deathLen = l.Get<float>(L"ReturnValue"); }
        uex::Log(L"fight: death montage length %.2f", g_deathLen);
    }
    float hold = (std::max)(0.f, g_deathLen - kDeathHold);
    if (CurMontage(ai) != dm) { MontPlay(ai, dm, 1.f, hold); MontSetRate(ai, dm, 0.f); return; }  // something else started
    if (MontPos(ai, dm) >= hold && MontRate(ai, dm) != 0.f) MontSetRate(ai, dm, 0.f);
}

void RemoveGhost(Ghost& g) {
    // only the very actor we spawned, in the world we spawned it in, never the player's own
    if (!GhostLive(g) || uex::World() != g_fightWorld || g.actor == uex::Me()) return;
    if (g_me && uex::Me() == g_me) uex::Hostile(g_me, g.actor, false);
    if (auto c = CtrlOf(g.actor); c && c != uex::PC()) uex::Destroy(c);
    DestroyActor(g.actor);
    uex::Log(L"fight: ghost %d destroy -> alive %d", g.slot, int(uex::Get(g.ref) != nullptr));
}
void RemoveGhosts() {
    for (auto& [slot, g] : g_ghosts) RemoveGhost(g);
    g_ghosts.clear();
}
void ForgetObjects() {
    g_montIds.clear(); g_remoteMont.clear(); g_hitMont[0] = g_hitMont[1] = {}; g_hitMontSearched = false;
    g_isHit.clear(); g_montByName.clear(); g_gaCls.clear(); g_deathLenOf = nullptr;
    g_fxSeen.clear(); g_fxPrimed = false;
}
}  // namespace

// "GA_Hun_*_A_C" etc.: "*" becomes the current weapon's ability key (e2e drives either weapon)
bool TestAbility(const wchar_t* n) {
    if (!g_me) return false;
    std::wstring s(n);
    if (auto p = s.find(L'*'); p != std::wstring::npos) s.replace(p, 1, AbilityKey(g_me));
    return ActivateAbility(g_me, s.c_str());
}

bool GhostAnchor(int slot, float out[3]) {
    auto it = g_ghosts.find(slot);
    if (it == g_ghosts.end() || !GhostLive(it->second)) return false;
    uex::Vec l = uex::Loc(it->second.actor);
    out[0] = float(l.x); out[1] = float(l.y); out[2] = float(l.z) + 120.f;  // capsule centre + about a head
    return true;
}

void Begin(int round) {
    if (g_me && g_me != uex::Me()) g_hpDelta = 0;  // that pawn (and our delta on it) is gone
    g_me = uex::Me();
    g_round = round; g_dead = false; g_lastHitter = -1; g_nextHp = 0; g_deathMont = {}; g_lastHitBack = false;
    g_deadBy = -1; g_deadSentAt = 0; g_deadLife = -1; g_protectUntil = 0; g_lastActAt = -100; g_maxHpFixes = 0;
    g_hitsSent = g_hitsOk = g_hitsUnconfirmed = g_dropsNotOurs = 0;
    g_rules = RoomRules();
    DrainEvents();  // hits still queued from the last round must not land in this one
    if (!g_me) { uex::Log(L"fight: Begin without player"); return; }
    SaveOriginal();
    {   // own weapon / Phantom Edge from the room (own slot); loopback test: TestWeapons
        std::string id = g_testWeapon, edge;
        net::Room r;
        if (net::GetRoom(r)) {
            for (auto& m : r.members) if (m.slot == r.you) { id = m.weapon; edge = m.edge; }
            id = PickWeapon(r, id);
            uex::Log(L"fight: rules %S (mode %S)", rules::Format(g_rules).c_str(), r.mode.c_str());
        }
        Equip(id);
        EquipEdge(edge);
        g_sentWeapon.clear(); g_nextWeaponSend = 0;
        g_sentEdge.clear(); g_nextEdgeSend = 0;
    }
    if (uex::World() != g_fightWorld) outfits::Forget();  // its kept components went with their world
    outfits::Wear(g_me, uex::Wide(outfits::Pick()));     // the lobby pick (no-op when already worn)
    g_sentOutfit.clear(); g_nextOutfitSend = 0;
    ResetLocal();
    houserules::Begin(g_rules, g_me);
    if (uex::World() != g_fightWorld) { RemoveGhosts(); g_fightWorld = uex::World(); }  // the old ones went with their world
    for (auto& [slot, g] : g_ghosts) { g.shownHp = -1; g.shownShachi = -1; g.applied = nullptr; g.wasDead = false; g.pendN = 0; g.pendFrac = 0; }
    g_spawnAt.clear();
    for (int slot : net::RemoteSlots())
        try { SpawnGhost(slot); } catch (...) { auto w = uex::ExcWhere(); uex::Log(L"fight: ghost %d spawn exception %s", slot, w.c_str()); }
    g_on = true;
    uex::Log(L"fight: begin round %d ghosts %zu", round, g_ghosts.size());
}

void Tick(float dt) {
    if (!g_on) return;
    UObject* me = uex::Me();
    if (!me) return;
    if (me != g_me) {  // new pawn (respawned by the level): ours again, hostile to the ghosts
        g_me = me; g_stopUntil = 0; g_hpDelta = 0; ResetLocal();
        outfits::Wear(g_me, uex::Wide(outfits::Pick()));
        for (auto& [slot, g] : g_ghosts) if (GhostLive(g)) uex::Hostile(g_me, g.actor, true);
    }
    // a pawn whose attributes were not there yet at Begin (HealthMax 0) gets its reset once they are
    if (!g_resetOk && !g_dead && uex::Attr(g_me, uex::attr::HealthMax) > 0) ResetLocal();
    g_rules = RoomRules();
    double now = net::Now();
    static double hpCheck;
    if (!g_dead && now >= hpCheck) { hpCheck = now + 0.5; ApplyHpRule(true); }  // the game may have recalculated Health_Max
    houserules::Tick(g_rules, g_me, dt);
    Events();
    HitStopTick();
    Publish();
    outfits::Tick();
    net::Room r; bool room = net::GetRoom(r);
    const net::Member* mine = room ? Mine(r) : nullptr;
    // death: Health_Min keeps us at 1, the game's own death/respawn never runs. Only in the fight
    // (the server takes it only then); sent again while the room still has us alive
    if (!g_dead && (!room || r.phase == "fight") && uex::Attr(g_me, uex::attr::HealthMax) > 0 &&
        uex::Attr(g_me, uex::attr::Health) <= 1.01f) {
        g_dead = true;
        g_deadBy = now - g_lastHitAt < 8 ? g_lastHitter : -1;
        g_deadLife = mine ? mine->life : -1;
        net::Dead(g_round, g_deadBy, g_deadLife); g_deadSentAt = now;
        uex::IgnoreInput(true, false);
        uex::Log(L"fight: dead round %d by %d life %d (hits sent %d ok %d unconfirmed %d, drops not ours %d)", g_round, g_deadBy, g_deadLife,
                 g_hitsSent, g_hitsOk, g_hitsUnconfirmed, g_dropsNotOurs);
        PlayDeath();
    }
    // the room has us alive again: kill mode with a bigger life = respawn; otherwise our dead was lost
    if (g_dead && mine && mine->alive && r.phase == "fight" && r.round == g_round) {
        if (r.mode == "kill" && mine->life > g_deadLife) Respawn(r, mine->life);
        else if (now - g_deadSentAt > 1.0) { net::Dead(g_round, g_deadBy, g_deadLife); g_deadSentAt = now; uex::Log(L"fight: dead sent again"); }
    }
    if (g_protectUntil && now >= g_protectUntil) { g_protectUntil = 0; uex::Log(L"fight: spawn protection over"); }
    if (g_dead) HoldDeath();
    // countdown: movement is ignored but attacks are not, one started now is cancelled (an equip /
    // weapon swap montage is left alone: Begin's Equip may still be playing it)
    if (!g_dead && room && (r.phase == "load" || r.phase == "count") && now >= g_countCheck) {
        g_countCheck = now + 0.1;
        if (auto ai = AnimOf(g_me)) if (auto m = CurMontage(ai)) {
            auto n = NameOf(m);
            bool keep = n.find(L"Equip") != std::wstring::npos || n.find(L"Switch") != std::wstring::npos ||
                        n.find(L"Draw") != std::wstring::npos || n.find(L"Weapon") != std::wstring::npos ||
                        n.find(L"Swap") != std::wstring::npos || n.find(L"Sheath") != std::wstring::npos;
            static UObject* last;
            if (m != last) { last = m; uex::Log(L"fight: countdown montage %s%s", n.c_str(), keep ? L" (kept)" : L" -> cancelled"); }
            if (!keep) CancelMoves(g_me);
        }
    }
    if (now >= g_nextHp) {
        g_nextHp = now + 0.25;
        float hm = uex::Attr(g_me, uex::attr::HealthMax);
        net::Hp(g_round, g_dead ? 0 : hm > 0 ? int(1000 * uex::Attr(g_me, uex::attr::Health) / hm) : 1000);
    }
    auto slots = net::RemoteSlots();
    for (int slot : slots)
        if ((!g_ghosts.count(slot) || !GhostLive(g_ghosts[slot])) && now >= g_spawnAt[slot]) { g_spawnAt[slot] = now + 0.5; SpawnGhost(slot); }
    for (auto it = g_ghosts.begin(); it != g_ghosts.end();) {  // peer gone (left / timed out)
        if (std::find(slots.begin(), slots.end(), it->first) != slots.end()) { ++it; continue; }
        uex::Log(L"fight: slot %d gone, removing its ghost", it->first);
        RemoveGhost(it->second); it = g_ghosts.erase(it);
    }
    for (auto& [slot, g] : g_ghosts) {
        if (!GhostLive(g)) continue;
        net::State s;
        if (!net::Sample(slot, s)) continue;
        try {
            GhostWeapon(g, s);
            GhostEdge(g);
            GhostOutfit(g);
            GhostLife(g, s);
            DriveGhost(g, s);
            if (auto ai = AnimOf(g.actor)) SyncMontage(g, ai, s);
            GhostHp(g, s);
        } catch (...) { static int n; auto w = uex::ExcWhere(); if (n++ < 5) uex::Log(L"fight: ghost %d exception %s", slot, w.c_str()); }
    }
}

void End() {
    if (!g_on && g_ghosts.empty()) return;
    g_on = false;
    uex::Log(L"fight: round %d hits sent %d confirmed %d unconfirmed %d, drops not ours %d", g_round, g_hitsSent, g_hitsOk, g_hitsUnconfirmed, g_dropsNotOurs);
    houserules::End(g_me && uex::Me() == g_me ? g_me : nullptr);
    RemoveGhosts();
    if (g_me && uex::Me() == g_me) {
        RemoveHpRule();
        CancelMoves(g_me);
        if (uex::Attr(g_me, uex::attr::Health) < 1.f) uex::SetAttr(g_me, uex::attr::Health, 1.f);
    }
    if (g_dead) uex::IgnoreInput(false, false);
    g_dead = false; g_deathMont = {}; g_lastHitBack = false; g_protectUntil = 0; g_hpDelta = 0;
    g_remoteMont.clear();  // a peer that reconnected interns its montages again
    uex::Log(L"fight: end");
}

void MatchOver() {
    End();
    UObject* me = uex::Me();
    if (g_saved.valid && me && uex::Alive(me)) {
        using namespace uex::attr;
        uex::SetAttr(me, HealthMin, g_saved.healthMin);
        // hp 1 at the first Begin is a leftover of our own Health_Min clamp (mp reloaded mid-match)
        if (g_saved.health <= 1.01f) g_saved.health = uex::Attr(me, HealthMax);
        uex::SetAttr(me, Health, (std::max)(1.f, (std::min)(g_saved.health, uex::Attr(me, HealthMax))));  // the hp rule is off again
        uex::SetAttr(me, Shachi, g_saved.shachi);
        uex::SetAttr(me, Shield, g_saved.shield);
        uex::SetAttr(me, Ult, g_saved.ult);
        uex::SetAttr(me, uex::attr::Spirit, g_saved.spirit);
        uex::Log(L"fight: match over, restored hp %.0f min %.0f", g_saved.health, g_saved.healthMin);
    }
    g_saved = {};
    if (me) RestoreWeapons();
    outfits::Restore();  // the own look back, the wardrobe's save suppression ended (no save)
    g_remoteWeapon.clear(); g_sentWeapon.clear(); g_remoteEdge.clear(); g_sentEdge.clear();
    g_remoteOutfit.clear(); g_sentOutfit.clear();
    ForgetObjects();
}

bool LocalDead() { return g_dead; }
void Shutdown() { MatchOver(); }
}  // namespace fight

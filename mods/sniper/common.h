// common.h: declarations shared by the sniper mod's files. Everything runs on the game thread (the
// loader's tick) except overlay.cpp's window thread and XAudio2's own thread.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "uex.h"

using ue::UObject;
struct TA { void* d; int32_t n, m; };  // TArray view
template <class T> inline TA Arr(std::vector<T>& v) { return {v.data(), int32_t(v.size()), int32_t(v.size())}; }
struct FLinColor { float r, g, b, a; };

extern const PbzHost* g_h;
extern std::wstring g_modDir;  // mod folder, ends with '\'
extern bool g_dbg;             // extra logging (test command "dbg")
extern double g_now;           // seconds, mod clock (advances while equipped)

// ---- vectors (UE: X forward, Y right, Z up; cm, degrees) ----
namespace ik {
struct V { double x, y, z; };
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V operator*(V a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(V a) { return sqrt(Dot(a, a)); }
inline V Norm(V a) { double l = Len(a); return l > 1e-9 ? a * (1 / l) : V{0, 0, 0}; }
inline V FromU(uex::Vec v) { return {v.x, v.y, v.z}; }
inline uex::Vec ToU(V v) { return {v.x, v.y, v.z}; }
void Axes(uex::Rot r, V& f, V& rt, V& up);  // rotator -> forward / right / up
inline V Local(uex::Rot r, V org, V p) { V f, rt, up; Axes(r, f, rt, up); return org + f * p.x + rt * p.y + up * p.z; }
uex::Rot RotOf(V dir);                      // direction -> pitch / yaw
}  // namespace ik

// ---- weapons (wpn.cpp): CS values from fpsgame shared/weapons_data.js ----
struct SndAt { float t; const wchar_t* snd; };  // seconds after the event; t <= 0 ends a list
struct Stats { float maxSpeed, spread, stand, crouch, move, jump, fire, rMag, rMagVar; };
struct Wpn {
    const wchar_t *id, *label;              // file prefix in sniper_cache, HUD name
    float damage, head, rangeMod;           // CS damage (x kDmgScale), head multiplier, falloff per 500 units
    int clip, reserve; bool autoFire;
    float cycle, reload, deploy;            // seconds
    Stats st[2];                            // [0] normal, [1] zoomed
    float rec[4], trans[2];                 // recovery time: stand, stand late, crouch, crouch late; late over shots trans
    float rAngle, rAngleVar; uint32_t seed; // random recoil pattern (weapons without a spray table)
    const float (*spray)[3]; int nSpray;    // spray table: shot, bullet offset up / right (deg)
    float pattern[2][64][2];                // WpnInit: per-shot punch kick up / right (pattern weapons)
    int zoomLevels; float zoomFov[2];
    const wchar_t* shot[4]; int nShot; float shotVol;
    const wchar_t* draw;
    SndAt reloadSnd[5], boltSnd[3];
    ik::V grip, muzzle;                     // model space, cm
    int tracerFreq; const wchar_t* muzzleFx; double fxScale;
};
constexpr int kWpnN = 3;
extern Wpn g_wpn[kWpnN];
constexpr float kDmgScale = 5.5f;  // CS hp 100 ~ a PBZ thug's 552
void WpnInit();

struct Acc { double penalty = 0, index = 0, punch[2] = {0, 0}, lastShot = -9; };  // punch: up, right (deg)
struct MoveSt { double speed; bool grounded, crouch; };                         // speed: horizontal units/s
void AccRecover(Acc& a, const Wpn& w, double dt, bool crouch, double now);
double AccInacc(const Wpn& w, int alt, const Acc& a, const MoveSt& m);
void AccFire(Acc& a, const Wpn& w, int alt, double now, double before[2]);
double Rand();                                           // 0..1
ik::V ShotDir(uex::Rot r, double inacc, double spread);  // r already includes the punch

// ---- audio (audio.cpp): sniper_cache/snd/<name>.wav through an XAudio2 voice pool ----
namespace audio {
bool Init();                                   // XAudio2 + every wav in sniper_cache/snd (first call only)
void Listener(ik::V loc, ik::V right);         // camera, every tick
void Play(const wchar_t* name, float vol = 1);
void PlayAt(const wchar_t* name, ik::V at, float vol = 1);  // distance falloff and pan from the camera
void SetMaster(float v);
void Shutdown();
}

// ---- engine helpers, model and hold pose (model.cpp) ----
bool Live(UObject* o);
UObject* MeshOf(UObject* ch);
UObject* AnimOf(UObject* ch);
UObject* OwnerOf(UObject* comp);
UObject* KLib();
UObject* CamMgr();
bool CamView(ik::V* loc, uex::Rot* rot);
uex::Rot CtrlRot();
void SetCtrlRot(uex::Rot r);  // also the baseline for the zoom sensitivity scaling
extern uex::Rot g_ctrlLast;
ik::V Sock(UObject* mesh, const wchar_t* bone);
void SetFov(float fov);       // 0 restores
uint8_t* BoolAt(UObject* o, const wchar_t* name, uint8_t* mask);  // bool (bitfield) property: byte, *mask
extern UObject* g_modelOwner;
extern ik::V g_hgR, g_hgL;
extern uex::Rot g_wpnOff;
extern double g_pitchFollow;
extern double g_poseAt, g_faceOff, g_yawOff;  // hold pose tuning (tests): frozen frame, extra body yaw, stance
extern bool g_noFace, g_yawOffOk;
void PoseRefreeze();                           // freeze again and re-measure the stance
void PoseUse(UObject* me, UObject* seq, double rate, double at, const wchar_t* slot);  // test: another hold animation
bool ModelShow(UObject* me, int wpn);  // spawn the model actor if needed, show weapon wpn's mesh only
void ModelDrop();
void ModelHide(bool hidden);
UObject* ModelActor();                 // the actor carrying the weapon meshes
void ModelDebug();                     // test: log the weapon mesh state
void Hold(UObject* me, float dt, const Wpn& w);  // pose + lay the rifle in the hands
void HoldOff(UObject* me);
bool MuzzlePos(const Wpn& w, ik::V* at, ik::V* dir);
void HideWeapons(UObject* me, bool hide);         // the game's own weapons on the player

// ---- shooting, damage and effects (shoot.cpp) ----
void CharsRefresh(UObject* me);  // live characters near the player, every ~0.5 s
UObject* NearestEnemy(UObject* me);
struct ShotRes { int kind; int dmg; UObject* victim; ik::V end; };  // kind: -1 miss, 0 body, 1 head, 2 kill, 3 headshot kill
ShotRes Shoot(UObject* me, const Wpn& w, ik::V eye, ik::V dir, ik::V muzzle, bool tracer);
void Fx(const wchar_t* path, ik::V at, uex::Rot rot = {}, double scale = 1);
void Tracer(ik::V a, ik::V b, bool strong);
void FxTick(double dt);  // tracers in flight, every tick
void FxClear();
void ImpactFx(ik::V at, ik::V normal, ik::V dir);
void FxList(UObject* me, bool killTracers);  // test
void MuzzleFx(const Wpn& w, ik::V at, ik::V dir);
void PlayHitReact(UObject* victim, ik::V dir);
void KillTest(UObject* victim, int mode, ik::V dir);  // test: one death path (0 the one shots use, 1 Kill, 2 attribute system, 3 GE_Kill, 4-7 with the death montage)
void DeathTick();                                     // holds the death poses we play
void Aggro(UObject* victim, UObject* me, float dmg, ik::V at);
bool HeadPos(UObject* victim, ik::V* out);  // head bone
extern double g_fxK[4];                     // effect scale: impact, spark, blood, tracer
extern double g_headR, g_bodyMargin;        // hit test: head sphere radius, capsule margin (cm)

// ---- input (input.cpp) ----
bool Front();
bool Down(int vk);
bool Pressed(int vk);  // edge since the last call for this key
void BlockKeys(bool on);

// ---- tests (tests.cpp): <mod dir>sniper.cmd, one command per line, read every tick ----
struct TestState { int skip = 0; bool interact = false, fire = false, hold = false, aimHead = false, ads = false; int burst = 0, aimSteps = 0; UObject* aimAt = nullptr; double aimOff = 30; double walkT = 0, walkF = 0, walkR = 0; };
extern TestState g_test;
void TestCmds(UObject* me);
void AimStep();  // turns the camera onto g_test.aimAt, a step per tick
void WalkStep(UObject* me, double dt);  // test: movement input relative to the camera

// ---- main loop (sniper.cpp) ----
int CurWpn();
bool MainCmd(UObject* me, const std::wstring& c);  // test commands about the weapon state; true if handled

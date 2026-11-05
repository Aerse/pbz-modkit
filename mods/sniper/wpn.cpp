// wpn.cpp: the weapon table (CS2 values from fpsgame shared/weapons_data.js) and the CS accuracy model
// ported from fpsgame shared/weapons.js: inaccuracy = stand / crouch + movement + air + a fire penalty
// that decays to 10% in the recovery time; aim punch from a spray table (AK, M4) or a seeded random
// walk (AWP); bullet = view + 2 x punch, then a random offset inside the inaccuracy + spread cone.
#include "common.h"
#include <algorithm>

constexpr double kRecoilScale = 2, kDuckMod = .34, kDeg = 0.0174532925;
static double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// spray tables (fpsgame SPRAY_KEYS): shot, bullet offset up / right in degrees, linear in between
static const float kSprayAk[][3] = {{0, 0, 0}, {3, 1.6f, .05f}, {6, 4.6f, .25f}, {9, 7.0f, .1f}, {12, 7.7f, -1.4f},
    {15, 8.0f, -2.6f}, {18, 8.1f, -.6f}, {22, 8.0f, 2.0f}, {26, 8.2f, .4f}, {29, 8.3f, -1.1f}};
static const float kSprayM4[][3] = {{0, 0, 0}, {3, 1.3f, 0}, {6, 3.6f, -.1f}, {10, 5.8f, .1f}, {13, 6.3f, 1.2f},
    {16, 6.5f, 2.1f}, {20, 6.6f, .2f}, {24, 6.6f, -1.5f}, {29, 6.8f, .4f}};

Wpn g_wpn[kWpnN];
#define MUZ L"/PBZVFXPack/VFX/BattleFX/GuiChai/"
static void Fill() {
    Wpn& a = g_wpn[0];  // AWP: Stats {maxSpeed, spread, stand, crouch, move, jump, fire, rMag, rMagVar}
    a.id = L"awp"; a.label = L"AWP"; a.damage = 115; a.head = 4; a.rangeMod = .99f; a.posture = .25f;
    a.clip = 5; a.reserve = 30; a.autoFire = false; a.cycle = .6f; a.reload = 3.666667f; a.deploy = 1.266667f;
    a.st[0] = {200, .0002f, .0808f, .0606f, .17648f, .13383f, .05385f, 78, 15};
    a.st[1] = {100, .0002f, .002f, .0015f, .17648f, .13383f, .05385f, 25, 2};
    a.rec[0] = a.rec[1] = .34539f; a.rec[2] = a.rec[3] = .24671f; a.trans[0] = 2; a.trans[1] = 5;
    a.rAngle = 0; a.rAngleVar = 20; a.seed = 4100; a.spray = nullptr; a.nSpray = 0;
    a.zoomLevels = 2; a.zoomFov[0] = 40; a.zoomFov[1] = 15;
    a.shot[0] = L"awp_01"; a.shot[1] = L"awp_02"; a.nShot = 2; a.shotVol = 1; a.draw = L"awp_draw";
    a.reloadSnd[0] = {.9f, L"awp_clipout"}; a.reloadSnd[1] = {1.9f, L"awp_clipin"}; a.reloadSnd[2] = {2.4f, L"awp_boltback"};
    a.reloadSnd[3] = {2.75f, L"awp_boltforward"}; a.reloadSnd[4] = {0, nullptr};
    a.boltSnd[0] = {.12f, L"awp_boltback"}; a.boltSnd[1] = {.38f, L"awp_boltforward"}; a.boltSnd[2] = {0, nullptr};
    a.grip = {4, -1, 13}; a.muzzle = {108, -1, 23.5}; a.tracerFreq = 1; a.muzzleFx = MUZ L"NS_GuiChaiBlack_Rifle_004_MuzzleStrong.NS_GuiChaiBlack_Rifle_004_MuzzleStrong"; a.fxScale = 1;

    Wpn& k = g_wpn[1];  // AK-47
    k.id = L"ak47"; k.label = L"AK-47"; k.damage = 36; k.head = 4; k.rangeMod = .98f; k.posture = .05f;
    k.clip = 30; k.reserve = 90; k.autoFire = true; k.cycle = .1f; k.reload = 2.466667f; k.deploy = 1;
    k.st[0] = k.st[1] = {215, .0006f, .00641f, .00481f, .17506f, .14076f, .0078f, 30, 0};
    k.rec[0] = .368f; k.rec[1] = .506f; k.rec[2] = .305257f; k.rec[3] = .419728f; k.trans[0] = 2; k.trans[1] = 5;
    k.rAngle = 0; k.rAngleVar = 70; k.seed = 223; k.spray = kSprayAk; k.nSpray = int(std::size(kSprayAk));
    k.zoomLevels = 0; k.zoomFov[0] = k.zoomFov[1] = 0;
    k.shot[0] = L"ak47_01"; k.shot[1] = L"ak47_02"; k.shot[2] = L"ak47_04"; k.nShot = 3; k.shotVol = .8f; k.draw = L"ak47_draw";
    k.reloadSnd[0] = {.45f, L"ak47_clipout_01"}; k.reloadSnd[1] = {1.35f, L"m4a1_clipin"}; k.reloadSnd[2] = {2.0f, L"ak47_boltpull"};
    k.reloadSnd[3] = {0, nullptr}; k.boltSnd[0] = {0, nullptr};
    k.grip = {2, -1, -5}; k.muzzle = {66, -1, 6.5}; k.tracerFreq = 3; k.muzzleFx = MUZ L"NS_GuiChaiBlack_Rifle_002_Muzzle.NS_GuiChaiBlack_Rifle_002_Muzzle"; k.fxScale = .8;

    Wpn& m = g_wpn[2];  // M4A4
    m.id = L"m4a4"; m.label = L"M4A4"; m.damage = 33; m.head = 4; m.rangeMod = .97f; m.posture = .045f;
    m.clip = 30; m.reserve = 120; m.autoFire = true; m.cycle = .09f; m.reload = 3.066667f; m.deploy = 1.133333f;
    m.st[0] = m.st[1] = {225, .0006f, .0049f, .0041f, .13788f, .09727f, .007f, 23, 0};
    m.rec[0] = .338941f; m.rec[1] = .466044f; m.rec[2] = .2421f; m.rec[3] = .332888f; m.trans[0] = 2; m.trans[1] = 5;
    m.rAngle = 0; m.rAngleVar = 70; m.seed = 38965; m.spray = kSprayM4; m.nSpray = int(std::size(kSprayM4));
    m.zoomLevels = 0; m.zoomFov[0] = m.zoomFov[1] = 0;
    m.shot[0] = L"m4a1_01"; m.shot[1] = L"m4a1_02"; m.shot[2] = L"m4a1_03"; m.shot[3] = L"m4a1_04"; m.nShot = 4; m.shotVol = .75f; m.draw = L"m4a1_draw";
    m.reloadSnd[0] = {.83f, L"m4a1_clipout"}; m.reloadSnd[1] = {2.09f, L"m4a1_clipin"}; m.reloadSnd[2] = {2.6f, L"m4a1_boltback"};
    m.reloadSnd[3] = {2.8f, L"m4a1_boltforward"}; m.reloadSnd[4] = {0, nullptr}; m.boltSnd[0] = {0, nullptr};
    m.grip = {35, -1, 20}; m.muzzle = {93, 0, 30}; m.tracerFreq = 3; m.muzzleFx = MUZ L"NS_GuiChaiBlack_Rifle_002_Muzzle.NS_GuiChaiBlack_Rifle_002_Muzzle"; m.fxScale = .7;
}

// fixed per-weapon kick sequence for weapons without a spray table: the kick angle walks randomly,
// almost straight up for the first shots and swinging wider later (fpsgame wpPattern)
static void Pattern(Wpn& w) {
    for (int alt = 0; alt < 2; ++alt) {
        uint32_t x = w.seed ? w.seed : 1;
        auto r = [&] { x = x * 1103515245u + 12345u; return x / 4294967296.0; };
        double ang = w.rAngle;
        for (int i = 0; i < 64; ++i) {
            double sa = w.rAngleVar * std::min(1.0, i / 8.0);
            ang = std::clamp(ang + (r() - .5) * sa * .6, -sa * .5, sa * .5);
            double mag = w.st[alt].rMag + (r() * 2 - 1) * w.st[alt].rMagVar;
            w.pattern[alt][i][0] = float(cos(ang * kDeg) * mag);  // up
            w.pattern[alt][i][1] = float(sin(ang * kDeg) * mag);  // right
        }
    }
}
void WpnInit() { Fill(); for (auto& w : g_wpn) Pattern(w); }

// spray table lookup: bullet offset (up, right) in degrees at shot i (fractional), last key beyond the end
static bool SprayAt(const Wpn& w, double i, double out[2]) {
    if (!w.spray) return false;
    auto k = w.spray; int n = w.nSpray;
    if (i >= k[n - 1][0]) { out[0] = k[n - 1][1]; out[1] = k[n - 1][2]; return true; }
    for (int j = 1; j < n; ++j) if (i <= k[j][0]) {
        double t = (i - k[j - 1][0]) / (k[j][0] - k[j - 1][0]);
        out[0] = k[j - 1][1] + (k[j][1] - k[j - 1][1]) * t; out[1] = k[j - 1][2] + (k[j][2] - k[j - 1][2]) * t;
        return true;
    }
    out[0] = out[1] = 0; return true;
}
// recovery time (to 10%): stand / crouch, from the early to the late value over shots trans[0]..trans[1]
static double RecTime(const Wpn& w, const Acc& a, bool crouch) {
    double a0 = crouch ? w.rec[2] : w.rec[0], a1 = crouch ? w.rec[3] : w.rec[1];
    double t = Clamp01((a.index - w.trans[0]) / std::max(1.f, w.trans[1] - w.trans[0]));
    return a0 + (a1 - a0) * t;
}
void AccRecover(Acc& a, const Wpn& w, double dt, bool crouch, double now) {
    double rt = RecTime(w, a, crouch);
    a.penalty *= exp(-dt * log(10.0) / rt);
    double since = now - a.lastShot;
    if (since > w.cycle * 1.5) a.index = std::max(0.0, a.index - dt / rt * std::max(1.0, a.index) * .9);
    // aim punch: slow decay between shots of a burst, fast return once the trigger is released
    bool firing = since < w.cycle * 1.6;
    double ex = exp(-dt * (firing ? 2.5 : 8)), lin = (firing ? 0 : 18) * dt, h = hypot(a.punch[0], a.punch[1]);
    if (h < 1e-9) h = 1;
    for (int i = 0; i < 2; ++i) {
        double p = a.punch[i] * ex, m = fabs(p) - lin * fabs(p) / h;
        a.punch[i] = m > 0 ? (p < 0 ? -m : m) : 0;
    }
}
static const double kInaccScale = .3;
double AccInacc(const Wpn& w, int alt, const Acc& a, const MoveSt& m) {
    const Stats& s = w.st[alt];
    double inacc = m.crouch ? s.crouch : s.stand;
    double lo = s.maxSpeed * kDuckMod, hi = s.maxSpeed * .95;
    inacc += Clamp01((m.speed - lo) / (hi - lo)) * s.move;
    if (!m.grounded) inacc += s.jump;
    return (inacc + a.penalty) * kInaccScale;  // tighter than CS, feels better in third person
}
// one shot: before[] gets the punch this bullet uses (the value before the kick), then the kick is added
void AccFire(Acc& a, const Wpn& w, int alt, double now, double before[2]) {
    before[0] = a.punch[0]; before[1] = a.punch[1];
    double sp[2], cur[2];
    if (SprayAt(w, a.index + 1, sp)) {
        // spraying: this bullet takes the table's offset at the current shot, no recovery discount
        SprayAt(w, a.index, cur);
        if (now - a.lastShot < w.cycle * 1.6) { before[0] = cur[0] / kRecoilScale; before[1] = cur[1] / kRecoilScale; }
        a.punch[0] = sp[0] / kRecoilScale; a.punch[1] = sp[1] / kRecoilScale;
    } else {
        auto& p = w.pattern[alt][std::min(63, int(a.index))];
        a.punch[0] += p[0] * .029; a.punch[1] += p[1] * .029;
    }
    a.index += 1; a.lastShot = now; a.penalty += w.st[alt].fire;
}

static uint32_t g_rng = 0x2545F491u;
double Rand() { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 8) / 16777216.0; }
// view rotation r (punch already added) -> a direction inside the inaccuracy + spread cone (radians)
ik::V ShotDir(uex::Rot r, double inacc, double spread) {
    ik::V f, rt, up; ik::Axes(r, f, rt, up);
    double t0 = Rand() * 6.2831853, r0 = Rand() * inacc, t1 = Rand() * 6.2831853, r1 = Rand() * spread;
    double sx = cos(t0) * r0 + cos(t1) * r1, sy = sin(t0) * r0 + sin(t1) * r1;
    return ik::Norm(f + rt * sx + up * sy);
}

namespace ik {
void Axes(uex::Rot r, V& f, V& rt, V& up) {
    double cp = cos(r.pitch * kDeg), sp = sin(r.pitch * kDeg), cy = cos(r.yaw * kDeg), sy = sin(r.yaw * kDeg),
           cr = cos(r.roll * kDeg), sr = sin(r.roll * kDeg);
    f = {cp * cy, cp * sy, sp};
    rt = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    up = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
}
uex::Rot RotOf(V d) {
    d = Norm(d);
    return {atan2(d.z, sqrt(d.x * d.x + d.y * d.y)) / kDeg, atan2(d.y, d.x) / kDeg, 0};
}
}  // namespace ik

// model.cpp: engine helpers, the rifle models and the hold pose.
// Models: sniper_cache/<id>.bin (tools/glb2bin.mjs) and <id>_color / _normal / _orm.png, one
// ProceduralMeshComponent per weapon on a StaticMeshActor attached to the player's hand_r bone (so the
// rifle follows the hand inside the frame, no lag), built the first time the weapon is drawn.
// Hold: the golden musket's idle on the upper-body slot, frozen (stock at the shoulder, right hand on the
// grip, left hand under the fore-end), the rifle laid along hand_r -> hand_l every frame, the character
// turned to the camera's yaw.
#include "common.h"
#include <algorithm>

bool Live(UObject* o) { return o && uex::Alive(o); }
static uex::Prop kMesh{L"Class /Script/Engine.Character", L"Mesh", 0x330};
static uex::Prop kHidden{L"Class /Script/Engine.Actor", L"bHidden", 0x58, 0x80};
UObject* MeshOf(UObject* ch) { return ch ? kMesh.Of<UObject*>(ch) : nullptr; }
UObject* AnimOf(UObject* ch) {
    auto m = MeshOf(ch); if (!m) return nullptr;
    ue::Params g(uex::Fn(m, L"GetAnimInstance")); g.Call(m); return g.Get<UObject*>(L"ReturnValue");
}
UObject* OwnerOf(UObject* c) {
    if (!c) return nullptr;
    ue::Params p(uex::Fn(c, L"GetOwner")); p.Call(c); return p.Get<UObject*>(L"ReturnValue");
}
UObject* KLib() { return uex::Lib(L"KismetMathLibrary /Script/Engine.Default__KismetMathLibrary"); }
UObject* CamMgr() {
    auto gs = uex::GameplayStatics(); if (!gs) return nullptr;
    ue::Params g(uex::Fn(gs, L"GetPlayerCameraManager"));
    g.Set(L"WorldContextObject", uex::World()); g.Set<int32_t>(L"PlayerIndex", 0); g.Call(gs);
    return g.Get<UObject*>(L"ReturnValue");
}
bool CamView(ik::V* loc, uex::Rot* rot) {
    auto cm = CamMgr(); if (!cm) return false;
    { ue::Params q(uex::Fn(cm, L"GetCameraLocation")); q.Call(cm); *loc = ik::FromU(q.Get<uex::Vec>(L"ReturnValue")); }
    { ue::Params q(uex::Fn(cm, L"GetCameraRotation")); q.Call(cm); *rot = q.Get<uex::Rot>(L"ReturnValue"); }
    return true;
}
uex::Rot g_ctrlLast;
uex::Rot CtrlRot() {
    auto pc = uex::PC(); if (!pc) return {};
    ue::Params g(uex::Fn(pc, L"GetControlRotation")); g.Call(pc); return g.Get<uex::Rot>(L"ReturnValue");
}
void SetCtrlRot(uex::Rot r) {
    auto pc = uex::PC(); if (!pc) return;
    ue::Params q(uex::Fn(pc, L"SetControlRotation")); q.Set(L"NewRotation", r); q.Call(pc);
    g_ctrlLast = r;
}
ik::V Sock(UObject* m, const wchar_t* b) {
    ue::Params s(uex::Fn(m, L"GetSocketLocation")); s.Set(L"InSocketName", uex::Name(b)); s.Call(m);
    return ik::FromU(s.Get<uex::Vec>(L"ReturnValue"));
}
void SetFov(float fov) {  // PlayerCameraManager LockedFOV (what the FOV exec sets); 0 unlocks
    // the PlayerController FOV exec does nothing in shipping builds since the 11-04 update.
    // LockedFOV is native, right after DefaultFOV (APlayerCameraManager: DefaultFOV, LockedFOV, ...)
    auto pc = uex::PC(); if (!pc) return;
    int po = ue::FindProp(pc->cls, L"PlayerCameraManager").offset; if (po < 0) return;
    auto cm = ue::At<UObject*>(pc, po); if (!cm) return;
    int fo = ue::FindProp(cm->cls, L"DefaultFOV").offset; if (fo < 0) return;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(cm) + fo + 4) = fov > 0 ? fov : 0.f;
}

// ---- meshes: glb metres, Y up, barrel along +Z -> UE cm, Z up, barrel along +X ----
struct MeshData {
    bool tried = false;
    std::vector<uex::Vec> v, n; std::vector<double> uv; std::vector<int32_t> tri; std::vector<uint32_t> col;
};
static MeshData g_md[kWpnN];
static bool LoadMesh(int i) {
    auto& m = g_md[i];
    if (m.tried) return !m.tri.empty();
    m.tried = true;
    std::wstring path = g_cacheDir + g_wpn[i].id + L".bin";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { uex::Log(L"sniper: no %s", path.c_str()); return false; }
    char tag[4]; uint32_t nv = 0, ni = 0;
    fread(tag, 1, 4, f); fread(&nv, 4, 1, f); fread(&ni, 4, 1, f);
    if (memcmp(tag, "AWPM", 4) || nv > 2000000 || ni > 6000000 || ni % 3) { fclose(f); return false; }
    std::vector<float> p(nv * 3), nn(nv * 3), uv(nv * 2); std::vector<uint32_t> idx(ni);
    fread(p.data(), 4, nv * 3, f); fread(nn.data(), 4, nv * 3, f); fread(uv.data(), 4, nv * 2, f);
    size_t got = fread(idx.data(), 4, ni, f); fclose(f);
    if (got != ni) return false;
    m.v.resize(nv); m.n.resize(nv); m.uv.resize(nv * 2); m.col.assign(nv, 0xFF000000u);  // black: the material's base layer
    for (size_t k = 0; k < nv; ++k) {
        const float* q = &p[k * 3]; const float* o = &nn[k * 3];
        m.v[k] = {q[2] * 100.0, q[0] * 100.0, q[1] * 100.0};
        m.n[k] = {o[2], o[0], o[1]};
        m.uv[k * 2] = uv[k * 2]; m.uv[k * 2 + 1] = uv[k * 2 + 1];
    }
    m.tri.resize(ni);
    for (size_t k = 0; k + 2 < ni; k += 3) {  // the x / y swap flips handedness: reverse the winding
        m.tri[k] = int32_t(idx[k]); m.tri[k + 1] = int32_t(idx[k + 2]); m.tri[k + 2] = int32_t(idx[k + 1]);
    }
    uex::Log(L"sniper: %s mesh %u verts %u tris", g_wpn[i].id, nv, ni / 3);
    return true;
}
// a material the game renders (the wooden pole's), with the weapon's albedo / normal / ORM textures
static UObject* MakeMaterial(UObject* owner, int i) {
    static const wchar_t* kPole = L"/Game/PBZContent/Environments/Props/Wood/Mesh/SM_Wooden_Pole_002.SM_Wooden_Pole_002";
    UObject* base = nullptr;
    if (auto sm = uex::LoadAsset(kPole)) {
        ue::Params q(uex::Fn(sm, L"GetMaterial")); q.Set<int32_t>(L"MaterialIndex", 0); q.Call(sm); base = q.Get<UObject*>(L"ReturnValue");
    }
    auto lib = uex::Lib(L"KismetMaterialLibrary /Script/Engine.Default__KismetMaterialLibrary");
    auto rl = uex::Lib(L"KismetRenderingLibrary /Script/Engine.Default__KismetRenderingLibrary");
    if (!base || !lib) { uex::Log(L"sniper: no base material"); return nullptr; }
    ue::Params p(uex::Fn(lib, L"CreateDynamicMaterialInstance"));
    p.Set(L"WorldContextObject", owner); p.Set(L"Parent", base); p.Call(lib);
    auto mat = p.Get<UObject*>(L"ReturnValue");
    if (!mat || !rl) return mat;
    static const wchar_t* tex[][2] = {{L"Albedo", L"_color.png"}, {L"Normal", L"_normal.png"}, {L"Mix", L"_orm.png"}};
    for (auto& t : tex) {
        std::wstring path = g_cacheDir + g_wpn[i].id + t[1];
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        struct FS { const wchar_t* d; int32_t n, m; } fs{path.c_str(), int32_t(path.size() + 1), int32_t(path.size() + 1)};
        ue::Params q(uex::Fn(rl, L"ImportFileAsTexture2D")); q.Set(L"WorldContextObject", owner); q.Set(L"Filename", fs); q.Call(rl);
        auto tx = q.Get<UObject*>(L"ReturnValue");
        if (!tx) { uex::Log(L"sniper: texture %s failed", path.c_str()); continue; }
        ue::Params c(uex::Fn(mat, L"SetTextureParameterValue")); c.Set(L"ParameterName", uex::Name(t[0])); c.Set(L"Value", tx); c.Call(mat);
    }
    return mat;
}

// ---- the model actor: attached to hand_r (follows the hand within the frame), one mesh per weapon ----
UObject* g_modelOwner;
static UObject* g_model;
static UObject* g_comp[kWpnN];
static int g_cur = -1;
static bool g_modelHidden;
static void SetCompHidden(UObject* c, bool h) {
    ue::Params q(uex::Fn(c, L"SetHiddenInGame")); q.Set<bool>(L"NewHidden", h); q.Set<bool>(L"bPropagateToChildren", false); q.Call(c);
}
static bool MakeActor(UObject* me) {
    auto cls = uex::Class(L"Class /Script/Engine.StaticMeshActor"); auto mesh = MeshOf(me);
    if (!cls || !mesh) return false;
    auto a = uex::Spawn(cls, uex::Loc(me), 0, me); if (!a) { uex::Log(L"sniper: model spawn failed"); return false; }
    { ue::Params q(uex::Fn(a, L"SetMobility")); q.Set<uint8_t>(L"InMobility", 2); q.Call(a); }
    { ue::Params q(uex::Fn(a, L"SetActorEnableCollision")); q.Set<bool>(L"bNewActorEnableCollision", false); q.Call(a); }
    ue::Params t(uex::Fn(a, L"K2_AttachToComponent"));
    t.Set(L"Parent", mesh); t.Set(L"SocketName", uex::Name(L"hand_r"));
    t.Set<uint8_t>(L"LocationRule", 2); t.Set<uint8_t>(L"RotationRule", 2); t.Set<uint8_t>(L"ScaleRule", 1);
    t.Set<bool>(L"bWeldSimulatedBodies", false); t.Call(a);
    g_model = a; g_modelOwner = me; for (auto& c : g_comp) c = nullptr; g_cur = -1;
    return true;
}
static UObject* MakeComp(int i) {
    if (!LoadMesh(i)) return nullptr;
    auto cls = uex::Class(L"Class /Script/ProceduralMeshComponent.ProceduralMeshComponent"); if (!cls) return nullptr;
    ue::Params ac(uex::Fn(g_model, L"AddComponentByClass"));
    ac.Set(L"Class", cls); ac.Set<bool>(L"bManualAttachment", false); ac.Set<bool>(L"bDeferredFinish", false); ac.Call(g_model);
    auto pm = ac.Get<UObject*>(L"ReturnValue"); if (!pm) { uex::Log(L"sniper: AddComponentByClass failed"); return nullptr; }
    // RelativeTransform left zeroed above means scale 0: set it
    { ue::Params q(uex::Fn(pm, L"SetRelativeScale3D")); q.Set(L"NewScale3D", uex::Vec{1, 1, 1}); q.Call(pm); }
    auto& m = g_md[i];
    ue::Params p(uex::Fn(pm, L"CreateMeshSection"));
    p.Set<int32_t>(L"SectionIndex", 0); p.Set(L"Vertices", Arr(m.v)); p.Set(L"Triangles", Arr(m.tri)); p.Set(L"Normals", Arr(m.n));
    TA uvs{m.uv.data(), int32_t(m.v.size()), int32_t(m.v.size())}; p.Set(L"UV0", uvs); p.Set(L"VertexColors", Arr(m.col));
    p.Set<bool>(L"bCreateCollision", false); p.Call(pm);
    if (auto mat = MakeMaterial(g_model, i)) { ue::Params s(uex::Fn(pm, L"SetMaterial")); s.Set<int32_t>(L"ElementIndex", 0); s.Set(L"Material", mat); s.Call(pm); }
    { ue::Params q(uex::Fn(pm, L"SetCollisionEnabled")); q.Set<uint8_t>(L"NewType", 0); q.Call(pm); }
    if (auto f = uex::Fn(pm, L"UpdateBounds")) { ue::Params q(f); q.Call(pm); }
    uex::Log(L"sniper: %s model built", g_wpn[i].id);
    return pm;
}
bool ModelShow(UObject* me, int wpn) {
    try {
        if (g_modelOwner != me || !Live(g_model)) { g_model = nullptr; if (!MakeActor(me)) return false; }
        if (!g_comp[wpn] || !Live(g_comp[wpn])) g_comp[wpn] = MakeComp(wpn);
        for (int i = 0; i < kWpnN; ++i) if (g_comp[i]) SetCompHidden(g_comp[i], i != wpn || g_modelHidden);
        g_cur = wpn;
        return g_comp[wpn] != nullptr;
    } catch (...) { uex::Log(L"sniper: model show crashed"); return false; }
}
void ModelDrop() {
    if (Live(g_model)) uex::Destroy(g_model);
    g_model = nullptr; g_modelOwner = nullptr; g_cur = -1; g_modelHidden = false;
    for (auto& c : g_comp) c = nullptr;
}
UObject* ModelActor() { return g_model; }
void ModelHide(bool h) {
    if (h == g_modelHidden) return;
    g_modelHidden = h;
    if (g_cur >= 0 && Live(g_comp[g_cur])) SetCompHidden(g_comp[g_cur], h);
}


// ---- hold pose ----
// The pose sequence plays as a dynamic montage (blend in .2 s) and stays paused on one frame; the bow
// draw montage (played at 1.5x up to full draw) is the fallback if the sequence won't load.
// Montage_IsPlaying is false while paused, so "was it interrupted" uses Montage_IsActive (a dodge or a
// hit reaction stops our montage: play it again); if something resumed or moved it, set it back.
ik::V g_hgR{-2, 0, -3};        // hand_r bone -> grip contact, along hand_r->hand_l (x), its right (y), up (z)
ik::V g_hgL{6, 8, 0};          // hand_l bone -> palm under the fore-end (same axes): the barrel runs through it
uex::Rot g_wpnOff{0, 0, 0};    // model axes -> the hand line's axes
double g_pitchFollow = 0;      // share of the camera pitch the rifle follows (turning about the grip)
double g_poseAt = .5;          // position of the frozen frame (s)
double g_faceOff = 0;          // test: extra body yaw (look at the hold from the side)
bool g_noFace;                 // test: leave the body's facing alone
double g_yawOff; bool g_yawOffOk;  // barrel yaw - body yaw in the frozen pose, measured once
// the golden musket's idle (shouldered, both hands on the gun), upper body only: the legs keep walking
static const wchar_t* kPoseSeq = L"/HuaShira/Characters/Hero/Hero_Hun/Animation/ShaChi_Weapon/HuangJinHuoChong/Anim/Locomotion/Hun_ShaChi_HuoChong_Idle_Loop.Hun_ShaChi_HuoChong_Idle_Loop";
static UObject* g_poseMont;           // the playing (dynamic) montage
static UObject* g_poseSeq;
static bool g_poseSeqTried;
static float g_poseRate = 1;
static std::wstring g_poseSlot = L"UpperBodyAimOverride";
static bool g_posing, g_frozen;
static double g_frozenFor, g_spanRef;
static UObject* g_holdMe;
static UObject* FindMont(const wchar_t* name) {
    auto mcls = uex::Class(L"Class /Script/Engine.AnimMontage"); if (!mcls) return nullptr;
    for (int k = 0, n = ue::NumObjects(); k < n; ++k) { auto o = ue::ObjectAt(k); if (o && o->cls == mcls && ue::NameOf(o) == name) return o; }
    return nullptr;
}
static bool MontBool(UObject* ai, const wchar_t* fn) {
    ue::Params q(uex::Fn(ai, fn)); q.Set(L"Montage", g_poseMont); q.Call(ai); return q.Get<bool>(L"ReturnValue");
}
static void PoseKeep(UObject* me, float dt) {
    auto ai = AnimOf(me); if (!ai) return;
    if (!g_poseSeq && !g_poseSeqTried) { g_poseSeqTried = true; g_poseSeq = uex::LoadAsset(kPoseSeq); }
    if (!g_poseMont && !g_poseSeq) g_poseMont = FindMont(L"AM_Hun_UnnamedBow_Charge");  // fallback
    if (!g_poseMont && !g_poseSeq) { static bool told; if (!told) { told = true; uex::Log(L"sniper: no hold pose montage"); } return; }
    if (!g_posing || !Live(g_poseMont) || !MontBool(ai, L"Montage_IsActive")) {
        if (g_poseSeq) {
            ue::Params p(uex::Fn(ai, L"PlaySlotAnimationAsDynamicMontage"));
            p.Set(L"Asset", g_poseSeq); p.Set(L"SlotNodeName", uex::Name(g_poseSlot.c_str()));
            p.Set<float>(L"BlendInTime", .2f); p.Set<float>(L"BlendOutTime", .2f); p.Set<float>(L"InPlayRate", g_poseRate);
            p.Set<int32_t>(L"LoopCount", 100000); p.Set<float>(L"BlendOutTriggerTime", -1.f); p.Set<float>(L"InTimeToStartMontageAt", 0.f);
            p.Call(ai); g_poseMont = p.Get<UObject*>(L"ReturnValue");
            if (!g_poseMont) { uex::Log(L"sniper: hold pose sequence won't play, using the bow draw"); g_poseSeq = nullptr; g_poseRate = 1.5f; g_poseAt = .675; g_poseSlot = L"DefaultSlot"; return; }
        } else {
            ue::Params p(uex::Fn(ai, L"Montage_Play")); p.Set(L"MontageToPlay", g_poseMont); p.Set<float>(L"InPlayRate", g_poseRate);
            p.Set<uint8_t>(L"ReturnValueType", 0); p.Set<float>(L"InTimeToStartMontageAt", 0.f); p.Set<bool>(L"bStopAllMontages", true); p.Call(ai);
        }
        if (g_posing) uex::Log(L"sniper: hold pose interrupted, raising again");
        g_posing = true; g_frozen = false; g_frozenFor = 0;
        return;
    }
    float pos; { ue::Params q(uex::Fn(ai, L"Montage_GetPosition")); q.Set(L"Montage", g_poseMont); q.Call(ai); pos = q.Get<float>(L"ReturnValue"); }
    bool playing = MontBool(ai, L"Montage_IsPlaying");
    if (g_poseAt < 0) return;  // test: let it play (loops)
    if ((!g_frozen && pos >= g_poseAt) || (g_frozen && (playing || fabs(pos - g_poseAt) > .02))) {
        { ue::Params q(uex::Fn(ai, L"Montage_SetPosition")); q.Set(L"Montage", g_poseMont); q.Set<float>(L"NewPosition", float(g_poseAt)); q.Call(ai); }
        { ue::Params q(uex::Fn(ai, L"Montage_Pause")); q.Set(L"Montage", g_poseMont); q.Call(ai); }
        if (g_frozen) uex::Log(L"sniper: hold pose re-frozen (pos %.2f playing %d)", pos, playing);
        g_frozen = true;
    }
    if (g_frozen) g_frozenFor += dt;
}
void PoseRefreeze() { g_frozen = false; g_frozenFor = 0; g_yawOffOk = false; g_spanRef = 0; }
// test: hold with seq (nullptr: back to the bow draw), played at rate on slot, frozen at `at` s (< 0: plays)
void PoseUse(UObject* me, UObject* seq, double rate, double at, const wchar_t* slot) {
    if (Live(me) && g_posing && g_poseMont) if (auto ai = AnimOf(me)) {
        ue::Params p(uex::Fn(ai, L"Montage_Stop")); p.Set<float>(L"InBlendOutTime", .1f); p.Set(L"Montage", g_poseMont); p.Call(ai);
    }
    g_poseSeq = seq; g_poseMont = nullptr; g_poseRate = float(rate); g_poseAt = at; if (slot && *slot) g_poseSlot = slot;
    g_posing = false; PoseRefreeze();
}

// bool bitfield properties (the pawn's / movement's rotation flags): byte = offset + ByteOffset, mask = FieldMask
uint8_t* BoolAt(UObject* o, const wchar_t* name, uint8_t* mask) {
    for (auto c = o ? o->cls : nullptr; c; c = ue::At<UObject*>(c, ue::kUStruct_Super))
        for (auto p = ue::At<const uint8_t*>(c, ue::kUStruct_ChildProps); p; p = ue::At<const uint8_t*>(p, ue::kFField_Next))
            if (ue::NameIs(*reinterpret_cast<const ue::FName*>(p + ue::kFField_Name), name)) {
                *mask = ue::At<uint8_t>(p, ue::kProp_Extra + 3);
                return reinterpret_cast<uint8_t*>(o) + ue::At<int32_t>(p, ue::kProp_Offset) + ue::At<uint8_t>(p, ue::kProp_Extra + 1);
            }
    return nullptr;
}
struct SavedBool { UObject* o; uint8_t* b; uint8_t m; bool v; };
static std::vector<SavedBool> g_flags;
static void SetFlag(UObject* o, const wchar_t* name, bool v) {
    uint8_t m = 0; auto b = BoolAt(o, name, &m); if (!b || !m) { uex::Log(L"sniper: no flag %s", name); return; }
    g_flags.push_back({o, b, m, (*b & m) != 0});
    *b = v ? uint8_t(*b | m) : uint8_t(*b & ~m);
}
static void FlagsRestore() {
    for (auto& s : g_flags) if (Live(s.o)) *s.b = s.v ? uint8_t(*s.b | s.m) : uint8_t(*s.b & ~s.m);
    g_flags.clear();
}
static double Wrap(double a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static bool g_placed;
// the upper body's aim pitch (the anim blueprint's combat aim offset, degrees): closed loop, so the barrel
// points at what the crosshair is on (kAimDist ahead of the camera), from whatever stance the pose has
static double g_aimPitch; static bool g_aimOn;
constexpr double kAimDist = 3000;
static void AimPitchSet(UObject* me, double pitch, float alpha) {
    auto ai = AnimOf(me); if (!ai) return;
    if (auto f = uex::Fn(ai, L"SetCombatAimAlpha")) { ue::Params q(f); q.Set<float>(L"Alpha", alpha); q.Call(ai); }
    if (auto f = uex::Fn(ai, L"SetCombatAimPitchOffset")) { ue::Params q(f); q.Set<float>(L"PitchOffset", float(pitch)); q.Call(ai); }
}
static void AimPitchStep(UObject* me, ik::V barrelAt, ik::V barrelDir, float dt) {
    ik::V eye; uex::Rot cam; if (!CamView(&eye, &cam)) return;
    ik::V f, rt, up; ik::Axes(cam, f, rt, up);
    double want = ik::RotOf(eye + f * kAimDist - barrelAt).pitch, have = ik::RotOf(barrelDir).pitch;
    g_aimPitch += std::clamp(want - have, -20.0, 20.0) * std::min(1.0, dt * 12.0);
    g_aimPitch = std::clamp(g_aimPitch, -75.0, 75.0);
    AimPitchSet(me, g_aimPitch, 1.f); g_aimOn = true;
    if (g_dbg) { static double t; if ((t += dt) > 1) { t = 0; uex::Log(L"sniper: aim pitch cam %.1f want %.1f barrel %.1f offset %.1f", cam.pitch, want, have, g_aimPitch); } }
}

// every frame while equipped: keep the pose; turn the body so the barrel points along the camera's yaw
// (the bow stance is side-on: the offset is measured once in the frozen pose); lay the rifle in the hands
// the game is doing something with the body: a montage that isn't ours, or a movement mode other than walking / falling
static bool Busy(UObject* me) {
    if (auto cm = uex::Movement(me)) {
        static int off = -2; if (off == -2) off = ue::FindProp(cm->cls, L"MovementMode").offset;
        if (off >= 0) { uint8_t mode = ue::At<uint8_t>(cm, off); if (mode != 1 && mode != 3 && mode != 0) return true; }  // 1 walking, 3 falling
    }
    auto ai = AnimOf(me); if (!ai) return false;
    ue::Params q(uex::Fn(ai, L"GetCurrentActiveMontage")); q.Call(ai);
    auto m = q.Get<UObject*>(L"ReturnValue");
    return m && m != g_poseMont;
}
void Hold(UObject* me, float dt, const Wpn& w) {
    using namespace ik;
    try {
        if (g_holdMe != me) {
            if (g_holdMe) HoldOff(g_holdMe);
            g_holdMe = me; PoseRefreeze(); g_placed = false;
            if (!(g_test.skip & 64)) {
            SetFlag(me, L"bUseControllerRotationYaw", false);  // we turn the body ourselves
            if (auto cm = uex::Movement(me)) { SetFlag(cm, L"bOrientRotationToMovement", false); SetFlag(cm, L"bUseControllerDesiredRotation", false); }
            }
        }
        if (Busy(me)) {  // climbing / mantling / another montage: let the game move the body, rifle stays on hand_r
            if (g_posing) {
                if (auto ai = AnimOf(me); ai && Live(g_poseMont)) { ue::Params p(uex::Fn(ai, L"Montage_Stop")); p.Set<float>(L"InBlendOutTime", .1f); p.Set(L"Montage", g_poseMont); p.Call(ai); }
                g_posing = false; g_frozen = false; g_frozenFor = 0; if (g_aimOn) AimPitchSet(me, 0, 0.f); g_aimOn = false; g_aimPitch = 0; uex::Log(L"sniper: hold paused (busy)"); }
            return;
        }
        if (!(g_test.skip & 32)) PoseKeep(me, dt);
        uex::Rot cr = CtrlRot();
        double body = cr.yaw - (g_yawOffOk ? g_yawOff : 0) + g_faceOff;
        if (!g_noFace) { ue::Params q(uex::Fn(me, L"K2_SetActorRotation")); q.Set(L"NewRotation", uex::Rot{0, body, 0}); q.Set<bool>(L"bTeleportPhysics", false); q.Call(me); }
        else body = uex::GetRot(me).yaw;
        auto mesh = MeshOf(me); auto comp = g_cur >= 0 ? g_comp[g_cur] : nullptr; auto lib = KLib();
        if (!mesh || !Live(comp) || !lib) return;
        V hr = Sock(mesh, L"hand_r"), hl = Sock(mesh, L"hand_l");
        double span = Len(hl - hr);
        if (g_frozen && g_frozenFor < .4) g_spanRef = span;  // the frozen pose's hand span
        bool odd = span < 8 || span > 95 || (g_spanRef > 0 && g_frozenFor >= .4 && fabs(span - g_spanRef) > 10);
        if (g_dbg) { static double t; if ((t += dt) > 1) { t = 0; uex::Log(L"sniper: hold span %.0f ref %.0f frozen %d odd %d stance %.1f", span, g_spanRef, g_frozen, odd, g_yawOff); } }
        if (odd && g_placed) return;  // a hit reaction / dodge in between: the rifle stays on hand_r (attached)
        uex::Rot r0 = RotOf(hl - hr); r0.roll = 0;
        V f, rt, up; Axes(r0, f, rt, up);
        V gr = hr + f * g_hgR.x + rt * g_hgR.y + up * g_hgR.z;
        V pl = hl + f * g_hgL.x + rt * g_hgL.y + up * g_hgL.z;  // the left palm: the fore-end rests on it
        uex::Rot r = RotOf(pl - gr); r.roll = 0;
        Axes(r, f, rt, up);
        uex::Rot ar; { ue::Params m(uex::Fn(lib, L"ComposeRotators")); m.Set(L"A", g_wpnOff); m.Set(L"B", r); m.Call(lib); ar = m.Get<uex::Rot>(L"ReturnValue"); }
        V bf, br, bu; Axes(ar, bf, br, bu);
        V org = gr - (bf * w.grip.x + br * w.grip.y + bu * w.grip.z);  // grip point (model space) onto gr
        ue::Params q(uex::Fn(comp, L"K2_SetWorldLocationAndRotation"));
        q.Set(L"NewLocation", ToU(org)); q.Set(L"NewRotation", ar); q.Set<bool>(L"bSweep", false); q.Set<bool>(L"bTeleport", true); q.Call(comp);
        g_placed = true;
        if (g_frozen) AimPitchStep(me, gr, bf, dt);
        if (g_frozen && g_frozenFor > .25 && !g_yawOffOk && !g_noFace) {
            g_yawOff = Wrap(atan2(bf.y, bf.x) / 0.0174532925 - body);
            g_yawOffOk = true; uex::Log(L"sniper: hold stance: barrel %.1f deg off the body, span %.0f", g_yawOff, span);
        }
    } catch (...) { uex::Log(L"sniper: hold crashed"); }
}
void HoldOff(UObject* me) {
    try {
        if (Live(me) && g_posing && g_poseMont) if (auto ai = AnimOf(me)) {
            ue::Params p(uex::Fn(ai, L"Montage_Stop")); p.Set<float>(L"InBlendOutTime", .2f); p.Set(L"Montage", g_poseMont); p.Call(ai);
        }
    } catch (...) {}
    try { if (g_aimOn && Live(me)) AimPitchSet(me, 0, 0.f); } catch (...) {}
    g_aimOn = false; g_aimPitch = 0;
    FlagsRestore();
    g_posing = g_frozen = g_placed = false; g_holdMe = nullptr;
}
// the muzzle from the rifle's current transform
bool MuzzlePos(const Wpn& w, ik::V* at, ik::V* dir) {
    auto comp = g_cur >= 0 ? g_comp[g_cur] : nullptr; if (!Live(comp)) return false;
    ik::V loc; uex::Rot rot;
    { ue::Params q(uex::Fn(comp, L"K2_GetComponentLocation")); q.Call(comp); loc = ik::FromU(q.Get<uex::Vec>(L"ReturnValue")); }
    { ue::Params q(uex::Fn(comp, L"K2_GetComponentRotation")); q.Call(comp); rot = q.Get<uex::Rot>(L"ReturnValue"); }
    *at = ik::Local(rot, loc, w.muzzle);
    ik::V f, rt, up; ik::Axes(rot, f, rt, up); *dir = f;
    return true;
}
// the game's own weapons (swords, bow) attached to the player: hidden while a rifle is out
static std::vector<UObject*> g_hidden;
void HideWeapons(UObject* me, bool hide) {
    try {
        if (!hide) {
            for (auto a : g_hidden) if (Live(a)) { ue::Params q(uex::Fn(a, L"SetActorHiddenInGame")); q.Set<bool>(L"bNewHidden", false); q.Call(a); }
            g_hidden.clear(); return;
        }
        if (!Live(me)) return;
        ue::Params g(uex::Fn(me, L"GetAttachedActors")); g.Set<bool>(L"bResetArray", true); g.Set<bool>(L"bRecursivelyIncludeAttachedActors", true); g.Call(me);
        auto arr = g.Get<TA>(L"OutActors");
        for (int i = 0; i < arr.n; ++i) {
            auto a = static_cast<UObject**>(arr.d)[i];
            if (!a || a == g_model || std::find(g_hidden.begin(), g_hidden.end(), a) != g_hidden.end()) continue;
            if (kHidden.Bit(a)) continue;  // already hidden by the game
            ue::Params q(uex::Fn(a, L"SetActorHiddenInGame")); q.Set<bool>(L"bNewHidden", true); q.Call(a);
            g_hidden.push_back(a); uex::Log(L"sniper: hide %s", ue::NameOf(a).c_str());
        }
    } catch (...) { uex::Log(L"sniper: hide weapons crashed"); }
}
// test: where the current weapon mesh is and whether it renders
void ModelDebug() {
    auto comp = g_cur >= 0 ? g_comp[g_cur] : nullptr;
    uex::Log(L"sniper: mdbg model %p live %d cur %d comp %p live %d hidden %d", g_model, Live(g_model), g_cur, comp, Live(comp), g_modelHidden);
    if (!Live(comp)) return;
    ik::V loc; { ue::Params q(uex::Fn(comp, L"K2_GetComponentLocation")); q.Call(comp); loc = ik::FromU(q.Get<uex::Vec>(L"ReturnValue")); }
    uex::Vec sc; { ue::Params q(uex::Fn(comp, L"K2_GetComponentScale")); q.Call(comp); sc = q.Get<uex::Vec>(L"ReturnValue"); }
    bool vis; { ue::Params q(uex::Fn(comp, L"IsVisible")); q.Call(comp); vis = q.Get<bool>(L"ReturnValue"); }
    int ns = -1; { ue::Params q(uex::Fn(comp, L"GetNumSections")); q.Call(comp); ns = q.Get<int32_t>(L"ReturnValue"); }
    UObject* par = nullptr; { ue::Params q(uex::Fn(comp, L"GetAttachParent")); q.Call(comp); par = q.Get<UObject*>(L"ReturnValue"); }
    auto me = uex::Me(); ik::V pl = me ? ik::FromU(uex::Loc(me)) : ik::V{};
    uex::Log(L"sniper: mdbg at %.0f %.0f %.0f (player %.0f %.0f %.0f) scale %.2f vis %d sections %d parent %s actorHidden %d",
        loc.x, loc.y, loc.z, pl.x, pl.y, pl.z, sc.x, vis, ns, par ? ue::NameOf(par).c_str() : L"-", kHidden.Bit(g_model));
}

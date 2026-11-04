// arena: an enclosed walled courtyard, 50 m across, built from PBZ environment art. Floats 1 km
// above the world origin, far over any terrain. Layout (original):
//   - flagstone floor (Pangzhen road tiles) over a hidden collision disc
//   - 36-sided grey brick courtyard wall (town kit panels at native size) with tiled coping; a hidden
//     24-sided cube ring guarantees the wall blocks
//   - two roofed memorial archways on the +y / -y axes, flanked by stone lions and lanterns
//   - 8 red hall pillars on carved bases engaged in the wall, each with a paper lantern; banners between
//   - 4 braziers with Niagara fire, embers and warm point lights on the diagonals
//   - war drum behind slot 0 (+x), bronze bell behind slot 1 (-x)
//   - pines and maples outside the wall, cliffs and far mountains as a backdrop
// The centre 40 m stays open. Only parts players should bump into get collision.
#include "arena.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace ue;
using uex::Vec;

namespace arena {
namespace {
constexpr double kPi = 3.14159265358979;
constexpr Vec kC{0, 0, 100000};        // floor top centre (cm)
constexpr double kR = 2500;            // inner wall radius
constexpr int kSegs = 24;
constexpr double kWallH = 800, kWallT = 100;  // hidden collision ring
constexpr double kSpawnR = 600;        // slots 0/1 are 12 m apart, face to face
constexpr double kFallZ = 500;         // below floor top -> return

#define ENV L"/Game/PBZContent/Environments/"
const wchar_t* kCube = L"/Engine/BasicShapes/Cube.Cube";
const wchar_t* kCyl = L"/Engine/BasicShapes/Cylinder.Cylinder";
const wchar_t* kFloor = ENV L"Props/Road/Mesh/SM_Pangzhen_Floor_001.SM_Pangzhen_Floor_001";        // 5 x 5 m
// town courtyard kit (xtyl): grey brick wall panel 4.4 x 4.4 m with its own tiled coping 4.2 m
const wchar_t* kWall = ENV L"Arch/Town/SM_xtyl/Mesh/SM_xtyl_stonewall_001.SM_xtyl_stonewall_001";
const wchar_t* kCoping = ENV L"Arch/Town/SM_xtyl/Mesh/SM_xtyl_tile_003_001.SM_xtyl_tile_003_001";
const wchar_t* kStep = ENV L"Arch/Town/SM_xtyl/Mesh/SM_xtyl_step_001_001.SM_xtyl_step_001_001";     // 3.5 m stone sill
const wchar_t* kArch = ENV L"Arch/MemorialArchway/MemorialArchway_S/Mesh/SM_MemorialArchway_S.SM_MemorialArchway_S";
// hall pillar and its carved base share a pivot (the shaft starts at the base's 56 cm top)
const wchar_t* kPillar = ENV L"Arch/Pillar/Mesh/SM_Hall_Pillar_M_001.SM_Hall_Pillar_M_001";
const wchar_t* kPillarBase = ENV L"Arch/Pillar/Mesh/SM_Hall_Pillar_Base_M_001.SM_Hall_Pillar_Base_M_001";
const wchar_t* kHangLamp = ENV L"Props/Lantern/Mesh/SM_lantern002_001.SM_lantern002_001";
const wchar_t* kLion = ENV L"Props/Statue/Mesh/SM_LionStatue_002.SM_LionStatue_002";
const wchar_t* kStand = ENV L"Props/Brazier/Mesh/SM_Scan_BrazierStand_002.SM_Scan_BrazierStand_002";
const wchar_t* kBowl = ENV L"Props/Brazier/Mesh/SM_Brazier_001.SM_Brazier_001";
const wchar_t* kDrum = ENV L"Props/Deco_Big/Mesh/SM_Scan_WarDrum_001.SM_Scan_WarDrum_001";
const wchar_t* kBell = ENV L"Props/Deco/Mesh/SM_Prop_BigBell_001.SM_Prop_BigBell_001";
const wchar_t* kLamp = ENV L"Props/Lamp/Mesh/SM_PBR_Lamp_001.SM_PBR_Lamp_001";
const wchar_t* kBanner = L"/Game/LP_PBZ_WP_LD/StaticMesh/SM_Prop_Wall_Banner_06.SM_Prop_Wall_Banner_06";
const wchar_t* kPine = ENV L"Foliage/Tree/PineTree/Mesh/PineTree_003_USD/SM_Scan_PineTree_007.SM_Scan_PineTree_007";
const wchar_t* kMaple = ENV L"Foliage/Tree/MapleTree/Mesh/SM_MapleTree_002.SM_MapleTree_002";
const wchar_t* kCliff = ENV L"Nature/Cliff/Mesh/SM_SharpCliff_001.SM_SharpCliff_001";
const wchar_t* kFarMtn = ENV L"Nature/Cliff/Mesh/SM_Scan_FarMountain_004.SM_Scan_FarMountain_004";
#undef ENV
const wchar_t* kBigFire = L"/Game/PBZContent/VFX/SceneFX/BossFight/Troop/NS_Troop_TowerFire_001.NS_Troop_TowerFire_001";
const wchar_t* kEmber = L"/Game/PBZContent/VFX/SceneFX/SmokeAndFIre/NS_Ember_001.NS_Ember_001";
const wchar_t* kDome = L"/Weather/UltraDynamicSky/Meshes/Ultra_Dynamic_Sky_Sphere.Ultra_Dynamic_Sky_Sphere";
const wchar_t* kDomeMat = L"/Game/LevelPrototyping/Materials/MI_BaseColor_DarkGrey.MI_BaseColor_DarkGrey";
std::vector<uex::Ref> g_parts;   // every spawned actor; g_parts[0] is the collision floor
UObject* g_world = nullptr;
bool g_complete = false;  // the last Build() got to the end (an exception midway leaves it false)
// the parts of this world are still there (Ready() also needs a finished build)
bool PartsAlive() { return !g_parts.empty() && g_world == uex::World() && uex::Get(g_parts[0]); }

// ---- maps ------------------------------------------------------------------------------------
// Both open L_Longzhu_xigu_Level (a small single level, no World Partition, loads in < 15 s).
//   0 石院: the walled courtyard built at kC, 1 km above the level
//   1 火焚寺: the level's burning temple yard itself, flat to +-90 cm for 14 m around the centre,
//     debris and fire 15-20 m out; only the hidden collider ring is added
//   2 三面坛: the round stone plaza in Xigu (World Partition, ~30 s to load), the three-faced
//     puppet's boss arena; flat to +-20 cm for 18 m, rock walls from 21.6 m. Ring only; the boss,
//     NPCs, fog doors and the chapter-opening cinematic go via ClearLevel
const wchar_t* kLongzhu = L"/Game/PBZContent/Maps/Battle_Env/L_Longzhu_xigu_Level";
const Map kMaps[] = {
    {L"石院", kLongzhu, kC, kR, true, L"0.png"},
    {L"火焚寺", kLongzhu, {0, -1000, 14}, 1400, false, L"1.png"},
    {L"三面坛", L"/Game/Maps/B02/PBZ_Xigu_WP", {3500, 288000, 1999318}, 1900, false, L"2.png"},
};
constexpr int kMapCount = int(sizeof kMaps / sizeof kMaps[0]);
int g_map = 0;
const Map& M() { return kMaps[g_map]; }

struct Bounds { double o[3], e[3], r; };
struct LinColor { float r, g, b, a; };
struct VParam { const wchar_t* name; LinColor v; };

void Call1(UObject* o, const wchar_t* fn, auto setter) {
    auto f = o ? uex::Fn(o, fn) : nullptr; if (!f) { uex::Log(L"arena: no %s", fn); return; }
    Params p(f); setter(p); p.Call(o);
}

UObject* MeshActor(UObject* m, Vec loc, double yaw, Vec scale, bool collide) {
    static UObject* smaCls = uex::Class(L"Class /Script/Engine.StaticMeshActor");
    static UObject* smcCls = uex::Class(L"Class /Script/Engine.StaticMeshComponent");
    if (!smaCls || !smcCls || !m) return nullptr;
    auto a = uex::Spawn(smaCls, loc, yaw);
    if (!a) return nullptr;
    g_parts.push_back(uex::Hold(a));
    auto c = uex::Comp(a, smcCls);
    if (!c) return a;
    Call1(c, L"SetMobility", [](Params& p) { p.Set<uint8_t>(L"NewMobility", 2); });  // runtime mesh change needs Movable
    Call1(c, L"SetStaticMesh", [&](Params& p) { p.Set(L"NewMesh", m); });
    if (collide) {
        Call1(c, L"SetCollisionProfileName", [](Params& p) { p.Set(L"InCollisionProfileName", uex::Name(L"BlockAll")); p.Set<bool>(L"bUpdateOverlaps", true); });
        Call1(c, L"SetCollisionEnabled", [](Params& p) { p.Set<uint8_t>(L"NewType", 3); });  // QueryAndPhysics
    } else {
        Call1(c, L"SetCollisionEnabled", [](Params& p) { p.Set<uint8_t>(L"NewType", 0); });
    }
    Call1(a, L"SetActorScale3D", [&](Params& p) { p.Set(L"NewScale3D", scale); });
    return a;
}

// invisible collider: engine shape (100 cm, pivot centred) at loc, size in cm
UObject* Collider(const wchar_t* shape, Vec loc, double yaw, Vec size) {
    auto a = MeshActor(uex::LoadAsset(shape), loc, yaw, {size.x / 100, size.y / 100, size.z / 100}, true);
    if (a) Call1(a, L"SetActorHiddenInGame", [](Params& p) { p.Set<bool>(L"bNewHidden", true); });
    return a;
}

Bounds GetBounds(UObject* m) {
    Params bp(uex::Fn(m, L"GetBounds")); bp.Call(m); return bp.Get<Bounds>(L"ReturnValue");
}

// art mesh placed by its bounds: the bottom centre of its (scaled, rotated) box lands on `base`
UObject* Place(const wchar_t* path, Vec base, double yaw, Vec s, bool collide = false, Bounds* out = nullptr) {
    auto m = uex::LoadAsset(path);
    if (!m) { uex::Log(L"arena: missing %s", path); return nullptr; }
    auto b = GetBounds(m);
    if (out) *out = b;
    double y = yaw * kPi / 180, lx = b.o[0] * s.x, ly = b.o[1] * s.y;
    Vec loc{base.x - (lx * std::cos(y) - ly * std::sin(y)), base.y - (lx * std::sin(y) + ly * std::cos(y)),
            base.z - (b.o[2] - b.e[2]) * s.z};
    return MeshActor(m, loc, yaw, s, collide);
}

Vec Ring(double r, double deg, double dz = 0) {
    double a = deg * kPi / 180; return {kC.x + r * std::cos(a), kC.y + r * std::sin(a), kC.z + dz};
}

void Light(Vec loc, float candela, double radius, LinColor col = {1.f, 0.62f, 0.4f, 1.f}) {
    static UObject* cls = uex::Class(L"Class /Script/Engine.PointLight");
    static UObject* lc = uex::Class(L"Class /Script/Engine.PointLightComponent");
    auto a = cls ? uex::Spawn(cls, loc, 0) : nullptr; if (!a) return;
    g_parts.push_back(uex::Hold(a));
    auto c = uex::Comp(a, lc); if (!c) return;
    Call1(c, L"SetMobility", [](Params& p) { p.Set<uint8_t>(L"NewMobility", 2); });
    Call1(c, L"SetIntensityUnits", [](Params& p) { p.Set<uint8_t>(L"NewIntensityUnits", 1); });  // candelas
    Call1(c, L"SetIntensity", [&](Params& p) { p.Set<float>(L"NewIntensity", candela); });
    Call1(c, L"SetLightColor", [&](Params& p) { p.Set(L"NewLightColor", col); p.Set<bool>(L"bSRGB", true); });
    Call1(c, L"SetAttenuationRadius", [&](Params& p) { p.Set<float>(L"NewRadius", float(radius)); });
    Call1(c, L"SetCastShadows", [](Params& p) { p.Set<bool>(L"bNewValue", false); });
}

// Niagara system in its own NiagaraActor, so Clear() destroys it like any other part
void Fx(const wchar_t* path, Vec loc, double scale) {
    static UObject* cls = uex::Class(L"Class /Script/Niagara.NiagaraActor");
    static UObject* nc = uex::Class(L"Class /Script/Niagara.NiagaraComponent");
    auto sys = cls && nc ? uex::LoadAsset(path) : nullptr;
    if (!sys) { uex::Log(L"arena: no fx %s", path); return; }
    auto a = uex::Spawn(cls, loc, 0); if (!a) return;
    g_parts.push_back(uex::Hold(a));
    auto c = uex::Comp(a, nc); if (!c) return;
    Call1(c, L"SetAsset", [&](Params& p) { p.Set(L"InAsset", sys); p.Set<bool>(L"bResetExistingOverrideParameters", false); });
    Call1(a, L"SetActorScale3D", [&](Params& p) { p.Set(L"NewScale3D", Vec{scale, scale, scale}); });
    Call1(c, L"SetAutoDestroy", [](Params& p) { p.Set<bool>(L"bInAutoDestroy", false); });
    Call1(c, L"Activate", [](Params& p) { p.Set<bool>(L"bReset", true); });
    Params ia(uex::Fn(c, L"IsActive")); ia.Call(c);
    if (!ia.Get<bool>(L"ReturnValue")) uex::Log(L"arena: fx %s not active", path);
}

double Dist2(Vec p) { return std::hypot(p.x - kC.x, p.y - kC.y); }

// every material slot of an arena mesh actor -> one PBZ material (already-shipped instance)
void Mat(UObject* a, const wchar_t* path) {
    static UObject* smcCls = uex::Class(L"Class /Script/Engine.StaticMeshComponent");
    auto m = a ? uex::LoadAsset(path) : nullptr; if (!m) { if (a) uex::Log(L"arena: no mat %s", path); return; }
    auto c = uex::Comp(a, smcCls); if (!c) return;
    Params n(uex::Fn(c, L"GetNumMaterials")); n.Call(c);
    for (int i = 0, k = n.Get<int32_t>(L"ReturnValue"); i < k; i++)
        Call1(c, L"SetMaterial", [&](Params& p) { p.Set<int32_t>(L"ElementIndex", i); p.Set(L"Material", m); });
}

// every material slot -> a dynamic instance of `path` with vector parameter overrides (MA_Environment
// "Albedo Controls" is brightness / saturation / contrast, "Albedo Overlay" an overlay tint, 0.5 = none).
// The MIDs are owned by the component, so they go with the actor in Clear().
void TintMat(UObject* a, UObject* m, std::initializer_list<VParam> ps) {
    static UObject* smcCls = uex::Class(L"Class /Script/Engine.StaticMeshComponent");
    if (!a || !m) return;
    auto c = uex::Comp(a, smcCls); if (!c) return;
    Params n(uex::Fn(c, L"GetNumMaterials")); n.Call(c);
    for (int i = 0, k = n.Get<int32_t>(L"ReturnValue"); i < k; i++) {
        auto f = uex::Fn(c, L"CreateDynamicMaterialInstance"); if (!f) return;
        Params p(f); p.Set<int32_t>(L"ElementIndex", i); p.Set(L"SourceMaterial", m); p.Call(c);
        auto mid = p.Get<UObject*>(L"ReturnValue"); if (!mid) continue;
        for (auto& v : ps)
            Call1(mid, L"SetVectorParameterValue", [&](Params& q) { q.Set(L"ParameterName", uex::Name(v.name)); q.Set(L"Value", v.v); });
    }
}

// sky: an inward-facing sphere (UDS sky mesh) 800 m out with deep blue-grey, no shadows, so the
// level's bright overcast never shows; height fog still tints it into a blue-black dusk
void Dome() {
    auto m = uex::LoadAsset(kDome); if (!m) { uex::Log(L"arena: no dome"); return; }
    auto b = GetBounds(m);
    double s = b.r > 1 ? 80000 / b.r : 1;
    auto a = MeshActor(m, {kC.x - b.o[0] * s, kC.y - b.o[1] * s, kC.z - b.o[2] * s}, 0, {s, s, s}, false);
    static UObject* smcCls = uex::Class(L"Class /Script/Engine.StaticMeshComponent");
    if (auto c = a ? uex::Comp(a, smcCls) : nullptr) {
        Call1(c, L"SetCastShadow", [](Params& p) { p.Set<bool>(L"NewCastShadow", false); });
        TintMat(a, uex::LoadAsset(kDomeMat), {{L"Color", {0.025f, 0.032f, 0.05f, 1.f}}});
    }
}

// reflected property writes on an object / struct instance (bools honour bitfield masks)
const uint8_t* Field(const UObject* st, const wchar_t* name) {
    for (auto c = st; c; c = At<UObject*>(c, kUStruct_Super))
        for (auto p = At<const uint8_t*>(c, kUStruct_ChildProps); p; p = At<const uint8_t*>(p, kFField_Next))
            if (NameIs(*reinterpret_cast<const FName*>(p + kFField_Name), name)) return p;
    uex::Log(L"arena: no field %s", name);
    return nullptr;
}
void PutBool(uint8_t* base, const UObject* st, const wchar_t* name, bool v) {
    auto p = Field(st, name); if (!p) return;
    uint8_t* b = base + At<int32_t>(p, kProp_Offset) + At<uint8_t>(p, kProp_Extra + 1);  // ByteOffset
    uint8_t m = At<uint8_t>(p, kProp_Extra + 3);                                          // FieldMask
    *b = v ? uint8_t(*b | m) : uint8_t(*b & ~m);
}
template <class T> void Put(uint8_t* base, const UObject* st, const wchar_t* name, const T& v) {
    auto p = Field(st, name); if (!p) return;
    if (At<int32_t>(p, kProp_ElemSize) != int(sizeof(T))) { uex::Log(L"arena: %s size %d", name, At<int32_t>(p, kProp_ElemSize)); return; }
    std::memcpy(base + At<int32_t>(p, kProp_Offset), &v, sizeof(T));
}
struct Vec4 { double x, y, z, w; };

// moonlight: a low cool directional light with dynamic (and contact) shadows, so the yard gets real
// cast shadows from the wall, pillars and fighters
void Moon() {
    static UObject* cls = uex::Class(L"Class /Script/Engine.DirectionalLight");
    static UObject* lc = uex::Class(L"Class /Script/Engine.DirectionalLightComponent");
    auto a = cls ? uex::Spawn(cls, {kC.x, kC.y, kC.z + 3000}, 0) : nullptr; if (!a) return;
    g_parts.push_back(uex::Hold(a));
    Call1(a, L"K2_SetActorRotation", [](Params& p) { p.Set(L"NewRotation", uex::Rot{-35, 40, 0}); p.Set<bool>(L"bTeleportPhysics", true); });
    auto c = uex::Comp(a, lc); if (!c) return;
    Call1(c, L"SetMobility", [](Params& p) { p.Set<uint8_t>(L"NewMobility", 2); });
    Call1(c, L"SetIntensity", [](Params& p) { p.Set<float>(L"NewIntensity", 8.f); });  // lux
    Call1(c, L"SetLightColor", [](Params& p) { p.Set(L"NewLightColor", LinColor{0.45f, 0.6f, 1.f, 1.f}); p.Set<bool>(L"bSRGB", true); });
    Call1(c, L"SetDynamicShadowDistanceMovableLight", [](Params& p) { p.Set<float>(L"NewValue", 8000.f); });
    Call1(c, L"SetDynamicShadowCascades", [](Params& p) { p.Set<int32_t>(L"NewValue", 4); });
    Put<float>(reinterpret_cast<uint8_t*>(c), c->cls, L"ContactShadowLength", 0.05f);
    Call1(c, L"SetCastShadows", [](Params& p) { p.Set<bool>(L"bNewValue", false); });  // toggle: re-create render state
    Call1(c, L"SetCastShadows", [](Params& p) { p.Set<bool>(L"bNewValue", true); });
}

// mood: unbound post-process (moonlit exposure, cool dusk grade, light vignette) and a thin blue-grey
// height fog that still softens the far backdrop
void Atmosphere() {
    if (auto cls = uex::Class(L"Class /Script/Engine.PostProcessVolume")) {
        if (auto a = uex::Spawn(cls, kC, 0)) {
            g_parts.push_back(uex::Hold(a));
            auto base = reinterpret_cast<uint8_t*>(a);
            PutBool(base, a->cls, L"bUnbound", true);
            PutBool(base, a->cls, L"bEnabled", true);
            Put<float>(base, a->cls, L"Priority", 1000.f);
            Put<float>(base, a->cls, L"BlendWeight", 1.f);
            if (auto sp = Field(a->cls, L"Settings")) {
                auto st = At<const UObject*>(sp, kProp_Extra);
                auto s = base + At<int32_t>(sp, kProp_Offset);
                PutBool(s, st, L"bOverride_AutoExposureBias", true); Put<float>(s, st, L"AutoExposureBias", -0.9f);
                // the level's skylight / GI is a bright overcast: damp it so the moon and fire do the lighting
                PutBool(s, st, L"bOverride_IndirectLightingIntensity", true); Put<float>(s, st, L"IndirectLightingIntensity", 0.35f);
                PutBool(s, st, L"bOverride_AmbientOcclusionIntensity", true); Put<float>(s, st, L"AmbientOcclusionIntensity", 1.f);
                PutBool(s, st, L"bOverride_AmbientOcclusionRadius", true); Put<float>(s, st, L"AmbientOcclusionRadius", 120.f);
                PutBool(s, st, L"bOverride_ColorSaturation", true); Put(s, st, L"ColorSaturation", Vec4{1.1, 1.1, 1.1, 1.1});
                PutBool(s, st, L"bOverride_ColorContrast", true); Put(s, st, L"ColorContrast", Vec4{1.2, 1.2, 1.2, 1.15});
                // deep toe, cool shadows, warm-neutral highlights
                PutBool(s, st, L"bOverride_ColorGammaShadows", true); Put(s, st, L"ColorGammaShadows", Vec4{0.92, 0.95, 1.05, 0.85});
                PutBool(s, st, L"bOverride_ColorGainShadows", true); Put(s, st, L"ColorGainShadows", Vec4{0.9, 0.95, 1.1, 0.9});
                PutBool(s, st, L"bOverride_ColorGainHighlights", true); Put(s, st, L"ColorGainHighlights", Vec4{1.05, 1.0, 0.95, 1.0});
                PutBool(s, st, L"bOverride_FilmToe", true); Put<float>(s, st, L"FilmToe", 0.62f);
                PutBool(s, st, L"bOverride_VignetteIntensity", true); Put<float>(s, st, L"VignetteIntensity", 0.55f);
            }
        }
    }
    static UObject* fogCls = uex::Class(L"Class /Script/Engine.ExponentialHeightFog");
    static UObject* fogComp = uex::Class(L"Class /Script/Engine.ExponentialHeightFogComponent");
    if (auto a = fogCls ? uex::Spawn(fogCls, {kC.x, kC.y, kC.z - 200}, 0) : nullptr) {
        g_parts.push_back(uex::Hold(a));
        if (auto c = uex::Comp(a, fogComp)) {
            Call1(c, L"SetFogDensity", [](Params& p) { p.Set<float>(L"Value", 0.02f); });
            Call1(c, L"SetFogHeightFalloff", [](Params& p) { p.Set<float>(L"Value", 0.25f); });
            Call1(c, L"SetFogInscatteringColor", [](Params& p) { p.Set(L"Value", LinColor{0.015f, 0.022f, 0.04f, 1.f}); });
            Call1(c, L"SetDirectionalInscatteringColor", [](Params& p) { p.Set(L"Value", LinColor{0.04f, 0.05f, 0.08f, 1.f}); });
            Call1(c, L"SetStartDistance", [](Params& p) { p.Set<float>(L"Value", 3500.f); });  // past the wall
            Call1(c, L"SetFogMaxOpacity", [](Params& p) { p.Set<float>(L"Value", 1.f); });
        }
    }
    Moon();
}

void Dress() {
    // floor: 10 x 10 flagstone tiles at their native 5 m, alternating rotation (corners go under the wall)
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            Place(kFloor, {kC.x - 2250 + i * 500, kC.y - 2250 + j * 500, kC.z + 1}, (i + j) % 2 ? 90 : 0, {1, 1, 1});
    // wall: brick panels tiled on a 36-gon whose side is the panel's own 4.4 m (x scale ~1.02 only to
    // close the corners), inner face on the collision ring's inner face; the kit's coping on top
    constexpr int kWallSegs = 36;
    double wallTop = 0, wallT = 68;
    if (auto m = uex::LoadAsset(kWall)) { auto b = GetBounds(m); wallTop = 2 * b.e[2]; wallT = 2 * b.e[1]; }
    double rc = kR + wallT / 2, side = 2 * rc * std::tan(kPi / kWallSegs) + 6;
    for (int i = 0; i < kWallSegs; i++) {
        double deg = (i + 0.5) * 360.0 / kWallSegs;
        Place(kWall, Ring(rc, deg), deg + 90, {side / 438, 1, 1});
        Place(kCoping, Ring(rc, deg, wallTop - 10), deg + 90, {side / 424, 1, 1});
    }
    // gates: a roofed memorial archway against the wall on +y / -y on a stone sill, lions in front,
    // lamps beside
    for (int s = 0; s < 2; s++) {
        double deg = s ? 270 : 90;
        Place(kArch, Ring(kR - 230, deg), deg + 90, {1, 1, 1}, true);
        for (int k = -1; k <= 1; k += 2)  // two native 3.5 m sills side by side
            Place(kStep, Ring(kR - 480, deg + k * 176 / (kR - 480) * 180 / kPi), deg + 90, {1, 1.4, 1});
        for (int k = -1; k <= 1; k += 2) {
            Place(kLion, Ring(kR - 560, deg + k * 9), deg + 180, {1.3, 1.3, 1.3}, true);
            Place(kLamp, Ring(kR - 120, deg + k * 14), deg + 180, {1.3, 1.3, 1.3});
        }
    }
    // red hall pillars on carved bases engaged in the wall on the 22.5 + 45k diagonals (uniform 0.5:
    // 5.5 m, up to the coping), each with a paper lantern hung on its face
    constexpr double kPS = 0.5;
    for (int k = 0; k < 8; k++) {
        double deg = 22.5 + 45 * k;
        Vec p = Ring(kR - 40, deg);
        MeshActor(uex::LoadAsset(kPillarBase), p, deg, {kPS, kPS, kPS}, true);
        MeshActor(uex::LoadAsset(kPillar), p, deg, {kPS, kPS, kPS}, true);
        Place(kHangLamp, Ring(kR - 95, deg, 300), deg, {1.3, 1.3, 1.3});
        Light(Ring(kR - 130, deg, 330), 40, 450, {1.f, 0.6f, 0.3f, 1.f});  // faint glow on the wall
    }
    // banners hanging on the wall face (not over the gates), never longer than the wall
    double bannerH = 0;
    if (auto m = uex::LoadAsset(kBanner)) bannerH = std::min(2 * GetBounds(m).e[2] * 0.85, wallTop - 120);
    for (int k = 0; k < 8; k++) {
        if (k == 2 || k == 6) continue;
        double deg = 45 * k;
        TintMat(Place(kBanner, Ring(kR - 18, deg, wallTop - 60 - bannerH), deg + 90, {0.85, 0.85, 0.85}), uex::LoadAsset(kDomeMat),  // crimson cloth
                {{L"Color", {0.06f, 0.002f, 0.005f, 1.f}}});
    }
    // braziers with fire, embers and a warm light, on the 45 + 90k diagonals in front of the banners
    for (int k = 0; k < 4; k++) {
        double deg = 45 + 90 * k;
        Bounds b{};
        Place(kStand, Ring(kR - 420, deg), deg, {1, 1, 1}, true, &b);
        double top = 2 * b.e[2] - 8;
        Place(kBowl, Ring(kR - 420, deg, top), deg, {1.1, 1.1, 1.1});
        Fx(kBigFire, Ring(kR - 420, deg, top + 15), 0.7);  // tall tongued flame, reads across the yard
        Fx(kEmber, Ring(kR - 420, deg, top + 40), 1.0);
        Fx(kEmber, Ring(kR - 420, deg, top + 220), 2.5); // ember column above
        Light(Ring(kR - 450, deg, top + 90), 700, 2000, {1.f, 0.55f, 0.25f, 1.f});
    }
    // war drum behind slot 0 (+x), bronze bell behind slot 1 (-x), each with a soft fill light
    Place(kDrum, Ring(kR - 330, 0), 90, {1.2, 1.2, 1.2}, true);
    Place(kBell, Ring(kR - 330, 180), 90, {0.9, 0.9, 0.9}, true);
    Light(Ring(kR - 700, 0, 400), 200, 1500);
    Light(Ring(kR - 700, 180, 400), 200, 1500);
    // outside: a band of pines and maples over the wall, cliffs rising from below, far mountains
    for (int k = 0; k < 12; k++) {
        double deg = 7.5 + 30 * k;
        if (k % 3 == 2) Place(kMaple, Ring(kR + 650, deg), deg * 3, {1.4, 1.4, 1.4});
        else Place(kPine, Ring(kR + 700, deg + 6), deg * 5, {1.5, 1.5, 1.6});
    }
    for (int k = 0; k < 6; k++) {
        double deg = 30 + 60 * k;
        Place(kCliff, Ring(6200, deg, -2200), deg + 90, {3.5, 3, 3.5});
    }
    for (int k = 0; k < 4; k++) {
        double deg = 90 * k + 15;
        Place(kFarMtn, Ring(22000, deg, -9000), deg + 90, {6, 6, 6});
    }
}
// level map: only the hidden wall ring (the level's own ground is the floor); g_parts[0] is the
// first segment. Taller than the courtyard ring and sunk below the centre: the ground is not flat.
bool BuildRing() {
    auto& m = M();
    // World Partition streams the ground in around the player: park the (frozen) pawn over the
    // centre until a trace there hits, so nobody is released before the floor has collision
    Vec hit{}; auto me = uex::Me();
    if (!me || !uex::Trace(me, {m.centre.x, m.centre.y, m.centre.z + 800}, {m.centre.x, m.centre.y, m.centre.z - 1500}, &hit)) {
        static int n;
        if (me) uex::Teleport(me, {m.centre.x, m.centre.y, m.centre.z + 200}, 0);
        if (n++ % 300 == 0) uex::Log(L"arena: map %d waiting for ground", g_map);
        return false;
    }
    double segW = 2 * (m.radius + kWallT) * std::tan(kPi / kSegs) + 20, h = 1600;
    for (int i = 0; i < kSegs; i++) {
        double a = 2 * kPi * i / kSegs, r = m.radius + kWallT / 2;
        Collider(kCube, {m.centre.x + r * std::cos(a), m.centre.y + r * std::sin(a), m.centre.z - 400 + h / 2}, a * 180 / kPi, {kWallT, segW, h});
        if (i == 0 && !PartsAlive()) { Clear(); return false; }
    }
    g_complete = true;
    uex::Log(L"arena: map %d ring of %d at %.0f %.0f %.0f r %.0f", g_map, int(g_parts.size()), m.centre.x, m.centre.y, m.centre.z, m.radius);
    return Ready();
}
}  // namespace

const Map* Maps() { return kMaps; }
int MapCount() { return kMapCount; }
int CurMap() { return g_map; }
void SetMap(int i) {
    i = i < 0 || i >= kMapCount ? 0 : i;
    if (i != g_map && !g_parts.empty()) Clear();  // a different map: rebuild
    g_map = i;
}
bool InLevel(int i) {
    auto& m = kMaps[i < 0 || i >= kMapCount ? 0 : i];
    auto w = uex::World();
    if (!m.level) return w != nullptr;
    if (!w || !w->outer) return false;
    return PathOf(w->outer) == m.level;  // the world's package
}
// the level's own cast: AI / NPC pawns, their spawners and domains, fog doors, flow (quest) actors,
// level sequences (Xigu opens with a cinematic that holds the view target).
// Matched by class name prefix; only actors of the persistent level. Our ghosts are BP_Renewal_Hun.
int ClearLevel() {
    static const wchar_t* kill[] = {L"BP_PBZMetaAI", L"BP_NPC", L"BP_AISpawner", L"BP_NPCSpawner", L"BP_ChaosSpawner",
                                    L"BP_PBZAISpawner", L"BP_MobDomain", L"BP_FogDoor", L"BP_FlowActor",
                                    L"LevelSequenceActor", L"BP_AICombatTrigger"};
    static UObject* ac = uex::Class(L"Class /Script/Engine.Actor");
    if (!ac) return 0;
    auto me = uex::Me();
    std::vector<UObject*> v;
    for (int i = 0, n = NumObjects(); i < n; i++) {
        auto o = ObjectAt(i);
        // 0x40000000: already destroyed, waiting for GC (IsActorBeingDestroyed is false again by then)
        if (!o || (o->flags & (0x30 | 0x40000000)) || o == me || !o->outer || !IsA(o, ac)) continue;
        if (NameOf(o->outer->cls) != L"Level") continue;
        auto cn = NameOf(o->cls);
        for (auto k : kill) if (cn.rfind(k, 0) == 0) { v.push_back(o); break; }
    }
    for (auto o : v) {
        if (NameOf(o->cls) == L"LevelSequenceActor") {  // stop it first: it releases the camera
            Params g(uex::Fn(o, L"GetSequencePlayer"));
            UObject* pl = nullptr;
            if (g.Size()) { g.Call(o); pl = g.Get<UObject*>(L"ReturnValue"); }
            if (pl) Call1(pl, L"Stop", [](Params&) {});
        }
        uex::Destroy(o);
    }
    if (!v.empty()) uex::Log(L"arena: cleared %d level actors", int(v.size()));
    return int(v.size());
}

bool Ready() { return g_complete && PartsAlive(); }

// parts of another world went with it (their addresses may hold new objects): only forget them
void Clear() {
    if (g_world && g_world == uex::World())
        for (auto& a : g_parts) if (auto o = uex::Get(a)) uex::Destroy(o);
    g_parts.clear(); g_world = nullptr; g_complete = false;
}

bool Build() {
    if (Ready()) return true;
    static auto retryAt = std::chrono::steady_clock::time_point{};
    if (std::chrono::steady_clock::now() < retryAt) return false;
    try {
        if (!g_parts.empty() || g_complete) Clear();  // half built, or the world changed
        if (!uex::World()) return false;
        g_world = uex::World();
        if (!M().built) return BuildRing();
        // collision first (g_parts[0] is the floor; Ready() checks it): hidden disc and wall ring
        Collider(kCyl, {kC.x, kC.y, kC.z - 50}, 0, {2 * kR + 2 * kWallT + 200, 2 * kR + 2 * kWallT + 200, 100});
        if (!PartsAlive()) { Clear(); return false; }
        double segW = 2 * (kR + kWallT) * std::tan(kPi / kSegs) + 20;
        for (int i = 0; i < kSegs; i++) {
            double a = 2 * kPi * i / kSegs, r = kR + kWallT / 2;
            Collider(kCube, {kC.x + r * std::cos(a), kC.y + r * std::sin(a), kC.z + kWallH / 2}, a * 180 / kPi, {kWallT, segW, kWallH});
        }
        Dress();
        Dome();
        Atmosphere();
        g_complete = true;
        uex::Log(L"arena: built %d actors at %.0f %.0f %.0f", int(g_parts.size()), kC.x, kC.y, kC.z);
        return Ready();
    } catch (...) {  // a half-built arena is torn down and built again a second later
        auto w = uex::ExcWhere();
        uex::Log(L"arena: build exception %s (%d parts)", w.c_str(), int(g_parts.size()));
        retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        Clear();
        return false;
    }
}

// level ground is uneven: drop a point onto it (capsule centre ~100 above the hit)
static Vec OnGround(Vec p) {
    Vec h{};
    if (!M().built && uex::Me() && uex::Trace(uex::Me(), {p.x, p.y, p.z + 800}, {p.x, p.y, p.z - 1500}, &h)) p.z = h.z + 110;
    return p;
}

Vec SpawnPoint(int slot, double* yaw) {
    // slot 0 at +x, 1 opposite, then the remaining 6 fill the ring between them
    static const double ang[8] = {0, 180, 90, 270, 45, 225, 135, 315};
    double a = ang[slot & 7] * kPi / 180;
    if (yaw) *yaw = std::fmod(ang[slot & 7] + 180, 360);
    return OnGround({M().centre.x + kSpawnR * std::cos(a), M().centre.y + kSpawnR * std::sin(a), M().centre.z + 100});
}

bool Inside(Vec p) {
    auto& m = M();
    return std::hypot(p.x - m.centre.x, p.y - m.centre.y) < m.radius + 60 && p.z > m.centre.z - kFallZ && p.z < m.centre.z + kWallH + 2000;
}

void Tick(float) {
    if (!Ready()) return;
    auto me = uex::Me(); if (!me) return;
    Vec p = uex::Loc(me);
    if (Inside(p)) return;
    // back onto the ring at the player's bearing, facing the centre
    auto& m = M();
    double a = std::atan2(p.y - m.centre.y, p.x - m.centre.x);
    Vec to = OnGround({m.centre.x + kSpawnR * std::cos(a), m.centre.y + kSpawnR * std::sin(a), m.centre.z + 100});
    double yaw = std::fmod(a * 180 / kPi + 180, 360);
    static UObject* cmcCls = nullptr;
    if (!cmcCls) cmcCls = uex::Class(L"Class /Script/Engine.CharacterMovementComponent");
    if (auto cm = uex::Comp(me, cmcCls)) Call1(cm, L"StopMovementImmediately", [](Params&) {});
    uex::Teleport(me, to, yaw);
    uex::SetControlYaw(yaw);
    uex::Log(L"arena: out at %.0f %.0f %.0f, returned", p.x, p.y, p.z);
}
}  // namespace arena

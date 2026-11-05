// shoot.cpp: hitscan bullets, damage, kills, hit reactions and the effects / sounds of a shot.
// The line trace finds walls; characters are tested against the ray directly (capsule + head sphere),
// because their trace hits come back unresolved.
#include "common.h"
#include <algorithm>
#include <map>

#define VFX L"/PBZVFXPack/VFX/BattleFX/"
static const wchar_t* kHitStone = VFX L"GuiChai/NS_GuiChaiWhite_Pisto_004_Hit_Stone.NS_GuiChaiWhite_Pisto_004_Hit_Stone";
static const wchar_t* kSpark = L"/Game/PBZContent/VFX/SceneFX/Cages/NS_Cage_OpenSpark_005.NS_Cage_OpenSpark_005";
static const wchar_t* kBlood = VFX L"Common/NS_Blood_003.NS_Blood_003";
static const wchar_t* kBloodHead = VFX L"Common/Blood_V2/NS_BloodBodyBurst_003.NS_BloodBodyBurst_003";
static const wchar_t* kBullet = VFX L"GuiChai/NS_GuiChaiBlack_Rifle_001_Bullet.NS_GuiChaiBlack_Rifle_001_Bullet";
static const wchar_t* kBulletStrong = VFX L"GuiChai/NS_GuiChaiBlack_Rifle_003_BulletStrong.NS_GuiChaiBlack_Rifle_003_BulletStrong";
static const wchar_t* kDecal = L"/Game/PBZContent/Blueprint/BeHitEffects/M_HitDecal_2.M_HitDecal_2";
constexpr double kRange = 20000;  // cm
double g_fxK[4] = {1, .5, .6, 1};  // test tuning: impact, spark, blood, tracer scale

static UObject* FxC(const wchar_t* path, ik::V at, uex::Rot rot, double scale) {
    try {
        auto lib = uex::Lib(L"NiagaraFunctionLibrary /Script/Niagara.Default__NiagaraFunctionLibrary");
        auto sys = path ? uex::LoadAsset(path) : nullptr; if (!lib || !sys) return nullptr;
        ue::Params p(uex::Fn(lib, L"SpawnSystemAtLocation"));
        p.Set(L"WorldContextObject", uex::World()); p.Set(L"SystemTemplate", sys); p.Set(L"Location", ik::ToU(at));
        p.Set(L"Rotation", rot); p.Set(L"Scale", uex::Vec{scale, scale, scale});
        p.Set<bool>(L"bAutoDestroy", true); p.Set<bool>(L"bAutoActivate", true); p.Set<uint8_t>(L"PoolingMethod", 0); p.Set<bool>(L"bPreCullCheck", false);
        p.Call(lib);
        return p.Get<UObject*>(L"ReturnValue");
    } catch (...) { uex::Log(L"sniper: fx crashed %s", path); return nullptr; }
}
void Fx(const wchar_t* path, ik::V at, uex::Rot rot, double scale) { FxC(path, at, rot, scale); }
void MuzzleFx(const Wpn& w, ik::V at, ik::V dir) { FxC(w.muzzleFx, at, ik::RotOf(dir), w.fxScale); }
// a bullet streak from the muzzle to the hit. The tracer systems loop (in the game a projectile carries
// them), so FxTick flies each one along the shot and destroys it on arrival.
struct Flying { UObject* c; ik::V a, d; double t, len; };
static std::vector<Flying> g_fly;
constexpr double kTracerSpeed = 30000;  // cm/s
static void Kill(UObject* c) {
    if (!c || !uex::Alive(c)) return;
    if (auto f = uex::Fn(c, L"DeactivateImmediate")) { ue::Params q(f); q.Call(c); }
    if (auto f = uex::Fn(c, L"K2_DestroyComponent")) { ue::Params q(f); q.Set(L"Object", c); q.Call(c); }
}
void Tracer(ik::V a, ik::V b, bool strong) {
    ik::V d = b - a; double len = ik::Len(d); if (len < 50) return;
    auto c = FxC(strong ? kBulletStrong : kBullet, a, ik::RotOf(d), g_fxK[3]); if (!c) return;
    g_fly.push_back({c, a, d * (1 / len), 0, len});
    if (g_fly.size() > 64) { Kill(g_fly.front().c); g_fly.erase(g_fly.begin()); }
}
void FxTick(double dt) {
    for (size_t i = 0; i < g_fly.size();) {
        auto& f = g_fly[i]; f.t += dt;
        double s = f.t * kTracerSpeed;
        if (s >= f.len || !uex::Alive(f.c)) { Kill(f.c); g_fly.erase(g_fly.begin() + i); continue; }
        ue::Params q(uex::Fn(f.c, L"K2_SetWorldLocation"));
        q.Set(L"NewLocation", ik::ToU(f.a + f.d * s)); q.Set<bool>(L"bSweep", false); q.Set<bool>(L"bTeleport", true); q.Call(f.c);
        ++i;
    }
}
void FxClear() { for (auto& f : g_fly) Kill(f.c); g_fly.clear(); }
static void Decal(ik::V at, ik::V normal) {
    auto gs = uex::GameplayStatics(); auto mat = uex::LoadAsset(kDecal); if (!gs || !mat) return;
    ue::Params p(uex::Fn(gs, L"SpawnDecalAtLocation"));
    p.Set(L"WorldContextObject", uex::World()); p.Set(L"DecalMaterial", mat); p.Set(L"DecalSize", uex::Vec{4, 3.5, 3.5});
    p.Set(L"Location", ik::ToU(at)); p.Set(L"Rotation", ik::RotOf(normal * -1)); p.Set<float>(L"LifeSpan", 20.f); p.Call(gs);
}
void ImpactFx(ik::V at, ik::V normal, ik::V dir) {
    uex::Rot r = ik::RotOf(normal);
    FxC(kHitStone, at, r, g_fxK[0]);
    if (Rand() < .5) FxC(kSpark, at, r, g_fxK[1]);
    Decal(at, normal);
    static const wchar_t* snd[] = {L"concrete_impact_bullet1", L"concrete_impact_bullet2", L"concrete_impact_bullet3"};
    audio::PlayAt(snd[int(Rand() * 3) % 3], at, .8f);
    (void)dir;
}

// hit reaction: the monster's own hit montage, picked by its type token (BP_Monster_<Type>_...) and by
// whether the shot came from its front or back
void PlayHitReact(UObject* victim, ik::V dir) {
    static std::map<std::wstring, std::pair<UObject*, UObject*>> cache;
    auto nm = ue::NameOf(victim->cls);
    size_t a = nm.find(L"Monster_"); if (a == std::wstring::npos) return;
    a += 8; size_t b = nm.find(L'_', a); auto type = nm.substr(a, b - a);
    auto it = cache.find(type);
    if (it == cache.end()) {
        auto mcls = uex::Class(L"Class /Script/Engine.AnimMontage");
        std::pair<UObject*, UObject*> m{}; int best[2] = {-1, -1};
        for (int k = 0, n = ue::NumObjects(); k < n; ++k) {
            auto o = ue::ObjectAt(k); if (!o || o->cls != mcls) continue;
            auto mn = ue::NameOf(o);
            if (mn.rfind(type + L"_", 0) != 0 || mn.find(L"_Hit_") == std::wstring::npos) continue;
            bool mid = mn.find(L"_Middle_") != std::wstring::npos, lit = mn.find(L"_Light_") != std::wstring::npos;
            if (!mid && !lit) continue;
            for (int d = 0; d < 2; ++d) {
                if (mn.find(d ? L"_B_" : L"_F_") == std::wstring::npos) continue;
                int sc = mid * 4 + (mn.find(L"_M_") != std::wstring::npos) * 2 + (mn.rfind(type + L"_Hit_", 0) == 0);
                if (sc > best[d]) { best[d] = sc; (d ? m.second : m.first) = o; }
            }
        }
        it = cache.emplace(type, m).first;
        uex::Log(L"sniper: %s hit montages F %s B %s", type.c_str(), m.first ? ue::NameOf(m.first).c_str() : L"-", m.second ? ue::NameOf(m.second).c_str() : L"-");
    }
    ue::Params f(uex::Fn(victim, L"GetActorForwardVector")); f.Call(victim); auto v = f.Get<uex::Vec>(L"ReturnValue");
    bool back = v.x * dir.x + v.y * dir.y > 0;  // the shot travels along its facing: from behind
    UObject* m = back && it->second.second ? it->second.second : it->second.first; if (!m) return;
    auto ai = AnimOf(victim); if (!ai) return;
    ue::Params p(uex::Fn(ai, L"Montage_Play"));
    p.Set(L"MontageToPlay", m); p.Set<float>(L"InPlayRate", 1.f); p.Set<uint8_t>(L"ReturnValueType", 0);
    p.Set<float>(L"InTimeToStartMontageAt", 0.f); p.Set<bool>(L"bStopAllMontages", true); p.Call(ai);
}
// ---- death ----
// the monster's death montage (<Type>[_<Sub>]_Dead_Normal_F/B_...), front or back by the shot, same
// lookup as the hit reaction; the one of its own weapon kind (BP_Monster_<Type>_<Sub>_...) first
static UObject* DeathMontage(UObject* victim, bool back) {
    static std::map<std::wstring, std::pair<UObject*, UObject*>> cache;
    auto nm = ue::NameOf(victim->cls);
    size_t a = nm.find(L"Monster_"); if (a == std::wstring::npos) return nullptr;
    a += 8; size_t b = nm.find(L'_', a); auto type = nm.substr(a, b - a);
    std::wstring sub;
    if (b != std::wstring::npos) { size_t c = nm.find(L'_', b + 1); sub = nm.substr(b + 1, c == std::wstring::npos ? std::wstring::npos : c - b - 1); }
    auto key = type + L"_" + sub;
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto mcls = uex::Class(L"Class /Script/Engine.AnimMontage");
        std::pair<UObject*, UObject*> m{}; int best[2] = {-1, -1};
        for (int k = 0, n = ue::NumObjects(); mcls && k < n; ++k) {
            auto o = ue::ObjectAt(k); if (!o || o->cls != mcls) continue;
            auto mn = ue::NameOf(o);
            if (mn.rfind(type + L"_", 0) != 0 || mn.find(L"_Dead_") == std::wstring::npos) continue;
            for (int d = 0; d < 2; ++d) {
                if (mn.find(d ? L"_B_" : L"_F_") == std::wstring::npos) continue;
                int sc = (mn.find(L"_Normal_") != std::wstring::npos) * 4 + (!sub.empty() && mn.rfind(key + L"_", 0) == 0) * 2
                       + (mn.find(L"Launch") == std::wstring::npos);
                if (sc > best[d]) { best[d] = sc; (d ? m.second : m.first) = o; }
            }
        }
        it = cache.emplace(key, m).first;
        uex::Log(L"sniper: %s death montages F %s B %s", key.c_str(), m.first ? ue::NameOf(m.first).c_str() : L"-",
                 m.second ? ue::NameOf(m.second).c_str() : L"-");
    }
    return back && it->second.second ? it->second.second : it->second.first ? it->second.first : it->second.second;
}
static float MontLen(UObject* m) { ue::Params q(uex::Fn(m, L"GetPlayLength")); q.Call(m); return q.Get<float>(L"ReturnValue"); }
static UObject* CurMont(UObject* ai) { ue::Params q(uex::Fn(ai, L"GetCurrentActiveMontage")); q.Call(ai); return q.Get<UObject*>(L"ReturnValue"); }
static float MontPos(UObject* ai, UObject* m) { ue::Params q(uex::Fn(ai, L"Montage_GetPosition")); q.Set(L"Montage", m); q.Call(ai); return q.Get<float>(L"ReturnValue"); }
static void MontRate(UObject* ai, UObject* m, float r) { ue::Params q(uex::Fn(ai, L"Montage_SetPlayRate")); q.Set(L"Montage", m); q.Set<float>(L"NewPlayRate", r); q.Call(ai); }
static float MontPlay(UObject* ai, UObject* m, float at) {
    ue::Params p(uex::Fn(ai, L"Montage_Play"));
    p.Set(L"MontageToPlay", m); p.Set<float>(L"InPlayRate", 1.f); p.Set<uint8_t>(L"ReturnValueType", 0);
    p.Set<float>(L"InTimeToStartMontageAt", at); p.Set<bool>(L"bStopAllMontages", true); p.Call(ai);
    return p.Get<float>(L"ReturnValue");
}
// bodies we play a death montage on: held on its last frames (its blend-out would stand them up)
struct Corpse { uex::Ref who; UObject* mont; float hold; double until; };
static std::vector<Corpse> g_corpses;
constexpr float kDeathHold = .25f;  // s before the montage's end
constexpr double kCorpseKeep = 30;  // s we keep holding (the game removes the body by then)
static bool PlayDeath(UObject* victim, ik::V dir) {
    ue::Params f(uex::Fn(victim, L"GetActorForwardVector")); f.Call(victim); auto v = f.Get<uex::Vec>(L"ReturnValue");
    auto m = DeathMontage(victim, v.x * dir.x + v.y * dir.y > 0); auto ai = AnimOf(victim);
    if (!m || !ai) return false;
    float len = MontPlay(ai, m, 0.f);
    if (len <= 0) return false;
    g_corpses.push_back({uex::Hold(victim), m, std::max(0.f, MontLen(m) - kDeathHold), g_now + kCorpseKeep});
    if (g_dbg) uex::Log(L"sniper: death %s plays %s (%.2f s)", ue::NameOf(victim).c_str(), ue::NameOf(m).c_str(), len);
    return true;
}
void DeathTick() {
    for (size_t i = 0; i < g_corpses.size();) {
        auto& c = g_corpses[i]; auto o = uex::Get(c.who); auto ai = o ? AnimOf(o) : nullptr;
        if (!ai || g_now > c.until) { g_corpses.erase(g_corpses.begin() + i); continue; }
        if (CurMont(ai) != c.mont) { MontPlay(ai, c.mont, c.hold); MontRate(ai, c.mont, 0.f); }  // something else started
        else if (MontPos(ai, c.mont) >= c.hold) MontRate(ai, c.mont, 0.f);
        ++i;
    }
}
// an attribute through the attribute system (its change callbacks run, unlike a direct write): the
// game's UI reacts (the health / posture bar)
static bool GasAdd(UObject* victim, const wchar_t* name, float delta) {
    static UObject *ulib, *blib, *setCls;
    if (!ulib) ulib = uex::Lib(L"AttributeUtils /Script/SGameAttributes.Default__AttributeUtils");
    if (!blib) blib = uex::Lib(L"PBZBlueprintFunctionLibrary /Script/ProjectPBZ.Default__PBZBlueprintFunctionLibrary");
    if (!setCls) setCls = uex::Class(L"Class /Script/CharacterBasics.BasicAttributeSet");
    if (!ulib || !blib || !setCls) { uex::Log(L"sniper: attribute libs missing %p %p %p", ulib, blib, setCls); return false; }
    struct GA { uint8_t b[0x38]; };  // FGameplayAttribute, opaque; only its fields below are read
    static uex::Prop kAttrField{L"ScriptStruct /Script/GameplayAbilities.GameplayAttribute", L"Attribute", 0x10},
        kAttrOwner{L"ScriptStruct /Script/GameplayAbilities.GameplayAttribute", L"AttributeOwner", 0x30};
    static bool sized = [] {
        int n = uex::StructSize(L"ScriptStruct /Script/GameplayAbilities.GameplayAttribute", int(sizeof(GA)));
        if (n != int(sizeof(GA))) uex::Log(L"sniper: GameplayAttribute is 0x%x bytes now (was 0x%x), attribute writes off", n, int(sizeof(GA)));
        return n == int(sizeof(GA));
    }();
    if (!sized) return false;
    static std::map<std::wstring, GA> attrs;  // FGameplayAttribute per name, made once
    auto it = attrs.find(name);
    if (it == attrs.end()) {
        int32_t n = int32_t(wcslen(name)) + 1;
        struct FS { const wchar_t* d; int32_t n, m; } nm{name, n, n};
        ue::Params q(uex::Fn(blib, L"MakeAttribute")); q.Set(L"AttributeName", nm); q.Set(L"Set", setCls); q.Call(blib);
        GA a = q.Get<GA>(L"ReturnValue");
        if (!kAttrOwner.Of<void*>(a.b) && !kAttrField.Of<void*>(a.b)) { uex::Log(L"sniper: MakeAttribute %s failed", name); return false; }
        it = attrs.emplace(name, a).first;
    }
    ue::Params p(uex::Fn(ulib, L"ModifyCharacterAttributeValue"));
    p.Set(L"Character", victim); p.Set(L"Attribute", it->second); p.Set<uint8_t>(L"ModiferOp", 0); p.Set<float>(L"Value", delta); p.Call(ulib);
    return true;
}
static bool GasAddHealth(UObject* victim, float delta) { return GasAdd(victim, L"Health", delta); }
static bool ApplyKillEffect(UObject* victim) {
    auto ge = uex::LoadAsset(L"/Game/PBZContent/Blueprint/Ability/GameplayEffects/GE_Kill.GE_Kill_C");
    auto asc = uex::ASC(victim); if (!ge || !asc) return false;
    ue::Params p(uex::Fn(asc, L"BP_ApplyGameplayEffectToSelf")); p.Set(L"GameplayEffectClass", ge); p.Set<float>(L"Level", 1.f); p.Call(asc);
    return true;
}
static bool NpcKill(UObject* victim, uint8_t reason) {
    auto f = uex::Fn(victim, L"Kill"); if (!f) return false;
    ue::Params k(f); k.Set<uint8_t>(L"DieReason", reason); k.Call(victim); return true;
}
static bool IsDeadAI(UObject* victim) {
    auto lib = uex::Lib(L"SGameAIStateUtils /Script/SGameAI.Default__SGameAIStateUtils"); if (!lib) return false;
    ue::Params q(uex::Fn(lib, L"IsDead")); q.Set(L"Pawn", victim); q.Call(lib); return q.Get<bool>(L"ReturnValue");
}
// the NPC dies the game's way: Health through the attribute system (dead for the AI, loot, the red
// dissolve, removal), right after our death montage starts so it falls as it fades. If the game doesn't
// take it: Health 0 and Kill (the AI stops, but no dissolve)
static bool Die(UObject* victim, float hp, ik::V dir) {
    try { PlayDeath(victim, dir); } catch (...) { uex::Log(L"sniper: death montage crashed (%s)", ue::NameOf(victim).c_str()); }
    if (GasAddHealth(victim, -hp - 1) && IsDeadAI(victim)) return true;
    uex::Log(L"sniper: %s: attribute death not taken, Kill instead", ue::NameOf(victim).c_str());
    uex::SetAttr(victim, uex::attr::Health, 0);
    return NpcKill(victim, 1);
}
void KillTest(UObject* victim, int mode, ik::V dir) {
    float hp = uex::Attr(victim, uex::attr::Health); bool ok = false;
    try {
        if (mode == 0) ok = Die(victim, hp, dir);
        else if (mode == 1) { uex::SetAttr(victim, uex::attr::Health, 0); ok = NpcKill(victim, 1); }
        else if (mode == 2) ok = GasAddHealth(victim, -hp - 1);
        else if (mode == 3) ok = ApplyKillEffect(victim);
        else if (mode == 4) { uex::SetAttr(victim, uex::attr::Health, 0); ok = PlayDeath(victim, dir); NpcKill(victim, 1); }
        else if (mode == 5) ok = PlayDeath(victim, dir);
        else if (mode == 6) { ok = PlayDeath(victim, dir); GasAddHealth(victim, -hp - 1); }
        else if (mode == 7) { GasAddHealth(victim, -hp - 1); ok = PlayDeath(victim, dir); }
    } catch (...) { uex::Log(L"sniper: kill %d crashed", mode); }
    auto ai = AnimOf(victim); auto m = ai ? CurMont(ai) : nullptr;
    uex::Log(L"sniper: kill %d %s ok %d hp %.0f -> %.0f dead %d mont %s", mode, ue::NameOf(victim).c_str(), ok, hp,
             uex::Attr(victim, uex::attr::Health), IsDeadAI(victim), m ? ue::NameOf(m).c_str() : L"-");
}

// the AI perceives the damage and turns on the player
void Aggro(UObject* victim, UObject* me, float dmg, ik::V at) {
    try {
        auto lib = uex::Lib(L"AISense_Damage /Script/AIModule.Default__AISense_Damage"); if (!lib) return;
        ue::Params p(uex::Fn(lib, L"ReportDamageEvent"));
        p.Set(L"WorldContextObject", me); p.Set(L"DamagedActor", victim); p.Set(L"Instigator", me);
        p.Set<float>(L"DamageAmount", dmg); p.Set(L"EventLocation", uex::Loc(me)); p.Set(L"HitLocation", ik::ToU(at)); p.Call(lib);
    } catch (...) { uex::Log(L"sniper: aggro crashed"); }
}
// Health is clamped at Health_Min (above zero for the ones that can't die); at zero an NPC dies (Die).
// Returns the damage done; *killed when it died.
static int Damage(UObject* victim, float dmg, ik::V dir, bool* killed) {
    float hp = uex::Attr(victim, uex::attr::Health), mn = uex::Attr(victim, uex::attr::HealthMin);
    float nh = std::max(hp - dmg, mn);
    *killed = false;
    static UObject* npcCls; if (!npcCls) npcCls = uex::Class(L"Class /Script/ProjectPBZ.PBZCharacter_NPC");
    if (nh <= .5f && npcCls && ue::IsA(victim, npcCls)) { *killed = Die(victim, hp, dir); nh = 0; }
    else {
        uex::SetAttr(victim, uex::attr::Health, nh);
        if (nh > .5f) try { PlayHitReact(victim, dir); } catch (...) { uex::Log(L"sniper: hit react crashed (%s)", ue::NameOf(victim).c_str()); }
    }
    if (g_dbg) uex::Log(L"sniper: hit %s hp %.0f -> %.0f%s", ue::NameOf(victim).c_str(), hp, nh, *killed ? L", killed" : L"");
    return int(hp - nh + .5f);
}

// posture (杀气, Shachi): full at Shachi_Max, a hit lowers it towards Shachi_Min (the bar empties). The
// drop is a fraction of the victim's Shachi_Max, so bosses and thugs react alike.
// Writing Shachi does not break by itself: the game breaks a character (HitreactComponent hit state
// CharacterState.HitReact.State.Break.Normal, tag CharacterState.Stand.Break, the NBreak montage) when a hit
// with AngerDamage arrives through its damage path while Shachi is at the minimum. So the shot that empties the
// bar also applies such a hit effect (GE_GrabHit) from the player's ASC; the Health that effect would take is
// held by Health_Min and put back, the shot already did its damage.
constexpr float kPostureHead = 1.5f;                  // posture multiplier on a headshot
static UObject* HitreactOf(UObject* ch) {
    static UObject* cls; if (!cls) cls = uex::Class(L"Class /Script/SGameCombatSystem.HitreactComponent");
    return cls ? uex::Comp(ch, cls) : nullptr;
}
static std::wstring HitState(UObject* ch) {
    auto h = HitreactOf(ch); if (!h) return L"-";
    ue::Params q(uex::Fn(h, L"GetCurrentHitState")); q.Call(h); return ue::NameOf(q.Get<ue::FName>(L"ReturnValue"));
}
static bool InBreak(UObject* ch) { return HitState(ch).find(L"Break") != std::wstring::npos; }
static void PostureBreak(UObject* victim, UObject* me) {
    static UObject* ge; if (!ge) ge = uex::LoadAsset(L"/PBZCombatAsset/Prototypes/GameplayEffects/Inflictions/GE_GrabHit.GE_GrabHit_C");
    auto src = uex::ASC(me), dst = uex::ASC(victim);
    if (!ge || !src || !dst) { uex::Log(L"sniper: posture break: missing %d %d %d", !!ge, !!src, !!dst); return; }
    float hp = uex::Attr(victim, uex::attr::Health), hmin = uex::Attr(victim, uex::attr::HealthMin);
    uex::SetAttr(victim, uex::attr::HealthMin, hp);  // the effect's own damage can't go below what the shot left
    ue::Params p(uex::Fn(src, L"BP_ApplyGameplayEffectToTarget"));  // empty context: the source ASC makes one
    p.Set(L"GameplayEffectClass", ge); p.Set(L"Target", dst); p.Set<float>(L"Level", 1.f); p.Call(src);
    uex::SetAttr(victim, uex::attr::HealthMin, hmin);
    float after = uex::Attr(victim, uex::attr::Health);
    if (after < hp - .5f && !GasAdd(victim, L"Health", hp - after)) uex::SetAttr(victim, uex::attr::Health, hp);
    bool viaGe = InBreak(victim);
    if (!viaGe) {  // the hit didn't take (an endure / a resisting boss): set the break state directly
        auto h = HitreactOf(victim); auto f = h ? uex::Fn(h, L"ChangeHitState") : nullptr;
        if (f) { ue::Params c(f); c.Set(L"NewHitState", uex::Name(L"CharacterState.HitReact.State.Break.Normal")); c.Set<bool>(L"EndStrengthAnimTask", true); c.Call(h); }
    }
    auto ai = AnimOf(victim); auto m = ai ? CurMont(ai) : nullptr;
    if (g_dbg) uex::Log(L"sniper: posture break %s state %s%s mont %s hp %.0f -> %.0f -> %.0f", ue::NameOf(victim).c_str(), HitState(victim).c_str(),
                        viaGe ? L"" : L" (direct)", m ? ue::NameOf(m).c_str() : L"-", hp, after, uex::Attr(victim, uex::attr::Health));
}
static void Posture(UObject* victim, UObject* me, float frac) {
    float smx = uex::Attr(victim, uex::attr::ShachiMax); if (smx <= 0 || frac <= 0) return;
    float sc = uex::Attr(victim, uex::attr::Shachi), smin = uex::Attr(victim, uex::attr::ShachiMin);
    float want = std::max(smin, sc - frac * smx);
    if (want >= sc) return;  // already at the minimum
    bool gas = false;
    try { gas = GasAdd(victim, L"Shachi", want - sc); } catch (...) { uex::Log(L"sniper: posture crashed (%s)", ue::NameOf(victim).c_str()); }
    float got = uex::Attr(victim, uex::attr::Shachi);
    if (!gas || got > sc - .5f * (sc - want)) { uex::SetAttr(victim, uex::attr::Shachi, want); got = want; }  // not taken: write it
    if (g_dbg) uex::Log(L"sniper: posture %s %.0f -> %.0f (min %.0f max %.0f)%s", ue::NameOf(victim).c_str(), sc, got, smin, smx, gas ? L"" : L", direct");
    if (got <= smin + .5f && !InBreak(victim))
        try { PostureBreak(victim, me); } catch (...) { uex::Log(L"sniper: posture break crashed (%s)", ue::NameOf(victim).c_str()); }
}
// test: the hit state, montage and break-related tags of a character
void BreakInfo(UObject* victim) {
    if (!victim) { uex::Log(L"sniper: break: no enemy"); return; }
    auto tc = uex::OwnedTags(victim);
    std::wstring tags;
    for (int i = 0; i < tc.n && i < 200; ++i) { auto t = ue::NameOf(tc.d[i]); if (t.find(L"Break") != std::wstring::npos || t.find(L"Hitreact") != std::wstring::npos || t.find(L"HitReact") != std::wstring::npos) tags += t + L" "; }
    auto ai = AnimOf(victim); auto m = ai ? CurMont(ai) : nullptr;
    uex::Log(L"sniper: break %s state %s mont %s shachi %.0f tags %s", ue::NameOf(victim).c_str(), HitState(victim).c_str(),
             m ? ue::NameOf(m).c_str() : L"-", uex::Attr(victim, uex::attr::Shachi), tags.c_str());
}
void PostureInfo(UObject* victim) {
    if (!victim) { uex::Log(L"sniper: posture: no enemy"); return; }
    uex::Log(L"sniper: posture %s hp %.0f/%.0f (min %.0f) shachi %.0f min %.0f max %.0f regen %.1f cooldown %.1f", ue::NameOf(victim).c_str(),
             uex::Attr(victim, uex::attr::Health), uex::Attr(victim, uex::attr::HealthMax), uex::Attr(victim, uex::attr::HealthMin),
             uex::Attr(victim, uex::attr::Shachi), uex::Attr(victim, uex::attr::ShachiMin), uex::Attr(victim, uex::attr::ShachiMax),
             uex::Attr(victim, uex::attr::ShachiRegen), uex::Attr(victim, uex::attr::ShachiRegenCooldown));
}

// ---- characters near the player (refreshed every ~0.5 s, the object array is large) ----
static std::vector<UObject*> g_chars;
static double g_charsAt = -9;
static UObject* g_charsMe;
static bool LiveChar(UObject* o) {
    return o && uex::Alive(o) && !(o->flags & 0x30) && uex::AttrSet(o) && uex::Attr(o, uex::attr::Health) > 0;
}
void CharsRefresh(UObject* me) {
    if (g_now - g_charsAt < .5 && g_charsMe == me) return;
    g_charsAt = g_now; g_charsMe = me; g_chars.clear();
    static UObject* chCls; if (!chCls) chCls = uex::Class(L"Class /Script/ProjectPBZ.PBZCharacter");
    if (!chCls) return;
    ik::V p = ik::FromU(uex::Loc(me));
    for (int k = 0, n = ue::NumObjects(); k < n; ++k) {
        auto o = ue::ObjectAt(k);
        if (!o || o == me || (o->flags & 0x30) || !ue::IsA(o, chCls) || !LiveChar(o)) continue;
        if (ik::Len(ik::FromU(uex::Loc(o)) - p) < kRange) g_chars.push_back(o);
    }
}
UObject* NearestEnemy(UObject* me) {
    static UObject* npcCls; if (!npcCls) npcCls = uex::Class(L"Class /Script/ProjectPBZ.PBZCharacter_NPC");
    g_charsAt = -9; CharsRefresh(me);
    ik::V p = ik::FromU(uex::Loc(me)); UObject* best = nullptr; double bd = 1e18;
    for (auto o : g_chars) {
        if (!npcCls || !ue::IsA(o, npcCls)) continue;
        double d = ik::Len(ik::FromU(uex::Loc(o)) - p); if (d < bd) { bd = d; best = o; }
    }
    return best;
}

// ---- the bullet ----
struct HB { uint8_t b[0x200]; };  // room for an FHitResult (0x100 in this build, size read at run time)
static int HitSize() { static int n = (uex::StructSize(L"ScriptStruct /Script/Engine.HitResult", 0x100) + 7) & ~7; return n; }  // array stride
static bool Invisible(UObject* comp, UObject* actor) {  // collision-only geometry (AI walls, volumes): bullets pass
    bool vis = true;
    if (auto f = uex::Fn(comp, L"IsVisible")) { ue::Params p(f); p.Call(comp); vis = p.Get<bool>(L"ReturnValue"); }
    static uex::Prop kHiddenInGame{L"Class /Script/Engine.SceneComponent", L"bHiddenInGame", 0x1a9, 0x8};
    static uex::Prop kActorHidden{L"Class /Script/Engine.Actor", L"bHidden", 0x58, 0x80};
    return !vis || kHiddenInGame.Bit(comp) || kActorHidden.Bit(actor);
}
bool HeadPos(UObject* victim, ik::V* out) {
    auto m = MeshOf(victim); if (!m) return false;
    for (auto bn : {L"head", L"Head", L"Bip001-Head", L"Bip001 Head", L"neck_01"}) {
        ue::Params q(uex::Fn(m, L"GetBoneIndex")); q.Set(L"BoneName", uex::Name(bn)); q.Call(m);
        if (q.Get<int32_t>(L"ReturnValue") < 0) continue;
        *out = Sock(m, bn); return true;
    }
    return false;
}
// distance along the ray to the closest approach to point p, and how far off the ray p is
static double RayPt(ik::V eye, ik::V dir, ik::V p, double* off) {
    double t = ik::Dot(p - eye, dir); *off = ik::Len(eye + dir * t - p); return t;
}
double g_headR = 15, g_bodyMargin = 6;  // cm: head sphere radius (around head bone + 5 up), capsule margin

ShotRes Shoot(UObject* me, const Wpn& w, ik::V eye, ik::V dir, ik::V muzzle, bool tracer) {
    ShotRes res{-1, 0, nullptr, eye + dir * kRange};
    auto lib = uex::SystemLib(); auto gs = uex::GameplayStatics(); if (!lib || !gs) return res;
    // start level with the player (a third-person camera has walls and foliage behind the player)
    double t0 = std::max(0.0, ik::Dot(ik::FromU(uex::Loc(me)) - eye, dir) - 40);
    ik::V start = eye + dir * t0, end = eye + dir * kRange;
    uint8_t types[3] = {0, 1, 2};  // WorldStatic, WorldDynamic, Pawn
    TA arr{types, 3, 3}, ignore{&me, 1, 1};
    ue::Params t(uex::Fn(lib, L"LineTraceMultiForObjects"));
    t.Set(L"WorldContextObject", me); t.Set(L"Start", ik::ToU(start)); t.Set(L"End", ik::ToU(end));
    t.Set(L"ObjectTypes", arr); t.Set<bool>(L"bTraceComplex", false); t.Set(L"ActorsToIgnore", ignore);
    t.Set<uint8_t>(L"DrawDebugType", 0); t.Set<bool>(L"bIgnoreSelf", true); t.Set<float>(L"DrawTime", 0.f);
    t.Call(lib);
    auto hits = t.Get<TA>(L"OutHits");
    static UObject* chCls = uex::Class(L"Class /Script/ProjectPBZ.PBZCharacter");
    static UObject* brushCls = uex::Class(L"Class /Script/Engine.BrushComponent");
    double wall = kRange; ik::V wallAt = end, wallN{0, 0, 1}; bool hitWall = false;
    const int hs = HitSize();
    if (hs <= 0 || hs > int(sizeof(HB))) { uex::Log(L"sniper: HitResult is 0x%x bytes, shots off", hs); return res; }
    for (int i = 0; i < hits.n && hits.d; ++i) {
        HB hb{}; memcpy(hb.b, static_cast<const uint8_t*>(hits.d) + i * hs, hs);
        ue::Params b(uex::Fn(gs, L"BreakHitResult")); b.Set(L"Hit", hb); b.Call(gs);
        auto comp = b.Get<UObject*>(L"HitComponent"); auto a = b.Get<UObject*>(L"HitActor"); if (!a) a = OwnerOf(comp);
        if (!comp || (brushCls && ue::IsA(comp, brushCls)) || !a || a == me || Invisible(comp, a)) continue;
        if (chCls && ue::IsA(a, chCls)) continue;        // characters: the ray test below
        if (OwnerOf(a) == me) continue;                  // our own (hidden) weapons
        wallAt = ik::FromU(b.Get<uex::Vec>(L"ImpactPoint")); wallN = ik::FromU(b.Get<uex::Vec>(L"ImpactNormal"));
        wall = ik::Dot(wallAt - eye, dir); hitWall = true;
        if (g_dbg) uex::Log(L"sniper: wall %s/%s at %.0fcm", ue::NameOf(a).c_str(), ue::NameOf(comp).c_str(), wall);
        break;
    }
    // characters: capsule (radius + margin, segment between the hemisphere centres) and the head sphere
    CharsRefresh(me);
    UObject* victim = nullptr; double best = wall; bool head = false;
    for (auto o : g_chars) {
        if (!LiveChar(o)) continue;
        ik::V c = ik::FromU(uex::Loc(o)); double rad = 40, hh = 90;
        static uex::Prop kCapsule{L"Class /Script/Engine.Character", L"CapsuleComponent", 0x340};
        if (auto cap = kCapsule.Of<UObject*>(o)) {
            { ue::Params q(uex::Fn(cap, L"GetScaledCapsuleRadius")); q.Call(cap); rad = q.Get<float>(L"ReturnValue"); }
            { ue::Params q(uex::Fn(cap, L"GetScaledCapsuleHalfHeight")); q.Call(cap); hh = q.Get<float>(L"ReturnValue"); }
        }
        ik::V hp; bool hasHead = HeadPos(o, &hp);
        if (hasHead) {
            double off, th = RayPt(eye, dir, hp + ik::V{0, 0, 5}, &off);
            if (th > t0 && th < best && off < g_headR) { best = th; victim = o; head = true; continue; }
        }
        double seg = std::max(0.0, hh - rad), r = rad * .8 + g_bodyMargin, tb = -1;
        for (int k = 0; k <= 16; ++k) {  // sample the axis: closest approach to the ray
            ik::V p = c + ik::V{0, 0, -seg + 2 * seg * k / 16};
            if (hasHead && p.z > hp.z) break;  // above the neck only the head counts
            double off, tt = RayPt(eye, dir, p, &off);
            if (off < r && tt > t0 && (tb < 0 || tt < tb)) tb = tt;
        }
        if (tb > 0 && tb < best) { best = tb; victim = o; head = false; }
    }
    if (!victim) {
        if (g_dbg) for (auto o : g_chars) {  // why nobody was hit: how far the ray passes each character
            double off, tt = RayPt(eye, dir, ik::FromU(uex::Loc(o)), &off);
            if (off < 300) uex::Log(L"sniper: miss %s at %.0fm, %.0fcm off the ray (wall %.0fm, live %d)", ue::NameOf(o).c_str(), tt / 100, off, wall / 100, LiveChar(o));
        }
        res.end = hitWall ? wallAt : end;
        if (hitWall) ImpactFx(wallAt, wallN, dir);
        if (tracer) Tracer(muzzle, res.end, !w.autoFire);
        return res;
    }
    ik::V at = eye + dir * best; res.end = at; res.victim = victim;
    double units = best / 2.54;
    float dmg = float(w.damage * kDmgScale * pow(w.rangeMod, units / 500) * (head ? w.head : 1));
    bool killed = false;
    Aggro(victim, me, dmg, at);
    res.dmg = Damage(victim, dmg, dir, &killed);
    if (!killed) Posture(victim, me, w.posture * (head ? kPostureHead : 1));
    res.kind = killed ? (head ? 3 : 2) : (head ? 1 : 0);
    Fx(head ? kBloodHead : kBlood, at, ik::RotOf(dir * -1), g_fxK[2]);
    static const wchar_t* flesh[] = {L"flesh_impact_bullet1", L"flesh_impact_bullet2", L"flesh_impact_bullet3"};
    audio::PlayAt(flesh[int(Rand() * 3) % 3], at, 1);
    if (head) audio::Play(L"headshot_noarmor_01", .9f);
    if (killed) { static const wchar_t* death[] = {L"death1", L"death2", L"death3", L"death4"}; audio::PlayAt(death[int(Rand() * 4) % 4], at, .9f); }
    if (tracer) Tracer(muzzle, at, !w.autoFire);
    if (g_dbg) uex::Log(L"sniper: %s %s %.0fm dmg %d kind %d", w.id, ue::NameOf(victim).c_str(), best / 100, res.dmg, res.kind);
    return res;
}
// test: Niagara components alive within 30 m of the player
void FxList(UObject* me, bool killTracers) {
    static UObject* nc = uex::Class(L"Class /Script/Niagara.NiagaraComponent");
    if (!nc || !me) return;
    ik::V pl = ik::FromU(uex::Loc(me)); int n = 0;
    for (int k = 0, N = ue::NumObjects(); k < N; ++k) {
        auto o = ue::ObjectAt(k); if (!o || !ue::IsA(o, nc) || (o->flags & 0x30)) continue;
        ik::V at; { ue::Params q(uex::Fn(o, L"K2_GetComponentLocation")); q.Call(o); at = ik::FromU(q.Get<uex::Vec>(L"ReturnValue")); }
        double d = ik::Len(at - pl); if (d > 3000) continue;
        UObject* as = nullptr; { ue::Params q(uex::Fn(o, L"GetAsset")); q.Call(o); as = q.Get<UObject*>(L"ReturnValue"); }
        bool act = false; { ue::Params q(uex::Fn(o, L"IsActive")); q.Call(o); act = q.Get<bool>(L"ReturnValue"); }
        auto own = OwnerOf(o);
        if (killTracers && as && ue::NameOf(as).find(L"Rifle_00") != std::wstring::npos && ue::NameOf(as).find(L"Bullet") != std::wstring::npos) { Kill(o); uex::Log(L"sniper: fx killed %s", ue::NameOf(o).c_str()); continue; }
        uex::Log(L"sniper: fx %s %s owner %s %.0f cm active %d", ue::NameOf(o).c_str(), as ? ue::NameOf(as).c_str() : L"-", own ? ue::NameOf(own).c_str() : L"-", d, act);
        if (++n > 40) break;
    }
}

// houserules: room rules (net::Room::rules, rules.h) that need the game's own systems: Phantom Edge
// damage / on-off, Shachi regen, ult charge, healing items, 慈母护符, accessory limit.
// Called by fight on the game thread; every function is a no-op outside a fight.
#pragma once
#include <string>
#include "../net/rules.h"
namespace ue { struct UObject; }
namespace houserules {
void Begin(const rules::Rules& r, ue::UObject* me);  // round start (after fight's ResetLocal)
void Tick(const rules::Rules& r, ue::UObject* me, float dt);  // every fight frame
void End(ue::UObject* me);   // round over / match left: undo everything Begin/Tick changed
float OutgoingScale(ue::UObject* me);
// Phantom Edge cooldown (ecd): seconds left (0 = ready) and the full length; HUD only.
// sinceBlocked: seconds since the last move cancelled for the cooldown (large = none)
struct EdgeCd { float left = 0, len = 0; double sinceBlocked = 1e9; };
EdgeCd EdgeCooldown();  // extra factor for the hit fight is reporting now (sd% on Phantom Edge moves, else 1)
// lobby check of the local loadout against the rules (cimu, acc, heal items carried, ...):
// "" = fine, else a short Chinese reason; ui shows it and refuses "ready" while it is not ""
std::wstring Violation(const rules::Rules& r, ue::UObject* me);
}

// Weapon and Phantom Edge (杀器) choices shared by ui (lobby picker) and fight (equip at round start).
// id goes over the network via net::SetWeapon / net::SetEdge; label is shown in the lobby.
#pragma once
#include <string>
#include <vector>
namespace ue { struct UObject; }
namespace fight {
struct Weapon { std::string id; std::wstring label; };
const std::vector<Weapon>& Weapons();   // [0] = keep current weapon
void Equip(const std::string& id);      // equip on the local player (no-op for "" / unknown)
// Phantom Edges: DT_ShaqiWeaponData ItemIds, label = the row's WeaponName; [0] = keep the save's.
// Read from the table on first use (retried every 5 s while it is not loaded).
const std::vector<Weapon>& Edges();
void EquipEdge(const std::string& id);  // into the save's current Phantom Edge slot; no-op for "" / unknown / none equipped
bool NoEdgeEquipped();                 // the save has no Phantom Edge in its slots (EquipEdge then does nothing); false if unknown

// ---- fight-internal (weapons.cpp) ----
// The id is the DT_WeaponData row / ItemId (e.g. "PoJun"). Equip swaps it into the backpack's
// current main slot (PBZBackPackSubsystem::ReplaceWeaponByIndex): the game spawns the weapon
// actors, links its anim layers and grants its abilities. Not owned weapons work too; nothing is
// added to the inventory. EquipEdge does the same with ReplaceShaChiWeaponByIndex, but only on an
// occupied ShaqiWeapons slot (an empty one would add the item to the inventory). The save's slots
// and the weapon infos the swaps touch (red points) are snapshotted on the first change of a match.
void RestoreWeapons();                                  // put the snapshotted slots and weapon infos back
std::string WeaponConfigPath(const std::string& id);  // DT_WeaponData row -> config soft path, "" if unknown
std::string CurrentWeaponPath(ue::UObject* ch);          // main-slot weapon config, "" if none; no allocation in the game heap
bool SetGhostWeapon(ue::UObject* ghost, const std::string& cfgPath, bool armed);  // CharacterWeaponsComponent::SetSlotWeapon
std::wstring AbilityKey(ue::UObject* ch);                // "Double", "PoJun", ... (GA_Hun_<key>_A_C)
std::string EdgeConfigPath(const std::string& id);     // DT_ShaqiWeaponData row -> config soft path, "" if unknown
std::string EdgeIconPath(const std::string& id);       // row RenderIcon (else Icon, WhiteIcon), "" if unknown
bool IsEdgeConfig(const std::string& path);            // a row's config or one in its folder (_Evolution); no _Interact
std::string CurrentEdgePath(ue::UObject* ch);          // Weapon.Slot.Shachi config, "" if none
void SetGhostEdge(ue::UObject* ghost, const std::string& cfgPath);  // SetSlotWeapon/SetCurrentWeapon on Weapon.Slot.Shachi
std::wstring LoadoutReport();                          // test hook: backpack slots, weapon info hashes, snapshot state
}

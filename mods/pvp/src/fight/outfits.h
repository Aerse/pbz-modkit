// Outfit choices shared by ui (lobby picker) and fight (round start, ghosts). Visual only: the
// game's PBZAppearanceComponent::ApplyOutfitVisual under BeginSuppressAppearanceSave; never
// EquipOutfit (it saves the wardrobe) and no other save path. Game thread only.
// id = ItemId of a DT_PBZAppearanceTable row of type Outfit (4); over the network as the GAME
// event "O <id> <skin> <hat>" (skin EPBZSkinColor 0-3, hat 1 = shown). Entitlement outfits (DLC,
// preorder) are worn only when this account owns them (IsEntitlementOutfit / IsOutfitOwned).
#pragma once
#include <string>
#include <vector>
#include "weapons.h"
namespace ue { struct UObject; }
namespace fight::outfits {
const std::vector<Weapon>& List();       // lobby cards: [0] = 当前外观 (id ""); the table's names once it was read
std::wstring IconPath(const std::string& id);  // card texture (AppearanceTexture, else its body row's), "" if none
void SetPick(const std::string& id);     // the local lobby pick, a List() id (anything else: the own look)
const std::string& Pick();
bool PickDenied();                       // the last Wear refused the pick: an entitlement outfit this account does not own
int Probe(std::wstring* out);            // one line per table row (test hook); rows written, -1 no table
void Wear(ue::UObject* pawn, const std::wstring& id);  // local player; again each round / new pawn
void WearGhost(ue::UObject* ghost, const std::string& id, int skin, int hat);  // remote's choice on its ghost
std::string Event();                     // "O <id> <skin> <hat>": what Wear applied, else the own look; "" if unknown
bool Parse(const std::string& ev, std::string* id, int* skin, int* hat);  // format and ranges only
void Tick();                             // fight's Tick: the local outfit applied once more 2 s after Wear
void Restore();                          // MatchOver, before SaveVeto(false): original look, end suppression
void Forget();                           // world change: drop all kept pointers
}

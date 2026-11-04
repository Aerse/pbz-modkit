// arena: an original enclosed fighting space built from PBZ assets, placed far from the level.
// Game thread only.
#pragma once
#include "uex.h"
namespace arena {
// maps the host can pick in the lobby (net::Room::map indexes this table)
struct Map {
    const wchar_t* name;     // lobby label
    const wchar_t* level;    // package to open (OpenLevel), nullptr = build in the current world
    uex::Vec centre;         // ground / floor top centre
    double radius;           // hidden wall ring radius
    bool built;              // true: the walled courtyard is built at `centre`; false: only the ring
    const wchar_t* preview;  // lobby card image, file name in <mod dir>\maps\ (PNG)
};
const Map* Maps();
int MapCount();
void SetMap(int i);              // map used by Build / SpawnPoint / Inside / Tick (clamped)
int CurMap();
bool InLevel(int i);             // the current world is map i's level (always true without a level)
int ClearLevel();                // destroys the level's NPCs, AI and spawners; returns how many

bool Build();                    // idempotent; true once the arena exists in the current world
void Clear();                    // destroys everything spawned
bool Ready();
uex::Vec SpawnPoint(int slot, double* yaw);  // slot 0..7, facing the centre
bool Inside(uex::Vec p);         // within the walls
void Tick(float dt);             // keeps the local player inside (teleport back if out)
}

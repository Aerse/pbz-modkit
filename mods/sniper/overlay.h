#pragma once
// overlay: the HUD and the AWP scope in a click-through layered window over the game's client area
namespace overlay {
struct HudState {
    bool on = false, scope = false, help = false;
    const wchar_t* label = L"";          // static strings only (the weapon table's)
    const wchar_t* slots[3] = {};
    int ammo = 0, clip = 0, reserve = 0, sel = 0;
    float prog = 0; int kind = 0;        // progress 0..1 of: 0 none, 1 bolt, 2 reload, 3 deploy
    int xmode = 0; float xgap = 0;       // crosshair 0 none, 1 CS lines; xgap: cone radius (rad)
    float fov = 90;                      // camera horizontal FOV (deg), for the crosshair gap
};
void Start();                    // creates the overlay thread (call once)
void Stop();                     // joins it (before the dll unloads)
void Set(const HudState& s);     // any thread
void Hit(int kind, int dmg);     // hit marker: 0 body, 1 head, 2 kill, 3 headshot kill
bool Snap(const wchar_t* path);  // render the overlay into a BMP (magenta = transparent), for tests
}

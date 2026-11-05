// ui: main-menu "多人游戏" entry, connect/create/join panel, lobby (ready, weapon, rounds), HUD, result.
// Reads net:: lobby state each tick and calls net:: lobby actions itself. Game thread only.
#pragma once
namespace ui {
void Tick(float dt);   // builds/updates widgets as needed (menu entry when on the start menu, lobby, HUD)
void Shutdown();       // removes every widget, restores input mode
bool InMenu();         // a mod panel currently owns the input
void SetModDir(const wchar_t* dir);  // PbzRun's folder: maps, settings and session files live there
}

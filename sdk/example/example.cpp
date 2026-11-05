// example.cpp: the smallest useful PBZ mod. Build with build.bat, copy example.dll and example.txt
// into the folder <game>\platform\steam\pbzmp\example\, tick it in Mod管理器, start the game.
// Shows: a per-frame callback, a hotkey (Ctrl+F9), a function hook through the loader, clean unload.
#include <windows.h>
#include <cstdio>
#include "../../loader/host.h"  // or pbz_host.h from a release

static const PbzHost* g_host;
static int g_tick, g_hot, g_hook;
static float g_time;
static int g_frames;

static void Log(const wchar_t* fmt, double v = 0) {
    wchar_t b[256];
    swprintf(b, 256, fmt, v);
    g_host->log(b);  // one line in pbzmp\loader.log
}

// runs on the game thread after every engine tick
static void OnTick(void*, float dt) {
    g_time += dt;
    if (++g_frames == 600) Log(L"example: 600 frames, %.1f s of game time", g_time);
}

// runs on the game thread when Ctrl+F9 goes down with the game window in front
static void OnHotkey(void*) { Log(L"example: Ctrl+F9 pressed at %.1f s", g_time); }

// hook demo: Beep is never called by the game, so this is harmless. A real mod hooks a game function
// address instead; the loader refuses a second mod hooking the same address and logs both names.
using BeepFn = BOOL(WINAPI*)(DWORD, DWORD);
static BeepFn g_origBeep;
static BOOL WINAPI BeepDetour(DWORD freq, DWORD ms) {
    Log(L"example: Beep hooked (freq %.0f), not beeping", freq);
    return TRUE;  // call g_origBeep(freq, ms) to keep the original behaviour
}

extern "C" __declspec(dllexport) void PbzInit(const PbzHost* host) {
    g_host = host;
    g_tick = host->addTick(OnTick, nullptr);
    if (host->version < 2) { Log(L"example: loader too old for hooks / hotkeys"); return; }
    g_hot = host->addHotkey(VK_F9, kPbzHotCtrl, OnHotkey, nullptr);
    if (!g_hot) Log(L"example: Ctrl+F9 belongs to another mod");
    g_hook = host->hook(reinterpret_cast<void*>(&Beep), reinterpret_cast<void*>(&BeepDetour), reinterpret_cast<void**>(&g_origBeep));
    if (g_hook) Beep(440, 1);
    Log(L"example: loaded (hotkey %.0f)", g_hot);
}

// game thread, before the dll is freed (Mod reload or "stop example"). Undo everything you did
// to the game; ticks, hooks and hotkeys registered through the host are removed by the loader anyway.
extern "C" __declspec(dllexport) void PbzUnload() {
    if (g_hook) g_host->unhook(g_hook);
    Log(L"example: unloaded");
}

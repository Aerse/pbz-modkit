// fileguard: refuses writes under ...\SaveGames\ (Win32 level) while on. Game thread only.
#pragma once
namespace fileguard {
bool On();      // idempotent; true if this process is guarded (by mp or by nosave.dll)
bool Active();
void Tick();    // logs refused writes
void Off();     // unhook (mp unload only)
void Pin();     // mp unload outside the title: keep the hooks, pin the DLL so they stay valid
}

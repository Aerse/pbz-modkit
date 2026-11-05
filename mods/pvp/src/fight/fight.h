// fight: remote ghosts (spawn, net::Sample playback, animation), hostility, hit/damage exchange,
// local state publishing (net::SetLocalState), death reporting. Game thread only.
#pragma once
namespace fight {
void Begin(int round);   // round start: reset hp, spawn ghosts for net::RemoteSlots(), enable damage
void Tick(float dt);     // every frame while fighting
void End();              // round over: freeze, remove ghosts, clear hostility
void MatchOver();        // match left/over: End + restore the hp/posture/shield/Health_Min and backpack weapons saved at the first Begin
bool LocalDead();
void Shutdown();         // MatchOver + caches
bool TestAbility(const wchar_t* gaClassName);  // test hook; '*' in the name = current weapon key (GA_Hun_*_A_C)
bool GhostAnchor(int slot, float out[3]);  // world cm just above that ghost's head; false = no live ghost
void TestGhostEdge(const char* id);  // test hook: ghosts show this Phantom Edge (DT ItemId; "" = the remote's own "E")
}

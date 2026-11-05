// discover: finds the game's UE globals at startup by scanning the running exe, so a game update
// needs no new addresses. Everything is checked structurally (object array items know their own
// index, the name pool starts with "None", Tick is the vtable slot whose function references the
// "UGameEngine::Tick..." string, ProcessEvent tests FUNC_Native). Section names in this exe are
// scrambled, so sections are picked by their flags only. Loader thread; all reads are guarded.
#pragma once
#include <cstdint>
#include <string>

namespace disc {
struct Result {
    uintptr_t objects = 0;     // GUObjectArray.Objects (FUObjectItem** chunk table)
    uintptr_t numObjects = 0;  // GUObjectArray.NumElements (int32)
    uintptr_t namePool = 0;    // FNamePool
    uintptr_t engineVt = 0;    // vtable of the GameEngine object
    int tickSlot = -1;         // UGameEngine::Tick in that vtable
    int processEvent = -1;     // UObject::ProcessEvent vtable index
    std::wstring how;          // what was found and how (for the log)
};
using LogFn = void (*)(const std::wstring&);

// Each returns false while the game is not far enough yet (call again later); on a structural
// mismatch (zero or several candidates) it logs once through `log`.
bool FindObjectArray(Result& r, LogFn log);
bool FindNamePool(Result& r, LogFn log);
// needs the two above and the engine object; fills engineVt, tickSlot, processEvent
bool FindEngine(Result& r, LogFn log);
std::wstring Hex(uintptr_t a);  // "exe+0x1234" inside the exe, else "0x..."
}  // namespace disc

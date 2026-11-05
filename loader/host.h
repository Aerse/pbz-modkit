// host.h: services the loader (version.dll, main.cpp) gives to modules.
// The loader owns the only hook on UGameEngine::Tick; modules never patch that vtable slot.
// It finds the engine globals itself at every start (version 3 fields), so a game update changes
// no address a module needs. framework/ue.h reads them from here.
#pragma once
#include <cstdint>

struct PbzHost {
    uint32_t version;          // 3 (fields are only ever appended: check version before using newer ones)
    const wchar_t* dir;        // mod root (pbzmp\), ends with '\'; your own folder comes in PbzRun
    void* (*engine)();         // UGameEngine* (the Tick hook's `this`), null before the first tick
    // fn(ctx, dt) runs on the game thread after every engine tick until removed. Returns id > 0.
    // A callback that crashes (SEH) is logged and removed.
    int (*addTick)(void (*fn)(void* ctx, float dt), void* ctx);
    void (*removeTick)(int id);
    // fn(ctx) runs once on the game thread after the next engine tick (FIFO).
    void (*post)(void (*fn)(void* ctx), void* ctx);
    void (*log)(const wchar_t* text);  // one line in loader.log
    // ---- version 2 ----
    // Function hook through the loader, so two mods never patch the same function blind.
    // target: address in the game; detour: your function; *original receives the trampoline.
    // Returns id > 0, or 0 if another mod already hooks target (logged with both names) or the
    // hook fails. The loader removes your hooks after PbzUnload. Call from PbzInit / the game thread.
    int (*hook)(void* target, void* detour, void** original);
    void (*unhook)(int id);
    // fn(ctx) on the game thread when the key goes down while the game window is in front.
    // vk: Windows virtual key; mods: bit 1 Ctrl, 2 Shift, 4 Alt (exact match). Returns id > 0,
    // or 0 if another mod owns that key combination (logged). Removed on unload.
    int (*addHotkey)(int vk, int mods, void (*fn)(void* ctx), void* ctx);
    void (*removeHotkey)(int id);
    // ---- version 3: engine globals, found by the loader at startup (set before any module runs) ----
    void* objects;             // FUObjectArray: +0 FUObjectItem** chunk table, +8 int32 count
    void* namePool;            // FNamePool: +8 current block (u32), +0x10 block pointers
    int32_t processEvent;      // UObject::ProcessEvent vtable index
    int32_t tickSlot;          // UGameEngine::Tick vtable index (informational)
};
constexpr int kPbzHotCtrl = 1, kPbzHotShift = 2, kPbzHotAlt = 4;

// Module exports, all optional, all extern "C":
//   void PbzInit(const PbzHost*)    loader thread, right after LoadLibrary
//   void PbzRun(const wchar_t* dir) loader thread, after PbzInit. dir = the mod's own folder
//                                   (pbzmp\<name>\), or the root for a legacy flat <name>.dll
//   void PbzUnload()                game thread, before the module is freed ("run X" again or
//                                   "stop X"). Must undo everything: ticks are removed by the
//                                   loader, but spawned actors, widgets and patches are yours.
//   void PbzKeep()                  legacy: keep loaded after PbzRun (no PbzUnload = never freed)

// clsprops.cpp: a tool for mod authors. It lists the fields of the classes and structs you name, as
// the running game has them: offset, size, type, bool bit, and a uex::Prop line ready to paste.
// Install: clsprops.dll, clsprops.txt and classes.txt into <game>\platform\steam\pbzmp\clsprops\.
// Put one class per line in classes.txt, then load the mod in Mod管理器 (or press Ctrl+F11 in game
// to list again after editing the file). The result is fields.txt in the same folder.
//   Class /Script/Engine.Character          full name: exactly that class
//   ScriptStruct /Script/Engine.HitResult   a struct
//   WB_StartMenu_V2_C                       a short name: that class (blueprints load with their level)
//   StartMenu                               part of a name: the first class or struct that has it
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "uex.h"

using ue::UObject;
using ue::At;

static const PbzHost* g_host;
static std::wstring g_dir;
static int g_hot;

static std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == 0xfeff || s[i] == L' ' || s[i] == L'\t')) ++i;
    return s.substr(i);
}
static std::wstring FName(const uint8_t* p, int off) { return ue::NameOf(*reinterpret_cast<const ue::FName*>(p + off)); }
// "IntProperty", "ObjectProperty<Actor>", "StructProperty<Vector>", "ArrayProperty<FloatProperty>"
static std::wstring TypeOf(const uint8_t* p, int depth = 0) {
    auto fc = At<const uint8_t*>(p, ue::kFField_Class);
    std::wstring t = fc ? FName(fc, ue::kFFieldClass_Name) : L"?";
    if (t == L"ObjectProperty" || t == L"ClassProperty" || t == L"StructProperty" || t == L"WeakObjectProperty" ||
        t == L"SoftObjectProperty" || t == L"SoftClassProperty" || t == L"InterfaceProperty" || t == L"LazyObjectProperty" ||
        t == L"EnumProperty" || t == L"ByteProperty") {
        if (auto x = At<UObject*>(p, ue::kProp_Extra); x && t != L"EnumProperty" && t != L"ByteProperty") t += L"<" + ue::NameOf(x) + L">";
    } else if ((t == L"ArrayProperty" || t == L"SetProperty") && depth < 2) {
        if (auto in = At<const uint8_t*>(p, ue::kProp_Extra)) t += L"<" + TypeOf(in, depth + 1) + L">";
    } else if (t == L"MapProperty" && depth < 2) {
        auto k = At<const uint8_t*>(p, ue::kProp_Extra), v = At<const uint8_t*>(p, ue::kProp_Extra + 8);
        if (k && v) t += L"<" + TypeOf(k, depth + 1) + L", " + TypeOf(v, depth + 1) + L">";
    }
    return t;
}
static bool IsStructType(UObject* o) {  // Class, BlueprintGeneratedClass, WidgetBlueprintGeneratedClass, ScriptStruct, ...
    if (!o || !o->cls) return false;
    auto c = ue::NameOf(o->cls);
    return c.size() >= 5 && (c.compare(c.size() - 5, 5, L"Class") == 0 || c == L"ScriptStruct" || c == L"UserDefinedStruct");
}
static UObject* Find(const std::wstring& key) {
    if (key.find(L' ') != std::wstring::npos) return ue::FindObject(key);
    UObject* part = nullptr;
    for (int i = 0, n = ue::NumObjects(); i < n; ++i) {
        auto o = ue::ObjectAt(i);
        if (!o || !IsStructType(o)) continue;
        auto nm = ue::NameOf(o);
        if (nm == key) return o;
        if (!part && nm.find(key) != std::wstring::npos) part = o;
    }
    return part;
}
static void Dump(FILE* f, const std::wstring& key) {
    UObject* st = Find(key);
    if (!st) { fwprintf(f, L"== %s: not found (not loaded yet? blueprints load with their level)\n\n", key.c_str()); return; }
    fwprintf(f, L"== %s  size 0x%x\n", ue::FullName(st).c_str(), At<int32_t>(st, ue::kUStruct_PropsSize));
    for (auto c = st; c; c = At<UObject*>(c, ue::kUStruct_Super)) {
        auto cn = ue::NameOf(c);
        if (cn == L"Object") break;
        if (c != st && (cn == L"Actor" || cn == L"ActorComponent" || cn == L"UserWidget" || cn == L"Widget")) {
            fwprintf(f, L" [%s: engine base class, put it on its own line to list it]\n", ue::FullName(c).c_str());
            break;
        }
        fwprintf(f, L" [%s]\n", ue::FullName(c).c_str());
        bool cNative = ue::PathOf(c).rfind(L"/Script/", 0) == 0;
        for (auto p = At<const uint8_t*>(c, ue::kUStruct_ChildProps); p; p = At<const uint8_t*>(p, ue::kFField_Next)) {
            auto nm = FName(p, ue::kFField_Name), t = TypeOf(p);
            int off = At<int32_t>(p, ue::kProp_Offset), sz = At<int32_t>(p, ue::kProp_ElemSize) * At<int32_t>(p, ue::kProp_ArrayDim);
            wchar_t bit[96] = L"";
            int mask = 0;
            if (t == L"BoolProperty") {  // FieldSize, ByteOffset, ByteMask, FieldMask; FieldMask 0xff = a plain bool
                int byteOff = At<uint8_t>(p, ue::kProp_Extra + 1), byteMask = At<uint8_t>(p, ue::kProp_Extra + 2);
                bool plain = At<uint8_t>(p, ue::kProp_Extra + 3) == 0xff;
                off += byteOff; mask = plain ? 0xff : byteMask;
                swprintf(bit, 96, plain ? L"  bool" : L"  bit mask 0x%x", byteMask);
            }
            fwprintf(f, L"  0x%-5x %4d  %-40s %s%s\n", off, sz, t.c_str(), nm.c_str(), bit);
            // native fields: the line for a mod (blueprint fields: uex::Field<T>(obj, name) instead)
            if (cNative)
                fwprintf(f, mask ? L"          uex::Prop k%s{L\"%s\", L\"%s\", 0x%x, 0x%x};\n" : L"          uex::Prop k%s{L\"%s\", L\"%s\", 0x%x};\n",
                         nm.c_str(), ue::FullName(c).c_str(), nm.c_str(), off, mask);
        }
    }
    fwprintf(f, L"\n");
}
static void Job(void*) {
    std::vector<std::wstring> keys;
    if (FILE* in = _wfopen((g_dir + L"classes.txt").c_str(), L"r, ccs=UTF-8")) {
        wchar_t line[512];
        while (fgetws(line, 511, in)) {
            auto s = Trim(line);
            if (!s.empty() && s[0] != L'#') keys.push_back(s);
        }
        fclose(in);
    }
    if (keys.empty()) { uex::Log(L"classes.txt is empty: one class name per line"); return; }
    FILE* f = _wfopen((g_dir + L"fields.txt").c_str(), L"w, ccs=UTF-8");
    if (!f) { uex::Log(L"can't write fields.txt"); return; }
    uex::SehScope seh;
    for (auto& k : keys) {
        try { Dump(f, k); } catch (...) { fwprintf(f, L"== %s: crashed while reading (%s)\n\n", k.c_str(), uex::ExcWhere().c_str()); }
    }
    fclose(f);
    uex::Log(L"%d classes -> %sfields.txt", int(keys.size()), g_dir.c_str());
}
static void OnHotkey(void*) { Job(nullptr); }

extern "C" __declspec(dllexport) void PbzInit(const PbzHost* host) {
    g_host = host;
    uex::SetHost(host);
    uex::SetLogName(L"clsprops");
    if (host->version < 3) { host->log(L"clsprops: needs loader api 3"); return; }
    g_hot = host->addHotkey(VK_F11, kPbzHotCtrl, OnHotkey, nullptr);
    if (!g_hot) uex::Log(L"Ctrl+F11 belongs to another mod: load the mod again to list again");
}
extern "C" __declspec(dllexport) void PbzRun(const wchar_t* dir) {
    g_dir = dir && *dir ? dir : g_host->dir;
    if (g_dir.back() != L'\\') g_dir += L'\\';
    if (g_host->version >= 3) g_host->post(Job, nullptr);  // game thread, after the next engine tick
}
extern "C" __declspec(dllexport) void PbzUnload() {}  // the hotkey is removed by the loader

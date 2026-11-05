// audio.cpp: CS sounds from <mod dir>sniper_cache/snd/*.wav through XAudio2 (preloaded, a voice pool, so
// rapid fire never drops or cuts a shot). PlayAt attenuates with distance and pans from the camera.
#include "common.h"
#include <xaudio2.h>
#include <map>
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace audio {
struct Clip { WAVEFORMATEX fmt{}; std::vector<uint8_t> data; };
struct Voice { IXAudio2SourceVoice* v; WAVEFORMATEX fmt; ULONGLONG started; };
static IXAudio2* g_xa;
static IXAudio2MasteringVoice* g_master;
static std::map<std::wstring, Clip> g_clips;
static std::vector<Voice> g_voices;
static bool g_tried;
static float g_vol = .7f;
static ik::V g_lisLoc, g_lisRight{0, 1, 0};
constexpr size_t kMaxVoices = 48;

// RIFF/WAVE: walk the chunks (CS files carry a LIST chunk before data)
static bool LoadWav(const std::wstring& path, Clip& c) {
    FILE* f = _wfopen(path.c_str(), L"rb"); if (!f) return false;
    std::vector<uint8_t> b; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 12) { b.resize(n); fread(b.data(), 1, n, f); }
    fclose(f);
    if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) return false;
    bool fmt = false;
    for (size_t o = 12; o + 8 <= b.size();) {
        uint32_t sz; memcpy(&sz, b.data() + o + 4, 4);
        const uint8_t* p = b.data() + o + 8; size_t avail = b.size() - o - 8; if (sz > avail) sz = uint32_t(avail);
        if (!memcmp(b.data() + o, "fmt ", 4) && sz >= 16) { memcpy(&c.fmt, p, 16); c.fmt.cbSize = 0; fmt = true; }
        else if (!memcmp(b.data() + o, "data", 4)) c.data.assign(p, p + sz);
        o += 8 + sz + (sz & 1);
    }
    return fmt && c.fmt.wFormatTag == WAVE_FORMAT_PCM && !c.data.empty();
}

bool Init() {
    if (g_tried) return g_xa != nullptr;
    g_tried = true;
    // XAudio2 2.9 (Windows 10+) needs no CoInitializeEx: leave the game thread's COM apartment alone
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { g_xa = nullptr; uex::Log(L"audio: XAudio2Create failed"); return false; }
    if (FAILED(g_xa->CreateMasteringVoice(&g_master, 2, 44100))) {
        g_xa->Release(); g_xa = nullptr; uex::Log(L"audio: no mastering voice"); return false;
    }
    std::wstring dir = g_cacheDir + L"snd\\";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"*.wav").c_str(), &fd);
    size_t bytes = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            Clip c; std::wstring nm(fd.cFileName); nm.resize(nm.size() - 4);
            if (LoadWav(dir + fd.cFileName, c)) { bytes += c.data.size(); g_clips.emplace(nm, std::move(c)); }
            else uex::Log(L"audio: bad wav %s", fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    uex::Log(L"audio: %zu sounds, %zu KB", g_clips.size(), bytes / 1024);
    return true;
}

// an idle voice of the same format, or a new one; the oldest playing voice when the pool is full
static IXAudio2SourceVoice* GetVoice(const WAVEFORMATEX& fmt) {
    Voice* oldest = nullptr;
    for (auto& v : g_voices) {
        if (memcmp(&v.fmt, &fmt, sizeof fmt)) continue;
        XAUDIO2_VOICE_STATE st; v.v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (!st.BuffersQueued) { v.started = GetTickCount64(); return v.v; }
        if (!oldest || v.started < oldest->started) oldest = &v;
    }
    if (g_voices.size() >= kMaxVoices && oldest) {
        oldest->v->Stop(); oldest->v->FlushSourceBuffers(); oldest->started = GetTickCount64(); return oldest->v;
    }
    IXAudio2SourceVoice* sv = nullptr;
    if (FAILED(g_xa->CreateSourceVoice(&sv, &fmt))) return nullptr;
    g_voices.push_back({sv, fmt, GetTickCount64()});
    return sv;
}
static void Start(const wchar_t* name, float vol, float pan) {
    if (!Init() || !name) return;
    auto it = g_clips.find(name);
    if (it == g_clips.end()) { uex::Log(L"audio: no sound %s", name); return; }
    auto& c = it->second;
    auto v = GetVoice(c.fmt); if (!v) return;
    v->Stop(); v->FlushSourceBuffers();
    XAUDIO2_BUFFER b{}; b.AudioBytes = UINT32(c.data.size()); b.pAudioData = c.data.data(); b.Flags = XAUDIO2_END_OF_STREAM;
    if (FAILED(v->SubmitSourceBuffer(&b))) return;
    float gl = pan > 0 ? 1 - pan : 1, gr = pan < 0 ? 1 + pan : 1;
    int ch = c.fmt.nChannels;
    float m[4];
    if (ch == 1) { m[0] = gl; m[1] = gr; }
    else { m[0] = gl; m[1] = 0; m[2] = 0; m[3] = gr; }
    v->SetOutputMatrix(g_master, ch, 2, m);
    v->SetVolume(vol * g_vol);
    v->Start();
}
void Play(const wchar_t* name, float vol) { Start(name, vol, 0); }
void Listener(ik::V loc, ik::V right) { g_lisLoc = loc; g_lisRight = right; }
// world sound: full volume within 5 m, then ~1/distance down to 8%; pan by the side it is on
void PlayAt(const wchar_t* name, ik::V at, float vol) {
    ik::V d = at - g_lisLoc; double dist = ik::Len(d);
    double k = dist < 500 ? 1 : 500 / dist; if (k < .08) k = .08;
    double pan = dist > 1 ? ik::Dot(ik::Norm(d), g_lisRight) * .6 : 0;
    Start(name, float(vol * k), float(pan));
}
void SetMaster(float v) { g_vol = v; }
void Shutdown() {
    for (auto& v : g_voices) { v.v->Stop(); v.v->DestroyVoice(); }
    g_voices.clear();
    if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
    if (g_xa) { g_xa->Release(); g_xa = nullptr; }
    g_clips.clear(); g_tried = false;
}
}  // namespace audio

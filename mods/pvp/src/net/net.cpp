// pbzmp network client. See net.h, docs/pvp-protocol.md (lobby) and docs/pvp-netcode.md (game channel).
// One global client guarded by one mutex; a private I/O thread does all socket work and timers.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "net.h"
#include "room_server.h"
#include "steam.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "ws2_32.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace net {

static double QpcFreq() {
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
    return f;
}
double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / QpcFreq();
}

namespace {
constexpr uint8_t kMagic = 0xB7, kProto = 1, kGver = 1;
constexpr int kMaxDgram = 1200, kMaxPayload = 1192, kEvBudget = 900, kMaxEvent = kEvBudget - 7;
constexpr int kMinHello = 64;  // smallest create/join datagram the server answers
constexpr double kSendIv = 1000.0 / 60.0, kSendIdle = 20.0;
constexpr double kWindowMs = 3000, kExtrapMs = 150, kQuickLateMs = 250;

double NowMs() { return Now() * 1000.0; }
uint32_t Ms32(double ms) { return (uint32_t)(uint64_t)std::llround(ms); }
int16_t D16(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }
int32_t D32(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
template <class T> T Clamp(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }

struct W {
    std::string b;
    void u8(uint8_t v) { b.push_back((char)v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void f32(float f) { uint32_t u; memcpy(&u, &f, 4); u32(u); }
    void raw(const void* p, size_t n) { b.append((const char*)p, n); }
};
struct R {
    const uint8_t* p; size_t n, i = 0; bool ok = true;
    R(const void* d, size_t len) : p((const uint8_t*)d), n(len) {}
    uint8_t u8() { if (i >= n) { ok = false; return 0; } return p[i++]; }
    uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (u8() << 8)); }
    uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
    float f32() { uint32_t u = u32(); float f; memcpy(&f, &u, 4); return f; }
    size_t left() const { return n - i; }
};

// ---- text helpers (lobby) ----
std::string PctEnc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back((char)c);
        else { o.push_back('%'); o.push_back(hex[c >> 4]); o.push_back(hex[c & 15]); }
    }
    return o;
}
int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
std::string PctDec(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int a = HexVal(s[i + 1]), b = HexVal(s[i + 2]);
            if (a >= 0 && b >= 0) { o.push_back((char)(a * 16 + b)); i += 2; continue; }
        }
        o.push_back(s[i]);
    }
    return o;
}
std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) { v.push_back(s.substr(a)); break; }
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}
using KV = std::map<std::string, std::string>;  // raw (still encoded) values
KV ParseKV(const std::vector<std::string>& t, size_t from) {
    KV kv;
    for (size_t i = from; i < t.size(); ++i) {
        size_t e = t[i].find('=');
        if (e != std::string::npos) kv[t[i].substr(0, e)] = t[i].substr(e + 1);
    }
    return kv;
}
int ToInt(const KV& kv, const char* k, int def) {
    auto it = kv.find(k);
    if (it == kv.end()) return def;
    try { return (int)std::stoll(PctDec(it->second)); } catch (...) { return def; }
}
std::string ToStr(const KV& kv, const char* k) {
    auto it = kv.find(k);
    return it == kv.end() ? std::string() : PctDec(it->second);
}

// ---- game channel state ----
struct Smp { double ts; State s; };         // ts: sender ms (unwrapped)
struct PendEv { double te; bool sure; std::string text; double due = 0; };

struct Remote {
    bool has = false;
    uint16_t session = 0;
    std::deque<uint16_t> oldSess;
    bool tsInit = false; double lastTs64 = 0;
    bool seqInit = false; uint16_t maxSeq = 0; int64_t expected = 0, received = 0;
    bool echoHas = false; uint32_t echoT = 0; double echoAt = 0;
    double lastRecv = -1e18;
    std::deque<Smp> samples;
    std::deque<std::pair<double, double>> offs;  // (tr, o)
    double B = 0, J = 0, D = 25, Dt = 0; bool bInit = false;
    double sendIv = kSendIv, lastSampleTs = -1e18;
    uint16_t sureNext = 1;
    std::unordered_map<uint16_t, PendEv> sureBuf;
    std::unordered_map<int, std::string> strings;
    std::deque<PendEv> sureQ;
    std::deque<PendEv> quickQ;
    std::unordered_set<uint16_t> quickSeen;
    std::deque<std::pair<double, uint16_t>> quickSeenT;
    double rtt = -1;
    int starves = 0; bool starving = false;
    std::deque<double> rxT;
};

struct OutSure { uint16_t id; double t; std::string bytes; uint32_t acked = 0; };
struct OutQuick { uint16_t id; double t; std::string bytes; int sent = 0; };
struct Pending { uint32_t req; std::string cmd, text; double next = 0; int tries = 0, maxTries = 20; };
struct LPkt { double at; std::string data; };

struct S {
    // server / lobby
    std::string host = "127.0.0.1"; int port = 27777;
    std::string password;  // sent as pw= in create/join when not empty
    bool resolveNeeded = true, resolved = false; sockaddr_in srv{};
    Status st = Status::Idle; std::string err;
    Room room; bool haveRoom = false;
    uint32_t token = 0, req = 0;
    bool resuming = false;  // Resume: a ping with the old token is out, InRoom once it is answered
    std::vector<Pending> pend;
    double lastPing = -1e18, lastSrvRecv = 0; int srvRtt = -1;
    // loopback
    bool loop = false; int lDelay = 0, lJit = 0; float lLoss = 0;
    std::vector<LPkt> lq;
    // game, local side
    bool active = false;
    uint16_t session = 0, seq = 0; double sessionAt = 0;
    bool haveLocal = false, newLocal = false; State local; double localAt = 0;
    bool havePrev = false; State prev; double prevAt = 0;
    double lastSend = -1e18, nextSend = 0;
    std::vector<std::string> internList;
    std::unordered_map<std::string, uint16_t> intern;
    uint16_t sureId = 1, quickId = 1;
    std::deque<OutSure> osure;
    std::deque<OutQuick> oquick;
    bool evPending = false;
    std::map<int, Remote> rem;
    std::mt19937 rng{std::random_device{}()};
    // Steam mode (net.h UseSteam): the room server runs in this process (host) or behind a lobby owner
    bool steam = false, steamHost = false;
    uint64_t hostId = 0;                  // joiner: the SteamID that runs the room server
    std::string findCode;                 // joiner: lobby search requested by Join, started by SteamPump
    bool lobbyHosted = false;             // host: HostLobby was called for this room
    bool finding = false;                 // joiner: FindLobby running
    double nextAccept = 0;                // host: next AcceptSessionWithUser sweep over the lobby members
    std::unique_ptr<rsrv::Server> rs;     // host: the room server (called under the lock only)
    std::vector<std::string> selfIn;      // host: server -> own client datagrams, drained by IoLoop
};

struct G {
    std::mutex mx;
    std::thread th;
    std::atomic<bool> stop{false};
    bool running = false;
    SOCKET sock = INVALID_SOCKET;
    HANDLE sockEv = nullptr, wakeEv = nullptr, timer = nullptr;
    S s;
    ~G() { if (th.joinable()) th.detach(); }  // process exit without Shutdown(): never std::terminate
};
G& g() { static G x; return x; }

void IoLoop();

void Wake() { if (g().wakeEv) SetEvent(g().wakeEv); }

void EnsureThread() {  // under lock
    G& G_ = g();
    if (G_.running) return;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    G_.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (G_.sock != INVALID_SOCKET) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = 0;
        bind(G_.sock, (sockaddr*)&a, sizeof a);
        BOOL f = FALSE; DWORD br = 0;
        WSAIoctl(G_.sock, SIO_UDP_CONNRESET, &f, sizeof f, nullptr, 0, &br, nullptr, nullptr);
        int sz = 1 << 20;
        setsockopt(G_.sock, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof sz);
        G_.sockEv = WSACreateEvent();
        WSAEventSelect(G_.sock, G_.sockEv, FD_READ);  // also makes the socket non-blocking
    }
    G_.wakeEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    G_.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!G_.timer) G_.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    G_.stop = false;
    G_.running = true;
    G_.th = std::thread(IoLoop);
}

// ---- sending ----
void SendRaw(uint8_t type, const std::string& payload) {  // to server
    S& s = g().s;
    if (!s.resolved || (!s.steam && g().sock == INVALID_SOCKET)) return;
    W w;
    w.u8(kMagic); w.u8(kProto); w.u8(type); w.u8(0); w.u32(s.token);
    w.raw(payload.data(), payload.size());
    if (w.b.size() > (size_t)kMaxDgram) return;
    if (s.steam) {  // own room server in-process, or the lobby owner over Steam P2P
        if (s.rs) s.rs->Handle(s.hostId, (uint32_t)s.hostId, (const uint8_t*)w.b.data(), w.b.size(), Now());
        else steam::Send(s.hostId, w.b.data(), w.b.size());
        return;
    }
    sendto(g().sock, w.b.data(), (int)w.b.size(), 0, (const sockaddr*)&s.srv, sizeof s.srv);
}

std::string CmdText(uint32_t req, const std::string& cmd, const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string t = std::to_string(req) + " " + cmd;
    for (auto& [k, v] : kv) t += " " + k + "=" + PctEnc(v);
    return t;
}

void QueueCmd(const std::string& cmd, const std::vector<std::pair<std::string, std::string>>& kv, int maxTries = 20) {
    S& s = g().s;
    Pending p;
    p.req = ++s.req;
    p.cmd = cmd;
    p.text = CmdText(p.req, cmd, kv);
    // create/join go out without a token: the server drops such datagrams below kMinHello bytes
    // (anti-amplification, docs/pvp-protocol.md), so pad them with pad=---...
    if ((cmd == "create" || cmd == "join") && 8 + p.text.size() + 5 < (size_t)kMinHello)
        p.text += " pad=" + std::string(kMinHello - 8 - p.text.size() - 5, '-');
    p.maxTries = maxTries;
    p.next = 0;
    s.pend.push_back(std::move(p));
    Wake();
}

uint32_t PeerMask() {
    S& s = g().s;
    if (s.loop) return 1u << 1;
    uint32_t m = 0;
    // peers heard from count too: a member the room STATE does not list yet must still get its SURE
    double now = NowMs();
    for (auto& [slot, r] : s.rem)
        if (r.has && slot >= 0 && slot < 32 && slot != s.room.you && now - r.lastRecv < 5000) m |= 1u << slot;
    if (!s.haveRoom) return m;
    for (auto& mb : s.room.members)
        if (mb.slot != s.room.you && mb.conn && mb.slot >= 0 && mb.slot < 32) m |= 1u << mb.slot;
    return m;
}

void QueueSureLocked(const std::string& bytes) {
    S& s = g().s;
    OutSure e{s.sureId++, NowMs(), bytes.substr(0, kMaxEvent)};
    s.osure.push_back(std::move(e));
    s.evPending = true;
}

void NewSession() {
    S& s = g().s;
    uint16_t old = s.session;
    do { s.session = (uint16_t)s.rng(); } while (s.session == 0 || s.session == old);
    s.seq = (uint16_t)s.rng();
    s.sureId = 1;
    s.quickId = 1;
    s.osure.clear();
    s.oquick.clear();
    s.sessionAt = NowMs();
    Wake();  // every peer starts over on the new session: PeerReset sends it the strings
}

// A peer whose receiver starts over (its first packet, or it came back with a new session) has none of
// our strings: they go out again. Older ids it never got are skipped through the floor (section 5).
void PeerReset(int slot) {
    S& s = g().s;
    if (slot < 0 || slot >= 32) return;
    for (size_t i = 0; i < s.internList.size(); ++i)
        QueueSureLocked(std::string("\x01 ") + std::to_string(i + 1) + " " + s.internList[i]);
    s.evPending = true;
}

void ResetRoom() {
    S& s = g().s;
    s.token = 0;
    s.resuming = false;
    s.room = Room{};
    s.haveRoom = false;
    s.pend.clear();
    s.active = false;
    s.rem.clear();
    s.osure.clear();
    s.oquick.clear();
    s.srvRtt = -1;
    s.rs.reset();
    s.selfIn.clear();
    s.steamHost = false;
    s.findCode.clear();
    s.finding = s.lobbyHosted = false;
    if (s.steam) { s.hostId = 0; s.resolved = false; }
}

void DoLeave() {  // under lock
    S& s = g().s;
    if (s.loop) { s.loop = false; s.lq.clear(); }
    if (s.token != 0 && s.resolved) {
        std::string t = CmdText(++s.req, "leave", {});
        for (int i = 0; i < 3; ++i) SendRaw(0x01, t);
    }
    ResetRoom();
    s.st = Status::Idle;
    s.err.clear();
}

void EncodeState(W& w, const State& st, uint16_t age) {
    uint8_t el = st.extLen;
    w.u8(1); w.u16((uint16_t)(49 + el));
    w.u16(age);
    for (int i = 0; i < 3; ++i) w.f32(st.pos[i]);
    for (int i = 0; i < 3; ++i) w.u16((uint16_t)(int16_t)Clamp<float>(std::round(st.vel[i]), -32767.f, 32767.f));
    for (int i = 0; i < 2; ++i) w.u16((uint16_t)(int16_t)Clamp<float>(std::round(st.acc[i] * 10000.f), -32767.f, 32767.f));
    double y = std::fmod((double)st.yaw, 360.0); if (y < 0) y += 360.0;
    w.u16((uint16_t)((uint32_t)std::llround(y / 360.0 * 65536.0) & 0xFFFF));
    w.u8(st.mode); w.u8(st.flags);
    w.u16((uint16_t)Clamp<float>(std::round(st.hp * 1000.f), 0.f, 65535.f));
    w.u16((uint16_t)Clamp<float>(std::round(st.posture * 1000.f), 0.f, 65535.f));
    w.u16(st.montage);
    w.u16((uint16_t)Clamp<float>(std::round(st.montPos * 1000.f), 0.f, 65535.f));
    w.u8((uint8_t)(int8_t)Clamp<float>(std::round(st.montRate * 32.f), -128.f, 127.f));
    w.u8(st.armed);
    w.raw(st.anim, 7);
    w.u8(0); w.u8(0); w.u8(0);
    w.u8(el);
    w.raw(st.ext, el);
}

void ProcessGame(const uint8_t* d, size_t n, int slot, double tr);

void SendGame() {
    S& s = g().s;
    double now = NowMs();
    uint32_t tSend = Ms32(now);
    W w;
    w.u8(kGver); w.u8(0); w.u16(s.session); w.u16(s.seq++); w.u32(tSend);
    if (s.haveLocal) {
        uint32_t sm = Ms32(s.localAt);
        EncodeState(w, s.local, (uint16_t)Clamp<int64_t>(D32(tSend, sm), 0, 65535));
        // Redundancy: the previously sent sample again (receiver dedups by ts), so a single lost
        // packet leaves no hole in the timeline. Skipped when it would crowd out events.
        if (s.havePrev && s.prevAt != s.localAt && s.prev.extLen <= 64 && now - s.prevAt < 100)
            EncodeState(w, s.prev, (uint16_t)Clamp<int64_t>(D32(tSend, Ms32(s.prevAt)), 0, 65535));
        if (s.prevAt != s.localAt) { s.prev = s.local; s.prevAt = s.localAt; s.havePrev = true; }
    }
    for (auto& [slot, r] : s.rem) {
        if (!r.echoHas) continue;
        uint32_t bits = 0;
        for (int i = 0; i < 32; ++i)
            if (r.sureBuf.count((uint16_t)(r.sureNext + 1 + i))) bits |= 1u << i;
        w.u8(4); w.u16(13);
        w.u8((uint8_t)slot);
        w.u32(r.echoT);
        w.u16((uint16_t)Clamp<double>(now - r.echoAt, 0, 65535));
        w.u16(r.sureNext);
        w.u32(bits);
    }
    // SURE floor: ids below it are not kept for re-sending any more (acked by every peer, or dropped
    // while no peer was there to ack them). A receiver still waiting for one skips to the floor.
    w.u8(5); w.u16(2);
    w.u16(s.osure.empty() ? s.sureId : s.osure.front().id);
    int budget = std::min<int>(kEvBudget, kMaxPayload - (int)w.b.size());
    for (auto& e : s.osure) {
        int need = 7 + (int)e.bytes.size();
        if (need > budget) break;
        budget -= need;
        w.u8(2); w.u16((uint16_t)(4 + e.bytes.size()));
        w.u16(e.id); w.u16((uint16_t)Clamp<double>(now - e.t, 0, 65535));
        w.raw(e.bytes.data(), e.bytes.size());
    }
    for (auto& e : s.oquick) {
        int need = 7 + (int)e.bytes.size();
        if (need > budget) break;
        budget -= need;
        w.u8(3); w.u16((uint16_t)(4 + e.bytes.size()));
        w.u16(e.id); w.u16((uint16_t)Clamp<double>(now - e.t, 0, 65535));
        w.raw(e.bytes.data(), e.bytes.size());
        e.sent++;
    }
    while (!s.oquick.empty() && s.oquick.front().sent >= 3) s.oquick.pop_front();
    s.lastSend = now;
    s.newLocal = false;
    s.evPending = false;

    if (s.loop) {
        if (std::uniform_real_distribution<float>(0, 100)(s.rng) < s.lLoss) return;
        double j = s.lJit > 0 ? std::uniform_real_distribution<double>(-s.lJit, s.lJit)(s.rng) : 0;
        double at = now + std::max(0.0, s.lDelay + j);
        s.lq.push_back({at, std::move(w.b)});
    } else {
        SendRaw(0x10, w.b);
    }
}

// ---- receiving ----
void ResetRemote(Remote& r) {
    std::deque<uint16_t> old = std::move(r.oldSess);
    r = Remote{};
    r.oldSess = std::move(old);
}

void UpdateTimeline(Remote& r, double ts, double tr) {
    double o = tr - ts;
    r.offs.push_back({tr, o});
    while (!r.offs.empty() && r.offs.front().first < tr - kWindowMs) r.offs.pop_front();
    double B = 1e300;
    for (auto& p : r.offs) B = std::min(B, p.second);
    std::vector<double> dev;
    dev.reserve(r.offs.size());
    for (auto& p : r.offs) dev.push_back(p.second - B);
    size_t k = (size_t)std::floor(0.95 * (double)(dev.size() - 1));
    std::nth_element(dev.begin(), dev.begin() + k, dev.end());
    r.B = B;
    r.J = dev[k];
    double Dstar = Clamp(r.J + r.sendIv + 8.0, 25.0, 300.0);
    if (!r.bInit || Dstar > r.D) r.D = Dstar;
    else r.D = std::max(Dstar, r.D - 10.0 * (tr - r.Dt) / 1000.0);
    r.Dt = tr;
    r.bInit = true;
}

void DeliverSureInOrder(Remote& r) {
    for (;;) {
        auto it = r.sureBuf.find(r.sureNext);
        if (it == r.sureBuf.end()) break;
        PendEv e = std::move(it->second);
        r.sureBuf.erase(it);
        r.sureNext++;
        if (!e.text.empty() && (uint8_t)e.text[0] == 0x01) {  // string definition, handled here
            size_t i = 1;
            while (i < e.text.size() && e.text[i] == ' ') ++i;
            size_t j = i;
            while (j < e.text.size() && isdigit((unsigned char)e.text[j])) ++j;
            if (j > i) {
                int id = atoi(e.text.substr(i, j - i).c_str());
                r.strings[id] = j < e.text.size() ? e.text.substr(j + 1) : std::string();
            }
            continue;
        }
        r.sureQ.push_back(std::move(e));
    }
}

void ProcessGame(const uint8_t* d, size_t n, int slot, double tr) {
    S& s = g().s;
    R rd(d, n);
    if (rd.u8() != kGver) return;
    rd.u8();
    uint16_t session = rd.u16(), seq = rd.u16();
    uint32_t tSend = rd.u32();
    if (!rd.ok) return;
    Remote& r = s.rem[slot];
    bool fresh = !r.has;
    if (r.has && session != r.session) {
        if (std::find(r.oldSess.begin(), r.oldSess.end(), session) != r.oldSess.end()) return;  // stale
        uint16_t prev = r.session;
        ResetRemote(r);
        r.oldSess.push_back(prev);
        if (r.oldSess.size() > 8) r.oldSess.pop_front();
        fresh = true;  // it restarted: its receiver of us starts over too
    }
    if (fresh) PeerReset(slot);
    r.has = true;
    r.session = session;
    // sequence / loss
    if (!r.seqInit) { r.seqInit = true; r.maxSeq = seq; r.expected = 1; r.received = 1; }
    else {
        int dd = D16(seq, r.maxSeq);
        if (dd > 0) { r.expected += dd; r.maxSeq = seq; r.received++; }
        else if (dd < 0) r.received++;
    }
    // unwrap sender clock; pick the 2^32 multiple that puts it nearest to our clock
    double ts64;
    if (!r.tsInit) {
        double k = std::round((tr - (double)tSend) / 4294967296.0);
        ts64 = (double)tSend + k * 4294967296.0;
        r.tsInit = true;
        r.lastTs64 = ts64;
    } else {
        ts64 = r.lastTs64 + (double)D32(tSend, Ms32(r.lastTs64));
        if (ts64 > r.lastTs64) r.lastTs64 = ts64;
    }
    if (!r.echoHas || D32(tSend, r.echoT) > 0) { r.echoHas = true; r.echoT = tSend; r.echoAt = tr; }
    r.lastRecv = tr;
    r.rxT.push_back(tr);
    while (!r.rxT.empty() && r.rxT.front() < tr - 1000) r.rxT.pop_front();

    int mySlot = s.loop ? 1 : s.room.you;
    while (rd.left() >= 3) {
        uint8_t type = rd.u8();
        uint16_t len = rd.u16();
        if (rd.left() < len) break;
        R sec(rd.p + rd.i, len);
        rd.i += len;
        if (type == 1) {
            if (len < 49) continue;
            uint16_t age = sec.u16();
            State st;
            for (int i = 0; i < 3; ++i) st.pos[i] = sec.f32();
            for (int i = 0; i < 3; ++i) st.vel[i] = (float)(int16_t)sec.u16();
            for (int i = 0; i < 2; ++i) st.acc[i] = (float)(int16_t)sec.u16() / 10000.f;
            st.yaw = (float)(sec.u16() * (360.0 / 65536.0));
            st.mode = sec.u8(); st.flags = sec.u8();
            st.hp = sec.u16() / 1000.f; st.posture = sec.u16() / 1000.f;
            st.montage = sec.u16();
            st.montPos = sec.u16() / 1000.f;
            st.montRate = (float)(int8_t)sec.u8() / 32.f;
            st.armed = sec.u8();
            for (int i = 0; i < 7; ++i) st.anim[i] = sec.u8();
            sec.u8(); sec.u8(); sec.u8();
            st.extLen = sec.u8();
            if (len != 49 + st.extLen) continue;
            if (age > 1000) continue;  // a stale state (the age saturates at 65535) would wreck the timeline
            memcpy(st.ext, sec.p + sec.i, st.extLen);
            double ts = ts64 - age;
            auto it = std::lower_bound(r.samples.begin(), r.samples.end(), ts, [](const Smp& a, double t) { return a.ts < t; });
            if (it != r.samples.end() && it->ts == ts) continue;  // duplicate
            bool newest = it == r.samples.end();
            r.samples.insert(it, Smp{ts, st});
            while (r.samples.size() > 2 && r.samples.front().ts < r.samples.back().ts - kWindowMs) r.samples.pop_front();
            if (newest) {
                if (r.lastSampleTs > -1e17) {
                    double iv = ts - r.lastSampleTs;
                    if (iv > 0 && iv < 1000) r.sendIv = Clamp(r.sendIv * 0.95 + iv * 0.05, 4.0, 100.0);
                }
                r.lastSampleTs = ts;
            }
            UpdateTimeline(r, ts, tr);
        } else if (type == 2 || type == 3) {
            if (len < 4) continue;
            uint16_t id = sec.u16(), age = sec.u16();
            std::string text((const char*)sec.p + 4, len - 4);
            double te = ts64 - age;
            if (type == 2) {
                int dd = D16(id, r.sureNext);
                if (dd < 0 || dd > 4096) continue;
                if (!r.sureBuf.count(id)) r.sureBuf.emplace(id, PendEv{te, true, std::move(text)});
                DeliverSureInOrder(r);
            } else {
                if (r.quickSeen.count(id)) continue;
                r.quickSeen.insert(id);
                r.quickSeenT.push_back({tr, id});
                while (!r.quickSeenT.empty() && r.quickSeenT.front().first < tr - 10000) {
                    r.quickSeen.erase(r.quickSeenT.front().second);
                    r.quickSeenT.pop_front();
                }
                double due = te + r.B + r.D;
                if (r.bInit && tr - due > kQuickLateMs) continue;  // late
                r.quickQ.push_back(PendEv{te, false, std::move(text)});
            }
        } else if (type == 4) {
            if (len < 13) continue;
            int aslot = sec.u8();
            uint32_t tEcho = sec.u32();
            uint16_t hold = sec.u16(), next = sec.u16();
            uint32_t bits = sec.u32();
            if (aslot != mySlot) continue;
            double rs = (double)D32(Ms32(tr), tEcho) - hold;
            if (rs >= 0 && rs < 10000) r.rtt = r.rtt < 0 ? rs : r.rtt * 0.875 + rs * 0.125;
            if (tr - s.sessionAt < 500) continue;          // may be an ACK for our previous session
            if (D16(next, s.sureId) > 0) continue;         // acks ids we never sent: stale
            if (slot < 0 || slot >= 32) continue;
            uint32_t bit = 1u << slot;
            for (auto& e : s.osure) {
                int dd = D16(e.id, next);
                if (dd < 0 || (dd >= 1 && dd <= 32 && (bits >> (dd - 1)) & 1)) e.acked |= bit;
            }
        } else if (type == 5) {  // the sender's floor for us: ids below it are never coming
            if (len < 2) continue;
            uint16_t f = sec.u16();
            if (D16(f, r.sureNext) <= 0 || D16(f, r.sureNext) > 4096) continue;
            for (auto it = r.sureBuf.begin(); it != r.sureBuf.end();) it = D16(it->first, f) < 0 ? r.sureBuf.erase(it) : std::next(it);
            r.sureNext = f;
            DeliverSureInOrder(r);
        }
    }
}

void HandleState(const std::string& text) {
    S& s = g().s;
    auto t = Split(text, ' ');
    KV kv = ParseKV(t, 0);
    uint32_t v = (uint32_t)ToInt(kv, "v", 0);
    std::string code = ToStr(kv, "code");
    if (s.haveRoom && code == s.room.code && D32(v, s.room.v) <= 0) return;
    Room rm;
    rm.v = v;
    rm.you = ToInt(kv, "you", -1);
    rm.host = ToInt(kv, "host", -1);
    rm.code = code;
    rm.phase = ToStr(kv, "phase");
    rm.msg = ToStr(kv, "msg");
    rm.round = ToInt(kv, "round", 0);
    rm.rounds = ToInt(kv, "rounds", 1);
    rm.timeSec = ToInt(kv, "time", 0);
    rm.dmg = ToInt(kv, "dmg", 100);
    rm.map = ToInt(kv, "map", 0);
    rm.max = ToInt(kv, "max", 2);
    rm.winner = ToInt(kv, "winner", -1);
    rm.leftMs = ToInt(kv, "left", 0);
    rm.recvAt = Now();
    for (auto& [k, val] : kv) {
        if (k.size() < 2 || k[0] != 'm' || !isdigit((unsigned char)k[1])) continue;
        auto f = Split(val, ',');
        Member m;
        m.slot = atoi(k.c_str() + 1);
        if (f.size() > 0) m.name = PctDec(f[0]);
        if (f.size() > 1) m.ready = PctDec(f[1]) == "1";
        if (f.size() > 2) m.weapon = PctDec(f[2]);
        if (f.size() > 3) m.score = atoi(PctDec(f[3]).c_str());
        if (f.size() > 4) m.alive = PctDec(f[4]) == "1";
        if (f.size() > 5) m.conn = PctDec(f[5]) == "1";
        if (f.size() > 6) m.edge = PctDec(f[6]);  // absent from servers older than the edge key
        if (f.size() > 7) m.mods = PctDec(f[7]);
        rm.members.push_back(std::move(m));
    }
    std::sort(rm.members.begin(), rm.members.end(), [](const Member& a, const Member& b) { return a.slot < b.slot; });
    s.room = std::move(rm);
    s.haveRoom = true;
}

void HandleReply(const std::string& text) {
    S& s = g().s;
    auto t = Split(text, ' ');
    if (t.size() < 2) return;
    uint32_t req = (uint32_t)strtoul(t[0].c_str(), nullptr, 10);
    bool ok = t[1] == "ok";
    KV kv = ParseKV(t, 2);
    auto it = std::find_if(s.pend.begin(), s.pend.end(), [&](const Pending& p) { return p.req == req; });
    if (it == s.pend.end()) return;
    std::string cmd = it->cmd;
    s.pend.erase(it);
    if (cmd == "create" || cmd == "join") {
        if (s.st != Status::Connecting) return;
        if (ok) {
            s.token = (uint32_t)strtoul(ToStr(kv, "token").c_str(), nullptr, 10);
            s.room = Room{};
            s.room.code = ToStr(kv, "code");
            s.room.you = ToInt(kv, "slot", -1);
            s.haveRoom = false;
            s.st = Status::InRoom;
            s.lastSrvRecv = NowMs();
            s.lastPing = -1e18;
            s.active = true;
            s.rem.clear();
            NewSession();
        } else {
            s.st = Status::Failed;
            s.err = ToStr(kv, "reason");
        }
    } else if (cmd == "ping" && s.resuming) {  // the server still knows our token: back in the room
        if (s.st != Status::Connecting) return;
        s.resuming = false;
        if (!ok) { ResetRoom(); s.st = Status::Failed; s.err = "expired"; return; }
        s.st = Status::InRoom;
        s.lastSrvRecv = NowMs();
        s.lastPing = -1e18;
        s.active = true;
        s.rem.clear();
        NewSession();
    } else if (cmd == "ping") {
        if (ok) s.srvRtt = (int)D32(Ms32(NowMs()), (uint32_t)strtoul(ToStr(kv, "t").c_str(), nullptr, 10));
    } else if (!ok) {
        s.err = ToStr(kv, "reason");
    } else if (s.st == Status::InRoom) {
        s.err.clear();  // a later request went through: the old error is moot
    }
}

void HandleDgram(const uint8_t* d, int n, double tr) {
    S& s = g().s;
    if (n < 8 || d[0] != kMagic || d[1] != kProto) return;
    uint8_t type = d[2], slot = d[3];
    std::string body((const char*)d + 8, n - 8);
    if (s.loop) return;
    if (s.st == Status::InRoom) s.lastSrvRecv = tr;
    if (type == 0x02) HandleReply(body);
    else if (type == 0x03) { if (s.st == Status::InRoom) HandleState(body); }
    else if (type == 0x11) {
        if (s.st == Status::InRoom && s.active && slot != s.room.you) ProcessGame(d + 8, n - 8, slot, tr);
    }
}

void Tick() {
    S& s = g().s;
    double now = NowMs();
    // loopback deliveries
    if (s.loop && !s.lq.empty()) {
        std::sort(s.lq.begin(), s.lq.end(), [](const LPkt& a, const LPkt& b) { return a.at < b.at; });
        size_t k = 0;
        while (k < s.lq.size() && s.lq[k].at <= now) ++k;
        std::vector<LPkt> due(std::make_move_iterator(s.lq.begin()), std::make_move_iterator(s.lq.begin() + k));
        s.lq.erase(s.lq.begin(), s.lq.begin() + k);
        for (auto& p : due) ProcessGame((const uint8_t*)p.data.data(), p.data.size(), 1, NowMs());
        now = NowMs();
    }
    // lobby requests
    if (!s.loop) {
        for (size_t i = 0; i < s.pend.size();) {
            Pending& p = s.pend[i];
            bool joinLike = p.cmd == "create" || p.cmd == "join";
            if (!s.resolved) { ++i; continue; }
            if (now >= p.next) {
                if (p.tries >= p.maxTries) {
                    if (joinLike) { if (s.st == Status::Connecting) { s.st = Status::Failed; s.err = "timeout"; } }
                    else if (p.cmd == "ping" && s.resuming && s.st == Status::Connecting) {  // token unknown by now
                        ResetRoom(); s.st = Status::Failed; s.err = "expired";
                    }
                    else if (p.cmd != "ping") s.err = "timeout";
                    s.pend.erase(s.pend.begin() + i);
                    continue;
                }
                SendRaw(0x01, p.text);
                p.tries++;
                p.next = now + 250;
            }
            ++i;
        }
        if (s.st == Status::InRoom) {
            if (now - s.lastPing >= 1000) {
                s.lastPing = now;
                QueueCmd("ping", {{"t", std::to_string(Ms32(now))}}, 1);
                SendRaw(0x01, s.pend.back().text);
                s.pend.back().tries = 1;
                s.pend.back().next = now + 3000;  // reply window
            }
            if (now - s.lastSrvRecv > 15000) {  // the server has dropped us by now
                ResetRoom();
                s.st = Status::Failed;
                s.err = "timeout";
            }
        }
    }
    // game channel
    if (s.active) {
        uint32_t peers = PeerMask();
        while (!s.osure.empty() && (s.osure.front().acked & peers) == peers) s.osure.pop_front();
        // drop acked ones in the middle too (keeps the packets small)
        s.osure.erase(std::remove_if(s.osure.begin(), s.osure.end(), [&](const OutSure& e) { return (e.acked & peers) == peers && peers; }), s.osure.end());
        if (peers == 0) s.osure.clear();
        // still send (ACKs, re-sent SURE) every 20 ms; queued events go out after 4 ms.
        // Frames are sent on a 60 Hz grid (average spacing 16.7 ms, never closer than 4 ms), so
        // game frame jitter does not halve the rate the way a strict per-packet 16.6 ms gap would.
        double since = now - s.lastSend;
        bool slot = now >= s.nextSend;
        if ((s.evPending && since >= 4) || (s.newLocal && slot && since >= 4) || since >= kSendIdle) {
            SendGame();
            if (slot) s.nextSend = std::max(s.nextSend + kSendIv, now - kSendIv);
        }
    }
}

double NextWake() {
    S& s = g().s;
    double now = NowMs(), w = now + (s.steam && (s.rs || s.resolved) ? 4 : 20);  // Steam messages have no event: poll
    if (s.loop) for (auto& p : s.lq) w = std::min(w, p.at);
    if (s.active) {
        w = std::min(w, s.newLocal ? std::max(s.nextSend, s.lastSend + 4) : s.lastSend + kSendIdle);
        if (s.evPending) w = std::min(w, s.lastSend + 4);
    }
    for (auto& p : s.pend) w = std::min(w, p.next);
    return std::max(w, now);
}

// Steam mode I/O (under lock, I/O thread): run the in-process room server, deliver its datagrams to
// our own client, and route P2P datagrams (host: into the server; joiner: from the host only).
void DrainSelf() {
    S& s = g().s;
    while (!s.selfIn.empty()) {
        auto v = std::move(s.selfIn);
        s.selfIn.clear();
        for (auto& d : v) HandleDgram((const uint8_t*)d.data(), (int)d.size(), NowMs());
    }
}
void SteamIo() {
    S& s = g().s;
    if (!s.steam) return;
    if (s.rs) s.rs->Tick(Now());
    DrainSelf();
    std::vector<std::pair<uint64_t, std::string>> in;
    steam::Recv(in);
    for (auto& [peer, d] : in) {
        if (s.rs) s.rs->Handle(peer, (uint32_t)peer, (const uint8_t*)d.data(), d.size(), Now());
        else if (s.resolved && peer == s.hostId) HandleDgram((const uint8_t*)d.data(), (int)d.size(), NowMs());
        DrainSelf();
    }
}

void IoLoop() {
    G& G_ = g();
    std::unique_lock lk(G_.mx);
    std::vector<uint8_t> buf(65536);
    while (!G_.stop) {
        S& s = G_.s;
        if (s.resolveNeeded) {
            s.resolveNeeded = false;
            std::string host = s.host;
            int port = s.port;
            lk.unlock();
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            bool ok = getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res;
            sockaddr_in a{};
            if (ok) { a = *(sockaddr_in*)res->ai_addr; a.sin_port = htons((u_short)port); }
            if (res) freeaddrinfo(res);
            lk.lock();
            if (G_.stop) break;
            if (!G_.s.resolveNeeded && !G_.s.steam) {  // nobody changed the server meanwhile
                G_.s.resolved = ok;
                G_.s.srv = a;
                if (!ok) {
                    auto& p = G_.s.pend;
                    p.erase(std::remove_if(p.begin(), p.end(), [](const Pending& x) { return x.cmd == "create" || x.cmd == "join"; }), p.end());
                    if (G_.s.st == Status::Connecting) { G_.s.st = Status::Failed; G_.s.err = "resolve"; }
                }
            }
            continue;
        }
        if (G_.sock != INVALID_SOCKET) {
            WSAResetEvent(G_.sockEv);
            for (;;) {
                sockaddr_in from{};
                int fl = sizeof from;
                int n = recvfrom(G_.sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
                if (n < 0) {
                    if (WSAGetLastError() == WSAECONNRESET) continue;
                    break;
                }
                if (!s.resolved || s.steam || from.sin_addr.s_addr != s.srv.sin_addr.s_addr || from.sin_port != s.srv.sin_port) continue;
                HandleDgram(buf.data(), n, NowMs());
            }
        }
        SteamIo();
        Tick();
        SteamIo();
        double waitMs = NextWake() - NowMs();
        lk.unlock();
        HANDLE hs[3];
        DWORD nh = 0;
        if (G_.sockEv) hs[nh++] = G_.sockEv;
        hs[nh++] = G_.wakeEv;
        if (waitMs > 0.05) {
            if (G_.timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(waitMs * 10000.0);
                SetWaitableTimer(G_.timer, &due, 0, nullptr, nullptr, FALSE);
                hs[nh++] = G_.timer;
            }
            WaitForMultipleObjects(nh, hs, FALSE, (DWORD)std::ceil(waitMs) + 1);
            if (G_.timer) CancelWaitableTimer(G_.timer);
        }
        lk.lock();
    }
}

Remote* FindRemote(int slot) {
    auto it = g().s.rem.find(slot);
    return it == g().s.rem.end() || !it->second.has ? nullptr : &it->second;
}

#define LOCK std::lock_guard<std::mutex> lk_(g().mx); EnsureThread(); S& s = g().s; (void)s
#define LOBBY_ONLY if (s.loop || s.st != Status::InRoom) return
}  // namespace

// ---------------------------------------------------------------------------------------- lobby
void SetServer(const std::string& host, int port) {
    LOCK;
    if (s.steam) { if (s.st == Status::InRoom || s.st == Status::Connecting) DoLeave(); s.steam = false; steam::LeaveLobby(); }
    s.host = host;
    s.port = port ? port : 27777;
    s.resolveNeeded = true;
    s.resolved = false;
    Wake();
}
void SetPassword(const std::string& password) { LOCK; s.password = password; }
bool SteamAvailable() { return steam::Available(); }
std::string SteamPersona() { return steam::PersonaName(); }
void UseSteam() {
    LOCK;
    if (s.steam) return;
    if (s.st == Status::InRoom || s.st == Status::Connecting) DoLeave();
    s.steam = true;
    s.resolveNeeded = false;
    s.resolved = false;
    s.hostId = 0;
}
bool SteamMode() { LOCK; return s.steam; }
void Create(const std::string& name, int maxPlayers) {
    LOCK;
    if (s.loop) return;
    if (s.st == Status::InRoom || s.st == Status::Connecting) DoLeave();
    s.st = Status::Connecting;
    s.err.clear();
    if (s.steam) {  // we are the room server; our own client is just another peer of it
        uint64_t me = steam::Me();
        if (!me) { s.st = Status::Failed; s.err = "steam"; return; }
        s.rs = std::make_unique<rsrv::Server>([](uint64_t peer, const uint8_t* d, size_t n) {
            S& s = g().s;  // called inside srv->Handle/Tick, lock held
            if (peer == s.hostId) s.selfIn.emplace_back((const char*)d, n);
            else steam::Send(peer, d, n);
        });
        s.steamHost = true;
        s.hostId = me;
        s.resolved = true;
    }
    std::vector<std::pair<std::string, std::string>> kv{{"name", name}, {"ver", "1"}, {"max", std::to_string(maxPlayers)}};
    if (!s.password.empty()) kv.push_back({"pw", s.password});
    QueueCmd("create", kv);
}
void Join(const std::string& code, const std::string& name) {
    LOCK;
    if (s.loop) return;
    if (s.st == Status::InRoom || s.st == Status::Connecting) DoLeave();
    s.st = Status::Connecting;
    s.err.clear();
    if (s.steam) { s.findCode = code; s.hostId = 0; s.resolved = false; }  // SteamPump finds the host first
    std::vector<std::pair<std::string, std::string>> kv{{"code", code}, {"name", name}, {"ver", "1"}};
    if (!s.password.empty()) kv.push_back({"pw", s.password});
    QueueCmd("join", kv, s.steam ? 60 : 20);  // the first P2P datagrams wait for a session (relay) to come up
}
void Resume(const std::string& code, uint32_t token, int slot) {
    LOCK;
    if (s.loop || !token) return;
    if (s.st == Status::InRoom || s.st == Status::Connecting) DoLeave();
    s.st = Status::Connecting;
    s.err.clear();
    s.token = token;
    s.resuming = true;
    s.room = Room{};
    s.room.code = code;
    s.room.you = slot;
    if (s.steam) { s.findCode = code; s.hostId = 0; s.resolved = false; }
    // any packet with a known token reconnects (server touch); the ping reply also brings a STATE
    QueueCmd("ping", {{"t", "0"}}, s.steam ? 60 : 20);
}
bool Session(std::string& code, uint32_t& token, int& slot) {
    LOCK;
    if (s.st != Status::InRoom || !s.token || s.loop || s.steamHost) return false;  // a Steam host's room dies with it
    code = s.room.code; token = s.token; slot = s.room.you;
    return true;
}
void GetServer(std::string& host, int& port) { LOCK; host = s.host; port = s.port; }
bool Resuming() { LOCK; return s.resuming && s.st == Status::Connecting; }
void Leave() { LOCK; DoLeave(); }
void SteamPump() {
    LOCK;
    if (!s.steam) return;
    steam::Pump();
    auto ls = steam::Lobby();
    if (s.steamHost == false && s.lobbyHosted == false && !s.finding && s.st != Status::InRoom && s.st != Status::Connecting &&
        ls != steam::LState::None) { steam::LeaveLobby(); return; }  // the room we hosted or joined is gone
    double now = Now();
    if (s.steamHost) {
        if (s.st == Status::InRoom && !s.lobbyHosted && !s.room.code.empty()) {  // the server gave us the code
            steam::HostLobby(s.room.code, std::max(2, s.room.max));
            s.lobbyHosted = true;
        } else if (s.lobbyHosted && ls == steam::LState::Failed) {  // nobody could find the room
            std::string e = steam::LobbyError();
            DoLeave(); s.st = Status::Failed; s.err = e.empty() ? "steam" : e;
        } else if (ls == steam::LState::Ready && now >= s.nextAccept) {
            // P2P sessions from lobby members only; others never reach the room server
            s.nextAccept = now + 0.25;
            uint64_t me = steam::Me();
            for (uint64_t m : steam::LobbyMembers()) if (m != me) steam::Accept(m);
        }
        return;
    }
    if (!s.findCode.empty() && s.st == Status::Connecting && !s.finding) { steam::FindLobby(s.findCode); s.finding = true; return; }
    if (s.finding) {
        if (ls == steam::LState::Failed) {
            std::string e = steam::LobbyError();
            ResetRoom(); s.st = Status::Failed; s.err = e.empty() ? "steam" : e;
            steam::LeaveLobby();
        } else if (ls == steam::LState::Ready && steam::LobbyOwner()) {
            s.hostId = steam::LobbyOwner(); s.resolved = true; s.finding = false; s.findCode.clear();
            steam::Accept(s.hostId);
            Wake();  // the queued join goes out now
        }
        return;
    }
    // in the room: the lobby owner changes when the host leaves or crashes, and the room went with it
    if (s.st == Status::InRoom && ls == steam::LState::Ready && steam::LobbyOwner() && steam::LobbyOwner() != s.hostId) {
        ResetRoom(); s.st = Status::Failed; s.err = "hostleft";
    }
}
Status GetStatus() { LOCK; return s.st; }
std::string LastError() { LOCK; return s.err; }
bool GetRoom(Room& out) {
    LOCK;
    if (s.st != Status::InRoom || !s.haveRoom) return false;
    out = s.room;
    return true;
}
int ServerRttMs() { LOCK; return s.srvRtt; }
void SetReady(bool ready) { LOCK; LOBBY_ONLY; QueueCmd("set", {{"ready", ready ? "1" : "0"}}); }
void SetWeapon(const std::string& path) { LOCK; LOBBY_ONLY; QueueCmd("set", {{"weapon", path}}); }
void SetEdge(const std::string& id) { LOCK; LOBBY_ONLY; QueueCmd("set", {{"edge", id}}); }
void SetMods(const std::string& list) { LOCK; LOBBY_ONLY; QueueCmd("set", {{"mods", list}}); }
void Conf(int rounds, int timeSec, int dmgPct, int maxPlayers) {
    LOCK; LOBBY_ONLY;
    std::vector<std::pair<std::string, std::string>> kv;
    if (rounds >= 0) kv.push_back({"rounds", std::to_string(rounds)});
    if (timeSec >= 0) kv.push_back({"time", std::to_string(timeSec)});
    if (dmgPct >= 0) kv.push_back({"dmg", std::to_string(dmgPct)});
    if (maxPlayers >= 0) kv.push_back({"max", std::to_string(maxPlayers)});
    if (!kv.empty()) QueueCmd("conf", kv);
}
void ConfMap(int map) { LOCK; LOBBY_ONLY; QueueCmd("conf", {{"map", std::to_string(map)}}); }
void Start() { LOCK; LOBBY_ONLY; QueueCmd("start", {}); }
void ArenaReady(int round) { LOCK; LOBBY_ONLY; QueueCmd("arena", {{"round", std::to_string(round)}}); }
void Dead(int round, int bySlot) { LOCK; LOBBY_ONLY; QueueCmd("dead", {{"round", std::to_string(round)}, {"by", std::to_string(bySlot)}}); }
void Hp(int round, int permille) {
    LOCK; LOBBY_ONLY;
    SendRaw(0x01, CmdText(0, "hp", {{"round", std::to_string(round)}, {"pct", std::to_string(Clamp(permille, 0, 1000))}}));
}

// --------------------------------------------------------------------------------- game channel
uint16_t Intern(const std::string& text) {
    LOCK;
    auto it = s.intern.find(text);
    if (it != s.intern.end()) return it->second;
    if (s.internList.size() >= 65535) return 0;
    s.internList.push_back(text);
    uint16_t id = (uint16_t)s.internList.size();
    s.intern[text] = id;
    if (s.active) { QueueSureLocked(std::string("\x01 ") + std::to_string(id) + " " + text); Wake(); }
    return id;
}
std::string Lookup(int slot, uint16_t id) {
    LOCK;
    Remote* r = FindRemote(slot);
    if (!r) return {};
    auto it = r->strings.find(id);
    return it == r->strings.end() ? std::string() : it->second;
}
void SetLocalState(const State& st) {
    LOCK;
    s.local = st;
    s.localAt = NowMs();
    s.haveLocal = true;
    s.newLocal = true;
    if (s.active) Wake();
}
void Sure(const std::string& text) {
    LOCK;
    if (!s.active) return;
    QueueSureLocked(text);
    Wake();
}
void Quick(const std::string& text) {
    LOCK;
    if (!s.active) return;
    s.oquick.push_back(OutQuick{s.quickId++, NowMs(), text.substr(0, kMaxEvent)});
    s.evPending = true;
    Wake();
}

bool Sample(int slot, State& out, double now) {
    LOCK;
    Remote* rp = FindRemote(slot);
    if (!rp || rp->samples.empty()) return false;
    Remote& r = *rp;
    double nowMs = (now < 0 ? Now() : now) * 1000.0;
    double t = nowMs - r.B - r.D;
    auto& sm = r.samples;
    auto it = std::upper_bound(sm.begin(), sm.end(), t, [](double v, const Smp& a) { return v < a.ts; });
    if (it == sm.begin()) {
        out = sm.front().s;
        r.starving = false;
    } else if (it == sm.end()) {
        const Smp& a = sm.back();
        out = a.s;
        double dt = std::min(t - a.ts, kExtrapMs) / 1000.0;
        for (int i = 0; i < 3; ++i) out.pos[i] = a.s.pos[i] + a.s.vel[i] * (float)dt;
        if (t > a.ts && !r.starving) { r.starves++; r.starving = true; }
    } else {
        const Smp& a = *(it - 1);
        const Smp& b = *it;
        double h = b.ts - a.ts, u = (t - a.ts) / h;
        double u2 = u * u, u3 = u2 * u;
        double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        double hs = h / 1000.0;
        out = a.s;
        for (int i = 0; i < 3; ++i) {
            out.pos[i] = (float)(h00 * a.s.pos[i] + h10 * hs * a.s.vel[i] + h01 * b.s.pos[i] + h11 * hs * b.s.vel[i]);
            out.vel[i] = (float)(a.s.vel[i] + (b.s.vel[i] - a.s.vel[i]) * u);
        }
        double dy = std::fmod((double)b.s.yaw - a.s.yaw + 540.0, 360.0) - 180.0;
        double y = std::fmod(a.s.yaw + dy * u + 360.0, 360.0);
        out.yaw = (float)y;
        out.hp = (float)(a.s.hp + (b.s.hp - a.s.hp) * u);
        out.posture = (float)(a.s.posture + (b.s.posture - a.s.posture) * u);
        r.starving = false;
    }
    if (out.montage && !r.strings.count(out.montage)) out.montage = 0;
    return true;
}

bool PollEvent(Event& out) {
    LOCK;
    double now = NowMs();
    Remote* best = nullptr;
    int bestSlot = -1;
    bool bestSure = false;
    size_t bestIdx = 0;
    double bestDue = 1e300;
    for (auto& [slot, r] : s.rem) {
        if (!r.has) continue;
        if (!r.sureQ.empty()) {
            double due = r.sureQ.front().te + r.B + r.D;
            if (due <= now && due < bestDue) { best = &r; bestSlot = slot; bestSure = true; bestDue = due; }
        }
        for (size_t i = 0; i < r.quickQ.size(); ++i) {
            double due = r.quickQ[i].te + r.B + r.D;
            if (due <= now && due < bestDue) { best = &r; bestSlot = slot; bestSure = false; bestIdx = i; bestDue = due; }
        }
    }
    if (!best) return false;
    PendEv e;
    if (bestSure) { e = std::move(best->sureQ.front()); best->sureQ.pop_front(); }
    else { e = std::move(best->quickQ[bestIdx]); best->quickQ.erase(best->quickQ.begin() + bestIdx); }
    out.slot = bestSlot;
    out.sure = bestSure;
    out.at = bestDue / 1000.0;
    out.text = std::move(e.text);
    return true;
}

Stats GetStats(int slot) {
    LOCK;
    Stats st;
    Remote* r = FindRemote(slot);
    if (!r) return st;
    double now = NowMs();
    st.rttMs = r->rtt < 0 ? -1 : (int)std::lround(r->rtt);
    st.baseMs = (float)r->B;
    st.jitterMs = (float)r->J;
    st.delayMs = (float)r->D;
    int rx = 0;
    for (double t : r->rxT) if (t >= now - 1000) ++rx;
    st.rxPerSec = (float)rx;
    st.lossPct = r->expected > 0 ? (float)std::max(0.0, 100.0 * (1.0 - (double)r->received / (double)r->expected)) : 0.f;
    st.starves = r->starves;
    return st;
}

std::vector<int> RemoteSlots() {
    LOCK;
    std::vector<int> v;
    double now = NowMs();
    for (auto& [slot, r] : s.rem)
        if (r.has && r.lastRecv >= now - 5000) v.push_back(slot);
    return v;
}

bool Loopback(int delayMs, int jitterMs, float lossPct) {
    LOCK;
    if (s.st == Status::InRoom || s.st == Status::Connecting || s.loop) DoLeave();
    s.loop = true;
    s.lDelay = std::max(0, delayMs);
    s.lJit = std::max(0, jitterMs);
    s.lLoss = Clamp(lossPct, 0.f, 100.f);
    s.active = true;
    s.rem.clear();
    NewSession();
    return true;
}

void Shutdown() {
    G& G_ = g();
    {
        std::lock_guard<std::mutex> lk(G_.mx);
        if (G_.s.steam) steam::LeaveLobby();
        if (!G_.running) { G_.s = S{}; return; }
        if (G_.s.token && G_.s.resolved) DoLeave();
        G_.stop = true;
        SetEvent(G_.wakeEv);
    }
    if (G_.th.joinable()) G_.th.join();
    std::lock_guard<std::mutex> lk(G_.mx);
    if (G_.sock != INVALID_SOCKET) closesocket(G_.sock);
    if (G_.sockEv) WSACloseEvent(G_.sockEv);
    if (G_.wakeEv) CloseHandle(G_.wakeEv);
    if (G_.timer) CloseHandle(G_.timer);
    G_.sock = INVALID_SOCKET;
    G_.sockEv = G_.wakeEv = G_.timer = nullptr;
    G_.running = false;
    G_.stop = false;
    G_.s = S{};
    WSACleanup();
}
}  // namespace net

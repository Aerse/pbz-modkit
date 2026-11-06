// nettest: link tests for mp/net (docs/pvp-netcode.md "链路测试").
//   nettest.exe                 run everything (loopback local, loopback bad link, lobby if server exists)
//   nettest.exe loop <delayMs> <jitterMs> <lossPct> <seconds>
//   nettest.exe lobby           two-process lobby test against server/pbzmp_server.py
//   nettest.exe client <port> <code> <seconds> [epoch]   (child process used by the lobby test)
//   nettest.exe state           STATE v2 decoding and send rate against a fake server (port PBZMP_FAKE_PORT, 27805)
//   nettest.exe spar ...        sparring partner, see Spar below
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "net.h"
#include "rules.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

// ---- synthetic target: piecewise segments, exact position/velocity for any time -----------
// Pattern (repeats every 10 s): circle r=300 cm at 6 m/s, straight line with sudden direction
// changes at 6 m/s, hard stop, dash bursts at 15 m/s.
struct Truth { double p[3], v[3]; };
Truth TargetCycle(double t) {
    Truth o{};
    o.p[2] = 90;
    double ox = 0;
    if (t < 4.0) {  // circle
        double w = 600.0 / 300.0;  // rad/s
        double a = w * t;
        o.p[0] = ox + 300 * std::cos(a); o.p[1] = 300 * std::sin(a);
        o.v[0] = -600 * std::sin(a); o.v[1] = 600 * std::cos(a);
        return o;
    }
    double x = ox + 300 * std::cos(8.0), y = 300 * std::sin(8.0);  // end of circle
    struct Seg { double dur, vx, vy; };
    static const Seg segs[] = {
        {0.5, 600, 0},  {0.4, -600, 0}, {0.4, 0, 600}, {0.3, 424, -424}, {0.4, -600, 0},
        {0.5, 0, 0},                                        // hard stop
        {0.25, 1500, 0}, {0.2, 0, 0}, {0.25, 0, -1500}, {0.2, 0, 0},  // dashes
        {0.3, -1500, 0}, {0.6, 600, 600}, {0.4, 0, -600}, {0.3, 0, 0}, {1.1, 0, 0}};
    double u = t - 4.0;
    for (const Seg& s : segs) {
        if (u <= s.dur) {
            o.p[0] = x + s.vx * u; o.p[1] = y + s.vy * u; o.v[0] = s.vx; o.v[1] = s.vy;
            return o;
        }
        x += s.vx * s.dur; y += s.vy * s.dur; u -= s.dur;
    }
    o.p[0] = x; o.p[1] = y;
    return o;
}
// Continuous in position across cycles: each cycle starts where the previous one ended.
Truth Target(double tSec) {
    const double T = 10.0;
    double cycle = std::floor(tSec / T);
    Truth o = TargetCycle(tSec - cycle * T);
    Truth a = TargetCycle(0), b = TargetCycle(T - 1e-9);
    for (int i = 0; i < 2; ++i) o.p[i] += cycle * (b.p[i] - a.p[i]);
    return o;
}

// ---- helpers --------------------------------------------------------------------------
HANDLE g_timer;
void SleepMs(double ms) {
    if (ms <= 0) return;
    if (!g_timer) g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    LARGE_INTEGER d; d.QuadPart = -(LONGLONG)(ms * 10000);
    SetWaitableTimer(g_timer, &d, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(g_timer, INFINITE);
}
double Pct(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[(size_t)std::floor(q * (double)(v.size() - 1))];
}
bool g_allOk = true;
void Check(const char* what, bool ok, const char* fmt, ...) {
    char buf[256];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    printf("  [%s] %-34s %s\n", ok ? "PASS" : "FAIL", what, buf);
    if (!ok) g_allOk = false;
}

net::State MakeState(double t0, double now, uint16_t montId) {
    Truth tr = Target(now - t0);
    net::State s;
    for (int i = 0; i < 3; ++i) { s.pos[i] = (float)tr.p[i]; s.vel[i] = (float)tr.v[i]; }
    double sp = std::hypot(tr.v[0], tr.v[1]);
    if (sp > 0) { s.acc[0] = (float)(tr.v[0] / sp); s.acc[1] = (float)(tr.v[1] / sp); s.yaw = (float)(std::atan2(tr.v[1], tr.v[0]) * 180 / kPi); }
    s.mode = 1; s.flags = 1 | (sp > 1000 ? 8 : 0);
    s.hp = 0.8f; s.posture = 0.5f; s.montage = montId; s.montPos = 0.25f; s.montRate = 1;
    s.extLen = 4; s.ext[0] = 1; s.ext[1] = 2; s.ext[2] = 3; s.ext[3] = 4;
    return s;
}

// Sender side: one "frame". Sends a SURE every 7 frames and a QUICK every 2 frames.
struct Sender {
    double t0 = 0; uint16_t mont = 0; int frame = 0, sureSent = 0, quickSent = 0;
    std::vector<double> quickAt;
    void Frame(double now) {
        net::SetLocalState(MakeState(t0, now, mont));
        if (frame % 7 == 0) net::Sure("S " + std::to_string(sureSent++) + std::string(40, 'x'));
        if (frame % 2 == 0) { quickAt.push_back(now); net::Quick("Q " + std::to_string(quickSent++)); }
        ++frame;
    }
};

// Receiver side: sample, compare with truth at the playback time, collect events.
struct Receiver {
    int slot = 1; double t0 = 0;
    std::vector<double> err;
    std::vector<double> Ds;
    double worst = 0, worstAt = 0;
    int sureGot = 0, sureNextExpected = 0, quickGot = 0; bool sureOrder = true, montOk = false, extOk = true;
    std::vector<int> quickIds;
    void Frame(double now, bool measure) {
        net::State st;
        if (net::Sample(slot, st, now)) {
            net::Stats s = net::GetStats(slot);
            double r = now - (s.baseMs + s.delayMs) / 1000.0;  // playback time, sender clock (shared QPC)
            if (measure && r > t0 + 0.5) {
                Truth tr = Target(r - t0);
                double e = std::sqrt(std::pow(st.pos[0] - tr.p[0], 2) + std::pow(st.pos[1] - tr.p[1], 2) + std::pow(st.pos[2] - tr.p[2], 2));
                err.push_back(e);
                if (e > worst) { worst = e; worstAt = r - t0; }
                Ds.push_back(s.delayMs);
                if (st.montage && net::Lookup(slot, st.montage) == "/Game/Anim/Test_Montage") montOk = true;
                if (st.extLen != 4 || st.ext[3] != 4) extOk = false;
            }
        }
        net::Event ev;
        while (net::PollEvent(ev)) {
            if (ev.text.size() > 2 && ev.text[0] == 'S') {
                int n = atoi(ev.text.c_str() + 2);
                if (n != sureNextExpected) sureOrder = false;
                sureNextExpected = n + 1;
                ++sureGot;
            } else if (ev.text.size() > 2 && ev.text[0] == 'Q') {
                quickIds.push_back(atoi(ev.text.c_str() + 2));
                ++quickGot;
            }
        }
    }
};

void Report(const char* name, Receiver& rx, Sender& tx, bool bad) {
    net::Stats s = net::GetStats(rx.slot);
    double p95 = Pct(rx.err, 0.95), mx = Pct(rx.err, 1.0), p50 = Pct(rx.err, 0.5);
    printf("%s: samples=%zu err p50=%.2f p95=%.2f max=%.2f cm | rtt=%d B=%.1f J=%.1f D=%.1f (max D %.1f) rx/s=%.0f loss=%.2f%% starves=%d\n",
           name, rx.err.size(), p50, p95, mx, s.rttMs, s.baseMs, s.jitterMs, s.delayMs, Pct(rx.Ds, 1.0), s.rxPerSec, s.lossPct, s.starves);
    // quick: count those sent before the tail (events still in flight at the end don't count)
    std::vector<char> got(tx.quickSent + 1, 0);
    for (int id : rx.quickIds) if (id >= 0 && id <= tx.quickSent) got[id] = 1;
    int considered = 0, arrived = 0;
    for (int i = 0; i < tx.quickSent; ++i) {
        if (tx.quickAt[i] > tx.quickAt.back() - 1.0) break;
        ++considered; arrived += got[i];
    }
    double qPct = considered ? 100.0 * arrived / considered : 0;
    printf("  worst error at target t=%.3f s (cycle t=%.3f)\n", rx.worstAt, std::fmod(rx.worstAt, 10.0));
    printf("  SURE sent=%d delivered=%d inOrder=%d | QUICK sent=%d delivered=%d (on-time window %d/%d = %.3f%%)\n",
           tx.sureSent, rx.sureGot, rx.sureOrder, tx.quickSent, rx.quickGot, arrived, considered, qPct);
    Check("interned montage resolved", rx.montOk, "");
    Check("ext bytes intact", rx.extOk, "");
    if (!bad) {
        Check("p95 position error <= 2 cm", p95 <= 2.0, "%.2f cm", p95);
        Check("D <= 30 ms", Pct(rx.Ds, 1.0) <= 30.0, "max %.1f ms", Pct(rx.Ds, 1.0));
    } else {
        Check("p95 position error <= 10 cm", p95 <= 10.0, "%.2f cm", p95);
        Check("max position error <= 40 cm", mx <= 40.0, "%.2f cm", mx);
        Check("QUICK >= 99.9%", qPct >= 99.9, "%.3f%%", qPct);
    }
    Check("SURE all delivered, in order", rx.sureOrder && rx.sureGot >= tx.sureSent - 60 && rx.sureGot > 0,
          "%d/%d (tail in flight allowed)", rx.sureGot, tx.sureSent);
}

// Frame loop at ~60 fps with +-2 ms frame jitter.
template <class F> void RunFrames(double seconds, F&& f) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> jit(-2.0, 2.0);
    double start = net::Now(), next = start;
    while (net::Now() - start < seconds) {
        f(net::Now());
        next += (1000.0 / 60.0 + jit(rng)) / 1000.0;
        SleepMs((next - net::Now()) * 1000.0);
    }
}

void LoopTest(const char* name, int delay, int jitter, float loss, double seconds, bool bad) {
    net::Loopback(delay, jitter, loss);
    Sender tx; Receiver rx;
    tx.t0 = rx.t0 = net::Now();
    tx.mont = net::Intern("/Game/Anim/Test_Montage");
    RunFrames(seconds, [&](double now) { tx.Frame(now); rx.Frame(now, true); });
    double end = net::Now();  // drain events still on the timeline
    while (net::Now() - end < 1.0) { rx.Frame(net::Now(), false); SleepMs(5); }
    Report(name, rx, tx, bad);
    net::Leave();
}

// ---- lobby test -----------------------------------------------------------------------
std::string ExePath() { char b[MAX_PATH]; GetModuleFileNameA(nullptr, b, MAX_PATH); return b; }
std::string RepoRoot() {  // exe is in <repo>/out
    std::string p = ExePath();
    for (int i = 0; i < 2; ++i) p = p.substr(0, p.find_last_of("\\/"));
    return p;
}
PROCESS_INFORMATION Spawn(const std::string& cmd) {
    STARTUPINFOA si{sizeof si}; PROCESS_INFORMATION pi{};
    std::string c = cmd;
    if (!CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) printf("spawn failed: %s\n", cmd.c_str());
    return pi;
}
bool WaitFor(double sec, const std::function<bool()>& pred);
}  // namespace

#include <functional>
namespace {
bool WaitFor(double sec, const std::function<bool()>& pred) {
    double t = net::Now();
    while (net::Now() - t < sec) { if (pred()) return true; SleepMs(10); }
    return pred();
}
std::string Phase() { net::Room r; return net::GetRoom(r) ? r.phase : std::string(); }

const char* kTestPw = "pw t%1";  // the lobby test runs the server with --password

int Client(int port, const std::string& code, double seconds, double epoch) {
    net::SetServer("127.0.0.1", port);
    net::SetPassword(kTestPw);
    net::Intern("/Game/Anim/Played_Before");  // a game that already played a match: its string table is resent on join
    net::Join(code, "child \xE4\xB9\x99 b,c");
    if (!WaitFor(5, [] { return net::GetStatus() == net::Status::InRoom && !Phase().empty(); })) { printf("child: join failed %s\n", net::LastError().c_str()); return 2; }
    net::SetMods("child.mod:1.0:a;x y:2:a");  // the host gets it through STATE modh + "mods slot="
    net::SetWeapon("/Game/Weapons/Sword A");
    net::SetReady(true);
    Sender tx; tx.t0 = epoch;  // epoch from the parent (same machine QPC clock)
    tx.mont = net::Intern("/Game/Anim/Test_Montage");
    bool arena = false, hostMont = false;
    Receiver rx; rx.t0 = epoch; rx.slot = -1;  // the other direction: the host's SURE and montage reach us
    RunFrames(seconds, [&](double now) {
        net::Room r;
        if (net::GetRoom(r)) {
            if (r.phase == "load" && !arena) { net::ArenaReady(r.round); arena = true; }
            if (r.phase == "fight") net::Hp(r.round, 900);
            if (rx.slot < 0 && r.host != r.you) rx.slot = r.host;
        }
        tx.Frame(now);
        if (rx.slot >= 0) {
            rx.Frame(now, false);
            net::State hs;
            if (net::Sample(rx.slot, hs, now) && hs.montage && net::Lookup(rx.slot, hs.montage) == "/Game/Anim/Host_Montage") hostMont = true;
        }
    });
    printf("child: sent sure=%d quick=%d | from host: sure=%d inOrder=%d montage=%d\n", tx.sureSent, tx.quickSent, rx.sureGot, rx.sureOrder, hostMont);
    net::Leave();
    net::Shutdown();
    return rx.sureOrder && rx.sureGot > 0 && hostMont ? 0 : 3;
}

// ---- STATE v2 test against a fake server -------------------------------------------------
// A UDP socket on 127.0.0.1 plays the server: it answers create/ping/mods/leave and sends hand-written
// STATE lines, so the client's v2 decoding (escaping, the new room and member fields, the "mods slot="
// query when modh changes) and its send rate (60 Hz with a redundant sample, 30 Hz without one above 4
// members) are checked byte for byte, independent of either server implementation.
constexpr uint32_t kFakeTok = 4242;
std::string FakeDec(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o.push_back((char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2;
        } else o.push_back(s[i]);
    }
    return o;
}
std::string FakeEnc(const std::string& s) {  // CTRL / REPLY rule: everything but A-Z a-z 0-9 - _ . ~
    std::string o; char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back((char)c);
        else { snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
struct FakeSrv {
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in cli{};
    bool haveCli = false, dirty = false;
    std::mutex mu;
    std::string state;                // current STATE text: sent when it changes and with every ping reply
    std::map<int, std::string> mods;  // answers to "mods slot="
    std::vector<std::string> ctrl;    // every CTRL received: "cmd k=v ..." (decoded, sorted keys, no pad)
    int game = 0, gameMulti = 0, badTok = 0;  // GAME datagrams, those with 2+ state sections, wrong tokens
    std::atomic<bool> stop{false};
    std::thread th;
    bool Start(int port) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (sock == INVALID_SOCKET || bind(sock, (sockaddr*)&a, sizeof a)) return false;
        th = std::thread([this] { Run(); });
        return true;
    }
    void Stop() {
        stop = true;
        if (th.joinable()) th.join();
        if (sock != INVALID_SOCKET) closesocket(sock);
        WSACleanup();
    }
    void SetState(const std::string& s) { std::lock_guard<std::mutex> g(mu); state = s; dirty = true; }
    int Count(const std::string& line) { std::lock_guard<std::mutex> g(mu); return (int)std::count(ctrl.begin(), ctrl.end(), line); }
    bool Has(const std::string& prefix, const std::string& part) {
        std::lock_guard<std::mutex> g(mu);
        for (auto& c : ctrl) if (c.rfind(prefix, 0) == 0 && c.find(part) != std::string::npos) return true;
        return false;
    }
    void Send(uint8_t type, const std::string& text) {  // under mu
        if (!haveCli) return;
        std::string p{(char)0xB7, 1, (char)type, 0, 0, 0, 0, 0};
        p += text;
        sendto(sock, p.data(), (int)p.size(), 0, (const sockaddr*)&cli, sizeof cli);
    }
    void Handle(uint8_t type, uint32_t tok, const std::string& body) {  // under mu
        if (type == 0x10) {
            if (tok != kFakeTok) ++badTok;
            ++game;
            int states = 0;
            for (size_t p = 10; p + 3 <= body.size();) {  // gver, 0, session, seq, t, then type/len sections
                uint8_t t = (uint8_t)body[p];
                size_t len = (uint8_t)body[p + 1] | (size_t)(uint8_t)body[p + 2] << 8;
                if (t == 1) ++states;
                p += 3 + len;
            }
            if (states > 1) ++gameMulti;
            return;
        }
        if (type != 0x01) return;
        std::vector<std::string> t;
        for (size_t a = 0;;) {
            size_t b = body.find(' ', a);
            t.push_back(body.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (t.size() < 2) return;
        std::map<std::string, std::string> kv;
        for (size_t i = 2; i < t.size(); ++i) {
            size_t e = t[i].find('=');
            if (e != std::string::npos) kv[t[i].substr(0, e)] = FakeDec(t[i].substr(e + 1));
        }
        const std::string& cmd = t[1];
        if (cmd != "create" && cmd != "join" && tok != kFakeTok) ++badTok;
        std::string line = cmd;
        for (auto& [k, v] : kv) if (k != "pad") line += " " + k + "=" + v;
        ctrl.push_back(line);
        std::string r = "ok";
        if (cmd == "create" || cmd == "join") { r = "ok code=ABCD slot=0 token=" + std::to_string(kFakeTok); dirty = true; }
        else if (cmd == "ping") { r = "ok t=" + kv["t"]; dirty = true; }
        else if (cmd == "mods") {
            int sl = atoi(kv["slot"].c_str());
            auto it = mods.find(sl);
            r = it == mods.end() ? "err reason=value" : "ok slot=" + std::to_string(sl) + " mods=" + FakeEnc(it->second);
        }
        if (t[0] != "0") Send(0x02, t[0] + " " + r);
    }
    void Run() {
        char b[2048];
        while (!stop) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(sock, &rd);
            timeval tv{0, 5000};
            if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
                sockaddr_in from{};
                int fl = sizeof from;
                int n = recvfrom(sock, b, sizeof b, 0, (sockaddr*)&from, &fl);
                if (n >= 8 && (uint8_t)b[0] == 0xB7 && b[1] == 1) {
                    std::lock_guard<std::mutex> g(mu);
                    cli = from; haveCli = true;
                    uint32_t tok; memcpy(&tok, b + 4, 4);
                    Handle((uint8_t)b[2], tok, std::string(b + 8, (size_t)n - 8));
                }
            }
            std::lock_guard<std::mutex> g(mu);
            if (dirty && haveCli && !state.empty()) { Send(0x03, state); dirty = false; }
        }
    }
};

void StateTest() {
    const char* pe = getenv("PBZMP_FAKE_PORT");  // fixed port for the fake server, default 27805
    int port = pe && atoi(pe) > 0 ? atoi(pe) : 27805;
    printf("state v2: fake server on port %d\n", port);
    FakeSrv f;
    if (!f.Start(port)) { Check("fake server bind", false, "port %d", port); f.Stop(); return; }
    const std::string mods1 = "evil.trainer:1.0:b;x y:2:a", mods2 = "y:3:a";
    // "甲 a,b=c%d" with STATE escaping (UTF-8 raw), then "乙" percent-encoded: both must decode
    const std::string nameEsc = "\xE7\x94\xB2%20a%2Cb%3Dc%25d%E4%B9%99", name = "\xE7\x94\xB2 a,b=c%d\xE4\xB9\x99";
    auto room = [](int v, const std::string& phase) {
        return "v=" + std::to_string(v) + " you=0 code=ABCD phase=" + phase + " round=0 rounds=3 time=60 dmg=150 map=2 max=6 host=0"
               " left=0 winner=-1 mode=kill maps=12 respawn=3 kills=7 cap=6 rules=hp:200%2Cstun:4 msg=hi%20there"
               " m0=host,0,,0,1,1,,,0,0";
    };
    auto m1 = [&](const std::string& modh) { return " m1=" + nameEsc + ",1,/Game/W%20A,2,0,1,Edge%2C1," + modh + ",3,4"; };
    auto plain = [](int slot) { return " m" + std::to_string(slot) + "=p" + std::to_string(slot) + ",1,,0,1,1,,,0,0"; };
    { std::lock_guard<std::mutex> g(f.mu); f.mods[1] = mods1; }
    f.SetState(room(5, "lobby") + m1(rules::ModHash(mods1)));
    net::SetServer("127.0.0.1", port);
    net::SetPassword("");
    net::Create("x", 6);
    auto mem = [](int slot, net::Member& out) {
        net::Room r;
        if (!net::GetRoom(r)) return false;
        for (auto& m : r.members) if (m.slot == slot) { out = m; return true; }
        return false;
    };
    bool ok = WaitFor(5, [&] { net::Member m; return mem(1, m) && m.mods == mods1; });
    net::Room r; net::GetRoom(r);
    net::Member a, b;
    mem(0, a); mem(1, b);
    Check("v2 create ver=2", f.Has("create", " ver=2"), "");
    Check("v2 room fields", r.v == 5 && r.phase == "lobby" && r.mode == "kill" && r.maps == 12 && r.respawn == 3 && r.kills == 7 &&
          r.cap == 6 && r.rules == "hp:200,stun:4" && r.msg == "hi there" && r.max == 6 && r.dmg == 150 && r.map == 2 &&
          r.rounds == 3 && r.timeSec == 60, "mode=%s rules=%s msg=%s cap=%d", r.mode.c_str(), r.rules.c_str(), r.msg.c_str(), r.cap);
    Check("v2 name escaping", b.name == name, "'%s'", b.name.c_str());
    Check("v2 member fields", b.ready && b.weapon == "/Game/W A" && b.score == 2 && !b.alive && b.conn && b.edge == "Edge,1" &&
          b.modh == rules::ModHash(mods1) && b.life == 3 && b.deaths == 4, "weapon='%s' edge='%s' life=%d deaths=%d",
          b.weapon.c_str(), b.edge.c_str(), b.life, b.deaths);
    Check("v2 empty modh = no mods", a.slot == 0 && a.modh.empty() && a.mods.empty() && f.Count("mods slot=0") == 0, "");
    Check("v2 mods filled by query", ok && f.Count("mods slot=1") == 1, "mods='%s' queries=%d", b.mods.c_str(), f.Count("mods slot=1"));
    f.SetState(room(4, "load") + m1(rules::ModHash(mods1)));  // older v: ignored
    SleepMs(300);
    Check("v2 older STATE ignored", Phase() == "lobby", "phase=%s", Phase().c_str());
    { std::lock_guard<std::mutex> g(f.mu); f.mods[1] = mods2; }
    f.SetState(room(6, "lobby") + m1(rules::ModHash(mods2)));  // the member changed its list: asked again
    ok = WaitFor(5, [&] { net::Member m; return mem(1, m) && m.mods == mods2; });
    Check("v2 modh change re-queries", ok && f.Count("mods slot=1") == 2, "queries=%d", f.Count("mods slot=1"));
    // send rate: 5 members -> 30 Hz without the redundant sample, 2 members -> 60 Hz with it
    auto rate = [&](double sec, int& multi) {
        SleepMs(300);
        { std::lock_guard<std::mutex> g(f.mu); f.game = 0; f.gameMulti = 0; }
        double t0 = net::Now();
        RunFrames(sec, [&](double now) { net::SetLocalState(MakeState(t0, now, 0)); });
        std::lock_guard<std::mutex> g(f.mu);
        multi = f.gameMulti;
        return f.game / sec;
    };
    int multi = 0;
    f.SetState(room(7, "fight") + m1(rules::ModHash(mods2)) + plain(2) + plain(3) + plain(4));
    double hz = rate(3, multi);
    Check("5 members: ~30 Hz, no prev block", hz >= 26 && hz <= 36 && multi == 0, "%.1f/s, %d with 2 samples", hz, multi);
    Check("same modh: no new query", f.Count("mods slot=1") == 2, "queries=%d", f.Count("mods slot=1"));
    f.SetState(room(8, "fight") + m1(rules::ModHash(mods2)));
    hz = rate(3, multi);
    Check("2 members: ~60 Hz with prev block", hz >= 52 && hz <= 68 && multi > 0, "%.1f/s, %d with 2 samples", hz, multi);
    net::Leave();
    WaitFor(2, [&] { return f.Count("leave") > 0; });
    Check("v2 token on every packet", f.badTok == 0, "%d wrong", f.badTok);
    f.Stop();
}

void LobbyTest() {
    std::string root = RepoRoot(), srv = root + "\\mods\\pvp\\server\\pbzmp_server.py";
    if (GetFileAttributesA(srv.c_str()) == INVALID_FILE_ATTRIBUTES) { printf("lobby: %s not found, UNTESTED\n", srv.c_str()); return; }
    const char* pe = getenv("PBZMP_TEST_PORT");  // fixed port for the spawned server, default 27802
    int port = pe && atoi(pe) > 0 ? atoi(pe) : 27802;
    printf("lobby: server on port %d\n", port);
    PROCESS_INFORMATION ps = Spawn("python -I \"" + srv + "\" --port " + std::to_string(port) + " --password \"" + kTestPw + "\"");
    SleepMs(1500);
    net::SetServer("127.0.0.1", port);
    net::Create("host \xE7\x94\xB2", 2);  // no password yet: must be refused
    bool refused = WaitFor(5, [] { return net::GetStatus() == net::Status::Failed; });
    Check("create without password refused", refused && net::LastError() == "password", "%s", net::LastError().c_str());
    net::SetPassword(kTestPw);
    net::Intern("/Game/Anim/Host_Played_Before");  // the host played a match before too
    net::Create("host \xE7\x94\xB2", 2);
    bool ok = WaitFor(5, [] { return net::GetStatus() == net::Status::InRoom && !Phase().empty(); });
    Check("create", ok, "%s", ok ? "" : net::LastError().c_str());
    net::Room r; net::GetRoom(r);
    printf("  room code=%s you=%d host=%d phase=%s\n", r.code.c_str(), r.you, r.host, r.phase.c_str());
    if (!ok) { TerminateProcess(ps.hProcess, 0); return; }
    // bad join first
    double seconds = 16;
    // Shared epoch for the synthetic target: now, passed to the child. A fixed constant (the old 1000 s)
    // lies in the future when the machine booted less than ~17 minutes ago, and then nothing is measured.
    double epoch = net::Now();
    char eb[32]; snprintf(eb, sizeof eb, "%.6f", epoch);
    PROCESS_INFORMATION pc = Spawn("\"" + ExePath() + "\" client " + std::to_string(port) + " " + r.code + " " + std::to_string((int)seconds) + " " + eb);
    ok = WaitFor(6, [] { net::Room x; return net::GetRoom(x) && x.members.size() == 2 && x.members[1].ready; });
    net::GetRoom(r);
    Check("child joined and ready", ok, "members=%zu", r.members.size());
    if (r.members.size() == 2) printf("  m1 name='%s' weapon='%s'\n", r.members[1].name.c_str(), r.members[1].weapon.c_str());
    Check("member fields decoded", r.members.size() == 2 && r.members[1].name == "child \xE4\xB9\x99 b,c" && r.members[1].weapon == "/Game/Weapons/Sword A", "");
    ok = WaitFor(3, [] { net::Room x; return net::GetRoom(x) && x.members.size() == 2 && x.members[1].mods == "child.mod:1.0:a;x y:2:a"; });
    net::GetRoom(r);
    Check("member mods via modh query", ok && r.members[1].modh == rules::ModHash("child.mod:1.0:a;x y:2:a"), "mods='%s' modh=%s",
          r.members.size() == 2 ? r.members[1].mods.c_str() : "", r.members.size() == 2 ? r.members[1].modh.c_str() : "");
    Check("room v2 fields", r.mode == "duel" && r.cap == 8 && r.members[0].life == 0 && r.members[0].deaths == 0, "mode=%s cap=%d", r.mode.c_str(), r.cap);
    net::Conf(3, 60, 100, -1);
    WaitFor(2, [] { net::Room x; return net::GetRoom(x) && x.rounds == 3; });
    net::Start();
    std::vector<std::string> seen;
    Receiver rx; rx.slot = -1; rx.t0 = epoch;
    Sender htx; htx.t0 = epoch;  // the host sends too: its hits and montages must reach the child
    htx.mont = net::Intern("/Game/Anim/Host_Montage");
    bool arena = false, haveData = false;
    double t = net::Now();
    while (net::Now() - t < seconds - 4) {
        double now = net::Now();
        std::string ph = Phase();
        if (!ph.empty() && (seen.empty() || seen.back() != ph)) { seen.push_back(ph); printf("  phase -> %s (t=%.1f s, rtt %d ms)\n", ph.c_str(), now - t, net::ServerRttMs()); }
        if (ph == "load" && !arena) { net::GetRoom(r); net::ArenaReady(r.round); arena = true; }
        if (rx.slot < 0) { auto sl = net::RemoteSlots(); if (!sl.empty()) rx.slot = sl[0]; }
        if (rx.slot >= 0) { haveData = true; rx.Frame(now, true); }
        htx.Frame(now);
        SleepMs(1000.0 / 60);
    }
    auto has = [&](const char* p) { return std::find(seen.begin(), seen.end(), p) != seen.end(); };
    Check("phases lobby->load->count->fight", has("lobby") && has("load") && has("count") && has("fight"), "");
    Check("GAME data from child", haveData && !rx.err.empty(), "");
    net::Stats s = net::GetStats(rx.slot);
    printf("lobby GAME: samples=%zu err p50=%.2f p95=%.2f max=%.2f cm | rtt=%d B=%.1f J=%.1f D=%.1f rx/s=%.0f loss=%.2f%% starves=%d SURE=%d inOrder=%d QUICK=%d\n",
           rx.err.size(), Pct(rx.err, .5), Pct(rx.err, .95), Pct(rx.err, 1), s.rttMs, s.baseMs, s.jitterMs, s.delayMs, s.rxPerSec, s.lossPct, s.starves, rx.sureGot, rx.sureOrder, rx.quickGot);
    Check("lobby p95 <= 2 cm", Pct(rx.err, .95) <= 2.0, "%.2f cm", Pct(rx.err, .95));
    Check("lobby D <= 30 ms", Pct(rx.Ds, 1.0) <= 30.0, "%.1f ms", Pct(rx.Ds, 1.0));
    Check("lobby SURE in order", rx.sureOrder && rx.sureGot > 0, "");
    Check("lobby montage interned", rx.montOk, "");
    WaitForSingleObject(pc.hProcess, 10000);
    DWORD crc = 99; GetExitCodeProcess(pc.hProcess, &crc);
    Check("child got host SURE + montage", crc == 0, "child exit %lu", crc);
    net::Leave();
    TerminateProcess(ps.hProcess, 0);
    CloseHandle(ps.hProcess); CloseHandle(pc.hProcess);
}

// ---- sparring partner for in-game tests -------------------------------------------------
// nettest spar <port> <pw> <create|code> <seconds> <hitEvery> [codefile|-] [key=value ...]
// A player without a game, on the server at 127.0.0.1:<port>. "create" hosts a room (code written to
// codefile; "-" = none) and starts a match whenever every other member is ready and the room has at
// least <min> members; else it joins room <code> and readies. In a round it stands 180 cm in front of
// its target (the first other member), sends its weapon and Phantom Edge, deals a 10% hit every
// <hitEvery> s of the fight (0 = never), takes the hits the others report and sends dead when its hp
// runs out. In kill mode dead carries its life, and when the server respawns it (life+1 in STATE) it
// gets full hp back, ignores hits for 1.5 s and keeps fighting.
// key=value options (host settings only matter with "create"):
//   max=N (2-8, default 2)  mode=duel|ffa|kill (default duel, ffa when max>2)  rounds=N (3)  time=S (60)
//   kills=N (10)  respawn=S (5)  maps=MASK  min=N (members before the host starts, default 2)
//   name=TEXT (default "spar")  idx=N (name gets the number appended, so several spars can fill a room)
int Spar(int port, const std::string& pw, const std::string& what, double seconds, double hitEvery, const char* codeFile,
         const std::map<std::string, std::string>& opt) {
    auto oi = [&](const char* k, int def) { auto it = opt.find(k); return it == opt.end() ? def : atoi(it->second.c_str()); };
    auto os = [&](const char* k, const std::string& def) { auto it = opt.find(k); return it == opt.end() ? def : it->second; };
    const int maxP = std::max(2, std::min(net::kMaxPlayers, oi("max", 2))), minP = std::max(2, oi("min", 2));
    const std::string mode = os("mode", maxP > 2 ? "ffa" : "duel");
    std::string name = os("name", "spar");
    if (opt.count("idx")) name += std::to_string(oi("idx", 0));
    net::ConfArgs conf;
    conf.rounds = oi("rounds", 3); conf.timeSec = oi("time", 60); conf.dmg = 100; conf.max = maxP; conf.mode = mode;
    if (opt.count("kills")) conf.kills = oi("kills", 10);
    if (opt.count("respawn")) conf.respawn = oi("respawn", 5);
    if (opt.count("maps")) conf.maps = oi("maps", 0);
    net::SetServer("127.0.0.1", port);
    net::SetPassword(pw);
    if (what == "create") net::Create(name, maxP); else net::Join(what, name);
    if (!WaitFor(8, [] { return net::GetStatus() == net::Status::InRoom && !Phase().empty(); })) { printf("spar: %s failed: %s\n", what.c_str(), net::LastError().c_str()); return 2; }
    net::Room r; net::GetRoom(r);
    printf("spar: %s room %s slot %d host %d\n", name.c_str(), r.code.c_str(), r.you, r.host);
    if (codeFile && strcmp(codeFile, "-") != 0) if (FILE* f = fopen(codeFile, "w")) { fputs(r.code.c_str(), f); fclose(f); }
    if (what == "create") net::Conf(conf);
    const std::string weapon = "/PBZCombatAsset/ActorData/Players/Hun/Weapon/Double/Weapon_Hun_Double.Weapon_Hun_Double";
    const std::string edge = "/PBZCombatAsset/ActorData/Players/Hun/ShaqiWeapon/BrutalAxe/Weapon_BrutalAxe.Weapon_BrutalAxe";
    const std::string outfit = "O Hun_OutFit_ZhuiMing 0 1";  // the "O" event of a player in that outfit
    std::string phase, lastW, lastE, lastO;
    int readyRound = -1, sent = 0, got = 0, myLife = 0, respawns = 0;
    double t0 = net::Now(), hp = 1, nextHit = 0, nextW = 0, nextHp = 0, nextLog = 0, nextLobby = 0, pos[3]{}, faceYaw = 0, invulnTill = 0;
    bool placed = false, dead = false, wSent = false;
    RunFrames(seconds, [&](double now) {
        if (!net::GetRoom(r)) { if (phase != "gone") printf("spar: out of the room (%s)\n", net::LastError().c_str()); phase = "gone"; return; }
        int other = -1; bool othersReady = true, meReady = false;
        const net::Member* me = nullptr;
        for (auto& m : r.members) {
            if (m.slot == r.you) { meReady = m.ready; me = &m; continue; }
            if (other < 0) other = m.slot;
            if (m.slot != r.host) othersReady &= m.ready;
        }
        if (r.phase != phase) {
            printf("spar: %6.1f phase %s round %d/%d winner %d score", now - t0, r.phase.c_str(), r.round, r.rounds, r.winner);
            for (auto& m : r.members) printf(" s%d=%d/d%d", m.slot, m.score, m.deaths);
            printf("\n");
            phase = r.phase;
        }
        if (r.phase == "lobby") readyRound = -1;  // a new match starts at round 1 again
        if (r.phase == "lobby" && now >= nextLobby) {
            nextLobby = now + 1.5;
            if (r.you == r.host && (int)r.members.size() >= minP && othersReady) net::Start();
            if (r.you != r.host && !meReady) net::SetReady(true);
            // PBZMP_SPAR_MODS: the "mods" member field to send (e.g. "evil.trainer:1.0:b"); others' lists are printed
            static std::string shownMods;
            if (const char* sm = getenv("PBZMP_SPAR_MODS")) for (auto& m : r.members) if (m.slot == r.you && m.mods != sm) net::SetMods(sm);
            std::string all; for (auto& m : r.members) if (m.slot != r.you) all += " s" + std::to_string(m.slot) + "=" + m.mods;
            if (all != shownMods) { printf("spar: mods%s\n", all.c_str()); shownMods = all; }
        }
        if (r.phase == "load" && r.round != readyRound) {
            net::ArenaReady(r.round); readyRound = r.round;
            hp = 1; dead = false; nextHit = 0; placed = false; wSent = false; myLife = 0; invulnTill = 0;
        }
        if (me && r.phase == "fight" && me->life > myLife) {  // kill mode: the server respawned us
            myLife = me->life; ++respawns;
            hp = 1; dead = false; nextHit = 0; placed = false; invulnTill = now + 1.5;
            printf("spar: %6.1f respawned, life %d\n", now - t0, myLife);
        }
        net::Event e;
        while (net::PollEvent(e)) {
            int victim = -1, from = -1, rnd = -1; float amt = 0, frac = 0, posture = 0, ayaw = 0;
            if (sscanf(e.text.c_str(), "hit %d %f %d %f %d %f %f", &victim, &amt, &from, &frac, &rnd, &posture, &ayaw) >= 5) {
                if (victim != r.you || r.phase != "fight" || rnd != r.round || dead || now < invulnTill) { printf("spar: ignored %s\n", e.text.c_str()); continue; }
                hp -= frac > 0 ? frac : amt / 1000; ++got;
                printf("spar: %6.1f took %s -> hp %.2f\n", now - t0, e.text.c_str(), hp);
                // measure (v1.5): hits < 0.8 s apart form one chain
                static double lastTook = -100, chainT0 = 0; static int chainN = 0, chainMax = 0;
                if (now - lastTook >= 0.8) { chainN = 0; chainT0 = now; }
                ++chainN; chainMax = std::max(chainMax, chainN);
                printf("spar: chain n=%d gap=%.3f len=%.3f max=%d\n", chainN, now - lastTook < 99 ? now - lastTook : -1.0, now - chainT0, chainMax);
                lastTook = now;
                if (hp <= 0.001) {
                    hp = 0; dead = true;
                    net::Dead(r.round, from, r.mode == "kill" ? myLife : -1);
                    printf("spar: dead by %d (life %d)\n", from, myLife);
                }
            } else if (e.text.rfind("W ", 0) == 0) {
                if (e.text != lastW) printf("spar: slot %d weapon %s\n", e.slot, e.text.c_str() + 2);
                lastW = e.text;
            } else if (e.text.rfind("E ", 0) == 0) {
                if (e.text != lastE) printf("spar: slot %d edge %s\n", e.slot, e.text.c_str() + 2);
                lastE = e.text;
            } else if (e.text.rfind("O ", 0) == 0) {
                if (e.text != lastO) printf("spar: slot %d outfit %s\n", e.slot, e.text.c_str() + 2);
                lastO = e.text;
            } else printf("spar: event from %d: %.80s\n", e.slot, e.text.c_str());
        }
        bool match = r.phase == "load" || r.phase == "count" || r.phase == "fight" || r.phase == "end";
        net::State ps;
        if (!match || other < 0) return;
        if (!net::Sample(other, ps)) {  // nothing from the target yet (another spar waits for us too): stand near the origin
            ps = net::State{};
            ps.pos[0] = 200.f * (float)(other % 4); ps.pos[1] = 200.f * (float)(other / 4); ps.pos[2] = 90.f;
        }
        double py = ps.yaw * kPi / 180, want[3] = {ps.pos[0] + 180 * std::cos(py), ps.pos[1] + 180 * std::sin(py), ps.pos[2]};
        double v[3]{};
        if (!placed) { for (int i = 0; i < 3; ++i) pos[i] = want[i]; placed = true; }
        else if (!dead) {
            double dx = want[0] - pos[0], dy = want[1] - pos[1], d = std::hypot(dx, dy), step = 450.0 / 60;
            if (d > 1) { double k = std::min(1.0, step / d); pos[0] += dx * k; pos[1] += dy * k; v[0] = dx / d * 450 * std::min(1.0, d / 30); v[1] = dy / d * 450 * std::min(1.0, d / 30); }
            pos[2] = want[2];
        }
        faceYaw = std::atan2(ps.pos[1] - pos[1], ps.pos[0] - pos[0]) * 180 / kPi;
        net::State s{};
        for (int i = 0; i < 3; ++i) { s.pos[i] = (float)pos[i]; s.vel[i] = (float)v[i]; }
        s.acc[0] = (float)(v[0] / 450); s.acc[1] = (float)(v[1] / 450);
        s.yaw = (float)faceYaw; s.mode = 1; s.flags = 1 | (dead ? 16 : 0);
        s.hp = (float)hp; s.posture = 1; s.armed = 1;
        s.extLen = 3; s.ext[0] = 'S'; s.ext[1] = 0; s.ext[2] = 0;
        net::SetLocalState(s);
        if (!wSent) { net::Sure("W " + weapon); net::Sure("E " + edge); net::Sure(outfit); wSent = true; nextW = now + 2; }
        else if (now >= nextW) { net::Quick("W " + weapon); net::Quick("E " + edge); net::Quick(outfit); nextW = now + 2; }
        if (r.phase == "fight") {
            if (now >= nextHp) { nextHp = now + 0.25; net::Hp(r.round, (int)(hp * 1000)); }
            if (hitEvery > 0 && !dead) {
                if (nextHit == 0) nextHit = now + hitEvery;
                else if (now >= nextHit) {
                    nextHit = now + hitEvery;
                    char b[160]; snprintf(b, sizeof b, "hit %d %.2f %d %.5f %d %.4f %.0f", other, 100.0, r.you, 0.1, r.round, 0.05, faceYaw);
                    net::Sure(b); ++sent;
                    printf("spar: %6.1f sent %s\n", now - t0, b);
                }
            }
        }
        if (now >= nextLog) {
            nextLog = now + 5;
            net::Stats st = net::GetStats(other);
            printf("spar: %6.1f %s at %.0f %.0f %.0f, other at %.0f %.0f %.0f (dist %.0f) hp %.2f | rtt %d D %.1f loss %.1f%%\n", now - t0, r.phase.c_str(), pos[0], pos[1], pos[2],
                   ps.pos[0], ps.pos[1], ps.pos[2], std::hypot(ps.pos[0] - pos[0], ps.pos[1] - pos[1]), hp, st.rttMs, st.delayMs, st.lossPct);
        }
    });
    printf("spar: done, hits sent %d taken %d respawns %d\n", sent, got, respawns);
    net::Leave();
    net::Shutdown();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "client" && argc >= 5) return Client(atoi(argv[2]), argv[3], atof(argv[4]), argc >= 6 ? atof(argv[5]) : net::Now());
    if (mode == "spar" && argc >= 7) {
        // argv[7]: codefile, unless it is already a key=value option
        int k = 7;
        const char* codeFile = argc > 7 && !strchr(argv[7], '=') ? argv[k++] : nullptr;
        std::map<std::string, std::string> opt;
        for (; k < argc; ++k) {
            const char* e = strchr(argv[k], '=');
            if (!e) { printf("spar: bad option %s (want key=value)\n", argv[k]); return 2; }
            opt[std::string(argv[k], e - argv[k])] = e + 1;
        }
        return Spar(atoi(argv[2]), argv[3], argv[4], atof(argv[5]), atof(argv[6]), codeFile, opt);
    }
    if (mode == "loop" && argc >= 6) {
        LoopTest("loop", atoi(argv[2]), atoi(argv[3]), (float)atof(argv[4]), atof(argv[5]), atoi(argv[2]) > 0);
    } else {
        if (mode == "all" || mode == "local") LoopTest("loopback localhost (0/0/0)", 0, 0, 0, 10, false);
        if (mode == "all" || mode == "bad") LoopTest("loopback bad (50 ms one-way = 100 RTT, +-20 jitter, 2% loss)", 50, 20, 2.0f, 30, true);
        if (mode == "all" || mode == "state") StateTest();
        if (mode == "all" || mode == "lobby") LobbyTest();
    }
    net::Shutdown();
    net::Shutdown();  // second call must be harmless
    printf(g_allOk ? "ALL PASS\n" : "SOME FAILED\n");
    return g_allOk ? 0 : 1;
}

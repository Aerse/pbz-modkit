// nettest: link tests for mp/net (docs/pvp-netcode.md "链路测试").
//   nettest.exe                 run everything (loopback local, loopback bad link, lobby if server exists)
//   nettest.exe loop <delayMs> <jitterMs> <lossPct> <seconds>
//   nettest.exe lobby           two-process lobby test against server/pbzmp_server.py
//   nettest.exe client <port> <code> <seconds>   (child process used by the lobby test)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "net.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <random>
#include <string>
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
    if (!CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) printf("spawn failed: %s\n", cmd.c_str());
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

int Client(int port, const std::string& code, double seconds) {
    net::SetServer("127.0.0.1", port);
    net::SetPassword(kTestPw);
    net::Join(code, "child \xE4\xB9\x99 b,c");
    if (!WaitFor(5, [] { return net::GetStatus() == net::Status::InRoom && !Phase().empty(); })) { printf("child: join failed %s\n", net::LastError().c_str()); return 2; }
    net::SetWeapon("/Game/Weapons/Sword A");
    net::SetReady(true);
    Sender tx; tx.t0 = 1000.0;  // fixed epoch shared with the parent (same machine QPC clock)
    tx.mont = net::Intern("/Game/Anim/Test_Montage");
    bool arena = false;
    RunFrames(seconds, [&](double now) {
        net::Room r;
        if (net::GetRoom(r)) {
            if (r.phase == "load" && !arena) { net::ArenaReady(r.round); arena = true; }
            if (r.phase == "fight") net::Hp(r.round, 900);
        }
        tx.Frame(now);
    });
    printf("child: sent sure=%d quick=%d\n", tx.sureSent, tx.quickSent);
    net::Leave();
    net::Shutdown();
    return 0;
}

void LobbyTest() {
    std::string root = RepoRoot(), srv = root + "\\mods\\pvp\\server\\pbzmp_server.py";
    if (GetFileAttributesA(srv.c_str()) == INVALID_FILE_ATTRIBUTES) { printf("lobby: %s not found, UNTESTED\n", srv.c_str()); return; }
    std::mt19937 rng(GetTickCount());
    int port = 30000 + (int)(rng() % 20000);
    printf("lobby: server on port %d\n", port);
    PROCESS_INFORMATION ps = Spawn("python -I \"" + srv + "\" --port " + std::to_string(port) + " --password \"" + kTestPw + "\"");
    SleepMs(1500);
    net::SetServer("127.0.0.1", port);
    net::Create("host \xE7\x94\xB2", 2);  // no password yet: must be refused
    bool refused = WaitFor(5, [] { return net::GetStatus() == net::Status::Failed; });
    Check("create without password refused", refused && net::LastError() == "password", "%s", net::LastError().c_str());
    net::SetPassword(kTestPw);
    net::Create("host \xE7\x94\xB2", 2);
    bool ok = WaitFor(5, [] { return net::GetStatus() == net::Status::InRoom && !Phase().empty(); });
    Check("create", ok, "%s", ok ? "" : net::LastError().c_str());
    net::Room r; net::GetRoom(r);
    printf("  room code=%s you=%d host=%d phase=%s\n", r.code.c_str(), r.you, r.host, r.phase.c_str());
    if (!ok) { TerminateProcess(ps.hProcess, 0); return; }
    // bad join first
    double seconds = 16;
    PROCESS_INFORMATION pc = Spawn("\"" + ExePath() + "\" client " + std::to_string(port) + " " + r.code + " " + std::to_string((int)seconds));
    ok = WaitFor(6, [] { net::Room x; return net::GetRoom(x) && x.members.size() == 2 && x.members[1].ready; });
    net::GetRoom(r);
    Check("child joined and ready", ok, "members=%zu", r.members.size());
    if (r.members.size() == 2) printf("  m1 name='%s' weapon='%s'\n", r.members[1].name.c_str(), r.members[1].weapon.c_str());
    Check("member fields decoded", r.members.size() == 2 && r.members[1].name == "child \xE4\xB9\x99 b,c" && r.members[1].weapon == "/Game/Weapons/Sword A", "");
    net::Conf(3, 60, 100, -1);
    WaitFor(2, [] { net::Room x; return net::GetRoom(x) && x.rounds == 3; });
    net::Start();
    std::vector<std::string> seen;
    Receiver rx; rx.slot = -1; rx.t0 = 1000.0;
    bool arena = false, haveData = false;
    double t = net::Now();
    while (net::Now() - t < seconds - 4) {
        double now = net::Now();
        std::string ph = Phase();
        if (!ph.empty() && (seen.empty() || seen.back() != ph)) { seen.push_back(ph); printf("  phase -> %s (t=%.1f s, rtt %d ms)\n", ph.c_str(), now - t, net::ServerRttMs()); }
        if (ph == "load" && !arena) { net::GetRoom(r); net::ArenaReady(r.round); arena = true; }
        if (rx.slot < 0) { auto sl = net::RemoteSlots(); if (!sl.empty()) rx.slot = sl[0]; }
        if (rx.slot >= 0) { haveData = true; rx.Frame(now, true); }
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
    net::Leave();
    TerminateProcess(ps.hProcess, 0);
    CloseHandle(ps.hProcess); CloseHandle(pc.hProcess);
}

// ---- sparring partner for in-game tests -------------------------------------------------
// nettest spar <port> <pw> <create|code> <seconds> <hitEvery> [codefile]
// A second player without a game, on the server at 127.0.0.1:<port>. "create" hosts a room
// (code written to codefile) and starts a 3-round match whenever the other member is ready;
// else it joins room <code> and readies. In a round it stands 180 cm in front of the other
// player, sends its weapon and Phantom Edge, deals a 10% hit every <hitEvery> s of the fight (0 = never), takes
// the hits the other side reports and sends dead when its hp runs out.
int Spar(int port, const std::string& pw, const std::string& what, double seconds, double hitEvery, const char* codeFile) {
    net::SetServer("127.0.0.1", port);
    net::SetPassword(pw);
    if (what == "create") net::Create("spar", 2); else net::Join(what, "spar");
    if (!WaitFor(8, [] { return net::GetStatus() == net::Status::InRoom && !Phase().empty(); })) { printf("spar: %s failed: %s\n", what.c_str(), net::LastError().c_str()); return 2; }
    net::Room r; net::GetRoom(r);
    printf("spar: room %s slot %d host %d\n", r.code.c_str(), r.you, r.host);
    if (codeFile) if (FILE* f = fopen(codeFile, "w")) { fputs(r.code.c_str(), f); fclose(f); }
    const std::string weapon = "/PBZCombatAsset/ActorData/Players/Hun/Weapon/Double/Weapon_Hun_Double.Weapon_Hun_Double";
    const std::string edge = "/PBZCombatAsset/ActorData/Players/Hun/ShaqiWeapon/BrutalAxe/Weapon_BrutalAxe.Weapon_BrutalAxe";
    const std::string outfit = "O Hun_OutFit_ZhuiMing 0 1";  // the "O" event of a player in that outfit
    std::string phase, lastW, lastE, lastO;
    int readyRound = -1, sent = 0, got = 0;
    double t0 = net::Now(), hp = 1, nextHit = 0, nextW = 0, nextHp = 0, nextLog = 0, nextLobby = 0, pos[3]{}, faceYaw = 0;
    bool placed = false, dead = false, wSent = false;
    RunFrames(seconds, [&](double now) {
        if (!net::GetRoom(r)) { if (phase != "gone") printf("spar: out of the room (%s)\n", net::LastError().c_str()); phase = "gone"; return; }
        int other = -1; bool otherReady = false, meReady = false;
        for (auto& m : r.members) { if (m.slot != r.you && other < 0) { other = m.slot; otherReady = m.ready; } if (m.slot == r.you) meReady = m.ready; }
        if (r.phase != phase) {
            printf("spar: %6.1f phase %s round %d/%d winner %d score", now - t0, r.phase.c_str(), r.round, r.rounds, r.winner);
            for (auto& m : r.members) printf(" s%d=%d", m.slot, m.score);
            printf("\n");
            phase = r.phase;
        }
        if (r.phase == "lobby") readyRound = -1;  // a new match starts at round 1 again
        if (r.phase == "lobby" && now >= nextLobby) {
            nextLobby = now + 1.5;
            if (r.you == r.host && other >= 0 && otherReady) { net::Conf(3, 60, 100, -1); net::Start(); }
            if (r.you != r.host && !meReady) net::SetReady(true);
            // PBZMP_SPAR_MODS: the "mods" member field to send (e.g. "evil.trainer:1.0:b"); others' lists are printed
            static std::string shownMods;
            if (const char* sm = getenv("PBZMP_SPAR_MODS")) for (auto& m : r.members) if (m.slot == r.you && m.mods != sm) net::SetMods(sm);
            std::string all; for (auto& m : r.members) if (m.slot != r.you) all += " s" + std::to_string(m.slot) + "=" + m.mods;
            if (all != shownMods) { printf("spar: mods%s\n", all.c_str()); shownMods = all; }
        }
        if (r.phase == "load" && r.round != readyRound) {
            net::ArenaReady(r.round); readyRound = r.round;
            hp = 1; dead = false; nextHit = 0; placed = false; wSent = false;
        }
        net::Event e;
        while (net::PollEvent(e)) {
            int victim = -1, from = -1, rnd = -1; float amt = 0, frac = 0, posture = 0, ayaw = 0;
            if (sscanf(e.text.c_str(), "hit %d %f %d %f %d %f %f", &victim, &amt, &from, &frac, &rnd, &posture, &ayaw) >= 5) {
                if (victim != r.you || r.phase != "fight" || rnd != r.round || dead) { printf("spar: ignored %s\n", e.text.c_str()); continue; }
                hp -= frac > 0 ? frac : amt / 1000; ++got;
                printf("spar: %6.1f took %s -> hp %.2f\n", now - t0, e.text.c_str(), hp);
                if (hp <= 0.001) { hp = 0; dead = true; net::Dead(r.round, from); printf("spar: dead by %d\n", from); }
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
        if (!match || other < 0 || !net::Sample(other, ps)) return;
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
    printf("spar: done, hits sent %d taken %d\n", sent, got);
    net::Leave();
    net::Shutdown();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "client" && argc >= 5) return Client(atoi(argv[2]), argv[3], atof(argv[4]));
    if (mode == "spar" && argc >= 7) return Spar(atoi(argv[2]), argv[3], argv[4], atof(argv[5]), atof(argv[6]), argc > 7 ? argv[7] : nullptr);
    if (mode == "loop" && argc >= 6) {
        LoopTest("loop", atoi(argv[2]), atoi(argv[3]), (float)atof(argv[4]), atof(argv[5]), atoi(argv[2]) > 0);
    } else {
        if (mode == "all" || mode == "local") LoopTest("loopback localhost (0/0/0)", 0, 0, 0, 10, false);
        if (mode == "all" || mode == "bad") LoopTest("loopback bad (50 ms one-way = 100 RTT, +-20 jitter, 2% loss)", 50, 20, 2.0f, 30, true);
        if (mode == "all" || mode == "lobby") LobbyTest();
    }
    net::Shutdown();
    net::Shutdown();  // second call must be harmless
    printf(g_allOk ? "ALL PASS\n" : "SOME FAILED\n");
    return g_allOk ? 0 : 1;
}

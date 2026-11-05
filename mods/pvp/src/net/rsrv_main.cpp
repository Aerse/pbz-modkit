// Test-only UDP wrapper around rsrv::Server (room_server.cpp), so server/test_server.py can run against the
// C++ port: set PBZMP_CPP_SERVER=<path to out_rsrv\rsrv.exe>. Same CLI as pbzmp_server.py minus the net sim,
// plus --timing/--limits overrides and --control (line commands on stdin, for the tests that poke internals).
// --max-players / --egress-kb as in pbzmp_server.py.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <share.h>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "room_server.h"

namespace {

std::atomic<bool> running{true};
FILE* logFile = nullptr;  // --log-file; like the Python, a log file replaces stdout
bool quiet = false;

void Log(const std::string& s) {
    if (quiet && !logFile) return;
    char ts[32];
    time_t now = time(nullptr);
    tm lt;
    localtime_s(&lt, &now);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &lt);
    FILE* f = logFile ? logFile : stdout;
    fprintf(f, "%s %s\n", ts, s.c_str());
    fflush(f);
}

BOOL WINAPI OnCtrl(DWORD) {  // Ctrl+C / Ctrl+Break / close: stop the loop, main exits with 0
    running = false;
    return TRUE;
}

std::string PeerText(uint64_t peer) {  // peer = ip << 16 | port (host order)
    uint32_t ip = (uint32_t)(peer >> 16);
    char b[48];
    snprintf(b, sizeof b, "('%u.%u.%u.%u', %u)", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255,
             (unsigned)(peer & 0xFFFF));
    return b;
}

double Mono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// name=value lists for --timing / --limits and the control commands "t"/"lim".
bool SetTiming(rsrv::Timing& t, const std::string& k, const std::string& v) {
    struct { const char* n; double rsrv::Timing::*f; } fs[] = {
        {"load", &rsrv::Timing::load}, {"count", &rsrv::Timing::count}, {"end", &rsrv::Timing::end},
        {"over", &rsrv::Timing::over}, {"conn_lost", &rsrv::Timing::conn_lost}, {"remove", &rsrv::Timing::remove},
        {"remove_match", &rsrv::Timing::remove_match},
        {"dedupe_ttl", &rsrv::Timing::dedupe_ttl}, {"verify", &rsrv::Timing::verify},
        {"room_idle", &rsrv::Timing::room_idle}, {"status", &rsrv::Timing::status}};
    for (auto& f : fs) if (k == f.n) { t.*f.f = atof(v.c_str()); return true; }
    return false;
}
bool SetLimit(rsrv::Limits& l, const std::string& k, const std::string& v) {
    struct { const char* n; int rsrv::Limits::*f; } fs[] = {
        {"max_rooms", &rsrv::Limits::max_rooms}, {"rooms_per_ip", &rsrv::Limits::rooms_per_ip},
        {"creates_per_min", &rsrv::Limits::creates_per_min}, {"joins_per_min", &rsrv::Limits::joins_per_min},
        {"pps_per_ip", &rsrv::Limits::pps_per_ip}, {"ctrl_per_addr", &rsrv::Limits::ctrl_per_addr},
        {"game_pkts", &rsrv::Limits::game_pkts}, {"game_bytes", &rsrv::Limits::game_bytes},
        {"badjoin_per_10min", &rsrv::Limits::badjoin_per_10min}, {"badtok_per_min", &rsrv::Limits::badtok_per_min},
        {"egress_bytes", &rsrv::Limits::egress_bytes}, {"max_ips", &rsrv::Limits::max_ips},
        {"max_players", &rsrv::Limits::max_players}};
    for (auto& f : fs) if (k == f.n) { l.*f.f = atoi(v.c_str()); return true; }
    return false;
}
template <class F> bool KvList(const std::string& s, F set) {
    for (size_t p = 0; p < s.size();) {
        size_t q = s.find(',', p), e = s.find('=', p);
        if (q == std::string::npos) q = s.size();
        if (e == std::string::npos || e > q || !set(s.substr(p, e - p), s.substr(e + 1, q - e - 1))) return false;
        p = q + 1;
    }
    return true;
}

// --control: one command per stdin line, each answered with one "@..." stdout line. Read on a thread, run on
// the main loop, because the Server is single-threaded.
std::mutex ctlMu;
std::deque<std::string> ctlLines;
std::atomic<bool> ctlEof{false};
void StdinThread() {
    char b[512];
    while (fgets(b, sizeof b, stdin)) {
        std::string s(b);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        std::lock_guard<std::mutex> g(ctlMu);
        ctlLines.push_back(s);
    }
    ctlEof = true;  // the test runner went away
}
void Out(const std::string& s) { fprintf(stdout, "%s\n", s.c_str()); fflush(stdout); }

double nextStatus = 0;
std::vector<size_t> sentSizes;  // datagram sizes since the last "sent" command
bool recordSent = false;

// Commands (each answered "@ok", "@err" or "@<data>"):
//   t k=v,...    lim k=v,...   change Timing / Limits
//   stats        rooms, players, owner_ips (Python: rooms / tokens / rooms_by_ip)
//   expire       every room's phase deadline becomes now (Python: room.deadline = time.monotonic())
//   sent         sizes of datagrams sent since the previous "sent", comma-separated (Python: wrap send)
//   log PATH     append the log to PATH from now on (Python: server.logf = ...)
//   status SEC   status interval, next line SEC from now (Python: t.status and next_status)
void Control(rsrv::Server& srv, const std::string& l, double now) {
    size_t sp = l.find(' ');
    std::string cmd = l.substr(0, sp), arg = sp == std::string::npos ? "" : l.substr(sp + 1);
    if (cmd == "t") {
        Out(KvList(arg, [&](const auto& k, const auto& v) { return SetTiming(srv.timing(), k, v); }) ? "@ok" : "@err");
    } else if (cmd == "lim") {
        Out(KvList(arg, [&](const auto& k, const auto& v) { return SetLimit(srv.limits(), k, v); }) ? "@ok" : "@err");
    } else if (cmd == "stats") {
        rsrv::Stats st = srv.GetStats();
        Out("@rooms=" + std::to_string(st.rooms) + " players=" + std::to_string(st.players) +
            " owner_ips=" + std::to_string(st.owner_ips));
    } else if (cmd == "expire") {
        for (auto& c : srv.RoomCodes()) srv.ExpireTimer(c, now);
        Out("@ok");
    } else if (cmd == "sent") {
        std::string r = "@";
        for (size_t i = 0; i < sentSizes.size(); i++) r += (i ? "," : "") + std::to_string(sentSizes[i]);
        sentSizes.clear();
        recordSent = true;
        Out(r);
    } else if (cmd == "log") {
        FILE* f = _fsopen(arg.c_str(), "a", _SH_DENYNO);  // shared, so a reader can tail it while we run
        if (!f) { Out("@err"); return; }
        if (logFile) fclose(logFile);
        logFile = f;
        Out("@ok");
    } else if (cmd == "status") {
        srv.timing().status = atof(arg.c_str());
        nextStatus = now + srv.timing().status;
        Out("@ok");
    } else {
        Out("@err");
    }
}

int Usage() {
    fprintf(stderr, "usage: rsrv [--port N] [--bind IP] [--password PW] [--log-file PATH] [--quiet]\n"
                    "            [--max-players N] [--egress-kb N]\n"
                    "            [--timing k=v,...] [--limits k=v,...] [--control]\n");
    return 2;
}

}  // namespace
int main(int argc, char** argv) {
    int port = 27777;
    std::string bind = "0.0.0.0", password, logPath;
    rsrv::Timing t;
    rsrv::Limits lim;
    bool control = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--quiet") quiet = true;
        else if (a == "--control") control = true;
        else if (!(v = val())) return Usage();
        else if (a == "--port") port = atoi(v);
        else if (a == "--bind") bind = v;
        else if (a == "--password") password = v;
        else if (a == "--log-file") logPath = v;
        else if (a == "--max-players") {
            lim.max_players = atoi(v);
            if (lim.max_players < 2 || lim.max_players > 8) { fprintf(stderr, "--max-players: 2-8\n"); return 2; }
        }
        else if (a == "--egress-kb") {
            int kb = atoi(v);
            if (kb < 0 || kb > 1 << 20) { fprintf(stderr, "--egress-kb: 0 or more\n"); return 2; }
            lim.egress_bytes = kb * 1024;
        }
        else if (a == "--timing") { if (!KvList(v, [&](const auto& k, const auto& x) { return SetTiming(t, k, x); })) return Usage(); }
        else if (a == "--limits") { if (!KvList(v, [&](const auto& k, const auto& x) { return SetLimit(lim, k, x); })) return Usage(); }
        else return Usage();
    }
    bool ctl = false;
    for (unsigned char c : password) ctl |= c < 32 || c == 127;
    if (password.size() > 64 || ctl) { fprintf(stderr, "--password: at most 64 bytes, no control chars\n"); return 2; }
    if (!logPath.empty() && !(logFile = _fsopen(logPath.c_str(), "a", _SH_DENYNO))) { fprintf(stderr, "cannot open %s\n", logPath.c_str()); return 2; }

    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int buf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof buf);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof buf);
    BOOL off = FALSE;  // otherwise an ICMP port-unreachable makes the next recvfrom fail with WSAECONNRESET
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, bind.c_str(), &sa.sin_addr) != 1 || ::bind(s, (sockaddr*)&sa, sizeof sa)) {
        fprintf(stderr, "bind %s:%d failed (%d)\n", bind.c_str(), port, WSAGetLastError());
        return 1;
    }
    int sl = sizeof sa;
    getsockname(s, (sockaddr*)&sa, &sl);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    Out("port=" + std::to_string(ntohs(sa.sin_port)));  // the test runner waits for this line

    rsrv::Server srv([s](uint64_t peer, const uint8_t* d, size_t n) {
        if (recordSent) sentSizes.push_back(n);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl((uint32_t)(peer >> 16));
        to.sin_port = htons((u_short)(peer & 0xFFFF));
        sendto(s, (const char*)d, (int)n, 0, (sockaddr*)&to, sizeof to);
    }, t, lim, password);
    srv.SetLog(Log);
    srv.SetPeerName(PeerText);
    char ip[32];
    inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip);
    Log("listening on udp ('" + std::string(ip) + "', " + std::to_string(ntohs(sa.sin_port)) + ")" +
        (password.empty() ? "" : " (password set)"));
    if (control) std::thread(StdinThread).detach();
    nextStatus = Mono() + srv.timing().status;
    uint64_t rxAtStatus = 0;
    sockaddr_in from{};
    char pkt[2048];
    while (running) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, 5000};  // 5 ms: phase timers in the tests are 100 ms
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
            for (int k = 0; k < 256; k++) {
                int fl = sizeof from;
                int n = recvfrom(s, pkt, sizeof pkt, 0, (sockaddr*)&from, &fl);
                if (n < 0) break;
                uint32_t a = ntohl(from.sin_addr.s_addr);
                srv.Handle((uint64_t)a << 16 | ntohs(from.sin_port), a, (const uint8_t*)pkt, (size_t)n, Mono());
            }
        }
        double now = Mono();
        if (control) {
            std::deque<std::string> lines;
            {
                std::lock_guard<std::mutex> g(ctlMu);
                lines.swap(ctlLines);
            }
            for (auto& l : lines) Control(srv, l, now);
            if (ctlEof) running = false;
        }
        srv.Tick(now);
        if (now >= nextStatus) {  // same format as the Python status line
            double span = srv.timing().status + (now - nextStatus);
            nextStatus = now + srv.timing().status;
            rsrv::Stats st = srv.GetStats();
            char b[160];
            snprintf(b, sizeof b, "status rooms=%zu players=%zu pps=%.1f ips=%zu", st.rooms, st.players,
                     (st.rx - rxAtStatus) / (span > 1e-3 ? span : 1e-3), st.ips);
            rxAtStatus = st.rx;
            Log(b);
        }
    }
    rsrv::Stats st = srv.GetStats();
    Log("shutting down: " + std::to_string(st.rooms) + " rooms, " + std::to_string(st.players) + " players dropped");
    closesocket(s);
    WSACleanup();
    if (logFile) fclose(logFile);
    return 0;
}

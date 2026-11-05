// pbzmp room/relay server: a C++ port of server/pbzmp_server.py (protocol v2, docs/pvp-protocol.md).
// Transport-agnostic, so the host's game can run it for Steam P2P play (net.cpp) and _rsrv_main.cpp can wrap
// it in a UDP exe for server/test_server.py. Not thread-safe: the caller serialises every call, and the
// send/log callbacks run inside Handle/Tick (they must not call back into the Server).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rsrv {

// Seconds; same names and defaults as Timing in pbzmp_server.py. A phase duration of 0 means "no limit".
struct Timing {
    double load = 60, count = 3, end = 4, over = 8;
    double conn_lost = 5, remove = 15;  // member silent this long: conn=0 / removed (lobby)
    double remove_match = 120;          // removed when silent this long mid-match: a crashed player can restart and resume
    double dedupe_ttl = 10;             // create/join (per peer) and leave (per token) replies kept for resends
    double verify = 5;                  // member that never sent a packet with its token is removed
    double room_idle = 3600;            // room without any state change is deleted
    double status = 60;                 // status log interval: used by the embedder, Server ignores it
};
// Same fields and defaults as Limits in pbzmp_server.py. "ip" is whatever the transport passes to Handle.
struct Limits {
    int max_rooms = 1000, rooms_per_ip = 4, creates_per_min = 6, joins_per_min = 20;
    int pps_per_ip = 1000;                          // datagrams/s from one ip, any type
    int ctrl_per_addr = 20;                         // CTRL/s from one peer
    int game_pkts = 200, game_bytes = 128 * 1024;   // GAME forwarded per member per second
    int badjoin_per_10min = 10;                     // joins with an unknown code
    int badtok_per_min = 30;                        // CTRL with an unknown token
    int egress_bytes = 512 * 1024;                  // bytes/s sent to one ip, 0 = no limit
    int max_ips = 65536;                            // tracked ips before forced pruning
    int max_players = 8;                            // room size cap 2..8 (--max-players), STATE cap=
};
struct Stats { size_t rooms = 0, players = 0, ips = 0, owner_ips = 0; uint64_t rx = 0; };  // rx: datagrams ever

class Server {
public:
    // peer: opaque id chosen by the transport (UDP: ip<<16|port, Steam: SteamID); replies go to the same id.
    using SendFn = std::function<void(uint64_t peer, const uint8_t* data, size_t n)>;
    using LogFn = std::function<void(const std::string& line)>;  // one event, no timestamp, no newline

    Server(SendFn send, Timing t = {}, Limits l = {}, std::string password = "");
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // One datagram from peer. ip only keys the per-ip limits and room counts. now: monotonic seconds.
    void Handle(uint64_t peer, uint32_t ip, const uint8_t* data, size_t n, double now);
    void Tick(double now);  // phase timers, member timeouts, cache expiry; call every few ms
    std::vector<std::string> RoomCodes() const;
    // Test hook: the room's phase deadline becomes now (Python tests set room.deadline directly).
    void ExpireTimer(const std::string& code, double now);
    void SetLog(LogFn f);
    void SetPeerName(std::function<std::string(uint64_t peer)> f);  // how logs show a peer (default: the number)
    Timing& timing();  // both may be changed between calls
    Limits& limits();
    Stats GetStats() const;  // rooms, players (live tokens), tracked ips, ips owning rooms, datagrams handled

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace rsrv

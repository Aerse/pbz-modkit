// pbzmp network client: lobby (docs/pvp-protocol.md) and game channel (docs/pvp-netcode.md).
// Plain C++20 + winsock, no UE. One client per process. Every function is thread-safe and
// non-blocking: the game thread calls them each frame, a private thread does the socket I/O.
// This header is the contract used by ui/, flow/ and fight/. Change it only together with them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace net {
double Now();  // seconds, monotonic (QueryPerformanceCounter); the clock of every time below

// ---- lobby -------------------------------------------------------------------------------
enum class Status { Idle, Connecting, InRoom, Failed };
struct Member {
    int slot = -1;
    std::string name, weapon;  // UTF-8
    int score = 0;
    bool ready = false, alive = false, conn = false;
    std::string edge;  // Phantom Edge (杀器) ItemId, "" = keep the save's; last: aggregate inits stay valid
    std::string mods;  // enabled client mods "id:ver:flag;..." (flag b = mp=block), "" = unknown / old client
    // v2 (net fills mods from the STATE "modh" hash + a "mods slot=" query, so mods may lag modh briefly)
    std::string modh;  // rules::ModHash(mods), "" = none
    int life = 0;      // this round's life number (kill mode: +1 per respawn, 0 at load)
    int deaths = 0;    // deaths this match (kill mode)
};
struct Room {
    uint32_t v = 0;
    int you = -1, host = -1;
    std::string code, phase, msg;  // phase: lobby load count fight end over
    int round = 0, rounds = 1, timeSec = 0, dmg = 100, max = 2, winner = -1;
    int map = 0;         // arena index chosen by the host (arena::MapCount)
    int leftMs = 0;      // phase time left when the STATE arrived (0 = no limit)
    double recvAt = 0;   // Now() when the STATE arrived
    std::vector<Member> members;  // sorted by slot
    // v2 (docs/pvp-protocol.md 模式 / 房间规则)
    std::string mode = "duel";  // duel | ffa | kill
    int maps = 0;        // random map bitmask (0 = use map); the server writes its pick into map at load
    int respawn = 5;     // kill mode: seconds dead before the server re-alives a member
    int kills = 10;      // kill mode: score that ends the match (0 = time only)
    int cap = 8;         // server's player limit (--max-players)
    std::string rules;   // raw "k:v,..." (rules::Parse in rules.h)
};
constexpr int kProtoVer = 2;   // create/join ver=
constexpr int kMaxPlayers = 8;

// Starts the I/O thread on first use. host: IPv4 address or name; port 0 means 27777.
// Resolving happens on the I/O thread; failure shows up as Failed / "resolve".
void SetServer(const std::string& host, int port);
// Server password (pbzmp_server.py --password), sent as pw= with the next Create/Join.
// "" = none (default). A wrong or missing one fails with LastError() "password".
void SetPassword(const std::string& password);
// Steam P2P mode: Create makes the local game the room server (rsrv::Server) and opens a Steam
// lobby tagged with the room code; Join finds that lobby by code and talks to its owner over
// ISteamNetworkingMessages. SetServer switches back to the relay. Extra LastError() reasons:
// "steam" (no Steam / lobby failed), "join" (lobby join refused), "hostleft" (the host is gone).
bool SteamAvailable();
std::string SteamPersona();  // UTF-8 Steam name, "" without Steam
void UseSteam();
bool SteamMode();
void SteamPump();  // game thread, every frame (drives the Steam lobby calls); no-op in relay mode
void Create(const std::string& name, int maxPlayers);     // -> InRoom, or Failed + LastError()
void Join(const std::string& code, const std::string& name);
void Leave();                                              // -> Idle; also ends Loopback()
// crash rejoin: Session gives the token of the room we are in (persisted by ui); after a restart
// Resume(code, token, slot) goes back into it (relay: SetServer first; Steam: UseSteam, the host's
// lobby is found by code). -> InRoom, or Failed with "expired" when the server forgot the token.
bool Session(std::string& code, uint32_t& token, int& slot);
void Resume(const std::string& code, uint32_t token, int slot);
bool Resuming();
void GetServer(std::string& host, int& port);  // last SetServer values
Status GetStatus();
std::string LastError();   // server reason (version, noroom, full, started, password, rate, ...) or timeout / resolve
bool GetRoom(Room& out);   // latest STATE; false while not in a room
int ServerRttMs();         // -1 = unknown

void SetReady(bool ready);
void SetWeapon(const std::string& path);
void SetEdge(const std::string& id);  // member field "edge" (needs a server that knows the key)
void SetMods(const std::string& list);  // member field "mods" (sent alone: an old server rejects the key)
void Conf(int rounds, int timeSec, int dmgPct, int maxPlayers);  // -1 keeps a value
// every room setting in one conf (the server applies them together); -1 / unset keeps a value
struct ConfArgs {
    int rounds = -1, timeSec = -1, dmg = -1, max = -1, map = -1, maps = -1, respawn = -1, kills = -1;
    std::string mode;                        // "" keeps
    bool setRules = false; std::string rules;
};
void Conf(const ConfArgs& c);
void ConfMap(int map);  // host only, lobby only
void Start();
void ArenaReady(int round);
void Dead(int round, int bySlot, int life = -1);  // life: Member::life when it died (-1 = not sent)
void Hp(int round, int permille);  // unreliable; call about 4 times a second during a fight

// ---- game channel -------------------------------------------------------------------------
struct State {
    float pos[3]{};              // capsule centre, world cm
    float vel[3]{};              // cm/s
    float acc[2]{};              // input direction x/y, -1..1 (0,0 = no input)
    float yaw = 0;               // degrees
    uint8_t mode = 0, flags = 0; // MovementMode; flag bits in docs/pvp-netcode.md
    float hp = 1, posture = 1;   // 0..1
    uint16_t montage = 0;        // Intern() id of the playing montage, 0 = none
    float montPos = 0, montRate = 1;  // seconds, play rate
    uint8_t armed = 0;
    uint8_t anim[7]{};           // opaque to net
    uint8_t extLen = 0;          // extra per-frame bytes for the fight layer, opaque to net,
    uint8_t ext[255]{};          // not interpolated (Sample returns the earlier sample's)
};
struct Event {
    int slot = -1;
    bool sure = false;
    double at = 0;               // local Now() time at which the event is due on the timeline
    std::string text;            // UTF-8
};
struct Stats {
    int rttMs = -1;
    float baseMs = 0, jitterMs = 0, delayMs = 0, rxPerSec = 0, lossPct = 0;
    int starves = 0;             // times playback ran past the newest sample
};

uint16_t Intern(const std::string& text);        // local id (1..); definition sent once on SURE
std::string Lookup(int slot, uint16_t id);      // a remote player's id -> text, "" if unknown
void SetLocalState(const State& s);             // every frame; sent at 60 Hz
void Sure(const std::string& text);             // reliable, ordered, exactly once
void Quick(const std::string& text);            // unreliable, at most once, dropped when late
bool Sample(int slot, State& out, double now = -1);  // timeline playback (now < 0: Now()); false = no data
bool PollEvent(Event& out);                     // next event whose time has come, in order per slot
Stats GetStats(int slot);
std::vector<int> RemoteSlots();                 // slots that sent game data in the last 5 s

// Test mode, no server: whatever this client sends comes back as slot 1 after the given
// one-way delay / jitter / loss (local slot is 0). Lobby calls do nothing. Leave() ends it.
bool Loopback(int delayMs, int jitterMs, float lossPct);
void Shutdown();  // stops/joins the I/O thread, closes the socket, resets everything; safe twice; later calls restart
}  // namespace net

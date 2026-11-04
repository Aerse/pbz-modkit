// steam: the game's Steamworks (flat API of the steam_api64.dll it already loaded and initialised).
// Lobbies carry the room code so a joiner finds the host; the room itself runs on the host
// (rsrv::Server inside net) and datagrams go over ISteamNetworkingMessages (P2P, Valve relay when
// a direct path fails). No callbacks are registered: call results are polled.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace steam {
bool Available();           // steam_api64 loaded, interfaces present, logged on
uint64_t Me();              // own SteamID, 0 if unavailable
std::string PersonaName();  // UTF-8, "" if unavailable

// ---- lobby: game thread only (Pump drives the async calls) ----
enum class LState { None, Busy, Ready, Failed };
void Pump();                                   // every frame
void HostLobby(const std::string& code, int max);  // public lobby with pbzmp=<ver>, code=<code>
void FindLobby(const std::string& code);           // search by code (retried for a while), then join
void LeaveLobby();
LState Lobby();
std::string LobbyError();    // "steam", "noroom", "full", "join"
uint64_t LobbyOwner();       // current owner (host) once Ready, else 0; any thread
std::vector<uint64_t> LobbyMembers();  // any thread; refreshed by Pump

// ---- P2P datagrams: any thread ----
bool Send(uint64_t to, const void* data, size_t n);  // unreliable, no Nagle
// appends (sender, payload) of every waiting datagram; returns how many
int Recv(std::vector<std::pair<uint64_t, std::string>>& out);
void Accept(uint64_t peer);  // accept a pending session (the host does this for lobby members)
void Close(uint64_t peer);
// relay network status: 100 = ready, < 0 failed, else still measuring
int RelayStatus(std::string* msg = nullptr);
}  // namespace steam

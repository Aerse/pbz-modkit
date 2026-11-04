// steam: see steam.h. Flat API from the steam_api64.dll the game loaded (Steamworks v165 names).
// Struct layouts are the SDK's (pack 8 on Windows); call results are polled through ISteamUtils.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "steam.h"

#include <mutex>

namespace steam {
namespace {
HMODULE api;
void *user, *friends, *mm, *utils, *msgs, *nutils;
template <class F> F P(const char* n) { return api ? reinterpret_cast<F>(GetProcAddress(api, n)) : nullptr; }

struct Ident { int type, size; uint64_t id; char pad[120]; };  // SteamNetworkingIdentity
static_assert(sizeof(Ident) == 136);
Ident Id(uint64_t sid) { Ident i{}; i.type = 16; i.size = 8; i.id = sid; return i; }  // k_ESteamNetworkingIdentityType_SteamID
struct Msg { void* data; int size; uint32_t conn; Ident peer; };  // SteamNetworkingMessage_t (head)
constexpr int kChannel = 7;
constexpr int kSendFlags = 1 | 32;  // unreliable, no Nagle, auto-restart broken session

// ISteamUtils call results: 513 LobbyCreated_t, 510 LobbyMatchList_t, 504 LobbyEnter_t
struct LobbyCreated { int result; uint64_t lobby; };
struct LobbyEnter { uint64_t lobby; uint32_t perms; bool locked; uint32_t response; };

bool InitOnce() {
    api = GetModuleHandleW(L"steam_api64.dll");
    if (!api) return false;
    auto get = [](const char* n) -> void* { auto f = P<void* (*)()>(n); return f ? f() : nullptr; };
    user = get("SteamAPI_SteamUser_v023");
    friends = get("SteamAPI_SteamFriends_v018");
    mm = get("SteamAPI_SteamMatchmaking_v009");
    utils = get("SteamAPI_SteamUtils_v011");
    msgs = get("SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    nutils = get("SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    if (nutils) if (auto f = P<void (*)(void*)>("SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess")) f(nutils);
    return user && mm && utils && msgs;
}
bool Init() { static bool ok = InitOnce(); return ok; }  // thread-safe static init (game and net threads)
// true when the call finished; *failed on IO failure or a wrong size
bool Result(uint64_t call, void* out, int size, int id, bool* failed) {
    static auto done = P<bool (*)(void*, uint64_t, bool*)>("SteamAPI_ISteamUtils_IsAPICallCompleted");
    static auto res = P<bool (*)(void*, uint64_t, void*, int, int, bool*)>("SteamAPI_ISteamUtils_GetAPICallResult");
    bool f = false;
    if (!done || !res || !done(utils, call, &f)) return false;
    *failed = !res(utils, call, out, size, id, &f) || f;
    return true;
}

// lobby state machine (game thread); owner/members are read by the net thread
enum class Job { None, Create, List, Join, In };
Job job = Job::None;
LState lst = LState::None;
std::string err, code;
int maxMembers = 2, tries;
uint64_t call, lobby;
uint64_t orphan;  // CreateLobby still running when we left: leave that lobby once it exists
ULONGLONG nextTry, deadline, nextRefresh;
std::mutex mx;
uint64_t owner;
std::vector<uint64_t> members;

void Fail(const char* why) { job = Job::None; lst = LState::Failed; err = why; std::lock_guard l(mx); owner = 0; members.clear(); }
void Refresh() {
    static auto own = P<uint64_t (*)(void*, uint64_t)>("SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    static auto num = P<int (*)(void*, uint64_t)>("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    static auto at = P<uint64_t (*)(void*, uint64_t, int)>("SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    if (!own || !num || !at) return;
    std::vector<uint64_t> m;
    for (int i = 0, n = num(mm, lobby); i < n && i < 16; ++i) m.push_back(at(mm, lobby, i));
    std::lock_guard l(mx); owner = own(mm, lobby); members = std::move(m);
}
bool RequestList() {
    auto str = P<void (*)(void*, const char*, const char*, int)>("SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    auto dist = P<void (*)(void*, int)>("SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    auto req = P<uint64_t (*)(void*)>("SteamAPI_ISteamMatchmaking_RequestLobbyList");
    ++tries; call = 0;
    if (!str || !req) return false;
    if (dist) dist(mm, 3);  // worldwide
    str(mm, "pbzmp", "1", 0);
    str(mm, "code", code.c_str(), 0);
    call = req(mm);
    return call != 0;
}
}  // namespace
bool Available() {
    if (!Init()) return false;
    auto on = P<bool (*)(void*)>("SteamAPI_ISteamUser_BLoggedOn");
    return !on || on(user);
}
uint64_t Me() {
    if (!Init()) return 0;
    auto f = P<uint64_t (*)(void*)>("SteamAPI_ISteamUser_GetSteamID");
    return f ? f(user) : 0;
}
std::string PersonaName() {
    if (!Init() || !friends) return "";
    auto f = P<const char* (*)(void*)>("SteamAPI_ISteamFriends_GetPersonaName");
    const char* n = f ? f(friends) : nullptr;
    return n ? n : "";
}

void HostLobby(const std::string& c, int max) {
    LeaveLobby();
    if (!Available()) { Fail("steam"); return; }
    code = c; maxMembers = max; err.clear(); lst = LState::Busy; job = Job::Create;
    auto f = P<uint64_t (*)(void*, int, int)>("SteamAPI_ISteamMatchmaking_CreateLobby");
    call = f ? f(mm, 2, max) : 0;  // k_ELobbyTypePublic
    if (!call) Fail("steam");
}
void FindLobby(const std::string& c) {
    LeaveLobby();
    if (!Available()) { Fail("steam"); return; }
    code = c; err.clear(); lst = LState::Busy; job = Job::List; tries = 0;
    deadline = GetTickCount64() + 25000;  // a new lobby takes a few seconds to show up in searches
    if (!RequestList()) nextTry = GetTickCount64() + 3000;
}
void LeaveLobby() {
    if (job == Job::Create && call) orphan = call;
    if (lobby && mm) if (auto f = P<void (*)(void*, uint64_t)>("SteamAPI_ISteamMatchmaking_LeaveLobby")) f(mm, lobby);
    lobby = 0; job = Job::None; lst = LState::None; err.clear();
    std::lock_guard l(mx); owner = 0; members.clear();
}
LState Lobby() { return lst; }
std::string LobbyError() { return err; }
uint64_t LobbyOwner() { std::lock_guard l(mx); return owner; }
std::vector<uint64_t> LobbyMembers() { std::lock_guard l(mx); return members; }

void Pump() {
    bool failed = false;
    if (orphan) {
        LobbyCreated r{};
        if (Result(orphan, &r, sizeof r, 513, &failed)) {
            auto f = P<void (*)(void*, uint64_t)>("SteamAPI_ISteamMatchmaking_LeaveLobby");
            if (f && !failed && r.result == 1) f(mm, r.lobby);
            orphan = 0;
        }
    }
    if (job == Job::None) return;
    ULONGLONG now = GetTickCount64();
    if (job == Job::Create) {
        LobbyCreated r{};
        if (!Result(call, &r, sizeof r, 513, &failed)) return;
        if (failed || r.result != 1) { Fail("steam"); return; }
        lobby = r.lobby;
        auto set = P<bool (*)(void*, uint64_t, const char*, const char*)>("SteamAPI_ISteamMatchmaking_SetLobbyData");
        if (!set) { call = 0; LeaveLobby(); Fail("steam"); return; }  // nobody could find it by its code
        set(mm, lobby, "pbzmp", "1"); set(mm, lobby, "code", code.c_str());
        job = Job::In; lst = LState::Ready; Refresh(); nextRefresh = now + 1000;
    } else if (job == Job::List) {
        if (call) {
            uint32_t n = 0;
            if (!Result(call, &n, sizeof n, 510, &failed)) return;
            call = 0;
            auto byIdx = P<uint64_t (*)(void*, int)>("SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
            auto data = P<const char* (*)(void*, uint64_t, const char*)>("SteamAPI_ISteamMatchmaking_GetLobbyData");
            auto join = P<uint64_t (*)(void*, uint64_t)>("SteamAPI_ISteamMatchmaking_JoinLobby");
            uint64_t l = 0;
            for (uint32_t i = 0; !failed && byIdx && i < n && i < 50 && !l; ++i) {  // the one whose code is ours
                uint64_t c = byIdx(mm, int(i));
                const char* v = data && c ? data(mm, c, "code") : nullptr;
                if (c && (!data || (v && _stricmp(v, code.c_str()) == 0))) l = c;
            }
            if (l) {
                call = join ? join(mm, l) : 0;
                if (!call) { Fail("join"); return; }
                job = Job::Join; return;
            }
            nextTry = now + 3000;
        }
        if (now >= deadline) { Fail("noroom"); return; }
        if (now >= nextTry && !RequestList()) nextTry = now + 3000;
    } else if (job == Job::Join) {
        LobbyEnter r{};
        if (!Result(call, &r, sizeof r, 504, &failed)) return;
        if (failed || r.response != 1) { Fail(r.response == 4 ? "full" : r.response == 2 ? "noroom" : "join"); return; }
        lobby = r.lobby; job = Job::In; lst = LState::Ready; Refresh(); nextRefresh = now + 1000;
    } else if (job == Job::In && now >= nextRefresh) {
        Refresh(); nextRefresh = now + 1000;
    }
}

bool Send(uint64_t to, const void* data, size_t n) {
    if (!Init()) return false;
    static auto f = P<int (*)(void*, const Ident*, const void*, uint32_t, int, int)>("SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    Ident id = Id(to);
    return f && f(msgs, &id, data, (uint32_t)n, kSendFlags, kChannel) == 1;  // k_EResultOK
}
int Recv(std::vector<std::pair<uint64_t, std::string>>& out) {
    if (!Init()) return 0;
    static auto rx = P<int (*)(void*, int, Msg**, int)>("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    static auto rel = P<void (*)(Msg*)>("SteamAPI_SteamNetworkingMessage_t_Release");
    if (!rx || !rel) return 0;
    int total = 0;
    for (;;) {
        Msg* m[32];
        int n = rx(msgs, kChannel, m, 32);
        for (int i = 0; i < n; ++i) {
            out.emplace_back(m[i]->peer.id, std::string((const char*)m[i]->data, m[i]->size));
            rel(m[i]);
        }
        total += n > 0 ? n : 0;
        if (n < 32) return total;
    }
}
void Accept(uint64_t peer) {
    if (!Init()) return;
    static auto f = P<bool (*)(void*, const Ident*)>("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    Ident id = Id(peer); if (f) f(msgs, &id);
}
void Close(uint64_t peer) {
    if (!Init()) return;
    static auto f = P<bool (*)(void*, const Ident*)>("SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    Ident id = Id(peer); if (f) f(msgs, &id);
}
int RelayStatus(std::string* msg) {
    if (!Init() || !nutils) return -1;
    static auto f = P<int (*)(void*, void*)>("SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    char st[512]{};  // SteamRelayNetworkStatus_t: availability, ping flags, ..., debug message at +16
    int a = f ? f(nutils, st) : -1;
    if (msg) *msg = st + 16;
    return a;
}
}  // namespace steam

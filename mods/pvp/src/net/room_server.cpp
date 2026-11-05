// Port of server/pbzmp_server.py; the Python file is the reference and its docstring lists the spec decisions.
// Differences by design: no loss/delay simulation, peers are opaque ids instead of (host, port) tuples,
// and "now" comes from the caller instead of time.monotonic().
#include "room_server.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rsrv {
namespace {

constexpr uint8_t kMagic = 0xB7, kVer = 1;  // header version; create/join ver= is kProtoVer
const char* const kProtoVer = "2";
constexpr int kMaxSlots = 8;  // protocol limit: slot / by ranges
constexpr uint8_t T_CTRL = 0x01, T_REPLY = 0x02, T_STATE = 0x03, T_GAME_IN = 0x10, T_GAME_OUT = 0x11;
constexpr size_t kHdr = 8, kMaxDgram = 1200;
constexpr size_t kMinHello = 64;  // token=0 CTRL shorter than this is dropped: no reply can outgrow its request
constexpr size_t kMaxArgs = 16, kCachePerMember = 64;
const char* const kCmds[] = {"create", "join", "leave", "set", "conf", "start", "arena", "dead", "hp", "ping", "mods"};

// Thrown for malformed input. req != 0: the parse got far enough to answer "err reason=bad".
struct Bad { uint64_t req = 0; };

bool Safe(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}
bool Digits(const std::string& s, size_t lo, size_t hi) {
    if (s.size() < lo || s.size() > hi) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}
bool ReReq(const std::string& s) { return Digits(s, 1, 10); }
bool ReCode(const std::string& s) { return Digits(s, 6, 6); }
bool ReInt(const std::string& s) { return s.size() && s[0] == '-' ? Digits(s.substr(1), 1, 6) : Digits(s, 1, 6); }
bool ReKey(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    for (char c : s) if (c < 'a' || c > 'z') return false;
    return true;
}
int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Strict UTF-8 like Python's bytes.decode("utf-8"): no overlongs, surrogates or code points past U+10FFFF.
bool Utf8(const std::string& s, std::vector<uint32_t>* out) {
    for (size_t i = 0, n = s.size(); i < n;) {
        unsigned char c = s[i];
        if (c < 0x80) { if (out) out->push_back(c); i++; continue; }
        size_t len; uint32_t cp; unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the 2nd byte
        if (c >= 0xC2 && c <= 0xDF) len = 2, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF) { len = 3, cp = c & 0x0F; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4, cp = c & 0x07; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            unsigned char d = s[i + k];
            if (d < (k == 1 ? lo : 0x80) || d > (k == 1 ? hi : 0xBF)) return false;
            cp = cp << 6 | (d & 0x3F);
        }
        if (out) out->push_back(cp);
        i += len;
    }
    return true;
}

// clean_text: no control chars or U+FFFD, cut to max_chars code points, then at most max_bytes bytes.
std::string CleanText(const std::string& v, size_t max_chars, size_t max_bytes) {
    std::vector<uint32_t> cps;
    if (!Utf8(v, &cps)) throw Bad{};  // can't happen: values were decoded strictly
    for (uint32_t c : cps) if (c < 32 || c == 127 || c == 0xFFFD) throw Bad{};
    std::string r = v;
    if (cps.size() > max_chars) {
        size_t chars = 0, i = 0;
        for (; i < r.size(); i++) if (((unsigned char)r[i] & 0xC0) != 0x80 && chars++ == max_chars) break;
        r.resize(i);
    }
    if (r.size() > max_bytes) throw Bad{};
    return r;
}

int ToInt(const std::string* v, int lo, int hi) {
    if (!v || !ReInt(*v)) throw Bad{};
    int n = std::stoi(*v);
    if (n < lo || n > hi) throw Bad{};
    return n;
}

std::string PctDecStrict(const std::string& v, Bad bad) {
    std::string out;
    for (size_t i = 0; i < v.size();) {
        if (v[i] != '%') { out += v[i++]; continue; }
        if (i + 2 >= v.size()) throw bad;
        int a = Hex(v[i + 1]), b = Hex(v[i + 2]);
        if (a < 0 || b < 0) throw bad;
        out += (char)(a << 4 | b);
        i += 3;
    }
    if (!Utf8(out, nullptr)) throw bad;
    return out;
}

std::string PctEnc(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (Safe(c)) r += (char)c;
        else r += '%', r += hx[c >> 4], r += hx[c & 15];
    }
    return r;
}
// STATE v2 value escaping: only 0x00-0x20 % , = 0x7F become %XX; UTF-8 stays raw.
std::string StateEnc(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (c <= 0x20 || c == '%' || c == ',' || c == '=' || c == 0x7F) r += '%', r += hx[c >> 4], r += hx[c & 15];
        else r += (char)c;
    }
    return r;
}
// FNV-1a 32 of the mods text as 8 lowercase hex digits, "" for "" (same as rules::ModHash).
std::string ModHash(const std::string& s) {
    if (s.empty()) return "";
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    char b[9];
    snprintf(b, sizeof b, "%08x", h);
    return b;
}
bool ReRules(const std::string& s) {
    if (s.size() > 200) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == ',')) return false;
    return true;
}
using Args = std::map<std::string, std::string>;
struct Ctrl { uint64_t req; std::string cmd; Args args; };

// parse_ctrl: Bad{0} if even the req number is unreadable (no reply possible), else Bad{req}.
Ctrl ParseCtrl(const uint8_t* b, size_t n) {
    std::string body((const char*)b, n);
    std::string head = body.substr(0, body.find(' '));
    if (!ReReq(head) || std::stoull(head) > 0xFFFFFFFFull) throw Bad{};
    Ctrl c{std::stoull(head), {}, {}};
    Bad bad{c.req};
    for (unsigned char ch : body) if (ch >= 128) throw bad;  // clients percent-encode
    std::vector<std::string> parts;
    for (size_t p = 0;;) {
        size_t q = body.find(' ', p);
        parts.push_back(body.substr(p, q == std::string::npos ? q : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (parts.size() < 2 || std::find(std::begin(kCmds), std::end(kCmds), parts[1]) == std::end(kCmds) ||
        parts.size() - 2 > kMaxArgs) throw bad;
    c.cmd = parts[1];
    for (size_t i = 2; i < parts.size(); i++) {
        const std::string& p = parts[i];
        size_t eq = p.find('=');
        if (eq == std::string::npos) throw bad;
        std::string k = p.substr(0, eq), v = p.substr(eq + 1);
        if (!ReKey(k) || c.args.count(k)) throw bad;
        for (unsigned char ch : v) if (!Safe(ch) && ch != '%') throw bad;
        c.args[k] = PctDecStrict(v, bad);
    }
    return c;
}

// check_keys: only the keys this command knows; pad (create/join filler) must be all '-'.
void CheckKeys(const std::string& cmd, const Args& a) {
    static const std::map<std::string, std::vector<std::string>> keys = {
        {"create", {"name", "ver", "max", "pw", "pad"}}, {"join", {"code", "name", "ver", "pw", "pad"}},
        {"leave", {}}, {"start", {}}, {"set", {"ready", "weapon", "edge", "mods"}},
        {"conf", {"rounds", "time", "dmg", "max", "map", "maps", "mode", "respawn", "kills", "rules"}},
        {"arena", {"round"}}, {"dead", {"round", "by", "life"}}, {"hp", {"round", "pct"}}, {"ping", {"t"}},
        {"mods", {"slot"}}};
    const auto& ks = keys.at(cmd);
    for (const auto& [k, v] : a) if (std::find(ks.begin(), ks.end(), k) == ks.end()) throw Bad{};
    auto p = a.find("pad");
    if (p != a.end() && p->second.find_first_not_of('-') != std::string::npos) throw Bad{};
}

const std::string* Get(const Args& a, const char* k) {
    auto it = a.find(k);
    return it == a.end() ? nullptr : &it->second;
}
std::string GetOr(const Args& a, const char* k, const char* def) { auto v = Get(a, k); return v ? *v : def; }
// Python repr() of a str, close enough for log lines.
std::string Repr(const std::string& s) {
    char q = s.find('\'') != std::string::npos && s.find('"') == std::string::npos ? '"' : '\'';
    std::string r(1, q);
    for (char c : s) {
        if (c == '\\' || c == q) r += '\\';
        r += c;
    }
    return r + q;
}

// Fixed-window counter.
struct Win {
    double start = -1e18;
    long long n = 0;
    bool Hit(double now, double span, long long limit) {  // count one event; true if within the limit
        if (now - start >= span) start = now, n = 0;
        return ++n <= limit;
    }
};

struct IpState { Win pkt, create, join, badjoin, badtok, out; double last = 0; };

struct Member {
    int slot = 0;
    std::string name;
    uint32_t token = 0;
    uint64_t addr = 0;  // peer id; changes when a packet with our token comes from elsewhere (NAT rebinding)
    uint32_t ip = 0;    // ip of addr, for the egress cap
    bool ready = false, alive = true, conn = true, arena = false;
    bool verified = false;  // a packet carrying our token has arrived
    std::string weapon, edge;  // lobby picks (edge = Phantom Edge / 杀器), opaque text
    std::string mods;          // enabled client mods "id:ver:flag;...", opaque text
    int score = 0, hp = 1000;
    int life = 0, deaths = 0;  // this round's life number (kill: +1 per respawn), deaths this match
    double respawn_at = 0;     // kill mode: when a dead member is re-alived (0 = not pending)
    double last = 0;
    std::deque<std::pair<uint64_t, std::string>> replies;  // req -> packet, oldest first
    double gwin = 0;
    long long gpkts = 0, gbytes = 0;
};

struct Room {
    std::string code;
    int max = 2, rounds = 3, time = 90, dmg = 100, map = 0;
    int maps = 0, respawn = 5, kills = 10;  // random map bitmask (0 = use map); kill-mode respawn seconds / target
    std::string mode = "duel", rules;       // create with max > 2 makes an ffa room
    std::string phase = "lobby";
    int round = 0, winner = -1, host = 0;
    long long v = 0;
    std::string msg;
    double deadline = 0;  // 0 = unlimited
    // Insertion order, like the Python dict: decides who is handled first when several members time out at once.
    // shared_ptr: a command may remove its own member (leave) and keep using it afterwards, as the Python does.
    std::vector<std::shared_ptr<Member>> members;
    uint32_t owner_ip = 0;
    double last_change = 0;

    bool InMatch() const { return phase != "lobby"; }
    Member* At(int slot) const {
        for (auto& m : members) if (m->slot == slot) return m.get();
        return nullptr;
    }
};

struct PairHash {
    size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
        return std::hash<uint64_t>()(p.first * 0x9E3779B97F4A7C15ull ^ p.second);
    }
};
using Cache = std::unordered_map<std::pair<uint64_t, uint64_t>, std::pair<std::string, double>, PairHash>;

}  // namespace

struct Server::Impl {
    using RoomP = std::shared_ptr<Room>;
    using MemP = std::shared_ptr<Member>;
    SendFn sendFn;
    LogFn logFn;
    std::function<std::string(uint64_t)> peerName;
    Timing t;
    Limits lim;
    std::string password;
    std::mt19937 rng;
    double now = 0;  // time of the call in progress; Python reads time.monotonic() at the same points
    uint64_t rx = 0;
    std::unordered_map<uint32_t, IpState> ips;
    std::unordered_map<uint32_t, int> roomsByIp;  // ip -> live rooms created from it
    std::unordered_map<uint64_t, Win> ctrlRate;   // peer -> Win
    std::map<std::string, RoomP> rooms;           // code -> Room
    std::unordered_map<uint32_t, std::pair<RoomP, MemP>> tokens;
    Cache addrCache;  // (peer, req) -> (packet, expiry): create/join replies
    Cache goneCache;  // (token, req) -> (packet, expiry): leave replies outliving the token

    void Log(const std::string& s) { if (logFn) logFn(s); }
    std::string Peer(uint64_t p) { return peerName ? peerName(p) : std::to_string(p); }

    // ---------- io ----------
    void Send(const std::string& d, uint64_t addr, uint32_t ip) {
        auto it = ips.find(ip);
        if (it != ips.end()) {  // egress cap per destination ip (bounds token-spoof redirects)
            Win& w = it->second.out;
            if (now - w.start >= 1.0) w.start = now, w.n = 0;
            if (lim.egress_bytes && w.n + (long long)d.size() > lim.egress_bytes) return;
            w.n += d.size();
        }
        if (sendFn) sendFn(addr, (const uint8_t*)d.data(), d.size());
    }
    static std::string Hdr(uint8_t typ, uint8_t slot) {
        return std::string{(char)kMagic, (char)kVer, (char)typ, (char)slot, 0, 0, 0, 0};
    }
    static std::string ReplyPkt(uint64_t req, const std::string& text) {
        return Hdr(T_REPLY, 0) + std::to_string(req) + " " + text;
    }
    // Reply to a packet without a valid token: never larger than it.
    void SendSmall(const std::string& pkt, uint64_t addr, uint32_t ip, size_t limit) {
        if (pkt.size() <= limit) Send(pkt, addr, ip);
    }
    // ---------- packets ----------
    void Handle(uint64_t addr, uint32_t ip, const uint8_t* data, size_t n) {
        rx++;
        auto it = ips.find(ip);
        if (it == ips.end()) {
            if (ips.size() >= (size_t)lim.max_ips) Prune(true);
            it = ips.emplace(ip, IpState{}).first;
        }
        IpState& st = it->second;
        st.last = now;
        if (!st.pkt.Hit(now, 1.0, lim.pps_per_ip)) return;
        if (n < kHdr || n > kMaxDgram) return;
        uint8_t typ = data[2];
        uint32_t token = data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24;
        if (data[0] != kMagic || data[1] != kVer || data[3] != 0) return;
        if (typ == T_CTRL) {
            if (!ctrlRate[addr].Hit(now, 1.0, lim.ctrl_per_addr)) return;
            OnCtrl(data, n, addr, ip, token, st);
        } else if (typ == T_GAME_IN) {
            auto e = token ? tokens.find(token) : tokens.end();
            if (e == tokens.end()) return;
            auto [room, m] = e->second;  // copies: touch may not drop them, but keep it obviously safe
            Touch(*room, *m, addr, ip);
            Forward(*room, *m, data + kHdr, n - kHdr);
        }
    }

    void Touch(Room& room, Member& m, uint64_t addr, uint32_t ip) {
        m.last = now;
        m.addr = addr, m.ip = ip;
        if (!m.verified) {
            m.verified = true;
            Log("[" + room.code + "] slot " + std::to_string(m.slot) + " verified " + Peer(addr));
            SendState(room, m);
        }
        if (!m.conn) {
            m.conn = true;
            Log("[" + room.code + "] slot " + std::to_string(m.slot) + " reconnected");
            Changed(room);
        }
    }

    void Forward(Room& room, Member& m, const uint8_t* p, size_t n) {
        if (now - m.gwin >= 1.0) m.gwin = now, m.gpkts = 0, m.gbytes = 0;
        if (m.gpkts + 1 > lim.game_pkts || m.gbytes + (long long)n > lim.game_bytes) return;
        m.gpkts++;
        m.gbytes += n;
        std::string pkt = Hdr(T_GAME_OUT, (uint8_t)m.slot) + std::string((const char*)p, n);
        for (auto& o : room.members)
            if (o.get() != &m && o->conn && o->verified) Send(pkt, o->addr, o->ip);
    }
    void OnCtrl(const uint8_t* data, size_t n, uint64_t addr, uint32_t ip, uint32_t token, IpState& st) {
        const uint8_t* body = data + kHdr;
        size_t bn = n - kHdr;
        if (token == 0) {
            // The source may be spoofed: hellos shorter than kMinHello are dropped, so no reply outgrows its request.
            if (n < kMinHello) return;
            Ctrl c;
            try { c = ParseCtrl(body, bn); } catch (const Bad& e) {
                if (e.req) SendSmall(ReplyPkt(e.req, "err reason=bad"), addr, ip, n);
                return;
            }
            if (c.cmd != "create" && c.cmd != "join") return;
            std::pair<uint64_t, uint64_t> key{addr, c.req};
            auto hit = c.req ? addrCache.find(key) : addrCache.end();
            if (hit != addrCache.end()) { SendSmall(hit->second.first, addr, ip, n); return; }
            std::string text;
            try {
                CheckKeys(c.cmd, c.args);
                text = c.cmd == "create" ? CmdCreate(c.args, addr, ip, st) : CmdJoin(c.args, addr, ip, st);
            } catch (const Bad&) { text = "err reason=bad"; }
            if (c.req) {
                std::string pkt = ReplyPkt(c.req, text);
                SendSmall(pkt, addr, ip, n);
                addrCache[key] = {pkt, now + t.dedupe_ttl};
            }
            return;
        }

        auto e = tokens.find(token);
        if (e == tokens.end()) {
            if (!st.badtok.Hit(now, 60.0, lim.badtok_per_min)) return;
            std::string body_s((const char*)body, bn), head = body_s.substr(0, body_s.find(' '));
            if (!ReReq(head)) return;
            auto g = goneCache.find({token, std::stoull(head)});
            if (g != goneCache.end()) SendSmall(g->second.first, addr, ip, n);
            return;
        }
        auto [room, m] = e->second;  // copies keep both alive through leave
        Touch(*room, *m, addr, ip);
        Ctrl c;
        try { c = ParseCtrl(body, bn); } catch (const Bad& b) {
            if (b.req) Send(ReplyPkt(b.req, "err reason=bad"), addr, ip);
            return;
        }
        if (c.req) {
            for (auto& [r, pkt] : m->replies)
                if (r == c.req) { Send(pkt, addr, ip); return; }
        }
        std::string text;  // every command answers (Python's "text is None" never happens)
        if (c.cmd == "create" || c.cmd == "join") text = "err reason=bad";
        else try {
            CheckKeys(c.cmd, c.args);
            text = Run(*room, m, c.cmd, c.args);
        } catch (const Bad&) { text = "err reason=bad"; }
        if (c.req) {
            std::string pkt = ReplyPkt(c.req, text);
            Send(pkt, addr, ip);
            m->replies.emplace_back(c.req, pkt);
            if (m->replies.size() > kCachePerMember) m->replies.pop_front();
            if (c.cmd == "leave") goneCache[{token, c.req}] = {pkt, now + t.dedupe_ttl};
        }
        if (c.cmd == "ping" && tokens.count(token)) SendState(*room, *m);
    }
    // ---------- commands ----------
    std::string Run(Room& room, const MemP& m, const std::string& cmd, Args& a) {
        if (cmd == "leave") return CmdLeave(room, m);
        if (cmd == "set") return CmdSet(room, *m, a);
        if (cmd == "conf") return CmdConf(room, *m, a);
        if (cmd == "start") return CmdStart(room, *m);
        if (cmd == "arena") return CmdArena(room, *m, a);
        if (cmd == "dead") return CmdDead(room, *m, a);
        if (cmd == "hp") return CmdHp(room, *m, a);
        if (cmd == "mods") return CmdMods(room, a);
        return CmdPing(a);
    }

    uint32_t NewToken() {
        for (;;) {
            uint32_t v = (uint32_t)rng();
            if (v && !tokens.count(v)) return v;
        }
    }

    Member& AddMember(const RoomP& room, const std::string& name, uint64_t addr, uint32_t ip) {
        int slot = 0;
        while (room->At(slot)) slot++;  // lowest free slot; callers checked there is one below max
        auto m = std::make_shared<Member>();
        m->slot = slot, m->name = name, m->token = NewToken(), m->addr = addr, m->ip = ip, m->last = now;
        room->members.push_back(m);
        tokens[m->token] = {room, m};
        return *m;
    }

    static bool SameConstTime(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;  // like hmac.compare_digest, only the length leaks
        unsigned char d = 0;
        for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ b[i]);
        return d == 0;
    }

    // Checks shared by create/join; returns the error text ("" = fine) and fills name.
    std::string HelloCommon(const Args& a, std::string& name) {
        auto ver = Get(a, "ver");
        if (!ver || *ver != kProtoVer) return "err reason=version";
        if (!password.empty() && !SameConstTime(GetOr(a, "pw", ""), password)) return "err reason=password";
        try { name = CleanText(GetOr(a, "name", ""), 24, SIZE_MAX); } catch (const Bad&) { return "err reason=value"; }
        return "";
    }
    std::string CmdCreate(const Args& a, uint64_t addr, uint32_t ip, IpState& st) {
        std::string name, err = HelloCommon(a, name);
        if (!err.empty()) return err;
        int mx;
        try { std::string v = GetOr(a, "max", "2"); mx = ToInt(&v, 2, lim.max_players); } catch (const Bad&) { return "err reason=value"; }
        if (!st.create.Hit(now, 60.0, lim.creates_per_min)) return "err reason=rate";
        auto rb = roomsByIp.find(ip);
        if ((rb == roomsByIp.end() ? 0 : rb->second) >= lim.rooms_per_ip) return "err reason=rate";
        if (rooms.size() >= (size_t)lim.max_rooms) return "err reason=server_full";
        std::string code;
        do {
            char b[8];
            snprintf(b, sizeof b, "%06u", (unsigned)std::uniform_int_distribution<uint32_t>(0, 999999)(rng));
            code = b;
        } while (rooms.count(code));
        auto room = std::make_shared<Room>();
        room->code = code, room->max = mx, room->owner_ip = ip;
        if (mx > 2) room->mode = "ffa";
        rooms[code] = room;
        roomsByIp[ip]++;
        Member& m = AddMember(room, name, addr, ip);
        room->host = m.slot;
        Log("[" + code + "] created by " + Repr(m.name) + " " + Peer(addr) + " max=" + std::to_string(mx));
        Changed(*room, m.name + " created the room");
        return "ok code=" + code + " slot=" + std::to_string(m.slot) + " token=" + std::to_string(m.token);
    }

    std::string CmdJoin(const Args& a, uint64_t addr, uint32_t ip, IpState& st) {
        std::string name, err = HelloCommon(a, name);
        if (!err.empty()) return err;
        if (!st.join.Hit(now, 60.0, lim.joins_per_min)) return "err reason=rate";
        Win& bj = st.badjoin;
        if (now - bj.start < 600.0 && bj.n >= lim.badjoin_per_10min) return "err reason=rate";  // also for right codes: no oracle
        std::string code = GetOr(a, "code", "");
        auto r = ReCode(code) ? rooms.find(code) : rooms.end();
        if (r == rooms.end()) {
            bj.Hit(now, 600.0, lim.badjoin_per_10min);
            if (bj.n >= lim.badjoin_per_10min) Log("join guessing from " + IpText(ip) + ", throttled");
            return "err reason=noroom";
        }
        RoomP room = r->second;
        if (room->InMatch()) return "err reason=started";
        if (room->members.size() >= (size_t)room->max) return "err reason=full";
        Member& m = AddMember(room, name, addr, ip);
        Log("[" + room->code + "] " + Repr(m.name) + " joined slot " + std::to_string(m.slot) + " " + Peer(addr));
        Changed(*room, m.name + " joined");
        return "ok code=" + room->code + " slot=" + std::to_string(m.slot) + " token=" + std::to_string(m.token);
    }

    static std::string IpText(uint32_t ip) {
        char b[16];
        snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
        return b;
    }
    std::string CmdLeave(Room& room, const MemP& m) {
        Log("[" + room.code + "] slot " + std::to_string(m->slot) + " left");
        Remove(room, *m, m->name + " left");
        return "ok";
    }

    std::string CmdSet(Room& room, Member& m, const Args& a) {
        if (room.phase != "lobby") return "err reason=phase";
        auto ready = Get(a, "ready"), weapon = Get(a, "weapon"), edge = Get(a, "edge"), mods = Get(a, "mods");
        if (ready && *ready != "0" && *ready != "1") return "err reason=value";
        std::string w, e, md;
        if (weapon) try { w = CleanText(*weapon, SIZE_MAX, 200); } catch (const Bad&) { return "err reason=value"; }
        if (edge) try { e = CleanText(*edge, SIZE_MAX, 200); } catch (const Bad&) { return "err reason=value"; }
        if (mods) try { md = CleanText(*mods, SIZE_MAX, 200); } catch (const Bad&) { return "err reason=value"; }
        if (ready) m.ready = *ready == "1";
        if (weapon) m.weapon = w;
        if (edge) m.edge = e;
        if (mods) m.mods = md;
        Changed(room);
        return "ok";
    }

    std::string CmdConf(Room& room, Member& m, const Args& a) {
        if (m.slot != room.host) return "err reason=nothost";
        if (room.phase != "lobby") return "err reason=phase";
        for (const auto& [k, v] : a) if (k != "mode" && k != "rules" && !ReInt(v)) return "err reason=value";
        auto num = [&](const char* k, int& out) { auto v = Get(a, k); if (v) out = std::stoi(*v); return v != nullptr; };
        int rounds = room.rounds, time = room.time, dmg = room.dmg, map = room.map, mx = room.max;
        int maps = room.maps, respawn = room.respawn, kills = room.kills;
        std::string mode = room.mode, rules = room.rules;
        auto md = Get(a, "mode"), rl = Get(a, "rules");
        if (md) { if (*md != "duel" && *md != "ffa" && *md != "kill") return "err reason=value"; mode = *md; }
        if (rl) { if (!ReRules(*rl)) return "err reason=value"; rules = *rl; }
        if (num("maps", maps) && !(maps >= 0 && maps <= 65535)) return "err reason=value";
        if (num("respawn", respawn) && !(respawn >= 1 && respawn <= 10)) return "err reason=value";
        if (num("kills", kills) && !(kills >= 0 && kills <= 50)) return "err reason=value";
        if (num("rounds", rounds) && !(rounds == 1 || rounds == 3 || rounds == 5 || rounds == 7 || rounds == 9))
            return "err reason=value";
        if (num("time", time) && !(time == 0 || (time >= 30 && time <= 600))) return "err reason=value";
        if (num("dmg", dmg) && !(dmg >= 10 && dmg <= 500)) return "err reason=value";
        if (num("map", map) && !(map >= 0 && map <= 15)) return "err reason=value";
        bool maxGiven = num("max", mx);
        if (maxGiven && (mx < 2 || mx > lim.max_players)) return "err reason=value";
        // all given fields take effect together, then the result is checked
        if (md && *md == "duel" && !maxGiven) mx = 2;
        if (mode == "duel" && mx != 2) return "err reason=value";
        for (auto& o : room.members) if (o->slot >= mx) return "err reason=value";
        room.rounds = rounds, room.time = time, room.dmg = dmg, room.map = map, room.max = mx;
        room.maps = maps, room.respawn = respawn, room.kills = kills, room.mode = mode, room.rules = rules;
        Changed(room);
        return "ok";
    }

    std::string CmdStart(Room& room, Member& m) {
        if (m.slot != room.host) return "err reason=nothost";
        if (room.phase != "lobby") return "err reason=phase";
        if (room.members.size() < 2) return "err reason=players";
        for (auto& o : room.members) if (o->slot != room.host && !o->ready) return "err reason=notready";
        for (auto& o : room.members) o->score = 0;
        room.round = 1;
        Log("[" + room.code + "] match start rounds=" + std::to_string(room.rounds) + " time=" + std::to_string(room.time));
        EnterLoad(room);
        return "ok";
    }
    // arena/dead/hp: round is compared as text (round=01 is valid but never matches), like the Python.
    std::string CmdArena(Room& room, Member& m, const Args& a) {
        ToInt(Get(a, "round"), 0, 99);
        if (room.phase == "load" && *Get(a, "round") == std::to_string(room.round)) {
            m.arena = true;
            CheckLoad(room);
        }
        return "ok";
    }

    std::string CmdDead(Room& room, Member& m, const Args& a) {
        ToInt(Get(a, "round"), 0, 99);
        if (auto by = Get(a, "by")) ToInt(by, -1, kMaxSlots - 1);
        if (auto life = Get(a, "life")) ToInt(life, 0, 9999);
        if (room.phase == "fight" && *Get(a, "round") == std::to_string(room.round) && m.alive) {
            m.alive = false;
            Log("[" + room.code + "] slot " + std::to_string(m.slot) + " dead by " + GetOr(a, "by", "-1"));
            if (!CheckFight(room)) Changed(room);
        }
        return "ok";
    }

    std::string CmdHp(Room& room, Member& m, const Args& a) {
        ToInt(Get(a, "round"), 0, 99);
        int pct = ToInt(Get(a, "pct"), 0, 1000);
        if (*Get(a, "round") == std::to_string(room.round)) m.hp = pct;
        return "ok";
    }

    std::string CmdMods(const Room& room, const Args& a) {
        int slot = ToInt(Get(a, "slot"), 0, kMaxSlots - 1);
        const Member* o = room.At(slot);
        if (!o) return "err reason=value";
        return "ok slot=" + std::to_string(slot) + " mods=" + PctEnc(o->mods);
    }

    std::string CmdPing(const Args& a) {
        std::string v = GetOr(a, "t", "");
        if (!ReReq(v)) throw Bad{};
        return "ok t=" + v;
    }

    // ---------- room logic ----------
    void DeleteRoom(Room& room, const std::string& why = "deleted") {
        auto it = rooms.find(room.code);
        if (it == rooms.end() || it->second.get() != &room) return;
        RoomP keep = it->second;  // the map held the last reference for an empty room
        rooms.erase(it);
        for (auto& o : room.members) tokens.erase(o->token);
        room.members.clear();
        auto rb = roomsByIp.find(room.owner_ip);
        if (rb != roomsByIp.end() && --rb->second <= 0) roomsByIp.erase(rb);
        Log("[" + room.code + "] " + why);
    }
    void Remove(Room& room, Member& m, std::string msg) {
        auto& ms = room.members;
        MemP keep;  // m may be the last reference once it leaves the room
        for (auto it = ms.begin(); it != ms.end(); ++it)
            if (it->get() == &m) { keep = *it; ms.erase(it); break; }
        tokens.erase(m.token);
        if (ms.empty()) { DeleteRoom(room); return; }
        if (room.host == m.slot) {
            int h = ms[0]->slot;
            for (auto& o : ms) h = std::min(h, o->slot);
            room.host = h;
            msg += "; " + room.At(h)->name + " is host";
        }
        if (room.InMatch() && ms.size() == 1 && room.phase != "over") {
            Enter(room, "over", t.over, ms[0]->slot, msg);
            return;
        }
        if (room.phase == "fight" && CheckFight(room, msg)) return;
        if (room.phase == "load" && CheckLoad(room, msg)) return;
        Changed(room, msg);
    }

    void Enter(Room& room, const char* phase, double dur, int winner = -1, const std::string& msg = "") {
        room.phase = phase;
        room.winner = winner;
        room.deadline = dur ? now + dur : 0.0;
        Log("[" + room.code + "] -> " + phase + " round=" + std::to_string(room.round) + " winner=" +
            std::to_string(winner));
        Changed(room, msg);
    }

    void EnterLoad(Room& room, const std::string& msg = "") {
        for (auto& o : room.members) o->alive = true, o->arena = false, o->hp = 1000;
        Enter(room, "load", t.load, -1, msg);
    }

    bool CheckLoad(Room& room, const std::string& msg = "") {  // only conn=1 members are waited for
        for (auto& o : room.members) if (o->conn && !o->arena) return false;
        Enter(room, "count", t.count, -1, msg);
        return true;
    }

    bool CheckFight(Room& room, const std::string& msg = "") {
        Member* alive = nullptr;
        int n = 0;
        for (auto& o : room.members) if (o->alive && !n++) alive = o.get();
        if (n > 1) return false;
        EndRound(room, alive, msg);
        return true;
    }

    void EndRound(Room& room, Member* win, const std::string& msg = "") {
        if (win) win->score++;
        Enter(room, "end", t.end, win ? win->slot : -1, msg);
    }
    void TimeUp(Room& room) {  // highest hp among the living wins the round; a tie is a draw
        int best = -1, top = 0;
        Member* first = nullptr;
        for (auto& o : room.members) if (o->alive) best = std::max(best, o->hp);
        for (auto& o : room.members)
            if (o->alive && o->hp == best && !top++) first = o.get();
        EndRound(room, top == 1 ? first : nullptr);
    }

    void AfterEnd(Room& room) {
        int need = room.rounds / 2 + 1;
        bool clinch = false;
        for (auto& o : room.members) clinch |= o->score >= need;
        if (clinch || room.round >= room.rounds) {
            int best = INT_MIN, top = 0;
            Member* first = nullptr;
            for (auto& o : room.members) best = std::max(best, o->score);
            for (auto& o : room.members)
                if (o->score == best && !top++) first = o.get();
            Enter(room, "over", t.over, top == 1 ? first->slot : -1);
        } else {
            room.round++;
            EnterLoad(room);
        }
    }

    void ToLobby(Room& room) {
        room.round = 0;
        for (auto& o : room.members) o->ready = false, o->alive = true;
        Enter(room, "lobby", 0);
    }

    // ---------- state ----------
    void Changed(Room& room, const std::string& msg = "") {
        room.v++;
        room.msg = msg;
        room.last_change = now;
        for (auto& m : room.members) if (m->verified) SendState(room, *m);
    }

    std::string StateText(const Room& room, int you) const {
        long long left = 0;
        if (room.deadline) left = std::max(1LL, (long long)((room.deadline - now) * 1000));
        std::string s = "v=" + std::to_string(room.v) + " you=" + std::to_string(you) + " code=" + room.code +
                        " phase=" + room.phase + " round=" + std::to_string(room.round) +
                        " rounds=" + std::to_string(room.rounds) + " time=" + std::to_string(room.time) +
                        " dmg=" + std::to_string(room.dmg) + " map=" + std::to_string(room.map) +
                        " max=" + std::to_string(room.max) + " host=" + std::to_string(room.host) +
                        " left=" + std::to_string(left) + " winner=" + std::to_string(room.winner) +
                        " mode=" + room.mode + " maps=" + std::to_string(room.maps) +
                        " respawn=" + std::to_string(room.respawn) + " kills=" + std::to_string(room.kills) +
                        " cap=" + std::to_string(lim.max_players) + " rules=" + StateEnc(room.rules);
        if (!room.msg.empty()) s += " msg=" + StateEnc(room.msg);
        std::vector<const Member*> ms;
        for (auto& o : room.members) ms.push_back(o.get());
        std::sort(ms.begin(), ms.end(), [](const Member* a, const Member* b) { return a->slot < b->slot; });
        for (const Member* o : ms)
            s += " m" + std::to_string(o->slot) + "=" + StateEnc(o->name) + "," + (o->ready ? "1" : "0") + "," +
                 StateEnc(o->weapon) + "," + std::to_string(o->score) + "," + (o->alive ? "1" : "0") + "," +
                 (o->conn ? "1" : "0") + "," + StateEnc(o->edge) + "," + ModHash(o->mods) + "," +
                 std::to_string(o->life) + "," + std::to_string(o->deaths);
        return s;
    }

    void SendState(const Room& room, const Member& m) { Send(Hdr(T_STATE, 0) + StateText(room, m.slot), m.addr, m.ip); }
    // ---------- timers ----------
    void Tick() {
        std::vector<RoomP> snap;
        for (auto& [c, r] : rooms) snap.push_back(r);
        for (const RoomP& rp : snap) {
            Room& room = *rp;
            auto live = [&] { auto it = rooms.find(room.code); return it != rooms.end() && it->second == rp; };
            if (!live()) continue;
            std::vector<MemP> ms = room.members;
            for (const MemP& m : ms) {
                if (!live()) break;
                std::string s = "[" + room.code + "] slot " + std::to_string(m->slot);
                double idle = now - m->last;
                if (!m->verified && idle >= t.verify) {
                    Log(s + " never verified, removed");
                    Remove(room, *m, m->name + " timed out");
                } else if (idle >= (room.InMatch() ? t.remove_match : t.remove)) {  // mid-match: time to restart and resume
                    Log(s + " timed out, removed");
                    Remove(room, *m, m->name + " timed out");
                } else if (idle >= t.conn_lost && m->conn) {
                    m->conn = false;
                    Log(s + " conn=0");
                    if (!(room.phase == "load" && CheckLoad(room))) Changed(room);
                }
            }
            if (live() && now - room.last_change >= t.room_idle) DeleteRoom(room, "idle, deleted");
            if (!live() || !room.deadline || now < room.deadline) continue;
            if (room.phase == "load") Enter(room, "count", t.count);
            else if (room.phase == "count") Enter(room, "fight", room.time);
            else if (room.phase == "fight") TimeUp(room);
            else if (room.phase == "end") AfterEnd(room);
            else if (room.phase == "over") ToLobby(room);
        }
        for (Cache* c : {&addrCache, &goneCache}) {
            if (c->size() > 256 || std::uniform_int_distribution<int>(0, 99)(rng) == 0)
                for (auto it = c->begin(); it != c->end();) it = it->second.second < now ? c->erase(it) : std::next(it);
        }
        if (ctrlRate.size() > 1024 || ips.size() > 1024) Prune(false);
    }

    void Prune(bool force) {
        for (auto it = ctrlRate.begin(); it != ctrlRate.end();) it = now - it->second.start > 2 ? ctrlRate.erase(it) : std::next(it);
        double keep = force ? 0.5 : 660.0;  // longest window is 10 min
        for (auto it = ips.begin(); it != ips.end();) it = now - it->second.last > keep ? ips.erase(it) : std::next(it);
    }
};
Server::Server(SendFn send, Timing t, Limits l, std::string password) : p_(std::make_unique<Impl>()) {
    p_->sendFn = std::move(send);
    p_->t = t;
    p_->lim = l;
    p_->password = std::move(password);
    std::random_device rd;  // codes and tokens must not be guessable
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    p_->rng.seed(seq);
}
Server::~Server() = default;

void Server::Handle(uint64_t peer, uint32_t ip, const uint8_t* data, size_t n, double now) {
    p_->now = now;
    try {
        p_->Handle(peer, ip, data, n);
    } catch (const std::exception& e) {  // never die on one bad packet
        p_->Log(std::string("error handling packet ") + p_->Peer(peer) + " " + e.what());
    } catch (...) {  // a stray Bad (not a std::exception) the command code failed to catch
        p_->Log("error handling packet " + p_->Peer(peer));
    }
}
void Server::Tick(double now) {
    p_->now = now;
    p_->Tick();
}
std::vector<std::string> Server::RoomCodes() const {
    std::vector<std::string> r;
    for (auto& [c, room] : p_->rooms) r.push_back(c);
    return r;
}
void Server::ExpireTimer(const std::string& code, double now) {
    auto it = p_->rooms.find(code);
    if (it != p_->rooms.end()) it->second->deadline = now;
}
void Server::SetLog(LogFn f) { p_->logFn = std::move(f); }
void Server::SetPeerName(std::function<std::string(uint64_t)> f) { p_->peerName = std::move(f); }
Timing& Server::timing() { return p_->t; }
Limits& Server::limits() { return p_->lim; }
Stats Server::GetStats() const {
    return {p_->rooms.size(), p_->tokens.size(), p_->ips.size(), p_->roomsByIp.size(), p_->rx};
}

}  // namespace rsrv

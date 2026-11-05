// Room rules (conf "rules=", docs/pvp-protocol.md "房间规则"): client-side settings the server only
// carries. Text form "k:v,k:v" ([a-z0-9:,], <= 200 bytes); unknown keys ignored, missing keys default.
// Header-only, plain C++ (used by ui/, fight/, nettest).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace rules {
struct Rules {
    int hp = 300;      // max HP %, 50..500
    int sd = 100;      // Phantom Edge (杀器) damage %, 0..300
    int sr = 100;      // 杀气 regen %, 0..300
    int ult = 100;     // ult charge %, 0..300
    int heal = 2;      // healing items: 0 none, 1 gourd only, 2 any
    int cimu = 0;      // 1 = 慈母护符 banned
    int acc = 9;       // accessory limit, 9 = any
    int edge = 1;      // 0 = Phantom Edge off
    uint32_t wban = 0; // banned weapons, bit i = fight::Weapons()[i + 1]
    int wrand = 0;     // 1 = random weapon each round
    int stun = 6;      // combo protection: no hit-stun after the Nth hit of a chain, 0 = off
    int cdmg = 1;      // 1 = combo damage falloff (from hit 4: -10% each, floor 40%)
};

inline int Clamp(long v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

inline Rules Parse(const std::string& s) {
    Rules r;
    size_t a = 0;
    while (a < s.size()) {
        size_t e = s.find(',', a); if (e == std::string::npos) e = s.size();
        std::string it = s.substr(a, e - a); a = e + 1;
        size_t c = it.find(':'); if (c == std::string::npos) continue;
        std::string k = it.substr(0, c);
        long v = strtol(it.c_str() + c + 1, nullptr, 10);
        if (k == "hp") r.hp = Clamp(v, 50, 500);
        else if (k == "sd") r.sd = Clamp(v, 0, 300);
        else if (k == "sr") r.sr = Clamp(v, 0, 300);
        else if (k == "ult") r.ult = Clamp(v, 0, 300);
        else if (k == "heal") r.heal = Clamp(v, 0, 2);
        else if (k == "cimu") r.cimu = Clamp(v, 0, 1);
        else if (k == "acc") r.acc = Clamp(v, 0, 9);
        else if (k == "edge") r.edge = Clamp(v, 0, 1);
        else if (k == "wban") r.wban = (uint32_t)strtoul(it.c_str() + c + 1, nullptr, 10) & 0x3FFFFFFFu;
        else if (k == "wrand") r.wrand = Clamp(v, 0, 1);
        else if (k == "stun") { r.stun = Clamp(v, 0, 10); if (r.stun && r.stun < 3) r.stun = 3; }
        else if (k == "cdmg") r.cdmg = Clamp(v, 0, 1);
    }
    return r;
}

// every key, so the text does not depend on the reader's defaults
inline std::string Format(const Rules& r) {
    auto kv = [](const char* k, long v) { return std::string(k) + ":" + std::to_string(v); };
    return kv("hp", r.hp) + "," + kv("sd", r.sd) + "," + kv("sr", r.sr) + "," + kv("ult", r.ult) + "," +
           kv("heal", r.heal) + "," + kv("cimu", r.cimu) + "," + kv("acc", r.acc) + "," + kv("edge", r.edge) + "," +
           kv("wban", (long)r.wban) + "," + kv("wrand", r.wrand) + "," + kv("stun", r.stun) + "," + kv("cdmg", r.cdmg);
}

// combo damage factor for the nth hit (1-based) of a chain
inline float ComboScale(const Rules& r, int nth) {
    if (!r.cdmg || nth < 4) return 1.f;
    float f = 1.f - 0.1f * float(nth - 3);
    return f < 0.4f ? 0.4f : f;
}

// FNV-1a 32 of the mods text, as the STATE "modh" subfield (8 lowercase hex, "" for "")
inline std::string ModHash(const std::string& s) {
    if (s.empty()) return "";
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    char b[9]; snprintf(b, sizeof b, "%08x", h); return b;
}
}  // namespace rules

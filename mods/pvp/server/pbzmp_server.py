#!/usr/bin/env python3
"""pbzmp lobby + relay server (protocol v2, see docs/pvp-protocol.md).

Standard library only, single UDP port.

Spec decisions where the doc is silent:
- STATE is pushed to every member on every change, and to a single member in
  reply to each of its pings (the spec's 1 s heartbeat). No extra timer push.
- `msg` in STATE carries the last room event (join/leave/host change) and is
  cleared by the next change that has no event.
- conf max=N is rejected with `value` if a current member occupies slot >= N.
- Any unknown cmd / malformed CTRL gets `err reason=bad`.
- A member that leaves / is removed during fight counts as dead (may end the
  round). During load, only conn=1 members are waited for.
- When end -> over because one player reached rounds/2+1, he is the winner;
  otherwise highest score wins, ties -> -1.
- Replies to `leave` stay cached for a short time after the token is gone, so
  a resent leave still gets its reply.
- Dedupe cache keeps the last 64 replies per member (and per address for
  create/join, expiring after 10 s).
- Rate limits use fixed windows (1 s for packets, 60 s for create/join).
- create with max > 2 makes an ffa room (duel needs max=2).
- `mods slot=` works in every phase.
- dead's life check and deaths+1 apply in every mode (life stays 0 outside
  kill mode). A member removed during a kill-mode fight does not end the round
  (only one member left still ends the match, as in every mode).
- kill mode: the round ends at kills with no extra score for the winner;
  a pending respawn is dropped when the round ends.

Hardening for a public UDP service (see "服务器限制" in the doc):
- Per-IP limits: packets/s (all types), CTRL/s per address, creates/min,
  joins/min, live rooms per IP; global room cap.
- No amplification: a reply to a packet without a valid token is one
  datagram no larger than the request, otherwise it is not sent (clients pad
  create/join with pad=...). A new member gets no STATE / GAME until a packet
  carrying its token arrives (proves the source address got the reply);
  unverified members are removed after 5 s.
- Every field is checked strictly; malformed CTRL gets `err reason=bad`.
- A member may change address (NAT rebinding), so a player could spoof his
  own token from a victim's address; egress is capped per destination IP.
- Codes and tokens come from the OS CSPRNG.
- Optional --password, sent as pw= in create/join (reason=password).
"""
import argparse
import heapq
import hmac
import random
import re
import secrets
import select
import signal
import socket
import struct
import sys
import time

MAGIC = 0xB7
VERSION = 1    # packet header version
PROTO_VER = 2  # create/join ver=
T_CTRL, T_REPLY, T_STATE, T_GAME_IN, T_GAME_OUT = 0x01, 0x02, 0x03, 0x10, 0x11
HDR = struct.Struct("<BBBBI")
MAX_DGRAM = 1200
SAFE = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~")


class Timing:
    load = 60.0
    count = 3.0
    end = 4.0
    over = 8.0
    conn_lost = 5.0
    remove = 15.0
    remove_match = 120.0  # silent member removed mid-match (time to restart the game and resume)
    dedupe_ttl = 10.0
    verify = 5.0          # unverified member (no packet with its token yet) removed
    room_idle = 3600.0    # room with no state change for this long is deleted
    status = 60.0         # status log line interval


class Limits:
    max_rooms = 1000
    rooms_per_ip = 4
    creates_per_min = 6
    joins_per_min = 20
    pps_per_ip = 1000
    ctrl_per_addr = 20
    game_pkts = 200
    game_bytes = 128 * 1024
    badjoin_per_10min = 10  # join attempts with an unknown code
    badtok_per_min = 30     # CTRL with an unknown token (resent leave lookups)
    egress_bytes = 512 * 1024  # bytes/s sent to one IP (bounds token-spoof redirects), 0 = no limit
    max_players = 8         # room size cap (--max-players), STATE cap=
    max_ips = 65536         # tracked IPs before forced pruning


CACHE_PER_MEMBER = 64
MIN_HELLO = 64  # create/join datagrams (token=0) shorter than this are dropped
MAX_ARGS = 16
RE_KEY = re.compile(r"[a-z]{1,16}\Z")
RE_INT = re.compile(r"-?[0-9]{1,6}\Z")
RE_REQ = re.compile(r"[0-9]{1,10}\Z")
RE_CODE = re.compile(r"[0-9]{6}\Z")
RE_RULES = re.compile(r"[a-z0-9:,]{0,200}\Z")
CMDS = {"create", "join", "leave", "set", "conf", "start", "arena", "dead",
        "hp", "ping", "mods"}
MODES = ("duel", "ffa", "kill")
MAX_SLOTS = 8  # protocol limit; slot/by ranges


# allowed argument keys per command
KEYS = {
    "create": {"name", "ver", "max", "pw", "pad"},
    "join": {"code", "name", "ver", "pw", "pad"},
    "leave": set(), "start": set(),
    "set": {"ready", "weapon", "edge", "mods"},
    "conf": {"rounds", "time", "dmg", "max", "map", "maps", "mode", "respawn",
             "kills", "rules"},
    "arena": {"round"}, "dead": {"round", "by", "life"},
    "hp": {"round", "pct"}, "ping": {"t"}, "mods": {"slot"},
}


class Bad(Exception):
    pass


def check_keys(cmd, args):
    if not args.keys() <= KEYS[cmd]:
        raise Bad
    if "pad" in args and args["pad"].strip("-"):
        raise Bad


def clean_text(v, max_chars=None, max_bytes=None):
    if has_ctl(v) or "\ufffd" in v:
        raise Bad
    if max_chars is not None:
        v = v[:max_chars]
    if max_bytes is not None and len(v.encode()) > max_bytes:
        raise Bad
    return v


def to_int(v, lo, hi):
    if v is None or not RE_INT.match(v):
        raise Bad
    n = int(v)
    if not lo <= n <= hi:
        raise Bad
    return n


def has_ctl(s):
    return any(ord(c) < 32 or ord(c) == 127 for c in s)


def pct_dec_strict(v: str) -> str:
    raw = v.encode("ascii")  # caller checked the alphabet
    out = bytearray()
    i = 0
    while i < len(raw):
        c = raw[i]
        if c == 0x25:
            h = raw[i + 1:i + 3]
            if len(h) != 2 or not all(x in b"0123456789ABCDEFabcdef" for x in h):
                raise Bad
            out.append(int(h, 16))
            i += 3
        else:
            out.append(c)
            i += 1
    return out.decode("utf-8")  # UnicodeDecodeError is a ValueError -> caller


def parse_ctrl(body: bytes):
    """-> (req, cmd, args) ; raises Bad (req known or None) on malformed."""
    head = body.split(b" ", 1)[0].decode("latin-1")
    if not RE_REQ.match(head) or int(head) > 0xFFFFFFFF:
        raise Bad(None)
    req = int(head)
    try:
        parts = body.decode("ascii").split(" ")  # clients percent-encode
    except UnicodeDecodeError:
        raise Bad(req)
    if len(parts) < 2 or parts[1] not in CMDS or len(parts) - 2 > MAX_ARGS:
        raise Bad(req)
    args = {}
    for p in parts[2:]:
        k, eq, v = p.partition("=")
        if not eq or not RE_KEY.match(k) or k in args:
            raise Bad(req)
        if any(ord(c) not in SAFE and c != "%" for c in v):
            raise Bad(req)
        try:
            args[k] = pct_dec_strict(v)
        except (Bad, ValueError):
            raise Bad(req)
    return req, parts[1], args


def pct_enc(s) -> str:
    b = s.encode("utf-8") if isinstance(s, str) else bytes(s)
    return "".join(chr(c) if c in SAFE else "%%%02X" % c for c in b)


def state_enc(s: str) -> str:
    """STATE v2 value escaping: only 0x00-0x20 % , = 0x7F; UTF-8 stays raw."""
    out = bytearray()
    for c in s.encode("utf-8"):
        if c <= 0x20 or c in b"%,=\x7f":
            out += b"%%%02X" % c
        else:
            out.append(c)
    return out.decode("utf-8")


def mod_hash(s: str) -> str:
    """FNV-1a 32 of the mods text, 8 lowercase hex digits ("" for "")."""
    if not s:
        return ""
    h = 2166136261
    for c in s.encode("utf-8"):
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return "%08x" % h


def pct_dec(s: str) -> str:
    out = bytearray()
    raw = s.encode("utf-8")
    i = 0
    while i < len(raw):
        c = raw[i]
        if c == 0x25 and i + 2 < len(raw):
            try:
                out.append(int(raw[i + 1:i + 3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out.append(c)
        i += 1
    return out.decode("utf-8", "replace")


def log(*a, file=None):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *a, flush=True,
          file=file or sys.stdout)


class Win:
    """Fixed-window counter."""
    __slots__ = ("start", "n")

    def __init__(self):
        self.start, self.n = -1e18, 0

    def hit(self, now, span, limit):
        """Count one event; True if it is within the limit."""
        if now - self.start >= span:
            self.start, self.n = now, 0
        self.n += 1
        return self.n <= limit


class IpState:
    __slots__ = ("pkt", "create", "join", "badjoin", "badtok", "out", "last")

    def __init__(self):
        self.pkt, self.create, self.join = Win(), Win(), Win()
        self.badjoin, self.badtok, self.out = Win(), Win(), Win()
        self.last = 0.0


class Member:
    def __init__(self, slot, name, token, addr, now):
        self.slot, self.name, self.token, self.addr = slot, name, token, addr
        self.ready = False
        self.weapon = ""
        self.edge = ""  # Phantom Edge (杀器) pick, same rules as weapon
        self.mods = ""  # enabled client mods "id:ver:flag;...", opaque text, same rules
        self.score = 0
        self.life = 0    # this round's life number (kill mode: +1 per respawn)
        self.deaths = 0  # deaths this match
        self.respawn_at = 0.0  # kill mode: when a dead member is re-alived (0 = not pending)
        self.alive = True
        self.conn = True
        self.last = now
        self.hp = 1000
        self.arena = False
        self.replies = {}  # req -> bytes
        self.gwin = 0.0
        self.gpkts = 0
        self.gbytes = 0
        self.verified = False  # a packet carrying our token has arrived
        self.ip = addr[0]


class Room:
    def __init__(self, code, max_):
        self.code = code
        self.max = max_
        self.rounds = 3
        self.time = 90
        self.dmg = 100
        self.map = 0
        self.maps = 0       # random map bitmask, 0 = use map
        self.mode = "duel" if max_ <= 2 else "ffa"
        self.respawn = 5
        self.kills = 10
        self.rules = ""
        self.phase = "lobby"
        self.round = 0
        self.winner = -1
        self.host = 0
        self.v = 0
        self.msg = ""
        self.deadline = 0.0  # 0 = unlimited
        self.members = {}  # slot -> Member
        self.owner_ip = ""
        self.last_change = 0.0

    def in_match(self):
        return self.phase != "lobby"


class Server:
    def __init__(self, bind="0.0.0.0", port=27777, loss=0.0, delay=0.0,
                 jitter=0.0, timing=None, quiet=False, seed=None,
                 password="", limits=None, log_file=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for opt in (socket.SO_RCVBUF, socket.SO_SNDBUF):
            try:
                self.sock.setsockopt(socket.SOL_SOCKET, opt, 1 << 20)
            except OSError:
                pass
        self.sock.bind((bind, port))
        self.sock.setblocking(False)
        self.port = self.sock.getsockname()[1]
        self.t = timing or Timing()
        self.loss, self.delay, self.jitter = loss, delay / 1000.0, jitter / 1000.0
        self.rng = random.Random(seed)
        self.quiet = quiet
        self.password = password or ""
        self.lim = limits or Limits()
        self.logf = log_file
        self.ips = {}          # ip -> IpState
        self.rooms_by_ip = {}  # ip -> live rooms created from it
        self.ctrl_rate = {}    # addr -> Win
        self.rx_count = 0      # datagrams received since the last status line
        self.next_status = 0.0
        self.rooms = {}       # code -> Room
        self.tokens = {}      # token -> (Room, Member)
        self.addr_cache = {}  # (addr, req) -> (bytes, expiry)
        self.gone_cache = {}  # (token, req) -> (bytes, expiry)
        self.outq = []        # (time, seq, data, addr)
        self.seq = 0
        self.running = False

    # ---------- io ----------
    def log(self, *a):
        if not self.quiet or self.logf:
            log(*a, file=self.logf)

    def send(self, data, addr):
        st = self.ips.get(addr[0])
        if st is not None:
            w = st.out
            if self.mono() - w.start >= 1.0:
                w.start, w.n = self.mono(), 0
            if self.lim.egress_bytes and w.n + len(data) > self.lim.egress_bytes:
                return
            w.n += len(data)
        if self.loss and self.rng.random() * 100 < self.loss:
            return
        d = self.delay + (self.rng.uniform(0, self.jitter) if self.jitter else 0)
        if d <= 0:
            self._raw(data, addr)
        else:
            self.seq += 1
            heapq.heappush(self.outq, (time.monotonic() + d, self.seq, data, addr))

    mono = staticmethod(time.monotonic)

    def _raw(self, data, addr):
        try:
            self.sock.sendto(data, addr)
        except OSError:
            pass

    def serve_forever(self):
        self.running = True
        self.next_status = time.monotonic() + self.t.status
        self.log(f"listening on udp {self.sock.getsockname()}"
                 + (" (password set)" if self.password else ""))
        while self.running:
            now = time.monotonic()
            tmo = 0.02
            if self.outq:
                tmo = max(0.0, min(tmo, self.outq[0][0] - now))
            r, _, _ = select.select([self.sock], [], [], tmo)
            if r:
                for _ in range(256):
                    try:
                        data, addr = self.sock.recvfrom(2048)
                    except (BlockingIOError, ConnectionResetError):
                        break
                    except OSError:
                        break
                    try:
                        self.handle(data, addr, time.monotonic())
                    except Exception as e:  # never die on one bad packet
                        self.log("error handling packet", addr, repr(e))
            now = time.monotonic()
            while self.outq and self.outq[0][0] <= now:
                _, _, d, a = heapq.heappop(self.outq)
                self._raw(d, a)
            self.tick(now)
            if now >= self.next_status:
                self.status_line(now)
        self.log(f"shutting down: {len(self.rooms)} rooms, "
                 f"{len(self.tokens)} players dropped")
        self.sock.close()

    def status_line(self, now):
        span = self.t.status + (now - self.next_status)
        self.next_status = now + self.t.status
        self.log(f"status rooms={len(self.rooms)} players={len(self.tokens)} "
                 f"pps={self.rx_count / max(span, 1e-3):.1f} ips={len(self.ips)}")
        self.rx_count = 0

    def stop(self):
        self.running = False

    # ---------- packets ----------
    def handle(self, data, addr, now):
        self.rx_count += 1
        ip = addr[0]
        st = self.ips.get(ip)
        if st is None:
            if len(self.ips) >= self.lim.max_ips:
                self.prune(now, force=True)
            st = self.ips[ip] = IpState()
        st.last = now
        if not st.pkt.hit(now, 1.0, self.lim.pps_per_ip):
            return
        if len(data) < HDR.size or len(data) > MAX_DGRAM:
            return
        magic, ver, typ, slot, token = HDR.unpack_from(data)
        if magic != MAGIC or ver != VERSION or slot != 0:
            return
        if typ == T_CTRL:
            w = self.ctrl_rate.get(addr)
            if w is None:
                w = self.ctrl_rate[addr] = Win()
            if not w.hit(now, 1.0, self.lim.ctrl_per_addr):
                return
            self.on_ctrl(data, addr, token, now, st)
        elif typ == T_GAME_IN:
            ent = self.tokens.get(token) if token else None
            if not ent:
                return
            room, m = ent
            self.touch(room, m, addr, now)
            self.forward(room, m, data[HDR.size:], now)

    def touch(self, room, m, addr, now):
        m.last = now
        m.addr = addr
        if not m.verified:
            m.verified = True
            self.log(f"[{room.code}] slot {m.slot} verified {addr}")
            self.send_state(room, m)
        if not m.conn:
            m.conn = True
            self.log(f"[{room.code}] slot {m.slot} reconnected")
            self.changed(room)

    def forward(self, room, m, payload, now):
        if now - m.gwin >= 1.0:
            m.gwin, m.gpkts, m.gbytes = now, 0, 0
        if m.gpkts + 1 > self.lim.game_pkts or \
                m.gbytes + len(payload) > self.lim.game_bytes:
            return
        m.gpkts += 1
        m.gbytes += len(payload)
        pkt = HDR.pack(MAGIC, VERSION, T_GAME_OUT, m.slot, 0) + payload
        for o in room.members.values():
            if o is not m and o.conn and o.verified:
                self.send(pkt, o.addr)

    def reply_pkt(self, req, text):
        return HDR.pack(MAGIC, VERSION, T_REPLY, 0, 0) + f"{req} {text}".encode()

    def reply(self, addr, req, text):
        pkt = self.reply_pkt(req, text)
        self.send(pkt, addr)
        return pkt

    def send_small(self, pkt, addr, limit):
        """Reply to a packet without a valid token: never larger than it."""
        if len(pkt) <= limit:
            self.send(pkt, addr)

    def on_ctrl(self, data, addr, token, now, st):
        body = data[HDR.size:]
        n = len(data)
        if token == 0:
            # The source address may be spoofed: hellos shorter than
            # MIN_HELLO are dropped, so no reply can be bigger than its request.
            if n < MIN_HELLO:
                return
            try:
                req, cmd, args = parse_ctrl(body)
            except Bad as e:
                if e.args and e.args[0]:
                    self.send_small(self.reply_pkt(e.args[0], "err reason=bad"),
                                    addr, n)
                return
            if cmd not in ("create", "join"):
                return
            key = (addr, req)
            if req and key in self.addr_cache:
                self.send_small(self.addr_cache[key][0], addr, n)
                return
            try:
                check_keys(cmd, args)
                text = self.cmd_create(args, addr, now, st) if cmd == "create" \
                    else self.cmd_join(args, addr, now, st)
            except Bad:
                text = "err reason=bad"
            if req:
                pkt = self.reply_pkt(req, text)
                self.send_small(pkt, addr, n)
                self.addr_cache[key] = (pkt, now + self.t.dedupe_ttl)
            return

        ent = self.tokens.get(token)
        if not ent:
            if not st.badtok.hit(now, 60.0, self.lim.badtok_per_min):
                return
            head = body.split(b" ", 1)[0]
            if not RE_REQ.match(head.decode("latin-1")):
                return
            g = self.gone_cache.get((token, int(head)))
            if g:
                self.send_small(g[0], addr, n)
            return
        room, m = ent
        self.touch(room, m, addr, now)
        try:
            req, cmd, args = parse_ctrl(body)
        except Bad as e:
            if e.args and e.args[0]:
                self.reply(addr, e.args[0], "err reason=bad")
            return
        if req and req in m.replies:
            self.send(m.replies[req], addr)
            return
        if cmd in ("create", "join"):
            text = "err reason=bad"
        else:
            try:
                check_keys(cmd, args)
                text = getattr(self, "cmd_" + cmd)(room, m, args, now)
            except Bad:
                text = "err reason=bad"
        if req and text is not None:
            pkt = self.reply(addr, req, text)
            m.replies[req] = pkt
            if len(m.replies) > CACHE_PER_MEMBER:
                m.replies.pop(next(iter(m.replies)))
            if cmd == "leave":
                self.gone_cache[(token, req)] = (pkt, now + self.t.dedupe_ttl)
        if cmd == "ping" and token in self.tokens:
            self.send_state(room, m)

    # ---------- commands ----------
    def new_token(self):
        while True:
            t = secrets.randbits(32)
            if t and t not in self.tokens:
                return t

    def add_member(self, room, name, addr, now):
        slot = min(s for s in range(room.max) if s not in room.members)
        m = Member(slot, name, self.new_token(), addr, now)
        room.members[slot] = m
        self.tokens[m.token] = (room, m)
        return m

    def hello_common(self, a):
        """Checks shared by create/join; returns (error_text, name)."""
        if a.get("ver") != str(PROTO_VER):
            return "err reason=version", ""
        if self.password and not hmac.compare_digest(
                a.get("pw", "").encode(), self.password.encode()):
            return "err reason=password", ""
        try:
            name = clean_text(a.get("name", ""), max_chars=24)
        except Bad:
            return "err reason=value", ""
        return None, name

    def cmd_create(self, a, addr, now, st):
        err, name = self.hello_common(a)
        if err:
            return err
        try:
            mx = to_int(a.get("max", "2"), 2, self.lim.max_players)
        except Bad:
            return "err reason=value"
        ip = addr[0]
        if not st.create.hit(now, 60.0, self.lim.creates_per_min):
            return "err reason=rate"
        if self.rooms_by_ip.get(ip, 0) >= self.lim.rooms_per_ip:
            return "err reason=rate"
        if len(self.rooms) >= self.lim.max_rooms:
            return "err reason=server_full"
        while True:
            code = "%06d" % secrets.randbelow(1000000)
            if code not in self.rooms:
                break
        room = Room(code, mx)
        room.owner_ip = ip
        self.rooms[code] = room
        self.rooms_by_ip[ip] = self.rooms_by_ip.get(ip, 0) + 1
        m = self.add_member(room, name, addr, now)
        room.host = m.slot
        self.log(f"[{code}] created by {m.name!r} {addr} max={mx}")
        self.changed(room, f"{m.name} created the room")
        return f"ok code={code} slot={m.slot} token={m.token}"

    def cmd_join(self, a, addr, now, st):
        err, name = self.hello_common(a)
        if err:
            return err
        if not st.join.hit(now, 60.0, self.lim.joins_per_min):
            return "err reason=rate"
        bj = st.badjoin
        if now - bj.start < 600.0 and bj.n >= self.lim.badjoin_per_10min:
            return "err reason=rate"  # also for right codes: no oracle
        code = a.get("code", "")
        room = self.rooms.get(code) if RE_CODE.match(code) else None
        if not room:
            bj.hit(now, 600.0, self.lim.badjoin_per_10min)
            if bj.n >= self.lim.badjoin_per_10min:
                self.log(f"join guessing from {addr[0]}, throttled")
            return "err reason=noroom"
        if room.in_match():
            return "err reason=started"
        if len(room.members) >= room.max:
            return "err reason=full"
        m = self.add_member(room, name, addr, now)
        self.log(f"[{room.code}] {m.name!r} joined slot {m.slot} {addr}")
        self.changed(room, f"{m.name} joined")
        return f"ok code={room.code} slot={m.slot} token={m.token}"

    def cmd_leave(self, room, m, a, now):
        self.log(f"[{room.code}] slot {m.slot} left")
        self.remove(room, m, now, f"{m.name} left")
        return "ok"

    def cmd_set(self, room, m, a, now):
        if room.phase != "lobby":
            return "err reason=phase"
        if "ready" in a:
            if a["ready"] not in ("0", "1"):
                return "err reason=value"
        for k in ("weapon", "edge", "mods"):
            if k in a:
                try:
                    a[k] = clean_text(a[k], max_bytes=200)
                except Bad:
                    return "err reason=value"
        if "ready" in a:
            m.ready = a["ready"] == "1"
        if "weapon" in a:
            m.weapon = a["weapon"]
        if "edge" in a:
            m.edge = a["edge"]
        if "mods" in a:
            m.mods = a["mods"]
        self.changed(room)
        return "ok"

    def cmd_conf(self, room, m, a, now):
        if m.slot != room.host:
            return "err reason=nothost"
        if room.phase != "lobby":
            return "err reason=phase"
        new = {}
        try:
            for k in a:
                if k not in ("mode", "rules") and not RE_INT.match(a[k]):
                    raise ValueError
            if "mode" in a:
                if a["mode"] not in MODES:
                    raise ValueError
                new["mode"] = a["mode"]
            if "rules" in a:
                if not RE_RULES.match(a["rules"]):
                    raise ValueError
                new["rules"] = a["rules"]
            if "maps" in a:
                v = int(a["maps"])
                if not 0 <= v <= 65535:
                    raise ValueError
                new["maps"] = v
            if "respawn" in a:
                v = int(a["respawn"])
                if not 1 <= v <= 10:
                    raise ValueError
                new["respawn"] = v
            if "kills" in a:
                v = int(a["kills"])
                if not 0 <= v <= 50:
                    raise ValueError
                new["kills"] = v
            if "rounds" in a:
                v = int(a["rounds"])
                if v not in (1, 3, 5, 7, 9):
                    raise ValueError
                new["rounds"] = v
            if "time" in a:
                v = int(a["time"])
                if not (v == 0 or 30 <= v <= 600):
                    raise ValueError
                new["time"] = v
            if "dmg" in a:
                v = int(a["dmg"])
                if not 10 <= v <= 500:
                    raise ValueError
                new["dmg"] = v
            if "map" in a:
                v = int(a["map"])
                if not 0 <= v <= 15:
                    raise ValueError
                new["map"] = v
            if "max" in a:
                v = int(a["max"])
                if not 2 <= v <= self.lim.max_players:
                    raise ValueError
                new["max"] = v
            # all given fields take effect together, then the result is checked
            if new.get("mode") == "duel" and "max" not in new:
                new["max"] = 2
            mode = new.get("mode", room.mode)
            mx = new.get("max", room.max)
            if mode == "duel" and mx != 2:
                raise ValueError
            if any(s >= mx for s in room.members):
                raise ValueError
        except ValueError:
            return "err reason=value"
        for k, v in new.items():
            setattr(room, k, v)
        self.changed(room)
        return "ok"

    def cmd_start(self, room, m, a, now):
        if m.slot != room.host:
            return "err reason=nothost"
        if room.phase != "lobby":
            return "err reason=phase"
        if len(room.members) < 2:
            return "err reason=players"
        if any(not o.ready for o in room.members.values() if o.slot != room.host):
            return "err reason=notready"
        if room.mode == "kill" and room.time == 0 and room.kills == 0:
            return "err reason=value"  # nothing would ever end the round
        for o in room.members.values():
            o.score = 0
            o.deaths = 0
        room.round = 1
        self.log(f"[{room.code}] match start rounds={room.rounds} time={room.time}")
        self.enter_load(room, now)
        return "ok"

    def cmd_arena(self, room, m, a, now):
        to_int(a.get("round"), 0, 99)
        if room.phase == "load" and a.get("round") == str(room.round):
            m.arena = True
            self.check_load(room, now)
        return "ok"

    def cmd_dead(self, room, m, a, now):
        to_int(a.get("round"), 0, 99)
        if "by" in a:
            to_int(a["by"], -1, MAX_SLOTS - 1)
        life = to_int(a["life"], 0, 9999) if "life" in a else m.life
        if (room.phase == "fight" and a.get("round") == str(room.round) and m.alive
                and life == m.life):
            m.alive = False
            m.deaths += 1
            by = int(a.get("by", "-1"))
            self.log(f"[{room.code}] slot {m.slot} dead by {by} life {m.life}")
            if room.mode == "kill":
                k = room.members.get(by) if by != m.slot else None
                if k:
                    k.score += 1
                if k and room.kills and k.score >= room.kills:
                    self.end_round(room, k, now, add=False)
                else:
                    m.respawn_at = now + room.respawn
                    self.changed(room)
            elif not self.check_fight(room, now):
                self.changed(room)
        return "ok"

    def cmd_hp(self, room, m, a, now):
        to_int(a.get("round"), 0, 99)
        pct = to_int(a.get("pct"), 0, 1000)
        if a.get("round") == str(room.round):
            m.hp = pct
        return "ok"

    def cmd_mods(self, room, m, a, now):
        slot = to_int(a.get("slot"), 0, MAX_SLOTS - 1)
        o = room.members.get(slot)
        if not o:
            return "err reason=value"
        return f"ok slot={slot} mods={pct_enc(o.mods)}"

    def cmd_ping(self, room, m, a, now):
        t = a.get("t", "")
        if not RE_REQ.match(t):
            raise Bad
        return "ok t=" + t

    # ---------- room logic ----------
    def delete_room(self, room, why="deleted"):
        if self.rooms.pop(room.code, None) is None:
            return
        for o in room.members.values():
            self.tokens.pop(o.token, None)
        room.members.clear()
        n = self.rooms_by_ip.get(room.owner_ip, 0) - 1
        if n > 0:
            self.rooms_by_ip[room.owner_ip] = n
        else:
            self.rooms_by_ip.pop(room.owner_ip, None)
        self.log(f"[{room.code}] {why}")

    def remove(self, room, m, now, msg):
        room.members.pop(m.slot, None)
        self.tokens.pop(m.token, None)
        if not room.members:
            self.delete_room(room)
            return
        if room.host == m.slot:
            room.host = min(room.members)
            msg += f"; {room.members[room.host].name} is host"
        if room.in_match() and len(room.members) == 1 and room.phase != "over":
            self.enter(room, "over", now, self.t.over,
                       winner=next(iter(room.members)), msg=msg)
            return
        if room.phase == "fight" and self.check_fight(room, now, msg):
            return
        if room.phase == "load" and self.check_load(room, now, msg):
            return
        self.changed(room, msg)

    def enter(self, room, phase, now, dur, winner=-1, msg=""):
        room.phase = phase
        room.winner = winner
        room.deadline = now + dur if dur else 0.0
        self.log(f"[{room.code}] -> {phase} round={room.round} winner={winner}")
        self.changed(room, msg)

    def enter_load(self, room, now, msg=""):
        for o in room.members.values():
            o.alive = True
            o.life = 0
            o.respawn_at = 0.0
            o.arena = False
            o.hp = 1000
        if room.maps:
            room.map = self.rng.choice([i for i in range(16) if room.maps >> i & 1])
        self.enter(room, "load", now, self.t.load, msg=msg)

    def check_load(self, room, now, msg=""):
        if all(o.arena for o in room.members.values() if o.conn):
            self.enter(room, "count", now, self.t.count, msg=msg)
            return True
        return False

    def check_fight(self, room, now, msg=""):
        if room.mode == "kill":  # the alive count never ends a kill round
            return False
        alive = [o for o in room.members.values() if o.alive]
        if len(alive) <= 1:
            self.end_round(room, alive[0] if alive else None, now, msg)
            return True
        return False

    def end_round(self, room, win, now, msg="", add=True):
        if win and add:
            win.score += 1
        for o in room.members.values():
            o.respawn_at = 0.0
        self.enter(room, "end", now, self.t.end,
                   winner=win.slot if win else -1, msg=msg)

    def time_up(self, room, now):
        if room.mode == "kill":  # highest score wins, a tie is a draw
            ms = list(room.members.values())
            best = max(o.score for o in ms)
            top = [o for o in ms if o.score == best]
            self.end_round(room, top[0] if len(top) == 1 else None, now, add=False)
            return
        alive = [o for o in room.members.values() if o.alive]
        best = max((o.hp for o in alive), default=-1)
        top = [o for o in alive if o.hp == best]
        self.end_round(room, top[0] if len(top) == 1 else None, now)

    def after_end(self, room, now):
        if room.mode == "kill":  # one round only: over with the same winner
            self.enter(room, "over", now, self.t.over, winner=room.winner)
            return
        need = room.rounds // 2 + 1
        ms = list(room.members.values())
        clinch = [o for o in ms if o.score >= need]
        if clinch or room.round >= room.rounds:
            best = max(o.score for o in ms)
            top = [o for o in ms if o.score == best]
            self.enter(room, "over", now, self.t.over,
                       winner=top[0].slot if len(top) == 1 else -1)
        else:
            room.round += 1
            self.enter_load(room, now)

    def to_lobby(self, room, now):
        room.round = 0
        for o in room.members.values():
            o.ready = False
            o.alive = True
        self.enter(room, "lobby", now, 0)

    def tick(self, now):
        for code in list(self.rooms):
            room = self.rooms.get(code)
            if not room:
                continue
            for m in list(room.members.values()):
                if code not in self.rooms:
                    break
                idle = now - m.last
                if not m.verified and idle >= self.t.verify:
                    self.log(f"[{code}] slot {m.slot} never verified, removed")
                    self.remove(room, m, now, f"{m.name} timed out")
                elif idle >= (self.t.remove_match if room.in_match() else self.t.remove):
                    self.log(f"[{code}] slot {m.slot} timed out, removed")
                    self.remove(room, m, now, f"{m.name} timed out")
                elif idle >= self.t.conn_lost and m.conn:
                    m.conn = False
                    self.log(f"[{code}] slot {m.slot} conn=0")
                    if not (room.phase == "load" and self.check_load(room, now)):
                        self.changed(room)
            if code in self.rooms and room.phase == "fight":
                back = [o for o in room.members.values()
                        if o.respawn_at and now >= o.respawn_at]
                for o in back:
                    o.alive, o.life, o.hp, o.respawn_at = True, o.life + 1, 1000, 0.0
                    self.log(f"[{code}] slot {o.slot} respawn life {o.life}")
                if back:
                    self.changed(room)
            if code in self.rooms and now - room.last_change >= self.t.room_idle:
                self.delete_room(room, "idle, deleted")
            if code not in self.rooms or not room.deadline or now < room.deadline:
                continue
            if room.phase == "load":
                self.enter(room, "count", now, self.t.count)
            elif room.phase == "count":
                self.enter(room, "fight", now, room.time)
            elif room.phase == "fight":
                self.time_up(room, now)
            elif room.phase == "end":
                self.after_end(room, now)
            elif room.phase == "over":
                self.to_lobby(room, now)
        for c in (self.addr_cache, self.gone_cache):
            if len(c) > 256 or self.rng.random() < 0.01:
                for k in [k for k, v in c.items() if v[1] < now]:
                    del c[k]
        if len(self.ctrl_rate) > 1024 or len(self.ips) > 1024:
            self.prune(now)

    def prune(self, now, force=False):
        for k in [k for k, w in self.ctrl_rate.items() if now - w.start > 2]:
            del self.ctrl_rate[k]
        keep = 0.5 if force else 660.0  # longest window is 10 min
        for k in [k for k, v in self.ips.items() if now - v.last > keep]:
            del self.ips[k]

    # ---------- state ----------
    def changed(self, room, msg=""):
        room.v += 1
        room.msg = msg
        room.last_change = time.monotonic()
        for m in room.members.values():
            if m.verified:
                self.send_state(room, m)

    def state_text(self, room, you):
        left = 0
        if room.deadline:
            left = max(1, int((room.deadline - time.monotonic()) * 1000))
        f = [f"v={room.v}", f"you={you}", f"code={room.code}",
             f"phase={room.phase}", f"round={room.round}",
             f"rounds={room.rounds}", f"time={room.time}", f"dmg={room.dmg}", f"map={room.map}",
             f"max={room.max}", f"host={room.host}", f"left={left}",
             f"winner={room.winner}", f"mode={room.mode}", f"maps={room.maps}",
             f"respawn={room.respawn}", f"kills={room.kills}",
             f"cap={self.lim.max_players}", "rules=" + state_enc(room.rules)]
        if room.msg:
            f.append("msg=" + state_enc(room.msg))
        for s in sorted(room.members):
            o = room.members[s]
            sub = [o.name, str(int(o.ready)), o.weapon, str(o.score),
                   str(int(o.alive)), str(int(o.conn)), o.edge,
                   mod_hash(o.mods), str(o.life), str(o.deaths)]
            f.append(f"m{s}=" + ",".join(state_enc(x) for x in sub))
        return " ".join(f)

    def send_state(self, room, m):
        pkt = HDR.pack(MAGIC, VERSION, T_STATE, 0, 0) + \
            self.state_text(room, m.slot).encode()
        self.send(pkt, m.addr)


def main():
    ap = argparse.ArgumentParser(description="pbzmp lobby/relay server")
    ap.add_argument("--port", type=int, default=27777)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--password", default="",
                    help="clients must send this in create/join (pw=)")
    ap.add_argument("--log-file", default="", help="append log here")
    ap.add_argument("--quiet", action="store_true", help="no stdout log")
    ap.add_argument("--loss", type=float, default=0.0, help="send loss %%")
    ap.add_argument("--delay", type=float, default=0.0, help="send delay ms")
    ap.add_argument("--jitter", type=float, default=0.0, help="extra 0..N ms")
    ap.add_argument("--max-players", type=int, default=8,
                    help="room size cap, 2-8 (STATE cap=)")
    ap.add_argument("--egress-kb", type=int, default=512,
                    help="KB/s sent to one IP, 0 = no limit")
    a = ap.parse_args()
    if len(a.password.encode()) > 64 or has_ctl(a.password):
        ap.error("--password: at most 64 bytes, no control chars")
    if not 2 <= a.max_players <= MAX_SLOTS:
        ap.error("--max-players: 2-8")
    if a.egress_kb < 0:
        ap.error("--egress-kb: 0 or more")
    lim = Limits()
    lim.max_players = a.max_players
    lim.egress_bytes = a.egress_kb * 1024
    lf = open(a.log_file, "a", encoding="utf-8") if a.log_file else None
    s = Server(a.bind, a.port, a.loss, a.delay, a.jitter,
               quiet=a.quiet, password=a.password, limits=lim, log_file=lf)

    def on_signal(_sig, _frm):
        s.stop()
    signal.signal(signal.SIGINT, on_signal)
    for name in ("SIGTERM", "SIGBREAK"):  # SIGBREAK: Ctrl+Break on Windows
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), on_signal)
    try:
        s.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if lf:
            lf.close()


if __name__ == "__main__":
    main()

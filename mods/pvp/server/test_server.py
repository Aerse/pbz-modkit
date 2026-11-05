"""Tests for pbzmp_server. Run: python server/test_server.py"""
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pbzmp_server as srv  # noqa: E402

HDR = srv.HDR


def fast_timing():
    t = srv.Timing()
    t.load, t.count, t.end, t.over = 0.5, 0.1, 0.1, 0.2
    t.conn_lost, t.remove = 5.0, 15.0
    return t


def parse_kv(text):
    out = {}
    for p in text.split(" "):
        if "=" in p:
            k, v = p.split("=", 1)
            out[k] = v if k.startswith("m") and k[1:].isdigit() else srv.pct_dec(v)
    return out


# PBZMP_CPP_SERVER=<path to out/rsrv.exe>: the same tests against the C++ port
# (room_server.cpp). The exe takes the fast timings on its command line; the tests that poke Python internals
# go through its --control stdin commands instead, or are skipped where there is no C++ equivalent.
CPP = os.environ.get("PBZMP_CPP_SERVER")


class _CppKnobs:
    """server.t / server.lim in C++ mode: setting a field sends "t k=v" / "lim k=v"."""

    def __init__(self, server, kind):
        object.__setattr__(self, "_srv", server)
        object.__setattr__(self, "_kind", kind)

    def __setattr__(self, k, v):
        r = self._srv.cmd(f"{self._kind} {k}={v}")
        if r != "ok":
            raise AttributeError(f"{self._kind}.{k}: {r}")


class CppServer:
    FAST = "load=0.5,count=0.1,end=0.1,over=0.2,conn_lost=5,remove=15"  # = fast_timing()

    def __init__(self, password=None):
        import subprocess
        args = [CPP, "--port", "0", "--bind", "127.0.0.1", "--quiet", "--control", "--timing", self.FAST]
        if password:
            args += ["--password", password]
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        line = self.p.stdout.readline().strip()
        if not line.startswith("port="):
            self.p.kill()
            raise RuntimeError(f"{CPP}: no port line, got {line!r}")
        self.port = int(line[5:])
        self.t = _CppKnobs(self, "t")
        self.lim = _CppKnobs(self, "lim")

    def cmd(self, line):
        """One control command; returns its reply without the leading '@'."""
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        r = self.p.stdout.readline().strip()
        if not r.startswith("@"):
            raise RuntimeError(f"control {line!r}: {r!r} (exit {self.p.poll()})")
        return r[1:]

    def stats(self):
        return {k: int(v) for k, v in (p.split("=") for p in self.cmd("stats").split())}

    def stop(self):
        try:
            self.p.stdin.close()  # stdin EOF ends the exe's loop
            self.p.wait(5)
        except Exception:
            self.p.kill()
            self.p.wait(5)
        self.p.stdout.close()


def cpp_skip(reason):
    return unittest.skipIf(CPP, "C++ mode: " + reason)


class Client:
    def __init__(self, port):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.settimeout(0.05)
        self.addr = ("127.0.0.1", port)
        self.token = 0
        self.slot = None
        self.req = 0
        self.states = []
        self.games = []
        self.replies = {}

    def close(self):
        self.s.close()

    def raw(self, typ, body, token=None, magic=srv.MAGIC, ver=srv.VERSION):
        tok = self.token if token is None else token
        self.s.sendto(HDR.pack(magic, ver, typ, 0, tok) + body, self.addr)

    def pump(self, dur=0.05):
        end = time.monotonic() + dur
        while time.monotonic() < end:
            try:
                d, _ = self.s.recvfrom(2048)
            except (socket.timeout, ConnectionResetError):
                continue
            _, _, typ, slot, tok = HDR.unpack_from(d)
            body = d[HDR.size:]
            if typ == srv.T_STATE:
                self.states.append(parse_kv(body.decode()))
            elif typ == srv.T_REPLY:
                t = body.decode()
                req, rest = t.split(" ", 1)
                self.replies.setdefault(int(req), []).append(rest)
            elif typ == srv.T_GAME_OUT:
                self.games.append((slot, tok, body))

    def ctrl(self, cmd, req=None, wait=1.0, pad=True, **kw):
        if req is None:
            self.req += 1
            req = self.req
        body = f"{req} {cmd}" + "".join(
            f" {k}={srv.pct_enc(str(v))}" for k, v in kw.items())
        if cmd in ("create", "join") and self.token == 0 and pad:
            n = srv.MIN_HELLO - HDR.size - len(body) - 5
            if n > 0:
                body += " pad=" + "-" * n
        self.raw(srv.T_CTRL, body.encode())
        if req == 0:
            return None
        end = time.monotonic() + wait
        while time.monotonic() < end:
            self.pump(0.02)
            if req in self.replies:
                rest = self.replies[req][-1]
                ok = rest.split(" ")[0] == "ok"
                return ok, parse_kv(rest)
        return None

    def state(self):
        self.pump(0.05)
        return self.states[-1] if self.states else None

    def wait_phase(self, phase, timeout=3.0):
        end = time.monotonic() + timeout
        seen = getattr(self, "_seen", 0)
        while True:
            for i in range(seen, len(self.states)):
                if self.states[i]["phase"] == phase:
                    self._seen = i + 1
                    return self.states[i]
            seen = len(self.states)
            if time.monotonic() >= end:
                break
            self.pump(0.02)
        self.fail_state = self.states[-1] if self.states else None
        raise AssertionError(f"phase {phase} not reached, last={self.fail_state}")

    def create(self, name="host", **kw):
        r = self.ctrl("create", name=name, ver=2, **kw)
        if r and r[0]:
            self.token = int(r[1]["token"])
            self.slot = int(r[1]["slot"])
            self.verify()
        return r

    def verify(self):
        """First packet with the token, as the real client's first ping."""
        self.raw(srv.T_CTRL, b"0 ping t=0")
        self.pump(0.03)

    def join(self, code, name="guest", ver=2):
        r = self.ctrl("join", code=code, name=name, ver=ver)
        if r and r[0]:
            self.token = int(r[1]["token"])
            self.slot = int(r[1]["slot"])
            self.verify()
        return r


class Base(unittest.TestCase):
    def setUp(self):
        self.clients = []
        if CPP:
            self.server = CppServer()
            return
        self.server = srv.Server("127.0.0.1", 0, timing=fast_timing(), quiet=True)
        self.th = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.th.start()

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.server.stop()
        if not CPP:
            self.th.join(2)

    # Server internals the tests look at, in both modes.
    def n_rooms(self):
        return self.server.stats()["rooms"] if CPP else len(self.server.rooms)

    def counts(self):
        """(rooms, live tokens, ips owning rooms)"""
        if CPP:
            s = self.server.stats()
            return s["rooms"], s["players"], s["owner_ips"]
        return len(self.server.rooms), len(self.server.tokens), len(self.server.rooms_by_ip)

    def expire_timer(self):
        """The (only) room's phase deadline becomes now."""
        if CPP:
            self.server.cmd("expire")
            return
        room = next(iter(self.server.rooms.values()))
        room.deadline = time.monotonic()

    def spy_send(self):
        """Returns a function giving the sizes of every datagram the server sent after this call."""
        sizes = []
        if CPP:
            self.server.cmd("sent")  # starts recording, drops anything earlier

            def get():
                sizes.extend(int(x) for x in self.server.cmd("sent").split(",") if x)
                return sizes
            return get
        orig = self.server.send

        def spy(data, addr):
            sizes.append(len(data))
            orig(data, addr)
        self.server.send = spy
        return lambda: sizes

    def client(self):
        c = Client(self.server.port)
        self.clients.append(c)
        return c

    def keepalive(self, *cs, dur=0.0):
        for c in cs:
            c.ctrl("ping", t=1)

    def room2(self, **conf):
        h, g = self.client(), self.client()
        ok, r = h.create(max=conf.pop("max", 2))
        self.assertTrue(ok)
        code = r["code"]
        self.assertTrue(g.join(code)[0])
        if conf:
            self.assertTrue(h.ctrl("conf", **conf)[0])
        return h, g, code

    def start_match(self, h, *gs):
        for g in gs:
            self.assertTrue(g.ctrl("set", ready=1)[0])
        self.assertEqual(h.ctrl("start"), (True, {}))

    def to_fight(self, rnd, *cs):
        for c in cs:
            while c.wait_phase("load")["round"] != str(rnd):
                pass
            c.ctrl("arena", round=rnd)
        for c in cs:
            st = c.wait_phase("fight")
            self.assertEqual(st["round"], str(rnd))


class TestCodec(unittest.TestCase):
    def test_pct(self):
        self.assertEqual(srv.pct_enc("a b,c~"), "a%20b%2Cc~")
        self.assertEqual(srv.pct_enc("é"), "%C3%A9")
        self.assertEqual(srv.pct_dec("%C3%A9%2c"), "é,")


class TestLobby(Base):
    def test_create_join(self):
        h = self.client()
        ok, r = h.create(name="Alice Smith")
        self.assertTrue(ok)
        self.assertRegex(r["code"], r"^\d{6}$")
        self.assertEqual(r["slot"], "0")
        self.assertNotEqual(int(r["token"]), 0)
        g = self.client()
        ok, r2 = g.join(r["code"], name="B" * 40)
        self.assertTrue(ok)
        self.assertEqual(r2["slot"], "1")
        st = h.wait_phase("lobby")
        h.pump(0.1)
        st = h.states[-1]
        self.assertEqual(st["you"], "0")
        self.assertEqual(st["host"], "0")
        self.assertEqual(st["m0"].split(",")[0], "Alice%20Smith")
        self.assertEqual(st["m1"].split(",")[0], "B" * 24)
        self.assertEqual(st["m1"], "B" * 24 + ",0,,0,1,1,,,0,0")
        self.assertEqual((st["mode"], st["maps"], st["respawn"], st["kills"], st["cap"], st["rules"]),
                         ("duel", "0", "5", "10", "8", ""))
        self.assertEqual(g.state()["you"], "1")

    def test_join_errors(self):
        h = self.client()
        self.assertEqual(h.ctrl("create", name="x", ver=1), (False, {"reason": "version"}))
        ok, r = h.create(max=2)
        code = r["code"]
        g = self.client()
        self.assertEqual(g.join(code, ver=9)[1]["reason"], "version")
        self.assertEqual(g.join("000000" if code != "000000" else "111111")[1]["reason"], "noroom")
        self.assertTrue(g.join(code)[0])
        x = self.client()
        self.assertEqual(x.join(code)[1]["reason"], "full")
        self.assertEqual(h.ctrl("conf", max=3)[1]["reason"], "value")  # duel needs max=2
        self.assertTrue(h.ctrl("conf", mode="ffa", max=3)[0])
        self.assertTrue(x.join(code)[0])

    def test_server_full(self):
        self.server.lim.max_rooms = 2
        for _ in range(2):
            self.assertTrue(self.client().create()[0])
        self.assertEqual(self.client().create()[1]["reason"], "server_full")

    def test_set_conf_validation(self):
        h, g, _ = self.room2()
        self.assertTrue(g.ctrl("set", weapon="/Game/W/a b,c")[0])
        g.pump(0.1)
        self.assertEqual(g.states[-1]["m1"], "guest,0,/Game/W/a%20b%2Cc,0,1,1,,,0,0")
        self.assertEqual(g.ctrl("set", weapon="x" * 201)[1]["reason"], "value")
        # edge (Phantom Edge pick): 7th member subfield, same text rules, leaves weapon alone
        self.assertTrue(g.ctrl("set", edge="Hu Pao,1")[0])
        g.pump(0.1)
        self.assertEqual(g.states[-1]["m1"], "guest,0,/Game/W/a%20b%2Cc,0,1,1,Hu%20Pao%2C1,,0,0")
        self.assertTrue(g.ctrl("set", edge="x" * 200, ready=1)[0])
        g.pump(0.1)
        self.assertEqual(g.states[-1]["m1"].split(","),
                         ["guest", "1", "/Game/W/a%20b%2Cc", "0", "1", "1", "x" * 200, "", "0", "0"])
        # mods (enabled client mods): STATE carries only the hash (8th subfield), "mods slot=" the list
        self.assertTrue(g.ctrl("set", mods="mp:1.0:a;evil.x:2:b")[0])
        g.pump(0.1)
        self.assertEqual(g.states[-1]["m1"].split(",")[7], srv.mod_hash("mp:1.0:a;evil.x:2:b"))
        self.assertEqual(h.ctrl("mods", slot=1), (True, {"slot": "1", "mods": "mp:1.0:a;evil.x:2:b"}))
        self.assertEqual(g.ctrl("set", mods="x" * 201)[1]["reason"], "value")
        self.assertEqual(g.ctrl("set", edge="x" * 201)[1]["reason"], "value")
        self.assertEqual(g.ctrl("set", edge="x" * 201, ready=0)[1]["reason"], "value")
        self.assertEqual(g.state()["m1"].split(",")[1], "1")  # a rejected set changes nothing
        self.assertTrue(g.ctrl("set", edge="")[0])
        g.pump(0.1)
        self.assertEqual(g.states[-1]["m1"].split(",")[6], "")
        self.assertEqual(g.ctrl("conf", rounds=3)[1]["reason"], "nothost")
        for kw in ({"rounds": 2}, {"time": 20}, {"time": 601}, {"dmg": 9},
                   {"dmg": 501}, {"max": 9}, {"max": 1}, {"mode": "ffa", "max": 9}):
            self.assertEqual(h.ctrl("conf", **kw)[1]["reason"], "value", kw)
        self.assertTrue(h.ctrl("conf", rounds=5, time=0, dmg=250, max=4, mode="ffa")[0])
        st = h.state()
        self.assertEqual((st["rounds"], st["time"], st["dmg"], st["max"]), ("5", "0", "250", "4"))

    def test_start_validation(self):
        h = self.client()
        h.create()
        self.assertEqual(h.ctrl("start")[1]["reason"], "players")
        g = self.client()
        g.join(h.states[-1]["code"] if h.states else h.state()["code"])
        self.assertEqual(g.ctrl("start")[1]["reason"], "nothost")
        self.assertEqual(h.ctrl("start")[1]["reason"], "notready")
        g.ctrl("set", ready=1)
        self.assertTrue(h.ctrl("start")[0])
        self.assertEqual(h.ctrl("start")[1]["reason"], "phase")
        self.assertEqual(g.ctrl("set", ready=0)[1]["reason"], "phase")
        self.assertEqual(g.ctrl("set", edge="HuPao")[1]["reason"], "phase")
        self.assertEqual(h.ctrl("conf", rounds=1)[1]["reason"], "phase")
        x = self.client()
        self.assertEqual(x.join(h.state()["code"])[1]["reason"], "started")

    def test_state_version_increases(self):
        h, g, _ = self.room2()
        g.ctrl("set", ready=1)
        g.ctrl("set", ready=0)
        h.pump(0.1)
        vs = [int(s["v"]) for s in h.states]
        self.assertEqual(vs, sorted(vs))
        self.assertGreater(vs[-1], vs[0])

    def test_ping(self):
        h = self.client()
        h.create()
        n = len(h.states)
        self.assertEqual(h.ctrl("ping", t=12345), (True, {"t": "12345"}))
        h.pump(0.05)
        self.assertGreater(len(h.states), n)

    def test_unknown_token_dropped(self):
        h = self.client()
        h.create()
        h.pump(0.05)
        r = h.ctrl("ping", wait=0.3, t=1) if False else None
        h.token = 12345 if h.token != 12345 else 54321
        self.assertIsNone(h.ctrl("ping", wait=0.3, t=1))

    def test_bad_magic_version_dropped(self):
        h = self.client()
        h.raw(srv.T_CTRL, b"1 create name=a ver=2", magic=0xB6)
        h.raw(srv.T_CTRL, b"2 create name=a ver=2", ver=2)
        h.pump(0.2)
        self.assertEqual(h.replies, {})
        self.assertEqual(self.n_rooms(), 0)


class TestDedupeRate(Base):
    def test_create_dedupe(self):
        h = self.client()
        a = h.ctrl("create", req=1, name="a", ver=2)
        b = h.ctrl("create", req=1, name="a", ver=2)
        self.assertEqual(a, b)
        self.assertEqual(self.n_rooms(), 1)

    def test_member_dedupe(self):
        h, g, _ = self.room2()
        r1 = g.ctrl("set", req=50, ready=1)
        g.ctrl("set", ready=0)
        r2 = g.ctrl("set", req=50, ready=1)  # resend: must not re-execute
        self.assertEqual(r1, r2)
        self.assertEqual(g.state()["m1"].split(",")[1], "0")

    def test_leave_resend(self):
        h, g, _ = self.room2()
        self.assertTrue(g.ctrl("leave", req=77)[0])
        g.replies.clear()
        self.assertEqual(g.ctrl("leave", req=77), (True, {}))

    def test_ctrl_rate_limit(self):
        h = self.client()
        h.create()
        for i in range(30):
            h.raw(srv.T_CTRL, f"{100 + i} ping t={i}".encode())
        h.pump(0.3)
        got = [r for r in h.replies if r >= 100]
        self.assertEqual(len(got), 18)  # create + verify ping used 2 of 20
        time.sleep(1.05)
        self.assertTrue(h.ctrl("ping", t=1)[0])


class TestGame(Base):
    def test_forward(self):
        h, g, _ = self.room2(max=3)
        x = self.client()
        self.assertTrue(x.join(h.state()["code"])[0])
        h.raw(srv.T_GAME_IN, b"\x01\x02payload")
        g.pump(0.1)
        x.pump(0.1)
        h.pump(0.1)
        self.assertEqual(g.games, [(0, 0, b"\x01\x02payload")])
        self.assertEqual(x.games, [(0, 0, b"\x01\x02payload")])
        self.assertEqual(h.games, [])
        g.raw(srv.T_GAME_IN, b"z")
        h.pump(0.1)
        self.assertEqual(h.games, [(1, 0, b"z")])

    def test_unknown_token_not_forwarded(self):
        h, g, _ = self.room2()
        h.raw(srv.T_GAME_IN, b"x", token=1 if h.token != 1 else 2)
        g.pump(0.1)
        self.assertEqual(g.games, [])

    def test_oversize_dropped(self):
        h, g, _ = self.room2()
        h.raw(srv.T_GAME_IN, b"x" * 1193)
        h.raw(srv.T_GAME_IN, b"y" * 1192)
        g.pump(0.1)
        self.assertEqual([len(p) for _, _, p in g.games], [1192])

    def test_packet_rate_limit(self):
        h, g, _ = self.room2()
        g.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        for i in range(250):
            h.raw(srv.T_GAME_IN, b"p")
        g.pump(0.5)
        self.assertEqual(len(g.games), 200)


    def test_byte_rate_limit(self):
        h, g, _ = self.room2()
        g.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        for i in range(150):
            h.raw(srv.T_GAME_IN, b"b" * 1000)
        g.pump(0.3)
        self.assertEqual(len(g.games), 131)  # 131 * 1000 <= 131072


class TestMatch(Base):
    def test_full_match_dead(self):
        h, g, _ = self.room2(rounds=3, time=0)
        self.start_match(h, g)
        st = h.wait_phase("load")
        self.assertEqual(st["round"], "1")
        self.assertEqual(st["m1"].split(",")[3:5], ["0", "1"])
        # wrong round arena ignored
        h.ctrl("arena", round=2)
        g.ctrl("arena", round=2)
        h.pump(0.1)
        self.assertEqual(h.states[-1]["phase"], "load")
        for c in (h, g):
            c.ctrl("arena", round=1)
        h.wait_phase("count")
        # dead outside fight ignored
        g.ctrl("dead", round=1, by=0)
        st = h.wait_phase("fight")
        self.assertEqual(st["left"], "0")
        self.assertEqual(st["m1"].split(",")[4], "1")
        g.ctrl("dead", round=1, by=0)
        st = h.wait_phase("end")
        self.assertEqual((st["winner"], st["m0"].split(",")[3]), ("0", "1"))
        self.to_fight(2, h, g)
        h.ctrl("dead", round=2, by=1)
        st = h.wait_phase("end")
        self.assertEqual(st["winner"], "1")
        self.to_fight(3, h, g)
        g.ctrl("dead", round=3, by=0)
        st = h.wait_phase("over")
        self.assertEqual(st["winner"], "0")
        self.assertEqual(st["m0"].split(",")[3], "2")
        st = h.wait_phase("lobby")
        self.assertEqual((st["round"], st["winner"]), ("0", "-1"))
        self.assertEqual(st["m1"].split(",")[1], "0")

    def test_early_clinch(self):
        h, g, _ = self.room2(rounds=3, time=0)
        self.start_match(h, g)
        for rnd in (1, 2):
            self.to_fight(rnd, h, g)
            g.ctrl("dead", round=rnd, by=0)
            h.wait_phase("end")
        st = h.wait_phase("over")
        self.assertEqual((st["round"], st["winner"]), ("2", "0"))

    def test_time_up_hp(self):
        h, g, _ = self.room2(rounds=1, time=30)
        self.server.t.load = 5
        self.start_match(h, g)
        self.to_fight(1, h, g)
        st = h.states[-1]
        self.assertGreater(int(st["left"]), 25000)
        h.ctrl("hp", req=0, round=1, pct=400)
        g.ctrl("hp", req=0, round=1, pct=700)
        h.pump(0.1)
        self.expire_timer()  # fast-forward the fight timer
        st = h.wait_phase("end")
        self.assertEqual(st["winner"], "1")
        st = h.wait_phase("over")
        self.assertEqual(st["winner"], "1")

    def test_time_up_tie_and_draw_match(self):
        h, g, _ = self.room2(rounds=1, time=30)
        self.start_match(h, g)
        self.to_fight(1, h, g)
        self.expire_timer()  # nobody reported hp -> both 1000
        st = h.wait_phase("end")
        self.assertEqual(st["winner"], "-1")
        st = h.wait_phase("over")
        self.assertEqual(st["winner"], "-1")

    def test_double_death_draw(self):
        h, g, x = self.client(), self.client(), self.client()
        ok, r = h.create(max=3)
        g.join(r["code"])
        x.join(r["code"])
        self.assertTrue(h.ctrl("conf", rounds=1, time=0)[0])
        self.start_match(h, g, x)
        self.to_fight(1, h, g, x)
        g.ctrl("dead", round=1, by=0)
        g.ctrl("dead", round=1, by=0)  # duplicate ignored
        h.pump(0.05)
        self.assertEqual(h.states[-1]["phase"], "fight")
        x.ctrl("dead", round=1, by=0)
        st = h.wait_phase("end")
        self.assertEqual(st["winner"], "0")

    def test_load_timeout(self):
        h, g, _ = self.room2(rounds=1, time=0)
        self.start_match(h, g)
        h.wait_phase("load")
        h.ctrl("arena", round=1)
        self.keepalive(g)
        h.wait_phase("count", timeout=2)


def mf(st, slot):
    """Member subfields of STATE m<slot>: name,ready,weapon,score,alive,conn,edge,modh,life,deaths."""
    return st["m%d" % slot].split(",")


class TestV2(Base):
    def room(self, n, **conf):
        """n members in an ffa room (max = conf max or n), conf applied, all ready."""
        cs = [self.client() for _ in range(n)]
        ok, r = cs[0].create(max=conf.pop("max", n))
        self.assertTrue(ok)
        for c in cs[1:]:
            self.assertTrue(c.join(r["code"], name="p%d" % len(self.clients))[0])
        if conf:
            self.assertEqual(cs[0].ctrl("conf", **conf), (True, {}))
        for c in cs[1:]:
            self.assertTrue(c.ctrl("set", ready=1)[0])
        return cs

    def wait_member(self, c, slot, pred, timeout=3.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            c.pump(0.02)
            if c.states and pred(mf(c.states[-1], slot)):
                return c.states[-1]
        raise AssertionError(f"m{slot} condition not met, last={c.states[-1] if c.states else None}")

    def test_version(self):
        h = self.client()
        ok, r = h.create()
        self.assertTrue(ok)
        g = self.client()
        self.assertEqual(g.join(r["code"], ver=1)[1]["reason"], "version")
        self.assertEqual(g.join(r["code"], ver=3)[1]["reason"], "version")
        self.assertTrue(g.join(r["code"])[0])
        self.assertEqual(srv.VERSION, 1)  # the header version stays 1

    def test_state_escaping(self):
        h = self.client()
        name = "甲 a,b=c%d"
        ok, r = h.create(name=name)
        self.assertTrue(ok)
        h.pump(0.1)
        st = h.states[-1]
        # UTF-8 raw, only space , = % (and controls) escaped
        self.assertEqual(mf(st, 0)[0], "甲%20a%2Cb%3Dc%25d")
        self.assertEqual(srv.state_enc(name), "甲%20a%2Cb%3Dc%25d")
        self.assertEqual(st["msg"], name + " created the room")
        self.assertTrue(h.ctrl("set", weapon="/Game/武器 1,2")[0])
        h.pump(0.1)
        self.assertEqual(mf(h.states[-1], 0)[2], "/Game/武器%201%2C2")
        self.assertTrue(h.ctrl("conf", rules="a:1,bb:22")[0])
        h.pump(0.1)
        self.assertEqual(h.states[-1]["rules"], "a:1,bb:22")  # sent as a:1%2Cbb:22

    def test_mods_query(self):
        h, g, _ = self.room2()
        mods = "mp:1.5:a;外挂:2,x:b"
        self.assertTrue(g.ctrl("set", mods=mods)[0])
        h.pump(0.1)
        self.assertEqual(mf(h.states[-1], 1)[7], srv.mod_hash(mods))
        self.assertEqual(mf(h.states[-1], 0)[7], "")
        self.assertEqual(h.ctrl("mods", slot=1), (True, {"slot": "1", "mods": mods}))
        self.assertEqual(g.ctrl("mods", slot=0), (True, {"slot": "0", "mods": ""}))
        self.assertEqual(h.ctrl("mods", slot=5)[1]["reason"], "value")
        self.assertEqual(h.ctrl("mods", slot=8)[1]["reason"], "bad")
        self.assertEqual(h.ctrl("mods")[1]["reason"], "bad")

    def test_eight_players_ffa(self):
        cs = self.room(8, rounds=1, time=0)
        st = cs[0].state()
        self.assertEqual((st["mode"], st["max"], st["cap"]), ("ffa", "8", "8"))
        self.assertEqual(sorted(k for k in st if k[:1] == "m" and k[1:].isdigit()), ["m%d" % i for i in range(8)])
        self.assertEqual(self.client().join(st["code"])[1]["reason"], "full")
        self.assertEqual(cs[0].ctrl("start"), (True, {}))
        self.to_fight(1, *cs)
        self.assertEqual(cs[7].ctrl("dead", round=1, by=8)[1]["reason"], "bad")
        for i in range(1, 7):
            self.assertTrue(cs[i].ctrl("dead", round=1, by=7 if i < 6 else 0)[0])
        cs[0].pump(0.1)
        st = cs[0].states[-1]
        self.assertEqual(st["phase"], "fight")  # two still alive
        self.assertEqual([mf(st, i)[4] for i in range(8)], ["1"] + ["0"] * 6 + ["1"])
        self.assertEqual(mf(st, 3)[9], "1")  # deaths count in every mode
        self.assertTrue(cs[0].ctrl("dead", round=1, by=7)[0])
        st = cs[3].wait_phase("end")
        self.assertEqual((st["winner"], mf(st, 7)[3], mf(st, 0)[3]), ("7", "1", "0"))  # ffa: round win = score
        st = cs[3].wait_phase("over")
        self.assertEqual(st["winner"], "7")

    def test_max_above_cap(self):
        self.assertEqual(self.client().create(max=9)[1]["reason"], "value")
        self.server.lim.max_players = 4
        self.assertEqual(self.client().create(max=5)[1]["reason"], "value")
        h = self.client()
        ok, r = h.create(max=4)
        self.assertTrue(ok)
        st = h.state()
        self.assertEqual((st["cap"], st["max"], st["mode"]), ("4", "4", "ffa"))
        self.assertEqual(h.ctrl("conf", max=5)[1]["reason"], "value")
        self.assertTrue(h.ctrl("conf", max=3)[0])

    def test_rules_validation(self):
        cs = self.room(3, max=4)
        h = cs[0]
        bad = [{"mode": "tdm"}, {"mode": "DUEL"}, {"maps": 65536}, {"maps": -1}, {"respawn": 0},
               {"respawn": 11}, {"kills": 51}, {"kills": -1}, {"rules": "A:1"}, {"rules": "a=1"},
               {"rules": "a:1;b:2"}, {"rules": "a" * 201}, {"time": 601}, {"time": 29},
               {"mode": "duel"},           # sets max 2 but slot 2 is taken
               {"mode": "duel", "max": 3}, {"max": 2},
               {"rounds": 3, "kills": 51}]  # nothing of a rejected conf applies
        for kw in bad:
            self.assertEqual(h.ctrl("conf", **kw)[1]["reason"], "value", kw)
        st = h.state()
        self.assertEqual((st["mode"], st["max"], st["rounds"], st["kills"]), ("ffa", "4", "3", "10"))
        ok = {"mode": "kill", "maps": 65535, "respawn": 10, "kills": 50, "rules": "a" * 200, "time": 600}
        self.assertEqual(h.ctrl("conf", **ok), (True, {}))
        st = h.state()
        self.assertEqual((st["mode"], st["maps"], st["respawn"], st["kills"], st["rules"], st["time"]),
                         ("kill", "65535", "10", "50", "a" * 200, "600"))
        self.assertTrue(h.ctrl("conf", respawn=1, kills=0, rules="")[0])
        cs[2].ctrl("leave")
        self.assertTrue(h.ctrl("conf", mode="duel")[0])  # max follows
        st = h.state()
        self.assertEqual((st["mode"], st["max"]), ("duel", "2"))

    def test_maps(self):
        h, g, _ = self.room2(maps=1 << 13)
        self.start_match(h, g)
        self.assertEqual(h.wait_phase("load")["map"], "13")
        h2, g2, _ = self.room2(maps=(1 << 2) | (1 << 9), map=4)
        self.start_match(h2, g2)
        self.to_fight(1, h2, g2)  # (to_fight waits for the load STATE itself)
        seen = {h2.states[-1]["map"]}
        g2.ctrl("dead", round=1, by=0)
        seen.add(h2.wait_phase("load")["map"])
        self.assertTrue(seen <= {"2", "9"}, seen)
        h3, g3, _ = self.room2(map=4)
        self.start_match(h3, g3)
        self.assertEqual(h3.wait_phase("load")["map"], "4")  # maps=0 -> map

    def test_kill_start_value(self):
        h, g, _ = self.room2(mode="kill", time=0, kills=0)
        self.assertTrue(g.ctrl("set", ready=1)[0])
        self.assertEqual(h.ctrl("start")[1]["reason"], "value")
        self.assertTrue(h.ctrl("conf", kills=1)[0])
        self.assertTrue(h.ctrl("start")[0])

    def test_kill_scoring_respawn_end(self):
        cs = self.room(3, mode="kill", kills=2, time=0, respawn=1, rounds=5)
        h, g, x = cs
        self.assertTrue(h.ctrl("start")[0])
        self.to_fight(1, h, g, x)
        self.assertEqual([mf(h.states[-1], i)[8:] for i in range(3)], [["0", "0"]] * 3)
        self.assertTrue(g.ctrl("dead", round=1, by=0, life=0)[0])
        self.assertTrue(x.ctrl("dead", round=1, by=2)[0])   # suicide: no score
        self.assertTrue(g.ctrl("dead", round=1, by=0, life=0)[0])  # duplicate
        h.pump(0.1)
        st = h.states[-1]
        self.assertEqual(st["phase"], "fight")  # one alive does not end a kill round
        self.assertEqual([mf(st, i)[3:5] for i in range(3)], [["1", "1"], ["0", "0"], ["0", "0"]])
        self.assertEqual([mf(st, i)[8:] for i in range(3)], [["0", "0"], ["0", "1"], ["0", "1"]])
        st = self.wait_member(h, 1, lambda f: f[4] == "1")  # respawn after 1 s
        self.assertEqual(mf(st, 1)[8:], ["1", "1"])
        self.wait_member(h, 2, lambda f: f[4] == "1")
        self.assertTrue(g.ctrl("dead", round=1, by=0, life=0)[0])  # a death from the old life
        h.pump(0.1)
        self.assertEqual(mf(h.states[-1], 1)[4], "1")
        self.assertEqual(mf(h.states[-1], 0)[3], "1")
        self.assertEqual(g.ctrl("dead", round=1, by=0, life=10000)[1]["reason"], "bad")
        self.assertTrue(g.ctrl("dead", round=1, by=0, life=1)[0])
        st = h.wait_phase("end")
        self.assertEqual((st["winner"], mf(st, 0)[3], mf(st, 1)[9]), ("0", "2", "2"))  # no extra round point
        st = h.wait_phase("over")
        self.assertEqual((st["winner"], st["round"], mf(st, 0)[3]), ("0", "1", "2"))
        st = h.wait_phase("lobby")
        for c in (g, x):
            self.assertTrue(c.ctrl("set", ready=1)[0])
        self.assertTrue(h.ctrl("start")[0])
        st = h.wait_phase("load")
        self.assertEqual([mf(st, i)[3] for i in range(3)] + [mf(st, i)[9] for i in range(3)], ["0"] * 6)

    def test_kill_time_up(self):
        h, g, _ = self.room2(mode="kill", time=30, kills=0)
        self.start_match(h, g)
        self.to_fight(1, h, g)
        self.expire_timer()
        st = h.wait_phase("end")
        self.assertEqual(st["winner"], "-1")  # 0:0 draw
        self.assertEqual(h.wait_phase("over")["winner"], "-1")
        h.wait_phase("lobby")
        self.start_match(h, g)
        self.to_fight(1, h, g)
        h.ctrl("dead", round=1, by=1)
        h.ctrl("hp", round=1, pct=1000)
        g.ctrl("hp", round=1, pct=10)  # hp does not matter in kill mode
        h.pump(0.1)
        self.expire_timer()
        self.assertEqual(h.wait_phase("end")["winner"], "1")
        self.assertEqual(h.wait_phase("over")["winner"], "1")


class TestDisconnect(Base):
    def test_conn_and_remove_and_host_migration(self):
        self.server.t.conn_lost, self.server.t.remove = 1.0, 2.0
        h, g, code = self.room2(max=3)
        x = self.client()
        x.join(code)
        # h goes silent, g and x keep pinging
        end = time.monotonic() + 1.2
        while time.monotonic() < end:
            g.ctrl("ping", t=1)
            x.ctrl("ping", t=1)
            time.sleep(0.05)
        st = g.state()
        self.assertEqual(st["m0"].split(",")[5], "0")
        # GAME not forwarded to conn=0 member
        g.raw(srv.T_GAME_IN, b"q")
        h.pump(0.05)
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            g.ctrl("ping", t=1)
            x.ctrl("ping", t=1)
            time.sleep(0.05)
        st = g.state()
        self.assertNotIn("m0", st)
        self.assertEqual(st["host"], "1")
        self.assertTrue(g.ctrl("conf", rounds=5)[0])

    def test_resume_mid_match_after_restart(self):
        # a crashed client restarts and resumes with its old token from a new port
        self.server.t.conn_lost, self.server.t.remove, self.server.t.remove_match = 0.5, 1.0, 3.0
        h, g, code = self.room2()
        g.ctrl("set", ready=1)
        self.assertTrue(h.ctrl("start")[0])
        h.wait_phase("load", timeout=2)
        end = time.monotonic() + 1.5  # longer than remove, shorter than remove_match
        while time.monotonic() < end:
            h.ctrl("ping", t=1)
            time.sleep(0.05)
        st = h.state()
        self.assertIn("m1", st)
        self.assertEqual(st["m1"].split(",")[5], "0")
        g2 = self.client()
        g2.token = g.token
        self.assertTrue(g2.ctrl("ping", t=0)[0])
        st = h.state()
        self.assertEqual(st["m1"].split(",")[5], "1")

    def test_reconnect_new_port(self):
        h, g, _ = self.room2()
        g2 = self.client()
        g2.token = g.token
        self.assertTrue(g2.ctrl("ping", t=5)[0])
        h.raw(srv.T_GAME_IN, b"hello")
        g2.pump(0.1)
        self.assertEqual(g2.games, [(0, 0, b"hello")])

    def test_leave_mid_match_over(self):
        h, g, _ = self.room2(rounds=3)
        self.start_match(h, g)
        h.wait_phase("load")
        self.assertTrue(g.ctrl("leave")[0])
        st = h.wait_phase("over")
        self.assertEqual(st["winner"], "0")
        st = h.wait_phase("lobby")
        self.assertNotIn("m1", st)

    def test_host_leave_and_room_delete(self):
        h, g, code = self.room2()
        h.ctrl("leave")
        st = g.state()
        self.assertEqual(st["host"], "1")
        g.ctrl("leave")
        self.assertEqual(self.n_rooms(), 0)
        self.assertEqual(self.client().join(code)[1]["reason"], "noroom")
        # slot reuse: new joiner takes lowest free slot
        a, b = self.client(), self.client()
        ok, r = a.create(max=3)
        b.join(r["code"])
        a.ctrl("leave")
        c = self.client()
        self.assertEqual(c.join(r["code"])[1]["slot"], "0")


def hello(req, cmd, **kw):
    body = f"{req} {cmd}" + "".join(f" {k}={srv.pct_enc(str(v))}" for k, v in kw.items())
    n = srv.MIN_HELLO - HDR.size - len(body) - 5
    if n > 0:
        body += " pad=" + "-" * n
    return body.encode()


class TestHardening(Base):
    def lim(self, **kw):
        for k, v in kw.items():
            setattr(self.server.lim, k, v)

    def test_rooms_per_ip_and_create_rate(self):
        self.lim(rooms_per_ip=2, creates_per_min=100)
        a, b = self.client(), self.client()
        self.assertTrue(a.create()[0])
        self.assertTrue(b.create()[0])
        self.assertEqual(self.client().create()[1]["reason"], "rate")
        a.ctrl("leave")  # frees one of this IP's rooms
        self.assertTrue(self.client().create()[0])

    def test_creates_per_minute(self):
        self.lim(creates_per_min=3, rooms_per_ip=100)
        for _ in range(3):
            c = self.client()
            self.assertTrue(c.create()[0])
            c.ctrl("leave")
        self.assertEqual(self.client().create()[1]["reason"], "rate")

    def test_join_guessing_throttled(self):
        self.lim(badjoin_per_10min=3)
        h = self.client()
        code = h.create()[1]["code"]
        bad = "%06d" % ((int(code) + 1) % 1000000)
        g = self.client()
        for _ in range(3):
            self.assertEqual(g.join(bad)[1]["reason"], "noroom")
        self.assertEqual(g.join(bad)[1]["reason"], "rate")
        # even the right code is refused now (no guessing oracle)
        self.assertEqual(self.client().join(code)[1]["reason"], "rate")

    def test_joins_per_minute(self):
        self.lim(joins_per_min=2)
        h = self.client()
        ok, r = h.create(max=4)
        for _ in range(2):
            self.assertTrue(self.client().join(r["code"])[0])
        self.assertEqual(self.client().join(r["code"])[1]["reason"], "rate")

    def test_pps_per_ip(self):
        self.lim(pps_per_ip=50)
        h, g, _ = self.room2()
        time.sleep(1.05)
        for _ in range(120):
            h.raw(srv.T_GAME_IN, b"p")
        g.pump(0.4)
        self.assertLessEqual(len(g.games), 50)
        self.assertGreater(len(g.games), 10)

    def test_egress_cap_per_ip(self):
        self.lim(egress_bytes=20000)
        h, g, _ = self.room2()
        g.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        time.sleep(1.05)
        for _ in range(100):
            h.raw(srv.T_GAME_IN, b"e" * 1000)
        g.pump(0.4)
        self.assertLessEqual(len(g.games), 20)
        self.assertGreater(len(g.games), 5)

    def test_no_amplification(self):
        c = self.client()
        # tiny unauthenticated create: no reply at all
        c.raw(srv.T_CTRL, b"1 create name=a ver=2")
        c.pump(0.2)
        self.assertEqual(c.replies, {})
        self.assertEqual(self.n_rooms(), 0)
        # padded: reply is no bigger than the request
        sizes = self.spy_send()
        req = hello(2, "create", name="a", ver=2)
        c.raw(srv.T_CTRL, req)
        c.pump(0.2)
        self.assertIn(2, c.replies)
        self.assertEqual(len(sizes()), 1)  # REPLY only, STATE waits for verify
        self.assertLessEqual(sizes()[0], HDR.size + len(req))
        # resend of the same request: cached reply, still not bigger
        c.raw(srv.T_CTRL, req)
        c.pump(0.1)
        self.assertLessEqual(max(sizes()), HDR.size + len(req))
        # a short unauthenticated join with a bad field: dropped
        c.replies.clear()
        c.raw(srv.T_CTRL, b"3 join code=x")
        c.pump(0.1)
        self.assertEqual(c.replies, {})

    def test_unverified_gets_nothing_and_expires(self):
        self.server.t.verify = 0.5
        h = self.client()
        code = h.create()[1]["code"]
        g = self.client()
        g.raw(srv.T_CTRL, hello(1, "join", code=code, name="spoof", ver=2))
        g.pump(0.15)
        self.assertIn(1, g.replies)
        self.assertEqual(g.states, [])
        h.raw(srv.T_GAME_IN, b"x")
        g.pump(0.1)
        self.assertEqual(g.games, [])
        self.assertIn("m1", h.state())
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            h.ctrl("ping", t=1)
            time.sleep(0.05)
        self.assertNotIn("m1", h.state())

    def test_field_validation(self):
        h, g, _ = self.room2()
        bad = [
            b"5 set ready=1 ready=0",       # duplicate key
            b"5 set bogus=1",               # unknown key
            b"5 set ready",                 # no '='
            b"5 set weapon=%ZZ",            # bad escape
            b"5 set weapon=%FF",            # bad utf-8
            b"5 set weapon=%0A",            # control char
            b"5 set weapon=a\xc3\xa9",      # raw non-ascii
            b"5 set edge=%0A",              # control char
            b"5 set edge=a edge=b",         # duplicate key
            b"5 hp round=1 pct=abc",
            b"5 hp round=1 pct=1001",
            b"5 dead round=1 by=9",
            b"5 arena round=x",
            b"5 ping t=-1",
            b"5 conf rounds=+3",
            b"5 nosuchcmd",
            b"5 set " + b" ".join(b"ready=1" for _ in range(20)),
        ]
        for i, body in enumerate(bad):
            g.replies.clear()
            body = body.replace(b"5 ", f"{500 + i} ".encode(), 1)
            g.raw(srv.T_CTRL, body)
            g.pump(0.06)
            rs = g.replies.get(500 + i, [])
            self.assertTrue(rs and (rs[-1].startswith("err reason=bad")
                                    or rs[-1].startswith("err reason=value")), (body, rs))
        g.raw(srv.T_CTRL, b"99999999999 ping t=1")  # req > u32
        g.raw(srv.T_CTRL, b"\xff\xfe")
        g.pump(0.1)
        self.assertNotIn(99999999999, g.replies)
        # slot byte must be 0 from clients
        g.s.sendto(HDR.pack(srv.MAGIC, srv.VERSION, srv.T_GAME_IN, 3, g.token) + b"z", g.addr)
        h.pump(0.1)
        self.assertEqual(h.games, [])
        # name with control chars is refused
        c = self.client()
        self.assertEqual(c.create(name="a\x07b")[1]["reason"], "value")
        self.assertEqual(c.create(max="2.0")[1]["reason"], "value")

    def test_tokens_required_for_game_and_ctrl(self):
        h, g, _ = self.room2()
        g.raw(srv.T_GAME_IN, b"x", token=0)
        g.raw(srv.T_GAME_IN, b"y", token=(g.token ^ 1) or 1)
        h.pump(0.1)
        self.assertEqual(h.games, [])
        g.ctrl("leave")
        g.raw(srv.T_GAME_IN, b"z")  # stale token after leave
        h.pump(0.1)
        self.assertEqual(h.games, [])
        # unauthenticated non-hello commands are ignored
        x = self.client()
        x.raw(srv.T_CTRL, hello(1, "start"))
        x.pump(0.1)
        self.assertEqual(x.replies, {})

    def test_stale_room_cleanup(self):
        self.server.t.room_idle = 0.5
        self.server.t.conn_lost, self.server.t.remove = 100, 100
        h = self.client()
        h.create()
        self.assertEqual(self.n_rooms(), 1)
        time.sleep(0.8)
        self.assertEqual(self.counts(), (0, 0, 0))  # rooms, tokens, rooms_by_ip all empty

    def test_status_line_and_log_file(self):
        if CPP:  # the exe logs to a file instead of a StringIO
            import tempfile
            d = tempfile.TemporaryDirectory()
            self.addCleanup(d.cleanup)  # runs after tearDown has stopped the exe (log closed)
            lf = os.path.join(d.name, "srv.log")
            self.assertEqual(self.server.cmd("log " + lf), "ok")
            self.assertEqual(self.server.cmd("status 0.3"), "ok")
        else:
            import io
            buf = io.StringIO()
            self.server.logf = buf
            self.server.t.status = 0.3
            self.server.next_status = time.monotonic() + 0.3
        h = self.client()
        h.create()
        time.sleep(0.5)
        if CPP:
            with open(lf, encoding="utf-8") as f:
                out = f.read()
        else:
            out = buf.getvalue()
        self.assertRegex(out, r"status rooms=1 players=1 pps=[0-9.]+")
        self.assertIn("created by", out)


class TestPassword(unittest.TestCase):
    def setUp(self):
        if CPP:
            self.server = CppServer(password="s3cret")
            self.c = Client(self.server.port)
            return
        self.server = srv.Server("127.0.0.1", 0, timing=fast_timing(), quiet=True,
                                 password="s3cret")
        self.th = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.th.start()
        self.c = Client(self.server.port)

    def tearDown(self):
        self.c.close()
        self.server.stop()
        if not CPP:
            self.th.join(2)

    def test_password(self):
        self.assertEqual(self.c.create()[1]["reason"], "password")
        self.assertEqual(self.c.create(pw="wrong")[1]["reason"], "password")
        ok, r = self.c.create(pw="s3cret")
        self.assertTrue(ok)
        d = Client(self.server.port)
        try:
            self.assertEqual(d.join(r["code"])[1]["reason"], "password")
            ok2 = d.ctrl("join", code=r["code"], name="g", ver=2, pw="s3cret")
            self.assertTrue(ok2[0])
        finally:
            d.close()


class TestProcess(unittest.TestCase):
    """Runs the real script (C++ mode: the exe): --log-file, status line, clean Ctrl+C / SIGTERM."""

    def test_cli_shutdown(self):
        import subprocess
        import tempfile
        port = int(os.environ.get("PBZMP_TEST_PORT", "27801"))  # a fixed port: the CLI has no "port=" line
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pbzmp_server.py")
        with tempfile.TemporaryDirectory() as d:
            lf = os.path.join(d, "srv.log")
            flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
            cmd = [CPP] if CPP else [sys.executable, "-I", path]
            p = subprocess.Popen(cmd + ["--port", str(port),
                                        "--bind", "127.0.0.1", "--quiet", "--log-file", lf,
                                        "--password", "pw1"], creationflags=flags)
            try:
                c = Client(port)
                r = None
                for _ in range(40):
                    r = c.ctrl("create", wait=0.25, name="a", ver=2, pw="pw1")
                    if r:
                        break
                c.close()
                self.assertTrue(r and r[0], r)
                if os.name == "nt":
                    import signal
                    p.send_signal(signal.CTRL_BREAK_EVENT)
                else:
                    p.send_signal(__import__("signal").SIGINT)
                self.assertEqual(p.wait(10), 0)
            finally:
                if p.poll() is None:
                    p.kill()
            with open(lf, encoding="utf-8") as f:
                txt = f.read()
            self.assertIn("password set", txt)
            self.assertIn("created by", txt)
            self.assertIn("shutting down: 1 rooms", txt)


@cpp_skip("loss/delay/jitter simulation (--loss/--delay/--jitter) is not ported; the C++ server has none")
class TestNetSim(unittest.TestCase):
    def test_loss_and_delay(self):
        s = srv.Server("127.0.0.1", 0, loss=100, quiet=True)
        th = threading.Thread(target=s.serve_forever, daemon=True)
        th.start()
        c = Client(s.port)
        try:
            self.assertIsNone(c.ctrl("create", wait=0.3, name="a", ver=2))
        finally:
            c.close(); s.stop(); th.join(2)
        s = srv.Server("127.0.0.1", 0, delay=200, jitter=50, quiet=True)
        th = threading.Thread(target=s.serve_forever, daemon=True)
        th.start()
        c = Client(s.port)
        try:
            t0 = time.monotonic()
            self.assertTrue(c.ctrl("create", wait=1.0, name="a", ver=2)[0])
            self.assertGreaterEqual(time.monotonic() - t0, 0.19)
        finally:
            c.close(); s.stop(); th.join(2)


if __name__ == "__main__":
    unittest.main(verbosity=1)

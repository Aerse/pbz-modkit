"""Builds the player zip dist/pbz_pvp-v<ver>.zip from out/ (run build.bat first).
<ver> is version= in mods/pvp/mp.txt, the one place the release version lives.
  --full  also packs the sniper mod with its local assets (mods/sniper/assets/cache, not in
          the repo, never published) into dist/pbz_mods_full-v<ver>.zip, for local testing.
Layout (unzip into <game>/platform/steam/):
  version.dll  Mod管理器.exe  README.txt
  pbzmp/  autorun.txt  mp.dll  mp.txt  maps/*.png
  server/ rsrv.exe  pbzmp_server.py  开服务器.bat
  sdk/    README.md  pbz_host.h  example/
"""
import os, re, shutil, sys, zipfile

R = os.path.dirname(os.path.abspath(__file__))
FULL = "--full" in sys.argv


def info(path, key):
    with open(os.path.join(R, path), encoding="utf-8-sig") as f:
        for line in f:
            if line.startswith(key + "="):
                return line.split("=", 1)[1].strip()
    sys.exit(f"{path}: no {key}=")


VER = info("mods/pvp/mp.txt", "version")
SNIPER_VER = info("mods/sniper/sniper.txt", "version")
GAME = info("mods/pvp/mp.txt", "game")  # PBZ.exe file version the offsets are for
if FULL and info("mods/sniper/sniper.txt", "game") != GAME:
    sys.exit("mp.txt and sniper.txt are for different game versions")
OUT, D = os.path.join(R, "out"), os.path.join(R, "dist", "pbz_pvp")


def cp(src, dst):
    os.makedirs(os.path.dirname(os.path.join(D, dst)), exist_ok=True)
    shutil.copyfile(os.path.join(R, src), os.path.join(D, dst))


def text(dst, s):
    os.makedirs(os.path.dirname(os.path.join(D, dst)), exist_ok=True)
    with open(os.path.join(D, dst), "wb") as f:
        f.write(s.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))


for f in ("out/version.dll", "out/modman.exe", "out/mp.dll", "out/rsrv.exe") + (("out/sniper.dll",) if FULL else ()):
    if not os.path.isfile(os.path.join(R, f)):
        sys.exit(f"missing {f}: run build.bat first")
shutil.rmtree(D, ignore_errors=True)
cp("out/version.dll", "version.dll")
cp("out/modman.exe", "Mod管理器.exe")
cp("out/mp.dll", "pbzmp/mp.dll")
cp("mods/pvp/mp.txt", "pbzmp/mp.txt")
text("pbzmp/autorun.txt", "mp\n")
for m in sorted(os.listdir(os.path.join(R, "mods/pvp/maps"))):
    cp("mods/pvp/maps/" + m, "pbzmp/maps/" + m)
cp("out/rsrv.exe", "server/rsrv.exe")
cp("mods/pvp/server/pbzmp_server.py", "server/pbzmp_server.py")
text("server/开服务器.bat", "@echo off\nrem PBZ PvP relay server, UDP 27777. Password: add  --password YOURPASS\n"
     "cd /d %~dp0\nrsrv.exe --port 27777\npause\n")
cp("sdk/README.md", "sdk/README.md")
cp("loader/host.h", "sdk/pbz_host.h")
for f in ("example.txt", "build.bat"):
    cp("sdk/example/" + f, "sdk/example/" + f)
with open(os.path.join(R, "sdk/example/example.cpp"), "rb") as f:
    src = f.read().replace(b'"../../loader/host.h"  // or pbz_host.h from a release', b'"../pbz_host.h"')
with open(os.path.join(D, "sdk/example/example.cpp"), "wb") as f:
    f.write(src)


def plain(md_path, cut=None):
    """markdown -> plain text for README.txt: no images, header marks, backticks, bold, tables"""
    with open(os.path.join(R, md_path), encoding="utf-8") as f:
        md = f.read()
    if cut:
        md = md.split(cut)[0]
    out = []
    for l in md.splitlines():
        if l.startswith("![") or re.match(r"^\|?\s*-{3,}", l):
            continue
        if l.startswith("|"):
            l = "  ".join(c.strip() for c in l.strip("|").split("|"))
        out.append(l)
    body = re.sub(r"(?m)^#+ ", "", "\n".join(out)).replace("`", "").replace("**", "")
    return re.sub(r"\n{3,}", "\n\n", body).strip() + "\n"


name = f"pbz_mods_full-v{VER}" if FULL else f"pbz_pvp-v{VER}"
head = (f"影之刃零 Mod 包 v{VER}\n\n适配游戏版本: {GAME}（Steam 版 PBZ.exe 的文件版本，右键 → 属性 → 详细信息可查看）\n"
        "游戏更新后 Mod 可能失效，请等待对应的新版本。\n\n"
        f"包含: 联机 PVP {VER}")
if FULL:
    head += f"、CS 步枪 {SNIPER_VER}（仅自测，资源版权属于 Valve，不要外传）"
readme = head + "\n解压到游戏目录的 platform\\steam\\（和 PBZ.exe 同一层）。\n\n" + "=" * 40 + "\n\n"
readme += plain("mods/pvp/README.md", "## 开发者")
if FULL:
    cp("out/sniper.dll", "pbzmp/sniper.dll")
    cp("mods/sniper/sniper.txt", "pbzmp/sniper.txt")
    cache = os.path.join(R, "mods/sniper/assets/cache")
    if not os.path.isdir(cache):
        sys.exit("missing mods/sniper/assets/cache")
    for d, _, fs in os.walk(cache):
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), R).replace(os.sep, "/")
            cp(rel, "pbzmp/sniper_cache/" + os.path.relpath(os.path.join(d, f), cache).replace(os.sep, "/"))
    readme += "\n" + "=" * 40 + "\n\n" + plain("mods/sniper/README.md", "## 资源")
    readme += "\n启用: 打开 Mod管理器.exe，勾选「CS 步枪」，保存，重启游戏。进联机房间时它会自动卸载，离开房间后自动恢复。\n"
text("README.txt", "﻿" + readme)  # BOM: old Notepad reads it as UTF-8

z = os.path.join(R, "dist", name + ".zip")
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    for d, _, fs in os.walk(D):
        for f in fs:
            full = os.path.join(d, f)
            zf.write(full, os.path.relpath(full, D).replace(os.sep, "/"))
print(z)

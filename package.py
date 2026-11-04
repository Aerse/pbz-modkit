"""Builds the player zip dist/pbz_pvp.zip from out/ (run build.bat first).
Layout (unzip into <game>/platform/steam/):
  version.dll  Mod管理器.exe  README.txt
  pbzmp/  autorun.txt  mp.dll  mp.txt  maps/*.png
  server/ rsrv.exe  pbzmp_server.py  开服务器.bat
  sdk/    README.md  pbz_host.h  example/
"""
import os, shutil, zipfile

R = os.path.dirname(os.path.abspath(__file__))
OUT, D = os.path.join(R, "out"), os.path.join(R, "dist", "pbz_pvp")


def cp(src, dst):
    os.makedirs(os.path.dirname(os.path.join(D, dst)), exist_ok=True)
    shutil.copyfile(os.path.join(R, src), os.path.join(D, dst))


def text(dst, s):
    os.makedirs(os.path.dirname(os.path.join(D, dst)), exist_ok=True)
    with open(os.path.join(D, dst), "wb") as f:
        f.write(s.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))


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
with open(os.path.join(R, "mods/pvp/README.md"), encoding="utf-8") as f:
    text("README.txt", f.read())

z = os.path.join(R, "dist", "pbz_pvp.zip")
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    for d, _, fs in os.walk(D):
        for f in fs:
            full = os.path.join(d, f)
            zf.write(full, os.path.relpath(full, D).replace(os.sep, "/"))
print(z)

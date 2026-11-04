# Start a test instance of the game without taking focus. Usage: launch.ps1 <tag> [x y w h]
# <tag> is the mod folder under platform\steam (its own cmd.txt and logs, own mutex name).
# The window opens without activation (SW_SHOWNOACTIVATE) at the given spot, -nosound.
param([string]$Tag = "pbzmpA", [int]$X = -4000, [int]$Y = 0, [int]$W = 960, [int]$H = 540, [switch]$Sound)
$root = if ($env:PBZ_ROOT) { $env:PBZ_ROOT } else { throw "set PBZ_ROOT to the game folder" }
$exe = "$root\platform\steam\PBZ.exe"
$cmd = "`"$exe`" ProjectPBZ -basedir=`"$root\ProjectPBZ\Binaries\Win64`" -pc-platform=Steam -windowed -WinX=$X -WinY=$Y -ResX=$W -ResY=$H$(if ($Sound) { '' } else { ' -nosound' }) -allowmultipleinstances -pbzmpdir=$Tag"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class PbzLaunch {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct SI { public int cb; public string r, d, t; public int x, y, xs, ys, xc, yc, fa, fl; public short sw, r2; public IntPtr r3, i, o, e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inh, int fl, IntPtr env, string dir, ref SI si, out PI pi);
  public static int Start(string cmd, string dir, short show) {
    var si = new SI(); si.cb = Marshal.SizeOf(si); si.fl = 1; si.sw = show; PI pi;
    if (!CreateProcessW(null, new StringBuilder(cmd), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, dir, ref si, out pi)) return -Marshal.GetLastWin32Error();
    return pi.pid;
  }
}
"@
$env:SteamAppId = "4115450"
New-Item -ItemType Directory -Force "$root\platform\steam\$Tag" | Out-Null
$p = [PbzLaunch]::Start($cmd, "$root\platform\steam", 4)
"started $Tag pid $p"

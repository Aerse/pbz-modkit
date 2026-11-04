# Stop test instances (started by launch.ps1). Usage: stop.ps1 [tag]  (no tag: all test instances)
param([string]$Tag = "")
$pat = if ($Tag) { "-pbzmpdir=$Tag" } else { "-pbzmpdir=" }
Get-CimInstance Win32_Process -Filter "Name='PBZ.exe'" | Where-Object { $_.CommandLine -like "*$pat*" } | ForEach-Object {
  Stop-Process -Id $_.ProcessId -Force; "stopped $($_.ProcessId)" }

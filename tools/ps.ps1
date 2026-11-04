Get-CimInstance Win32_Process -Filter "Name='PBZ.exe'" | ForEach-Object { "{0} parent {1} {2}" -f $_.ProcessId, $_.ParentProcessId, $_.CommandLine }

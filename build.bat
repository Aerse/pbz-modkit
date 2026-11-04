@echo off
rem Builds everything into out\ : version.dll (loader), modman.exe, mp.dll (pvp mod), nettest.exe, rsrv.exe, example.dll
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not defined VSCMD_VER call "%VCVARS%" >nul 2>nul
cd /d %~dp0
if not exist out\obj mkdir out\obj
set MH=third_party\minhook
set MHSRC=%MH%\src\buffer.c %MH%\src\hook.c %MH%\src\trampoline.c %MH%\src\hde\hde64.c
set CF=/nologo /O2 /EHa /std:c++20 /utf-8 /Foout\obj\ /Fdout\obj\
cl %CF% /LD /I%MH%\include loader\main.cpp %MHSRC% /Fe:out\version.dll /link user32.lib || exit /b 1
cl %CF% /W3 /MT modman\modman.cpp /Fe:out\modman.exe /link /SUBSYSTEM:WINDOWS || exit /b 1
set P=mods\pvp\src
cl %CF% /LD /I%MH%\include /Iframework %P%\mp.cpp %P%\fileguard.cpp framework\uex.cpp framework\ue.cpp %P%\net\net.cpp %P%\net\steam.cpp %P%\net\room_server.cpp %P%\ui\ui.cpp %P%\arena\arena.cpp %P%\fight\fight.cpp %P%\fight\weapons.cpp %P%\fight\outfits.cpp %MHSRC% ws2_32.lib user32.lib /Fe:out\mp.dll || exit /b 1
cl %CF% /W3 /D_CRT_SECURE_NO_WARNINGS %P%\net\nettest.cpp %P%\net\net.cpp %P%\net\steam.cpp %P%\net\room_server.cpp ws2_32.lib /Fe:out\nettest.exe || exit /b 1
cl %CF% /W3 %P%\net\rsrv_main.cpp %P%\net\room_server.cpp ws2_32.lib /Fe:out\rsrv.exe || exit /b 1
cl %CF% /LD sdk\example\example.cpp /Fe:out\example.dll /link user32.lib || exit /b 1
del out\*.exp out\*.lib 2>nul
echo BUILD OK

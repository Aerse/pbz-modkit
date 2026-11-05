@echo off
rem Needs Visual Studio 2022 (Build Tools is enough), x64. Output: hello.dll here.
rem Install: copy hello.dll and hello.txt into <game>\platform\steam\pbzmp\hello\
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %~dp0
set FW=..\framework
if not exist %FW%\uex.h set FW=..\..\framework
cl /nologo /O2 /EHa /std:c++20 /LD /utf-8 /I%FW% hello.cpp %FW%\uex.cpp %FW%\ue.cpp /Fe:hello.dll /link user32.lib || exit /b 1
del *.obj hello.lib hello.exp 2>nul

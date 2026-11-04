@echo off
rem Needs Visual Studio 2022 (Build Tools is enough), x64. Output: example.dll here.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %~dp0
cl /nologo /O2 /EHsc /std:c++20 /LD /utf-8 example.cpp /Fe:example.dll /link user32.lib || exit /b 1
del example.obj example.lib example.exp 2>nul

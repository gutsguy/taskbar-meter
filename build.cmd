@echo off
rem Builds TaskbarMeter.exe with Zig (https://ziglang.org/download/).
rem Uses %ZIG% if set, otherwise "zig" from PATH.
setlocal
cd /d "%~dp0"
if not defined ZIG set ZIG=zig
"%ZIG%" build-exe src\meter.c src\app.rc -lc -luser32 -lgdi32 -ladvapi32 -lshell32 -lpdh -liphlpapi ^
  -O ReleaseSmall -fstrip -target x86_64-windows-gnu --subsystem windows --name TaskbarMeter
if errorlevel 1 exit /b 1
del /q TaskbarMeter.pdb TaskbarMeter.lib 2>nul
echo Built TaskbarMeter.exe

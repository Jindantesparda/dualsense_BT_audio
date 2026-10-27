@echo off
setlocal
cd /d "%~dp0"

rem Builds libopus (once), then the CLI (ds5play.exe) and the GUI
rem (DualSenseMusic.exe). Needs g++ (MinGW-w64), cmake and ninja or mingw32-make.

set "GXX=g++"
where g++ >nul 2>nul
if errorlevel 1 if exist "C:\Strawberry\c\bin\g++.exe" set "GXX=C:\Strawberry\c\bin\g++.exe"
"%GXX%" --version >nul 2>nul
if errorlevel 1 (
    echo g++ was not found. Install Strawberry Perl or MSYS2 MinGW-w64.
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo cmake was not found. Install CMake and add it to PATH.
    exit /b 1
)

if not exist "build\opus\libopus.a" (
    echo Configuring libopus...
    cmake -S third_party\opus -B build\opus -G Ninja -DCMAKE_BUILD_TYPE=Release ^
          -DCMAKE_C_COMPILER=gcc -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
    if errorlevel 1 (
        echo Ninja not available, trying MinGW Makefiles...
        cmake -S third_party\opus -B build\opus -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
              -DCMAKE_C_COMPILER=gcc -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
        if errorlevel 1 exit /b 1
    )
    cmake --build build\opus -j
    if errorlevel 1 exit /b 1
)

set "FLAGS=-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE"
set "INCLUDES=-Ithird_party -Ithird_party\opus\include"
set "LIBS=build\opus\libopus.a -lhid -lsetupapi -lwinmm -lole32 -luser32 -luuid -lavrt -static -static-libgcc -static-libstdc++"

echo Building ds5play.exe...
"%GXX%" %FLAGS% src\main.cpp src\player.cpp src\ds5.cpp src\hid_win32.cpp %INCLUDES% %LIBS% -o ds5play.exe
if errorlevel 1 exit /b 1

echo Building DualSenseMusic.exe...
"%GXX%" %FLAGS% src\gui.cpp src\player.cpp src\ds5.cpp src\hid_win32.cpp %INCLUDES% %LIBS% ^
    -mwindows -lcomdlg32 -ldwmapi -lshell32 -lgdi32 -o DualSenseMusic.exe
if errorlevel 1 exit /b 1

echo.
echo Built ds5play.exe and DualSenseMusic.exe

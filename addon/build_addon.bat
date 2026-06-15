@echo off
rem
rem build_addon.bat -- direct cl.exe build for discord_voice_codec.node.
rem
rem node-gyp doesn't recognise VS preview SKUs (e.g. VS 18 Community), so we
rem skip its VS-finder entirely and drive cl.exe ourselves using the headers
rem and node.lib that node-gyp already downloaded into the npm cache for us.
rem
rem Steps:
rem   1) Locate a VS install that actually has vcvars64.bat, run it.
rem   2) Make sure npm has populated node_modules/ for the node-addon-api
rem      headers AND the electron header cache (a one-time npm install).
rem   3) cl.exe compile + link as a Win32 DLL with .node extension.
rem   4) Stage the output into ..\res\embedded\ so the resource compiler
rem      picks it up on the next dnp.exe build.
rem
rem Override the Electron target with
rem     set ELECTRON_TARGET=38.0.0  & build_addon.bat
rem
setlocal EnableDelayedExpansion

cd /d %~dp0

if not defined ELECTRON_TARGET set "ELECTRON_TARGET=34.0.0"
echo [addon] Targeting Electron !ELECTRON_TARGET! headers.

rem -- 1) VS environment ----------------------------------------------------

where cl >nul 2>&1
if errorlevel 1 (
    set "VSPATH="
    if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\18\Community"
    )
    if not defined VSPATH (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
            set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
        )
    )
    if not defined VSPATH (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
            set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
        )
    )
    if not defined VSPATH (
        if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
            set "VSPATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
        )
    )
    if not defined VSPATH (
        echo [addon] No VS install with C++ tools - vcvars64.bat not found.
        exit /b 1
    )
    echo [addon] vcvars64 - !VSPATH!
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 (
        echo [addon] vcvars64.bat failed.
        exit /b 1
    )
)

rem -- 2) npm install for headers (one-time) --------------------------------

where npm >nul 2>&1
if errorlevel 1 (
    echo [addon] npm not found on PATH. Install Node.js LTS first.
    exit /b 1
)

if not exist node_modules\node-addon-api (
    echo [addon] Pulling node-addon-api...
    call npm install --no-audit --no-fund --silent
    if errorlevel 1 (
        echo [addon] npm install failed.
        exit /b 1
    )
)

rem Trigger node-gyp's header download (no build) so the electron headers
rem and node.lib are present in the npm cache. The configure step will
rem fail on the VS-finder but the headers download happens first.
set "GYP_CACHE=%LOCALAPPDATA%\node-gyp\Cache\!ELECTRON_TARGET!"
if not exist "!GYP_CACHE!\include\node\node_api.h" (
    echo [addon] Fetching Electron !ELECTRON_TARGET! headers via node-gyp...
    call npx --yes node-gyp install --target=!ELECTRON_TARGET! ^
         --dist-url=https://electronjs.org/headers --arch=x64 >nul 2>&1
)
if not exist "!GYP_CACHE!\include\node\node_api.h" (
    echo [addon] Electron header fetch failed - check network and re-run.
    exit /b 1
)
if not exist "!GYP_CACHE!\x64\node.lib" (
    echo [addon] node.lib not found in cache at !GYP_CACHE!\x64\
    exit /b 1
)

rem -- 3) cl.exe direct compile + link --------------------------------------

set "OUT_DIR=build"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

set "INC_ELECTRON=!GYP_CACHE!\include\node"
set "INC_NAPI=node_modules\node-addon-api"
set "INC_COMMON=..\..\common"
set "NODE_LIB=!GYP_CACHE!\x64\node.lib"

echo [addon] Compiling...
cl /nologo /std:c++17 /utf-8 /LD /O2 /MT /Ob2 /Oi /GL /GR- /GS- /EHsc /W3 ^
   /D NAPI_VERSION=3 /D NAPI_CPP_EXCEPTIONS ^
   /D _CRT_SECURE_NO_WARNINGS /D WIN32_LEAN_AND_MEAN /D NOMINMAX ^
   /D NODE_GYP_MODULE_NAME=discord_voice_codec ^
   /D BUILDING_NODE_EXTENSION ^
   /I "!INC_ELECTRON!" /I "!INC_NAPI!" /I "!INC_COMMON!" ^
   binding.cpp ^
   /Fo:"%OUT_DIR%\\" /Fe:"%OUT_DIR%\discord_voice_codec.node" ^
   /link /DLL /LTCG /OPT:REF,ICF /DEBUG:NONE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA ^
   /OUT:"%OUT_DIR%\discord_voice_codec.node" ^
   "!NODE_LIB!" ^
   kernel32.lib
if errorlevel 1 (
    echo [addon] cl.exe failed.
    exit /b 1
)

rem -- 4) Stage into res/embedded ------------------------------------------

if not exist ..\res\embedded mkdir ..\res\embedded
copy /Y "%OUT_DIR%\discord_voice_codec.node" "..\res\embedded\discord_voice_codec.node" >nul
echo [addon] Built: %OUT_DIR%\discord_voice_codec.node
echo [addon] Staged: ..\res\embedded\discord_voice_codec.node

endlocal

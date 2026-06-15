@echo off
setlocal EnableDelayedExpansion
cd /d %~dp0

if not defined VCINSTALLDIR (
    call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not exist build_test mkdir build_test

set "GYP_CACHE=%LOCALAPPDATA%\node-gyp\Cache\38.0.0"

cl /nologo /std:c++17 /utf-8 /LD /O2 /MT /Ob2 /Oi /GR- /EHsc /W3 ^
   /D NAPI_VERSION=3 /D NAPI_CPP_EXCEPTIONS /D _CRT_SECURE_NO_WARNINGS ^
   /I "%GYP_CACHE%\include\node" /I node_modules\node-addon-api ^
   test_minimal.cpp ^
   /Fo:build_test\\ /Fe:build_test\discord_voice_codec.node ^
   /link /DLL /OUT:build_test\discord_voice_codec.node ^
   "%GYP_CACHE%\x64\node.lib"

if errorlevel 1 (
    echo TEST BUILD FAILED
    exit /b 1
)

copy /Y build_test\discord_voice_codec.node ..\res\embedded\discord_voice_codec.node >nul
echo TEST BUILD OK

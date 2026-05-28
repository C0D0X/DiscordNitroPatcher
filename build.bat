@echo off
rem build.bat — one-shot MSVC build. Auto-locates VS via vswhere if cl.exe missing on PATH.
setlocal EnableDelayedExpansion

where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [build] vswhere.exe not found. Install Visual Studio Build Tools.
        exit /b 1
    )
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VSPATH=%%i"
    )
    if not defined VSPATH (
        echo [build] No VS install with C++ tools found.
        exit /b 1
    )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 (
        echo [build] vcvars64.bat failed.
        exit /b 1
    )
)

if not exist build mkdir build

rem Compile resources.
rc /nologo /fo build\dnp.res res\dnp.rc
if errorlevel 1 (
    echo [build] rc.exe failed.
    exit /b 1
)

rem Compile and link.
cl /nologo /std:c++17 /O2 /MT /GS- /GL /EHsc /GR- /W3 ^
   /D _CRT_SECURE_NO_WARNINGS ^
   /I src ^
   src\main.cpp src\util.cpp src\json_lite.cpp src\asar.cpp src\patcher.cpp ^
   src\shortcuts.cpp src\updater.cpp src\installer.cpp src\ui.cpp ^
   build\dnp.res ^
   /Fe:build\dnp.exe /Fo:build\ ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF,ICF ^
   kernel32.lib user32.lib shell32.lib advapi32.lib winhttp.lib bcrypt.lib shlwapi.lib psapi.lib ole32.lib dwmapi.lib uxtheme.lib comctl32.lib gdi32.lib
if errorlevel 1 (
    echo [build] cl.exe failed.
    exit /b 1
)

echo [build] Built: build\dnp.exe
endlocal

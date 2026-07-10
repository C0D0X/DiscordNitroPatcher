@echo off
rem build.bat -- one-shot MSVC build. Auto-locates VS via vswhere; falls back
rem to known well-paths (BuildTools 2022, VS 18 Community) when vswhere can't
rem see the install (e.g. preview SKUs newer than the bundled vswhere).
setlocal EnableDelayedExpansion

where cl >nul 2>&1
if not errorlevel 1 goto :vs_ready

set "VSPATH="

rem (1) Direct probes for vcvars64.bat -- prefer installs that actually have
rem     the C++ toolchain present on disk. vswhere reports "installed" even
rem     when only the VS shell is registered, so the file check is the
rem     reliable signal.
if not defined VSPATH (
    if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\18\Community"
    )
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

rem (2) vswhere fallback for unusual install locations.
if not defined VSPATH (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -property installationPath`) do (
            if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%i"
        )
    )
)

if not defined VSPATH (
    echo [build] No VS install with C++ tools - vcvars64.bat not found.
    exit /b 1
)

echo [build] Using VS at !VSPATH!
call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build] vcvars64.bat failed.
    exit /b 1
)

:vs_ready

if not exist build mkdir build

rem Compile resources.
rc /nologo /fo build\dnp.res res\dnp.rc
if errorlevel 1 (
    echo [build] rc.exe failed.
    exit /b 1
)

rem Compile and link.
cl /nologo /std:c++17 /utf-8 /O2 /MT /GS- /GL /EHsc /GR- /W3 ^
   /D _CRT_SECURE_NO_WARNINGS /D WIN32_LEAN_AND_MEAN /D NOMINMAX ^
   /I src /I ..\common ^
   src\main.cpp src\util.cpp src\json_lite.cpp src\asar.cpp src\patcher.cpp ^
   src\shortcuts.cpp src\updater.cpp src\installer.cpp src\ui.cpp ^
   build\dnp.res ^
   /Fe:build\dnp.exe /Fo:build\ ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF,ICF /DEBUG:NONE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA ^
   kernel32.lib user32.lib shell32.lib advapi32.lib winhttp.lib bcrypt.lib shlwapi.lib psapi.lib ole32.lib dwmapi.lib uxtheme.lib comctl32.lib gdi32.lib
if errorlevel 1 (
    echo [build] cl.exe failed.
    exit /b 1
)

echo [build] Built: build\dnp.exe
endlocal

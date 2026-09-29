@echo off
REM Build nvcolor.exe with MSVC. Run from a "x64 Native Tools Command Prompt".
setlocal

if not defined VCINSTALLDIR (
    echo Error: open a Visual Studio developer prompt first.
    exit /b 1
)

if not exist build mkdir build

REM Safety net: every NVAPI id and struct size is pinned by a compile-time assertion.
cl /nologo /W3 /c /Isrc verify_structs.c /Fo:build\verify_structs.obj
if errorlevel 1 (
    echo verify_structs.c failed - NVAPI constants do not match.
    exit /b 1
)
echo verify_structs.c: constants OK

REM Version resource. rc.exe is invoked explicitly because cl warns D9024
REM and treats a .rc as a prebuilt object rather than compiling it.
rc /nologo /fo build\nvcolor.res src\nvcolor.rc
if errorlevel 1 (
    echo Resource compilation failed.
    exit /b 1
)

cl /nologo /W4 /WX /O2 /GS /MT ^
   /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Isrc ^
   src\nvcolor.c src\nvapi_min.c src\gamma.c src\state.c src\values.c ^
   /Fe:build\nvcolor.exe ^
   /Fo:build\ ^
   /link /SUBSYSTEM:WINDOWS build\nvcolor.res user32.lib gdi32.lib

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

REM Inject the application manifest post-link. This is what Visual Studio
REM itself does for custom manifests, and unlike an RT_MANIFEST reference
REM inside the .rc it does not depend on rc's file-path resolution.
mt /nologo -manifest src\nvcolor.manifest ^
   -outputresource:build\nvcolor.exe;#1
if errorlevel 1 (
    echo Manifest injection failed.
    exit /b 1
)

echo.
echo Built build\nvcolor.exe
REM Explicit, rather than relying on whatever errorlevel `endlocal` happens
REM to leave behind. Every failure path above exits 1, so a success exit is
REM always 0 - but relying on that is how a broken build gets published.
endlocal & exit /b 0

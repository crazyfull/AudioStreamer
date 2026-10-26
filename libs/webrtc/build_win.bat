@echo off
rem ===========================================================================
rem Builds webrtc-audio-processing 1.3 (+ abseil) as STATIC libraries with MSVC and copies
rem the result to libs\webrtc\win32\ :
rem     webrtc_apm_x64.lib   webrtc_apm_x64d.lib     (or _x86 / _x86d from an x86 prompt)
rem     include\             (webrtc + abseil headers)
rem
rem Two libs are needed because the project links the runtime statically: /MT (release) and
rem /MTd (debug) objects can't be mixed (LNK2038 RuntimeLibrary mismatch).
rem
rem Needs: Visual Studio C++ tools, Python (pip install meson ninja), internet (meson downloads abseil).
rem Run it from "x64 Native Tools Command Prompt for VS" (or "x86 Native Tools ..." for 32 bit):
rem     build_win.bat C:\path\to\webrtc-audio-processing-1.3
rem where that folder is the extracted https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/
rem 1.3 source (the folder that contains meson.build).
rem ===========================================================================
setlocal

if "%~1"=="" goto usage
set "SRC=%~f1"
if not exist "%SRC%\meson.build" (
    echo [error] "%SRC%\meson.build" not found
    goto usage
)
where cl >nul 2>&1 || (echo [error] cl.exe not found: start this from a "Native Tools Command Prompt for VS" & exit /b 1)
where meson >nul 2>&1 || (echo [error] meson not found: pip install meson ninja & exit /b 1)
where ninja >nul 2>&1 || (echo [error] ninja not found: pip install meson ninja & exit /b 1)

set "ARCH=x64"
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" set "ARCH=x86"

set "OUT=%~dp0win32"
set "WORK=%~dp0_build_%ARCH%"
if not exist "%OUT%" mkdir "%OUT%"

call :build release mt  ""  || exit /b 1
call :build debug   mtd "d" || exit /b 1

rem ---- headers: webrtc ones keep their folder layout (modules\audio_processing\include\...), abseil under absl\
robocopy "%SRC%\webrtc" "%OUT%\include" *.h /S /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (echo [error] copying webrtc headers failed & exit /b 1)
set "ABSL="
for /d %%d in ("%SRC%\subprojects\abseil-cpp-*") do set "ABSL=%%d"
if "%ABSL%"=="" (echo [error] abseil sources not found under "%SRC%\subprojects" & exit /b 1)
robocopy "%ABSL%\absl" "%OUT%\include\absl" *.h *.inc /S /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (echo [error] copying abseil headers failed & exit /b 1)

echo.
echo Done: "%OUT%"
dir /b "%OUT%\*.lib"
exit /b 0

:build
rem %1 = meson buildtype (release/debug)  %2 = b_vscrt (mt/mtd)  %3 = file name suffix
set "BDIR=%WORK%\%1"
if exist "%BDIR%" rmdir /s /q "%BDIR%"
meson setup "%BDIR%" "%SRC%" --buildtype=%1 --default-library=static -Db_vscrt=%2 || exit /b 1
meson compile -C "%BDIR%" || exit /b 1

rem One .lib for the app: the main library (it already contains webrtc's helper libs) + every abseil lib.
set "LIST=%BDIR%\libs.txt"
if exist "%LIST%" del "%LIST%"
for /r "%BDIR%" %%f in (*.lib) do (
    echo %%f| findstr /i /l /c:"\subprojects\" >nul && echo "%%f">>"%LIST%"
    echo %%f| findstr /i /l /c:"\webrtc-audio-processing-1.lib" >nul && echo "%%f">>"%LIST%"
)
if not exist "%LIST%" (echo [error] no .lib files found in "%BDIR%" & exit /b 1)
lib /nologo /OUT:"%OUT%\webrtc_apm_%ARCH%%~3.lib" @"%LIST%" || exit /b 1
exit /b 0

:usage
echo usage: build_win.bat ^<folder with the extracted webrtc-audio-processing-1.3 source^>
exit /b 1

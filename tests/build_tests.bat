@echo off
rem ===========================================================================
rem Builds the two echo canceller tests with MSVC (VS 2019, x64):
rem     tests\bin\aec_test.exe     the canceller alone (simulated room, echo only, then double talk)
rem     tests\bin\e2e_aec.exe      whole pipeline: recorder + opus + player + canceller (~20 s)
rem
rem Start it from "x64 Native Tools Command Prompt for VS 2019":   tests\build_tests.bat
rem
rem Settings (change here or set the variable before running):
rem   WEBRTC_INC   folder that contains modules\audio_processing\include\audio_processing.h (and absl\)
rem   WEBRTC_LIBS  folder with the webrtc + abseil libraries: every *.lib and *.a in it is linked
rem   CRT          /MT (same as VoiceStream.pro) or /MD. MUST be the same runtime the webrtc libs were built with.
rem ===========================================================================
setlocal EnableDelayedExpansion

set "ROOT=%~dp0.."
pushd "%ROOT%" || exit /b 1
set "ROOT=%CD%"

if "%CRT%"=="" set "CRT=/MT"
if "%WEBRTC_INC%"=="" set "WEBRTC_INC=%ROOT%\..\webRTC-audio-process\install\include\webrtc-audio-processing-1"
if "%WEBRTC_LIBS%"=="" set "WEBRTC_LIBS=%ROOT%\..\webRTC-audio-process\install\lib"
set "OPUS_INC=%ROOT%\libs\opus\win32\include"
set "OPUS_LIB=%ROOT%\libs\opus\win32\libopus_x64.lib"

where cl >nul 2>&1 || (echo [error] cl.exe not found: start from "x64 Native Tools Command Prompt for VS 2019" & goto fail)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" echo [warn] this prompt is not x64 (VSCMD_ARG_TGT_ARCH=%VSCMD_ARG_TGT_ARCH%), libopus_x64.lib needs x64
if not exist "%WEBRTC_INC%\modules\audio_processing\include\audio_processing.h" (
    echo [error] audio_processing.h not found under WEBRTC_INC=%WEBRTC_INC%
    goto fail
)
if not exist "%OPUS_LIB%" (echo [error] %OPUS_LIB% not found & goto fail)

rem every library of the webrtc folder (webrtc itself + abseil + helper libs)
set "LIBLIST="
for %%f in ("%WEBRTC_LIBS%\*.lib" "%WEBRTC_LIBS%\*.a") do set LIBLIST=!LIBLIST! "%%~ff"
if "%LIBLIST%"=="" (echo [error] no .lib / .a files in WEBRTC_LIBS=%WEBRTC_LIBS% & goto fail)

echo CRT      : %CRT%
echo webrtc   : %WEBRTC_INC%
echo libs     : %WEBRTC_LIBS%
echo.

if not exist tests\bin mkdir tests\bin
if not exist tests\obj mkdir tests\obj

set "CFLAGS=/nologo /std:c++17 /EHsc /O2 %CRT% /utf-8 /W3 /D_USE_MATH_DEFINES /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Isrc /I"%WEBRTC_INC%" /I"%OPUS_INC%""
set "SYSLIBS=winmm.lib dbghelp.lib advapi32.lib ws2_32.lib"

echo === aec_test.exe
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\aec_test.exe" tests\aec_test.cpp src\echoCanceller.cpp /link %LIBLIST% %SYSLIBS% || goto fail

echo.
echo === e2e_aec.exe
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\e2e_aec.exe" tests\e2e_aec.cpp tests\stub\audioDevice_room_stub.cpp ^
   src\echoCanceller.cpp src\clsAudioRecorder.cpp src\clsAudioPlayer.cpp src\jitterBuffer.cpp src\opusCodec.cpp ^
   /link "%OPUS_LIB%" %LIBLIST% %SYSLIBS% || goto fail

echo.
echo Built OK. Run:
echo     tests\bin\aec_test.exe             (add "noref" for the control run, it must show about 0 dB)
echo     tests\bin\e2e_aec.exe
popd
exit /b 0

:fail
echo.
echo Build FAILED. Common causes:
echo   LNK2038 RuntimeLibrary mismatch  -^> set CRT=/MD (or /MT) to match how the webrtc libs were built
echo   LNK2019 unresolved external __imp_XYZ  -^> add the Windows lib that holds XYZ to SYSLIBS in this file
popd
exit /b 1

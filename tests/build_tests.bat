@echo off
rem ===========================================================================
rem Builds the two echo canceller tests with MSVC (VS 2019, x64):
rem     tests\bin\aec_test.exe     the canceller alone (simulated room, echo only, then double talk)
rem     tests\bin\e2e_aec.exe      whole pipeline: recorder + opus + player + canceller (~20 s)
rem
rem Start it from "x64 Native Tools Command Prompt for VS 2019":   tests\build_tests.bat
rem
rem Settings (change here or set the variable before running):
rem Defaults are taken from VoiceStream.pro (win32 section): ..\webRTC-audio-process\install\...
rem   WEBRTC_ROOT  the "install" folder of webrtc-audio-processing (contains include\ and lib\)
rem   WEBRTC_LIB   the library file that VoiceStream.pro links (libwebrtc-audio-processing-1.lib)
rem   WEBRTC_EXTRA extra libs, only if the link asks for them (e.g. abseil .lib files), optional
rem   CRT          /MT (same as VoiceStream.pro release) or /MD. MUST be the runtime the webrtc lib was built with.
rem ===========================================================================
setlocal EnableDelayedExpansion

set "ROOT=%~dp0.."
pushd "%ROOT%" || exit /b 1
set "ROOT=%CD%"

if "%CRT%"=="" set "CRT=/MT"
if "%WEBRTC_ROOT%"=="" set "WEBRTC_ROOT=%ROOT%\..\webRTC-audio-process\install"
if "%WEBRTC_LIB%"=="" set "WEBRTC_LIB=%WEBRTC_ROOT%\lib\libwebrtc-audio-processing-1.lib"
rem same two include folders as VoiceStream.pro: webrtc headers, and install\include (abseil)
set "WEBRTC_INC=%WEBRTC_ROOT%\include\webrtc-audio-processing-1"
set "WEBRTC_INC2=%WEBRTC_ROOT%\include"
set "OPUS_INC=%ROOT%\libs\opus\win32\include"
set "OPUS_LIB=%ROOT%\libs\opus\win32\libopus_x64.lib"

where cl >nul 2>&1 || (echo [error] cl.exe not found: start from "x64 Native Tools Command Prompt for VS 2019" & goto fail)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" echo [warn] this prompt is not x64 (VSCMD_ARG_TGT_ARCH=%VSCMD_ARG_TGT_ARCH%), libopus_x64.lib needs x64
if not exist "%WEBRTC_INC%\modules\audio_processing\include\audio_processing.h" (
    echo [error] audio_processing.h not found under WEBRTC_INC=%WEBRTC_INC%
    goto fail
)
if not exist "%OPUS_LIB%" (echo [error] %OPUS_LIB% not found & goto fail)
if not exist "%WEBRTC_LIB%" (echo [error] %WEBRTC_LIB% not found & goto fail)

set LIBLIST="%WEBRTC_LIB%" %WEBRTC_EXTRA%

echo CRT      : %CRT%
echo include  : %WEBRTC_INC%
echo include  : %WEBRTC_INC2%
echo lib      : %WEBRTC_LIB% %WEBRTC_EXTRA%
echo.

if not exist tests\bin mkdir tests\bin
if not exist tests\obj mkdir tests\obj

set CFLAGS=/nologo /std:c++17 /EHsc /O2 %CRT% /utf-8 /W3 /D_USE_MATH_DEFINES /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DRAVAVA_WITH_WEBRTC /Isrc /I"%WEBRTC_INC%" /I"%WEBRTC_INC2%" /I"%OPUS_INC%"
set "SYSLIBS=winmm.lib dbghelp.lib advapi32.lib ws2_32.lib"

echo === aec_test.exe
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\aec_test.exe" tests\aec_test.cpp src\ravava\AudioProcessor.cpp /link %LIBLIST% %SYSLIBS% || goto fail

echo.
echo === e2e_aec.exe
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\e2e_aec.exe" tests\e2e_aec.cpp tests\stub\audio_device_room_stub.cpp ^
   src\ravava\AudioProcessor.cpp src\ravava\Recorder.cpp src\ravava\Player.cpp src\ravava\JitterBuffer.cpp src\ravava\OpusCodec.cpp ^
   /link "%OPUS_LIB%" %LIBLIST% %SYSLIBS% || goto fail

echo.
echo === unit_test.exe  (no webrtc needed)
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\unit_test.exe" tests\unit_test.cpp src\ravava\JitterBuffer.cpp src\ravava\OpusCodec.cpp src\ravava\net\PacketRouter.cpp ^
   /link "%OPUS_LIB%" || goto fail

echo.
echo === noalloc_test.exe  (no webrtc needed)
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\noalloc_test.exe" tests\noalloc_test.cpp tests\stub\audio_device_stub.cpp ^
   src\ravava\Recorder.cpp src\ravava\Player.cpp src\ravava\JitterBuffer.cpp src\ravava\OpusCodec.cpp src\ravava\net\PacketRouter.cpp /link "%OPUS_LIB%" || goto fail

echo.
echo === volume_test.exe  (no webrtc needed)
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\volume_test.exe" tests\volume_test.cpp tests\stub\audio_device_stub.cpp ^
   src\ravava\Player.cpp src\ravava\JitterBuffer.cpp src\ravava\OpusCodec.cpp src\ravava\AudioProcessor.cpp /link "%OPUS_LIB%" || goto fail

echo.
echo === format_test.exe  (no webrtc needed)
cl %CFLAGS% /Fo"tests\obj\\" /Fe"tests\bin\format_test.exe" tests\format_test.cpp tests\stub\audio_device_stub.cpp ^
   src\ravava\Player.cpp src\ravava\JitterBuffer.cpp src\ravava\OpusCodec.cpp src\ravava\AudioProcessor.cpp /link "%OPUS_LIB%" || goto fail

echo.
echo === jitter_diff_test.exe  (no webrtc needed)
cl %CFLAGS% /Itests\ref /Fo"tests\obj\\" /Fe"tests\bin\jitter_diff_test.exe" tests\jitter_diff_test.cpp tests\ref\refJitterBuffer.cpp ^
   src\ravava\JitterBuffer.cpp src\ravava\OpusCodec.cpp /link "%OPUS_LIB%" || goto fail

echo.
echo Built OK. Run:
echo     tests\bin\aec_test.exe             (add "noref" for the control run, it must show about 0 dB)
echo     tests\bin\e2e_aec.exe
echo     tests\bin\unit_test.exe            (must end with ALL PASS)
echo     tests\bin\noalloc_test.exe         (must end with PASS)
echo     tests\bin\volume_test.exe          (must end with PASS)
echo     tests\bin\format_test.exe          (must end with PASS)
echo     tests\bin\jitter_diff_test.exe     (must end with PASS)
popd
exit /b 0

:fail
echo.
echo Build FAILED. Common causes:
echo   LNK2038 RuntimeLibrary mismatch  -^> set CRT=/MD (or /MT) to match how the webrtc libs were built
echo   LNK2019 unresolved external __imp_XYZ  -^> add the Windows lib that holds XYZ to SYSLIBS in this file
popd
exit /b 1

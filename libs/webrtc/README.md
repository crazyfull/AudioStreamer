# webrtc-audio-processing 1.3 (echo cancellation)

Used only by `src/ravava/AudioProcessor.cpp` (AEC3, noise suppression, AGC2).

**Optional.** VoiceStream.pro defines `RAVAVA_WITH_WEBRTC` and links the library only when it is found
(pkg-config `webrtc-audio-processing-1` on Linux, `libwebrtc-audio-processing-1.lib` next to the project on Windows).
Not found, or `CONFIG += no_webrtc` = the program builds without it and the echo / noise / AGC controls are disabled.
Outside qmake: compile with `-DRAVAVA_WITH_WEBRTC` to turn it on. Source: https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/
(version **1.3**; the 2.x releases have a different header layout).

## Windows (static, MSVC)

1. Install Python + `pip install meson ninja`, download/extract the 1.3 source.
2. Open **x64 Native Tools Command Prompt for VS** (or the x86 one for a 32 bit app) and run

       libs\webrtc\build_win.bat C:\path\to\webrtc-audio-processing-1.3

   It builds abseil + webrtc twice (`/MT` release, `/MTd` debug, same runtime settings as VoiceStream.pro)
   and writes `libs/webrtc/win32/webrtc_apm_x64.lib`, `webrtc_apm_x64d.lib` and `include/`.
   meson downloads abseil itself (internet needed).
3. Build VoiceStream.pro as usual; it picks the debug or release lib by itself.

If the app link fails with `LNK2019 unresolved external ... __imp_XYZ`, add the Windows library that
contains XYZ to the `LIBS +=` line in VoiceStream.pro (`winmm dbghelp advapi32` are already there).

## Linux

Ubuntu 24.10+/Debian 13: `sudo apt install libwebrtc-audio-processing-1-dev`.
Older distros (e.g. Ubuntu 24.04 only ships 0.3.1, a different API): `sudo apt install meson libabsl-dev`, then in the 1.3 source folder

    meson setup build && ninja -C build && sudo ninja -C build install

(if it installs to /usr/local make sure `pkg-config --cflags webrtc-audio-processing-1` works, `PKG_CONFIG_PATH` / `ldconfig`).
`VoiceStream.pro` finds it through pkg-config.

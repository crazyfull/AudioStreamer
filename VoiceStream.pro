# Qt is only used for the GUI + UDP socket now. The whole audio part (src/*) is Qt-free.
QT += core gui
QT += network
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# src/* uses std::make_unique (C++14) - don't depend on the Qt kit's default standard
CONFIG += c++17

#QMAKE_LFLAGS += -no-pie # mohem, run kardan barname ba dubl click to linux


#Includes
INCLUDEPATH += $$PWD/miniaudio
DEPENDPATH += $$PWD/miniaudio


target.path = /root
# the .lib files are Windows-only: keep them out of Linux/macOS builds (the old "else" also hit those)
win32 {
    contains(QMAKE_TARGET.arch, x86_64) {
        message("win x64 build")
        LIBS += $$PWD/libs/opus/win32/libopus_x64.lib
        TARGET = ../../AudioStreamer_x64
    } else {
        message("win x86 build")
        LIBS += $$PWD/libs/opus/win32/libopus_x86.lib
        TARGET = ../../AudioStreamer_x86
    }
}

win32 {
    INCLUDEPATH += $$PWD/libs/opus/win32/include
    DEPENDPATH += $$PWD/libs/opus/win32/include

    QMAKE_CFLAGS_RELEASE += /MT
    QMAKE_CXXFLAGS_RELEASE += /MT
    QMAKE_CFLAGS_DEBUG += /MTd
    QMAKE_CXXFLAGS_DEBUG += /MTd


    LIBS += libucrt.lib libucrtd.lib libvcruntime.lib

    #CONFIG += -static-runtime
    #
    # static compile more details: https://blogs.msdn.microsoft.com/vcblog/2015/03/03/introducing-the-universal-crt/
    #LIBS += bcrypt.lib libcmt.lib Shell32.lib Shlwapi.lib User32.lib kernel32.lib Ws2_32.lib advapi32.lib crypt32.lib iphlpapi.lib psapi.lib gdi32.lib
}

linux {
    #GCC
    message("linux x64 build")
    TARGET = ../../AudioStreamer_x64

    unix:!macx: LIBS += -L$$PWD/../VoiceCodec/build/lib/ -lopus
    INCLUDEPATH += $$PWD/../VoiceCodec/build/include/opus
    DEPENDPATH += $$PWD/../VoiceCodec/build/include/opus
    unix:!macx: PRE_TARGETDEPS += $$PWD/../VoiceCodec/build/lib/libopus.a

    # miniaudio loads ALSA/Pulse/JACK dynamically
    LIBS += -ldl -lpthread -lm
}

macx {
    LIBS += -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
}

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    miniaudio/miniaudio.c \
    src/audioDevice.cpp \
    src/clsAudioPlayer.cpp \
    src/clsAudioRecorder.cpp \
    src/jitterBuffer.cpp \
    src/opusCodec.cpp

HEADERS += \
    mainwindow.h \
    src/AudioConfig.h \
    src/AudioTypes.h \
    src/audioDevice.h \
    src/clsAudioPlayer.h \
    src/clsAudioRecorder.h \
    src/jitterBuffer.h \
    src/VoicePacket.h \
    src/opusCodec.h

FORMS += \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

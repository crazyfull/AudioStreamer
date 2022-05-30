QT += core gui
QT += network
QT += multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

QMAKE_LFLAGS += -no-pie # mohem, run kardan barname ba dubl click to linux


win32 {
    #vc2019
    message("win32 x86 build")
    LIBS += $$PWD/libs/opus/win32/libopus_x86_vc2019.lib
    TARGET = ../../AudioStreamer_x86

    #include
    INCLUDEPATH += $$PWD/libs/opus/win32/include
    DEPENDPATH += $$PWD/libs/opus/win32/include
}

linux {
    #GCC
    message("linux x64 build")
    LIBS += $$PWD/libs/opus/linux/libopus_64.a
    TARGET = ../../AudioStreamer_x64

    #include
    INCLUDEPATH += $$PWD/libs/opus/linux/include
    DEPENDPATH += $$PWD/libs/opus/linux/include
}


SOURCES += \
    main.cpp \
    mainwindow.cpp \
    src/AudioConfig.cpp \
    src/Resampler.cpp \
    src/clsAudioPlayer.cpp \
    src/clsAudioRecorder.cpp \
    src/opusCodec.cpp

HEADERS += \
    mainwindow.h \
    src/AudioConfig.h \
    src/Resampler.h \
    src/clsAudioPlayer.h \
    src/clsAudioRecorder.h \
    src/opusCodec.h

FORMS += \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

QT += core gui
QT += network multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0


win32 {
    #vc2019
    message("win32 x86 build")
    LIBS += $$PWD/libs/opus/win32/libopus_x86_vc2019.lib
    TARGET = ../../VoiceStream_x86
}

#Headers
INCLUDEPATH += $$PWD/libs/opus/include
DEPENDPATH += $$PWD/libs/opus/include


SOURCES += \
    main.cpp \
    mainwindow.cpp \
    src/clsAudioRecorder.cpp \
    src/opusCodec.cpp

HEADERS += \
    mainwindow.h \
    src/clsAudioRecorder.h \
    src/opusCodec.h

FORMS += \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

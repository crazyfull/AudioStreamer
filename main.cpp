
#include "mainwindow.h"

#include <QApplication>
#include <QTimer>
#ifdef Q_OS_ANDROID
#include "androidpermissions.h"
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_ANDROID
    qputenv("QT_BLOCK_EVENT_LOOPS_WHEN_SUSPENDED", "0");
#endif

    QApplication a(argc, argv);
#ifdef Q_OS_ANDROID
    InitAndroidAudio();         // before MainWindow: its constructor lists the sound devices
#endif
    MainWindow w;
    w.show();
#ifdef Q_OS_ANDROID
    // after the window is up (the permission dialog needs a resumed activity)
    QTimer::singleShot(0, [](){ RequestAndroidAudioPermissions(); });
#endif
    return a.exec();
}

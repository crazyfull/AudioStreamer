#include "mainwindow.h"

#include <QApplication>
#include <QTimer>
#ifdef Q_OS_ANDROID
#include "androidpermissions.h"
#endif

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
#ifdef Q_OS_ANDROID
    // after the window is up (the permission dialog needs a resumed activity)
    QTimer::singleShot(0, [](){ RequestAndroidAudioPermissions(); });
#endif
    return a.exec();
}

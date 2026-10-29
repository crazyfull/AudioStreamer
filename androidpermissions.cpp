#include "androidpermissions.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QStringList>

// Works on every Qt 6 (no QPermission / QtBluetooth needed): the Qt activity is a normal android.app.Activity.
void RequestAndroidAudioPermissions()
{
    QJniObject activity(QNativeInterface::QAndroidApplication::context());
    if(!activity.isValid())
        return;

    QStringList want;
    want << "android.permission.RECORD_AUDIO";
    if(QNativeInterface::QAndroidApplication::sdkVersion() >= 31)
        want << "android.permission.BLUETOOTH_CONNECT";

    QStringList missing;
    for(const QString &p : want){
        const jint granted = activity.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
                                                       QJniObject::fromString(p).object<jstring>());
        if(granted != 0)            // PackageManager.PERMISSION_GRANTED == 0
            missing << p;
    }
    if(missing.isEmpty())
        return;

    QJniEnvironment env;
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray array = env->NewObjectArray(jsize(missing.size()), stringClass, nullptr);
    for(int i = 0; i < missing.size(); ++i){
        QJniObject s = QJniObject::fromString(missing[i]);
        env->SetObjectArrayElement(array, jsize(i), s.object<jstring>());
    }
    activity.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", array, jint(1));
    env->DeleteLocalRef(array);
    env->DeleteLocalRef(stringClass);
}

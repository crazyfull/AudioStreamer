#include "AndroidAudioRoute.h"
#include "../AudioTypes.h"

#include <jni.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <mutex>

namespace ravava {
namespace android {

// ============================================================ JNI plumbing
// No JNI_OnLoad needed (the Qt platform plugin has its own): the VM is found through JNI_GetCreatedJavaVMs, the
// application Context through ActivityThread.currentApplication().
static JavaVM *g_vm = nullptr;          // given by Init()
static jobject g_context = nullptr;     // global ref, given by Init()

static JavaVM *findVm()
{
    if(g_vm)
        return g_vm;
    using GetVms = jint (*)(JavaVM **, jsize, jsize *);
    static GetVms fn = reinterpret_cast<GetVms>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if(!fn){
        RAVAVA_WARN("android: no Java VM (Init() was not called and JNI_GetCreatedJavaVMs is not reachable)");
        return nullptr;
    }
    JavaVM *vm = nullptr;
    jsize n = 0;
    if(fn(&vm, 1, &n) != JNI_OK || n < 1){
        RAVAVA_WARN("android: JNI_GetCreatedJavaVMs found no VM");
        return nullptr;
    }
    g_vm = vm;
    return vm;
}

// Attaches the calling thread for the lifetime of the object (detaches only if WE attached it).
class Env
{
public:
    Env()
    {
        m_vm = findVm();
        if(!m_vm)
            return;
        if(m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6) == JNI_EDETACHED){
#ifdef __ANDROID__
            if(m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK)
#else
            if(m_vm->AttachCurrentThread(reinterpret_cast<void**>(&m_env), nullptr) == JNI_OK)
#endif
                m_attached = true;
            else
                m_env = nullptr;
        }
    }
    ~Env()
    {
        if(m_env && m_env->ExceptionCheck())
            m_env->ExceptionClear();
        if(m_attached)
            m_vm->DetachCurrentThread();
    }
    JNIEnv *operator->() const { return m_env; }
    JNIEnv *get() const { return m_env; }
    explicit operator bool() const { return m_env != nullptr; }
    // true (and the exception is cleared) if the last JNI call threw
    bool failed() const
    {
        if(!m_env || !m_env->ExceptionCheck())
            return false;
        m_env->ExceptionClear();
        return true;
    }
private:
    JavaVM *m_vm = nullptr;
    JNIEnv *m_env = nullptr;
    bool m_attached = false;
};

// owns a local reference
class Ref
{
public:
    Ref() {}
    Ref(JNIEnv *env, jobject o) : m_env(env), m_o(o) {}
    ~Ref() { if(m_o && m_env) m_env->DeleteLocalRef(m_o); }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref &&o) : m_env(o.m_env), m_o(o.m_o) { o.m_o = nullptr; }
    Ref& operator=(Ref &&o) { if(this != &o){ if(m_o && m_env) m_env->DeleteLocalRef(m_o); m_env = o.m_env; m_o = o.m_o; o.m_o = nullptr; } return *this; }
    jobject get() const { return m_o; }
    explicit operator bool() const { return m_o != nullptr; }
private:
    JNIEnv *m_env = nullptr;
    jobject m_o = nullptr;
};

static std::string toStd(Env &env, jstring s)
{
    if(!s) return std::string();
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if(c) env->ReleaseStringUTFChars(s, c);
    return out;
}

static int sdkInt(Env &env)
{
    static int cached = -1;
    if(cached >= 0) return cached;
    Ref cls(env.get(), env->FindClass("android/os/Build$VERSION"));
    if(!cls || env.failed()) return 0;
    jfieldID f = env->GetStaticFieldID(static_cast<jclass>(cls.get()), "SDK_INT", "I");
    if(!f || env.failed()) return 0;
    cached = env->GetStaticIntField(static_cast<jclass>(cls.get()), f);
    return cached;
}

// the AudioManager of the application (null if anything fails)
static Ref audioManager(Env &env)
{
    jobject app = g_context;
    Ref appRef;
    if(!app){
        // fallback: the hidden ActivityThread.currentApplication() (not allowed on every phone)
        Ref at(env.get(), env->FindClass("android/app/ActivityThread"));
        if(env.failed() || !at){ RAVAVA_WARN("android: ActivityThread not found, no Context (call Init())"); return Ref(); }
        jmethodID cur = env->GetStaticMethodID(static_cast<jclass>(at.get()), "currentApplication", "()Landroid/app/Application;");
        if(env.failed() || !cur){ RAVAVA_WARN("android: ActivityThread.currentApplication not available (call Init())"); return Ref(); }
        appRef = Ref(env.get(), env->CallStaticObjectMethod(static_cast<jclass>(at.get()), cur));
        if(env.failed() || !appRef){ RAVAVA_WARN("android: no application Context (call Init())"); return Ref(); }
        app = appRef.get();
    }

    Ref ctxCls(env.get(), env->FindClass("android/content/Context"));
    if(env.failed() || !ctxCls){ RAVAVA_WARN("android: Context class not found"); return Ref(); }
    jmethodID gss = env->GetMethodID(static_cast<jclass>(ctxCls.get()), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if(env.failed() || !gss){ RAVAVA_WARN("android: getSystemService not found"); return Ref(); }
    Ref name(env.get(), env->NewStringUTF("audio"));
    Ref am(env.get(), env->CallObjectMethod(app, gss, name.get()));
    if(env.failed() || !am){ RAVAVA_WARN("android: no AudioManager"); return Ref(); }
    return am;
}

static jmethodID methodOf(Env &env, jobject obj, const char *name, const char *sig)
{
    Ref cls(env.get(), env->GetObjectClass(obj));
    jmethodID m = cls ? env->GetMethodID(static_cast<jclass>(cls.get()), name, sig) : nullptr;
    if(env.failed()) return nullptr;
    return m;
}

// ============================================================ device info
struct Dev
{
    int id = 0;
    int type = 0;
    bool source = false;    // microphone-like
    bool sink = false;      // speaker-like
    std::string name;
};

static const char *typeLabel(int t)
{
    switch(t){
    case 1:  return "Earpiece";
    case 2:  return "Phone speaker";
    case 3:  return "Wired headset";
    case 4:  return "Wired headphones";
    case 5:  return "Line";
    case 7:  return "Bluetooth headset";        // TYPE_BLUETOOTH_SCO (handsfree)
    case 8:  return "Bluetooth";                // TYPE_BLUETOOTH_A2DP (music only)
    case 9:  return "HDMI";
    case 11: return "USB device";
    case 12: return "USB accessory";
    case 13: return "Dock";
    case 15: return "Phone microphone";
    case 16: return "Phone microphone (back)";
    case 21: return "Bus";
    case 22: return "USB headset";
    case 23: return "Hearing aid";
    case 26: return "Bluetooth LE headset";
    case 27: return "Bluetooth LE speaker";
    default: return "";
    }
}

static bool isHandsfree(int t) { return t == 7 || t == 26; }      // SCO / LE audio headset (call quality, has a microphone)
static bool isUseless(int t) { return t == 18 /*telephony*/ || t == 25 /*remote submix*/ || t == 19 /*FM*/ || t == 20 /*aux*/ || t == 0; }

static bool readDevice(Env &env, jobject info, Dev &d)
{
    jmethodID mId = methodOf(env, info, "getId", "()I");
    jmethodID mType = methodOf(env, info, "getType", "()I");
    jmethodID mSource = methodOf(env, info, "isSource", "()Z");
    jmethodID mName = methodOf(env, info, "getProductName", "()Ljava/lang/CharSequence;");
    if(!mId || !mType || !mSource || !mName)
        return false;
    d.id = env->CallIntMethod(info, mId);
    d.type = env->CallIntMethod(info, mType);
    d.source = env->CallBooleanMethod(info, mSource) != JNI_FALSE;
    d.sink = !d.source;
    Ref cs(env.get(), env->CallObjectMethod(info, mName));
    if(env.failed()) return false;
    d.name.clear();
    if(cs){
        jmethodID ts = methodOf(env, cs.get(), "toString", "()Ljava/lang/String;");
        if(ts){
            Ref s(env.get(), env->CallObjectMethod(cs.get(), ts));
            if(s && !env.failed())
                d.name = toStd(env, static_cast<jstring>(s.get()));
        }
    }
    return true;
}

// AudioManager.getDevices(flags): 1 = inputs, 2 = outputs, 3 = both
static std::vector<Dev> getDevices(Env &env, jobject am, int flags)
{
    std::vector<Dev> out;
    jmethodID m = methodOf(env, am, "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");
    if(!m) return out;
    Ref arr(env.get(), env->CallObjectMethod(am, m, jint(flags)));
    if(!arr || env.failed()) return out;
    const jsize n = env->GetArrayLength(static_cast<jarray>(arr.get()));
    for(jsize i = 0; i < n; ++i){
        Ref o(env.get(), env->GetObjectArrayElement(static_cast<jobjectArray>(arr.get()), i));
        Dev d;
        if(o && readDevice(env, o.get(), d))
            out.push_back(d);
    }
    return out;
}

// API 31+: the devices that setCommunicationDevice() accepts (this is where a connected Bluetooth headset is listed
// even before its SCO link is up)
static std::vector<Dev> communicationDevices(Env &env, jobject am)
{
    std::vector<Dev> out;
    if(sdkInt(env) < 31) return out;
    jmethodID m = methodOf(env, am, "getAvailableCommunicationDevices", "()Ljava/util/List;");
    if(!m) return out;
    Ref list(env.get(), env->CallObjectMethod(am, m));
    if(!list || env.failed()) return out;
    jmethodID size = methodOf(env, list.get(), "size", "()I");
    jmethodID get = methodOf(env, list.get(), "get", "(I)Ljava/lang/Object;");
    if(!size || !get) return out;
    const int n = env->CallIntMethod(list.get(), size);
    for(int i = 0; i < n; ++i){
        Ref o(env.get(), env->CallObjectMethod(list.get(), get, jint(i)));
        Dev d;
        if(o && !env.failed() && readDevice(env, o.get(), d))
            out.push_back(d);
    }
    return out;
}

void Init(JavaVM *vm, jobject context)
{
    g_vm = vm;
    if(!vm || !context)
        return;
    Env env;
    if(!env)
        return;
    if(g_context)
        env->DeleteGlobalRef(g_context);
    g_context = env->NewGlobalRef(context);
    RAVAVA_INFO("android: audio routing initialised (Java VM and Context from the GUI)");
}

static std::string idString(const char *prefix, int id) { return std::string(prefix) + std::to_string(id); }

std::vector<AudioDeviceInfo> ListDevices(bool capture)
{
    std::vector<AudioDeviceInfo> out;
    Env env;
    if(!env) return out;
    Ref am = audioManager(env);
    if(!am) return out;

    // Bluetooth handsfree headsets first: they are the ones people look for
    std::vector<Dev> bt;
    for(const Dev &d : communicationDevices(env, am.get()))
        if(isHandsfree(d.type))
            bt.push_back(d);
    for(const Dev &d : getDevices(env, am.get(), 3))
        if(isHandsfree(d.type)){
            bool have = false;
            for(const Dev &b : bt) if(b.id == d.id) have = true;
            if(!have) bt.push_back(d);
        }
    for(const Dev &d : bt){
        AudioDeviceInfo info;
        info.name = (d.name.empty() ? std::string("Bluetooth headset") : d.name) + " (Bluetooth handsfree)";
        info.id = idString("btsco:", d.id);
        out.push_back(info);
    }
    // A2DP-only headphones can't carry a microphone: they are listed as speakers by getDevices below.

    for(const Dev &d : getDevices(env, am.get(), capture ? 1 : 2)){
        if(isUseless(d.type) || isHandsfree(d.type))
            continue;
        AudioDeviceInfo info;
        const std::string label = typeLabel(d.type);
        info.name = d.name.empty() ? (label.empty() ? std::string("Audio device ") + std::to_string(d.id) : label)
                                   : (label.empty() || d.name == label ? d.name : d.name + " (" + label + ")");
        info.id = idString("aaudio:", d.id);
        out.push_back(info);
    }
    RAVAVA_INFO("android: %d %s device(s) listed", int(out.size()), capture ? "input" : "output");
    for(const AudioDeviceInfo &i : out)
        RAVAVA_INFO("android:   %s = %s", i.id.c_str(), i.name.c_str());
    return out;
}

// ============================================================ routing
static std::mutex g_routeMutex;
static int g_heldStreams = 0;           // open streams that hold the communication route
static int g_btSource = 0;              // ids of the headset's microphone / speaker (0 = unknown), valid while the route is held
static int g_btSink = 0;

static bool hasPrefix(const std::string &s, const char *p) { return s.compare(0, std::strlen(p), p) == 0; }

static std::string describe(const Dev &d)
{
    const std::string label = typeLabel(d.type);
    if(d.name.empty())
        return label.empty() ? std::string("Audio device ") + std::to_string(d.id) : label;
    if(label.empty() || d.name == label)
        return d.name;
    return d.name + " (" + label + ")";
}

// ids of the Bluetooth headset's own microphone and speaker (they only exist while the SCO link is up)
static void findHandsfreeIds(Env &env, jobject am, int *source, int *sink)
{
    for(const Dev &d : getDevices(env, am, 3)){
        if(!isHandsfree(d.type))
            continue;
        if(d.source && !*source) *source = d.id;
        if(d.sink && !*sink) *sink = d.id;
    }
}

static void requestBluetoothRoute(Env &env, jobject am, int wantId)
{
    // 1) the phone must be "in a call" for Android to use the headset's microphone
    if(jmethodID setMode = methodOf(env, am, "setMode", "(I)V"))
        env->CallVoidMethod(am, setMode, jint(3));              // MODE_IN_COMMUNICATION
    env.failed();

    // 2) pick the headset
    bool routed = false;
    if(sdkInt(env) >= 31){
        // setCommunicationDevice needs the AudioDeviceInfo object itself: find it again in the list
        jmethodID m = methodOf(env, am, "getAvailableCommunicationDevices", "()Ljava/util/List;");
        jmethodID set = methodOf(env, am, "setCommunicationDevice", "(Landroid/media/AudioDeviceInfo;)Z");
        if(m && set){
            Ref list(env.get(), env->CallObjectMethod(am, m));
            if(list && !env.failed()){
                jmethodID size = methodOf(env, list.get(), "size", "()I");
                jmethodID get = methodOf(env, list.get(), "get", "(I)Ljava/lang/Object;");
                const int n = size ? env->CallIntMethod(list.get(), size) : 0;
                for(int i = 0; get && i < n && !routed; ++i){
                    Ref o(env.get(), env->CallObjectMethod(list.get(), get, jint(i)));
                    Dev d;
                    if(o && !env.failed() && readDevice(env, o.get(), d) && isHandsfree(d.type) && (wantId == 0 || d.id == wantId)){
                        routed = env->CallBooleanMethod(am, set, o.get()) != JNI_FALSE;
                        env.failed();
                    }
                }
            }
        }
    }
    if(!routed){
        // API < 31 (or the call above was refused): the classic Bluetooth SCO link
        if(jmethodID start = methodOf(env, am, "startBluetoothSco", "()V"))
            env->CallVoidMethod(am, start);
        env.failed();
        if(jmethodID on = methodOf(env, am, "setBluetoothScoOn", "(Z)V"))
            env->CallVoidMethod(am, on, JNI_TRUE);
        env.failed();
    }
    RAVAVA_INFO("android: Bluetooth handsfree route requested (%s)", routed ? "communication device" : "SCO");
}

void PrepareStream(const std::string &deviceId, bool capture, int32_t *aaudioId, bool *voicePreset, bool *routeHeld)
{
    *aaudioId = 0;
    *voicePreset = false;
    *routeHeld = false;
    RAVAVA_INFO("android: opening the %s on device '%s'", capture ? "microphone" : "speaker", deviceId.empty() ? "(system default)" : deviceId.c_str());

    // a normal device chosen by id (phone mic, wired, USB...)
    if(hasPrefix(deviceId, "aaudio:")){
        *aaudioId = int32_t(std::atoi(deviceId.c_str() + 7));
        return;
    }

    std::lock_guard<std::mutex> lk(g_routeMutex);
    const bool wantsBt = hasPrefix(deviceId, "btsco:");
    // "System default" follows an active Bluetooth route: mic and speaker must not end up on different devices
    if(!wantsBt && !(deviceId.empty() && g_heldStreams > 0))
        return;

    Env env;
    if(!env){ RAVAVA_WARN("android: no JNI, Bluetooth route not set"); return; }
    Ref am = audioManager(env);
    if(!am){ RAVAVA_WARN("android: no AudioManager, Bluetooth route not set"); return; }

    if(wantsBt){
        if(g_heldStreams == 0){
            g_btSource = g_btSink = 0;
            requestBluetoothRoute(env, am.get(), std::atoi(deviceId.c_str() + 6));
        }
        ++g_heldStreams;
        *routeHeld = true;
    }

    // The headset's microphone / speaker only appear once the SCO link is up (takes up to a few seconds). A stream opened
    // before that would silently use the phone's microphone, so wait for them and pin the stream to them.
    for(int i = 0; i < 60 && !(capture ? g_btSource : g_btSink); ++i){
        findHandsfreeIds(env, am.get(), &g_btSource, &g_btSink);
        if(capture ? g_btSource : g_btSink)
            break;
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    *aaudioId = capture ? g_btSource : g_btSink;
    *voicePreset = true;
    if(*aaudioId)
        RAVAVA_INFO("android: %s pinned to the Bluetooth headset (device id %d)", capture ? "microphone" : "speaker", int(*aaudioId));
    else
        RAVAVA_WARN("android: the Bluetooth headset's %s did not appear (SCO link not up?), the system chooses", capture ? "microphone" : "speaker");
}

void ReleaseStream(bool routeHeld)
{
    if(!routeHeld)
        return;
    std::lock_guard<std::mutex> lk(g_routeMutex);
    if(g_heldStreams > 0 && --g_heldStreams > 0)
        return;                         // the other direction (mic / speaker) still uses the route
    g_heldStreams = 0;
    g_btSource = g_btSink = 0;

    Env env;
    if(!env) return;
    Ref am = audioManager(env);
    if(!am) return;
    if(sdkInt(env) >= 31)
        if(jmethodID clear = methodOf(env, am.get(), "clearCommunicationDevice", "()V"))
            env->CallVoidMethod(am.get(), clear);
    env.failed();
    if(jmethodID off = methodOf(env, am.get(), "setBluetoothScoOn", "(Z)V"))
        env->CallVoidMethod(am.get(), off, JNI_FALSE);
    env.failed();
    if(jmethodID stop = methodOf(env, am.get(), "stopBluetoothSco", "()V"))
        env->CallVoidMethod(am.get(), stop);
    env.failed();
    if(jmethodID setMode = methodOf(env, am.get(), "setMode", "(I)V"))
        env->CallVoidMethod(am.get(), setMode, jint(0));        // MODE_NORMAL
    env.failed();
}

std::string DescribeActiveDevice(bool capture)
{
    Env env;
    if(!env) return std::string();
    Ref am = audioManager(env);
    if(!am) return std::string();
    const int sdk = sdkInt(env);

    if(capture){
        if(sdk < 28) return std::string();
        jmethodID m = methodOf(env, am.get(), "getActiveRecordingConfigurations", "()Ljava/util/List;");
        if(!m) return std::string();
        Ref list(env.get(), env->CallObjectMethod(am.get(), m));
        if(!list || env.failed()) return std::string();
        jmethodID size = methodOf(env, list.get(), "size", "()I");
        jmethodID get = methodOf(env, list.get(), "get", "(I)Ljava/lang/Object;");
        if(!size || !get) return std::string();
        const int n = env->CallIntMethod(list.get(), size);
        for(int i = 0; i < n; ++i){
            Ref cfg(env.get(), env->CallObjectMethod(list.get(), get, jint(i)));
            if(!cfg || env.failed()) continue;
            jmethodID gd = methodOf(env, cfg.get(), "getAudioDevice", "()Landroid/media/AudioDeviceInfo;");
            if(!gd) continue;
            Ref info(env.get(), env->CallObjectMethod(cfg.get(), gd));
            Dev d;
            if(info && !env.failed() && readDevice(env, info.get(), d))
                return describe(d);
        }
        return std::string();
    }

    if(sdk < 31) return std::string();
    jmethodID m = methodOf(env, am.get(), "getCommunicationDevice", "()Landroid/media/AudioDeviceInfo;");
    if(!m) return std::string();
    Ref info(env.get(), env->CallObjectMethod(am.get(), m));
    Dev d;
    if(info && !env.failed() && readDevice(env, info.get(), d))
        return describe(d);
    return std::string();
}

} // namespace android
} // namespace ravava

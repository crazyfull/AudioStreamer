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

// ---- the "call" model --------------------------------------------------------------------------------------------------
// Like WhatsApp / Discord: the phone is put in MODE_IN_COMMUNICATION, both streams are opened as voice-communication
// streams WITHOUT a device id, and the ONE communication device (speaker / earpiece / wired / USB / Bluetooth headset) is
// chosen with AudioManager. Android then routes the speaker AND the microphone to it, keeps them together and re-routes
// by itself when a headset connects or a cable is pulled. (Pinning each stream to a device id, as an earlier version did,
// fights the system and ends in the wrong device after every notification / USB / call event.)
enum Kind { KindOther = 9, KindBluetooth = 0, KindWired = 1, KindSpeaker = 2, KindEarpiece = 3 };     // = priority of "automatic"

static int kindOf(int type)
{
    switch(type){
    case 7: case 26: case 23:           return KindBluetooth;       // SCO, LE headset, hearing aid
    case 3: case 4: case 22: case 11:   return KindWired;           // wired headset / headphones, USB headset / device
    case 2:                             return KindSpeaker;
    case 1:                             return KindEarpiece;
    default:                            return KindOther;
    }
}

struct CommItem { Ref obj; Dev d; };

// the devices setCommunicationDevice() accepts (API 31+), with their Java objects
static std::vector<CommItem> commItems(Env &env, jobject am)
{
    std::vector<CommItem> out;
    jmethodID m = methodOf(env, am, "getAvailableCommunicationDevices", "()Ljava/util/List;");
    if(!m) return out;
    Ref list(env.get(), env->CallObjectMethod(am, m));
    if(!list || env.failed()) return out;
    jmethodID size = methodOf(env, list.get(), "size", "()I");
    jmethodID get = methodOf(env, list.get(), "get", "(I)Ljava/lang/Object;");
    if(!size || !get) return out;
    const int n = env->CallIntMethod(list.get(), size);
    for(int i = 0; i < n; ++i){
        CommItem it;
        it.obj = Ref(env.get(), env->CallObjectMethod(list.get(), get, jint(i)));
        if(it.obj && !env.failed() && readDevice(env, it.obj.get(), it.d) && kindOf(it.d.type) != KindOther)
            out.push_back(std::move(it));
    }
    return out;
}

// index of the wanted device (by id), or of the best one when wantId is 0 / gone: Bluetooth > wired > speaker > earpiece
static int pickIndex(const std::vector<CommItem> &items, int wantId)
{
    int best = -1;
    for(size_t i = 0; i < items.size(); ++i){
        if(wantId != 0 && items[i].d.id == wantId)
            return int(i);
        if(best < 0 || kindOf(items[i].d.type) < kindOf(items[best].d.type))
            best = int(i);
    }
    return best;
}

// what the user can choose as the route (speaker side). The microphone follows it.
static std::vector<AudioDeviceInfo> listRoutes(Env &env, jobject am)
{
    std::vector<AudioDeviceInfo> out;
    std::vector<Dev> devs;
    if(sdkInt(env) >= 31){
        for(const CommItem &it : commItems(env, am))
            devs.push_back(it.d);
    }else{
        for(const Dev &d : getDevices(env, am, 2))
            if(kindOf(d.type) != KindOther)
                devs.push_back(d);
    }
    for(const Dev &d : devs){
        AudioDeviceInfo info;
        const std::string label = typeLabel(d.type);
        if(kindOf(d.type) == KindBluetooth)
            info.name = (d.name.empty() ? std::string("Bluetooth headset") : d.name) + " (Bluetooth handsfree)";
        else
            info.name = d.name.empty() ? (label.empty() ? std::string("Audio device ") + std::to_string(d.id) : label)
                                       : (label.empty() || d.name == label ? d.name : d.name + " (" + label + ")");
        info.id = idString(kindOf(d.type) == KindBluetooth ? "btsco:" : "aaudio:", d.id);
        out.push_back(info);
    }
    RAVAVA_INFO("android: %d output route(s) listed", int(out.size()));
    for(const AudioDeviceInfo &i : out)
        RAVAVA_INFO("android:   %s = %s", i.id.c_str(), i.name.c_str());
    return out;
}

std::vector<AudioDeviceInfo> ListDevices(bool capture)
{
    std::vector<AudioDeviceInfo> out;
    Env env;
    if(!env) return out;
    Ref am = audioManager(env);
    if(!am) return out;

    if(!capture)
        return listRoutes(env, am.get());

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
static int g_heldStreams = 0;           // open streams: while > 0 the phone is in call mode with our route
static int g_wantId = 0;                // the communication device the user chose (AudioDeviceInfo id), 0 = automatic
static int g_btSource = 0;              // ids of the headset's microphone / speaker (0 = unknown), found after the SCO link is up
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

static void callVoid(Env &env, jobject am, const char *name, const char *sig, jint arg)
{
    if(jmethodID m = methodOf(env, am, name, sig))
        env->CallVoidMethod(am, m, arg);
    env.failed();
}
static void callVoid(Env &env, jobject am, const char *name)
{
    if(jmethodID m = methodOf(env, am, name, "()V"))
        env->CallVoidMethod(am, m);
    env.failed();
}
static void setSpeakerphone(Env &env, jobject am, bool on)
{
    callVoid(env, am, "setSpeakerphoneOn", "(Z)V", on ? JNI_TRUE : JNI_FALSE);
}
static void setScoOn(Env &env, jobject am, bool on)
{
    callVoid(env, am, "setBluetoothScoOn", "(Z)V", on ? JNI_TRUE : JNI_FALSE);
}

// Selects the communication device. Returns a name for the log ("" = nothing could be selected).
// attempt 0: plain setCommunicationDevice. 1: clear it first. 2+: also start the classic SCO link for a Bluetooth headset (what
// call apps do); the system sometimes refuses the new API alone after a call.
static std::string applyRoute(Env &env, jobject am, int wantId, int attempt = 0)
{
    if(sdkInt(env) >= 31){
        std::vector<CommItem> items = commItems(env, am);
        const int i = pickIndex(items, wantId);
        if(i < 0)
            return std::string();
        if(attempt >= 1)
            callVoid(env, am, "clearCommunicationDevice");
        if(attempt >= 2 && kindOf(items[size_t(i)].d.type) == KindBluetooth){
            setSpeakerphone(env, am, false);
            callVoid(env, am, "startBluetoothSco");
            setScoOn(env, am, true);
        }
        jmethodID set = methodOf(env, am, "setCommunicationDevice", "(Landroid/media/AudioDeviceInfo;)Z");
        if(!set)
            return std::string();
        const bool ok = env->CallBooleanMethod(am, set, items[size_t(i)].obj.get()) != JNI_FALSE;
        env.failed();
        if(!ok)
            RAVAVA_WARN("android: setCommunicationDevice(%s) was refused (attempt %d)", describe(items[size_t(i)].d).c_str(), attempt);
        return ok ? describe(items[size_t(i)].d) : std::string();
    }

    // API < 31: speakerphone flag + the classic Bluetooth SCO link
    std::vector<Dev> outs;
    for(const Dev &d : getDevices(env, am, 2))
        if(kindOf(d.type) != KindOther)
            outs.push_back(d);
    int best = -1;
    for(size_t i = 0; i < outs.size(); ++i){
        if(wantId != 0 && outs[i].id == wantId){ best = int(i); break; }
        if(best < 0 || kindOf(outs[i].type) < kindOf(outs[size_t(best)].type)) best = int(i);
    }
    if(best < 0)
        return std::string();
    switch(kindOf(outs[size_t(best)].type)){
    case KindBluetooth:
        setSpeakerphone(env, am, false);
        callVoid(env, am, "startBluetoothSco");
        setScoOn(env, am, true);
        break;
    case KindSpeaker:
        setScoOn(env, am, false);
        callVoid(env, am, "stopBluetoothSco");
        setSpeakerphone(env, am, true);
        break;
    default:                            // earpiece / wired / USB: the system picks the wired one by itself
        setScoOn(env, am, false);
        callVoid(env, am, "stopBluetoothSco");
        setSpeakerphone(env, am, false);
        break;
    }
    return describe(outs[size_t(best)]);
}

// id of the current communication device (API 31+), 0 when unknown
static int currentCommId(Env &env, jobject am)
{
    int id = 0;
    if(jmethodID m = methodOf(env, am, "getCommunicationDevice", "()Landroid/media/AudioDeviceInfo;")){
        Ref info(env.get(), env->CallObjectMethod(am, m));
        Dev d;
        if(info && !env.failed() && readDevice(env, info.get(), d))
            id = d.id;
        env.failed();
    }
    return id;
}

// the system keeps refusing our route (e.g. the headset's SCO link cannot come up): stop fighting after a few tries, or the
// route is re-set every 2 s forever (audible as a loop of glitches). Reset by a new stream / a changed device list.
static int g_failStreak = 0;
static bool g_giveUp = false;
static std::string g_deviceSignature;

static std::string deviceSignature(Env &env, jobject am)
{
    std::string sig;
    for(const CommItem &it : commItems(env, am))
        sig += std::to_string(it.d.id) + ",";
    return sig;
}

static void sleepMs(int ms)
{
    struct timespec ts = {ms / 1000, long(ms % 1000) * 1000 * 1000};
    nanosleep(&ts, nullptr);
}

void PrepareStream(const std::string &deviceId, bool capture, int32_t *aaudioId, bool *voicePreset, bool *routeHeld)
{
    *aaudioId = 0;
    *voicePreset = false;
    *routeHeld = false;
    RAVAVA_INFO("android: opening the %s on device '%s'", capture ? "microphone" : "speaker", deviceId.empty() ? "(system default)" : deviceId.c_str());

    std::lock_guard<std::mutex> lk(g_routeMutex);
    Env env;
    if(!env){ RAVAVA_WARN("android: no JNI, the system default route is used"); return; }
    Ref am = audioManager(env);
    if(!am){ RAVAVA_WARN("android: no AudioManager, the system default route is used"); return; }

    const bool wantsBt = hasPrefix(deviceId, "btsco:");
    const int idNumber = wantsBt ? std::atoi(deviceId.c_str() + 6) : hasPrefix(deviceId, "aaudio:") ? std::atoi(deviceId.c_str() + 7) : 0;

    // 1) call mode (once) + the communication device
    const bool first = g_heldStreams == 0;
    if(first){
        g_btSource = g_btSink = 0;
        callVoid(env, am.get(), "setMode", "(I)V", 3);              // MODE_IN_COMMUNICATION
    }
    ++g_heldStreams;
    g_failStreak = 0;
    g_giveUp = false;
    g_deviceSignature.clear();
    *routeHeld = true;
    *voicePreset = true;                // voice-communication usage / input preset: the system routes them with the call

    std::string routed;
    if(!capture){
        g_wantId = idNumber;            // the speaker choice defines the route (0 = automatic: Bluetooth > wired > speaker)
        routed = applyRoute(env, am.get(), g_wantId);
    }else if(wantsBt){
        g_wantId = idNumber;            // a headset microphone: the headset is the route (the speaker follows, as in any call app)
        routed = applyRoute(env, am.get(), g_wantId);
    }else if(first){
        g_wantId = 0;
        routed = applyRoute(env, am.get(), 0);
    }
    if(!routed.empty())
        RAVAVA_INFO("android: call route = %s", routed.c_str());

    // 2) the microphone: only an explicitly chosen one is pinned (the system picks the one of the route otherwise)
    if(!capture)
        return;                         // the speaker is never pinned: it follows the communication device
    if(hasPrefix(deviceId, "aaudio:")){
        *aaudioId = idNumber;
        return;
    }
    if(wantsBt){
        // The headset's microphone only appears once the SCO link is up (a few seconds). A stream opened before that
        // would silently use the phone's microphone: wait for it and pin the stream to it.
        for(int i = 0; i < 40 && !g_btSource; ++i){
            findHandsfreeIds(env, am.get(), &g_btSource, &g_btSink);
            if(g_btSource)
                break;
            sleepMs(50);
        }
        *aaudioId = g_btSource;
        if(*aaudioId)
            RAVAVA_INFO("android: microphone pinned to the Bluetooth headset (device id %d)", int(*aaudioId));
        else
            RAVAVA_WARN("android: the Bluetooth headset's microphone did not appear (SCO link not up?), the system chooses");
    }
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
    g_wantId = 0;

    Env env;
    if(!env) return;
    Ref am = audioManager(env);
    if(!am) return;
    if(sdkInt(env) >= 31)
        callVoid(env, am.get(), "clearCommunicationDevice");
    setSpeakerphone(env, am.get(), false);
    setScoOn(env, am.get(), false);
    callVoid(env, am.get(), "stopBluetoothSco");
    callVoid(env, am.get(), "setMode", "(I)V", 0);                  // MODE_NORMAL
}

// The system changes the communication device / mode by itself (call, USB cable, a headset connecting): like every call
// app, look at it about once a second and put our route back (or follow a newly connected headset in automatic mode).
void ReassertRoute()
{
    static int64_t lastMs = 0;
    std::unique_lock<std::mutex> lk(g_routeMutex, std::try_to_lock);    // the device worker may be inside PrepareStream: don't wait for it
    if(!lk.owns_lock() || g_heldStreams == 0)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    if(now - lastMs < 2000)
        return;
    lastMs = now;

    Env env;
    if(!env) return;
    Ref am = audioManager(env);
    if(!am) return;
    jmethodID getMode = methodOf(env, am.get(), "getMode", "()I");
    if(!getMode) return;
    const int mode = env->CallIntMethod(am.get(), getMode);
    if(env.failed()) return;
    if(mode == 1 || mode == 2 || mode == 4)
        return;                         // ringing / a real call / call screening: the phone app owns the audio, don't fight it

    // a connected / removed device changes the situation: try again
    if(sdkInt(env) >= 31){
        const std::string sig = deviceSignature(env, am.get());
        if(sig != g_deviceSignature){
            g_deviceSignature = sig;
            g_failStreak = 0;
            g_giveUp = false;
        }
    }

    bool ok = (mode == 3);              // MODE_IN_COMMUNICATION: still the one we set
    if(ok && sdkInt(env) >= 31){
        std::vector<CommItem> items = commItems(env, am.get());
        const int want = pickIndex(items, g_wantId);
        ok = want < 0 || items[size_t(want)].d.id == currentCommId(env, am.get());
    }
    if(ok){
        g_failStreak = 0;
        return;
    }
    if(g_giveUp)
        return;
    if(g_failStreak >= 4){
        g_giveUp = true;
        RAVAVA_WARN("android: the system does not accept the call route (%s), leaving the route to the system",
                    DescribeRouteState().c_str());
        return;
    }
    RAVAVA_WARN("android: the call route was changed by the system (%s), restoring it (attempt %d)",
                DescribeRouteState().c_str(), g_failStreak);
    if(mode != 3)
        callVoid(env, am.get(), "setMode", "(I)V", 3);
    const std::string routed = applyRoute(env, am.get(), g_wantId, g_failStreak++);
    if(!routed.empty())
        RAVAVA_INFO("android: call route = %s", routed.c_str());
}

std::string DescribeRouteState()
{
    Env env;
    if(!env) return "(no JNI)";
    Ref am = audioManager(env);
    if(!am) return "(no AudioManager)";
    std::string out;
    if(jmethodID m = methodOf(env, am.get(), "getMode", "()I")){
        static const char *modes[] = {"NORMAL", "RINGTONE", "IN_CALL", "IN_COMMUNICATION", "CALL_SCREENING"};
        const int v = env->CallIntMethod(am.get(), m);
        out += std::string("mode=") + ((v >= 0 && v < 5) ? modes[v] : std::to_string(v).c_str());
    }
    env.failed();
    if(jmethodID m = methodOf(env, am.get(), "isBluetoothScoOn", "()Z"))
        out += env->CallBooleanMethod(am.get(), m) ? " scoOn" : " scoOff";
    env.failed();
    if(sdkInt(env) >= 31)
        if(jmethodID m = methodOf(env, am.get(), "getCommunicationDevice", "()Landroid/media/AudioDeviceInfo;")){
            Ref info(env.get(), env->CallObjectMethod(am.get(), m));
            Dev d;
            if(info && !env.failed() && readDevice(env, info.get(), d))
                out += " commDevice=" + describe(d);
        }
    env.failed();
    return out;
}

int StreamDeviceId(void *aaudioStream)
{
    using GetDeviceId = int32_t (*)(void *);
    static GetDeviceId fn = []() -> GetDeviceId {
        void *lib = dlopen("libaaudio.so", RTLD_NOW);
        return lib ? reinterpret_cast<GetDeviceId>(dlsym(lib, "AAudioStream_getDeviceId")) : nullptr;    // API 28+
    }();
    if(!fn || !aaudioStream)
        return -1;
    return int(fn(aaudioStream));
}

std::string DescribeDeviceId(int deviceId, bool capture)
{
    if(deviceId <= 0)
        return std::string();
    Env env;
    if(!env) return std::string();
    Ref am = audioManager(env);
    if(!am) return std::string();
    for(const Dev &d : getDevices(env, am.get(), capture ? 1 : 2))
        if(d.id == deviceId)
            return describe(d);
    return std::string("device #") + std::to_string(deviceId);
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

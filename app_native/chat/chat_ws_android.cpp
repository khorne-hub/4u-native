// The relay sockets on Android (chat_ws.h): the app's ChatSocket class does the work (an SSLSocket checked against the
// system's certificates and the relay's name, the WebSocket handshake and frames, a reading thread per socket); this
// calls its static methods. Text goes both ways as UTF-8 bytes (JNI's own strings are modified UTF-8, which an emoji
// would break).
#ifdef __ANDROID__
#include "chat_ws.h"

#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <jni.h>

#include <mutex>
#include <string>

namespace chatws {
namespace {
std::mutex g_mu;
std::string g_class_name;
jclass g_cls;
jmethodID m_open, m_state, m_send, m_poll, m_error, m_close;
bool g_failed;

bool jfail(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}
// (the activity's class loader: FindClass from a thread the runtime made sees only the system's classes)
jclass load_class(JNIEnv *env, const std::string &dotted) {
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!act) return nullptr;
    jclass act_cls = env->GetObjectClass(act), loader_cls = nullptr, found = nullptr;
    jobject loader = nullptr, cls = nullptr;
    jstring name = nullptr;
    jmethodID get_loader = env->GetMethodID(act_cls, "getClassLoader", "()Ljava/lang/ClassLoader;"), load = nullptr;
    if (!jfail(env) && get_loader) loader = env->CallObjectMethod(act, get_loader);
    if (!jfail(env) && loader) loader_cls = env->FindClass("java/lang/ClassLoader");
    if (!jfail(env) && loader_cls) load = env->GetMethodID(loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!jfail(env) && load) name = env->NewStringUTF(dotted.c_str());
    if (!jfail(env) && name) cls = env->CallObjectMethod(loader, load, name);
    if (!jfail(env) && cls) found = (jclass)env->NewGlobalRef(cls);
    for (jobject o : {cls, (jobject)name, (jobject)loader_cls, loader, (jobject)act_cls, act})
        if (o) env->DeleteLocalRef(o);
    return found;
}
JNIEnv *ready() {  // (g_mu held) the class and its methods, found once
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();  // (SDL attaches this thread)
    if (!env || g_failed) return nullptr;
    if (g_cls) return env;
    std::string dotted = g_class_name;
    for (char &c : dotted)
        if (c == '/') c = '.';
    g_cls = load_class(env, dotted);
    if (g_cls) {
        m_open = env->GetStaticMethodID(g_cls, "open", "(Ljava/lang/String;)I");
        m_state = env->GetStaticMethodID(g_cls, "state", "(I)I");
        m_send = env->GetStaticMethodID(g_cls, "send", "(I[B)Z");
        m_poll = env->GetStaticMethodID(g_cls, "poll", "(I)[B");
        m_error = env->GetStaticMethodID(g_cls, "error", "(I)Ljava/lang/String;");
        m_close = env->GetStaticMethodID(g_cls, "close", "(I)V");
    }
    if (jfail(env) || !g_cls || !m_open || !m_state || !m_send || !m_poll || !m_error || !m_close) {
        g_failed = true;
        __android_log_print(ANDROID_LOG_WARN, "chat", "%s not usable: no chat relays", g_class_name.c_str());
        return nullptr;
    }
    return env;
}
}  // namespace

void platform_init(const char *java_class) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_class_name.empty()) g_class_name = java_class;
    ready();
}
bool available() {
    std::lock_guard<std::mutex> l(g_mu);
    return ready() != nullptr;
}
int open(const std::string &url) {
    std::lock_guard<std::mutex> l(g_mu);
    JNIEnv *env = ready();
    if (!env) return 0;
    jstring u = env->NewStringUTF(url.c_str());  // (an address: ASCII)
    jint h = env->CallStaticIntMethod(g_cls, m_open, u);
    env->DeleteLocalRef(u);
    return jfail(env) ? 0 : (int)h;
}
int state(int h) {
    std::lock_guard<std::mutex> l(g_mu);
    JNIEnv *env = ready();
    if (!env) return CLOSED;
    jint s = env->CallStaticIntMethod(g_cls, m_state, (jint)h);
    return jfail(env) ? CLOSED : (int)s;
}
bool send(int h, const std::string &text) {
    std::lock_guard<std::mutex> l(g_mu);
    JNIEnv *env = ready();
    if (!env) return false;
    jbyteArray b = env->NewByteArray((jsize)text.size());
    if (!b || jfail(env)) return false;
    env->SetByteArrayRegion(b, 0, (jsize)text.size(), (const jbyte *)text.data());
    jboolean ok = env->CallStaticBooleanMethod(g_cls, m_send, (jint)h, b);
    env->DeleteLocalRef(b);
    return !jfail(env) && ok;
}
bool poll(int h, std::string &text) {
    std::lock_guard<std::mutex> l(g_mu);
    JNIEnv *env = ready();
    if (!env) return false;
    jbyteArray b = (jbyteArray)env->CallStaticObjectMethod(g_cls, m_poll, (jint)h);
    if (jfail(env) || !b) return false;
    jsize n = env->GetArrayLength(b);
    text.resize((size_t)n);
    if (n) env->GetByteArrayRegion(b, 0, n, (jbyte *)&text[0]);
    env->DeleteLocalRef(b);
    return !jfail(env);
}
std::string error(int h) {
    std::lock_guard<std::mutex> l(g_mu);
    JNIEnv *env = ready();
    if (!env) return "";
    jstring s = (jstring)env->CallStaticObjectMethod(g_cls, m_error, (jint)h);
    if (jfail(env) || !s) return "";
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
    return r;
}
void close(int h) {
    std::lock_guard<std::mutex> l(g_mu);
    JNIEnv *env = ready();
    if (!env) return;
    env->CallStaticVoidMethod(g_cls, m_close, (jint)h);
    jfail(env);
}

}  // namespace chatws
#endif

// Android build of src/net/transport.h (HTTPS requests and the WebSocket client).
//
// The desktop builds get TLS from the operating system (WinHTTP) or from OpenSSL; the NDK exposes
// no TLS library, so the HTTP exchanges go through the system's own stack on the Java side:
// com.scacelith.game.Http (HttpURLConnection) runs one exchange on the calling thread and hands
// the head and each piece of the body back through Native.nativeHttpHead / nativeHttpBody. That
// carries the coach's voice model download and the online client's requests.
//
// Still missing: the WebSocket (wsConnect reports "unavailable"), so live online games and direct
// match do not connect; an RFC 6455 client over a Java socket is the next step.
#if !defined(__ANDROID__)
// The desktop CMakeLists globs src/net/*.cpp into scacelith_core, and this file is Android-only
// (the desktop builds get their transport from transport_openssl.cpp / transport_win.cpp). Its body
// is therefore compiled for Android alone: on any other target this is an empty translation unit.
#else
#include "transport.h"
#include "../core/log.h"
#include "../platform/platform_android.h"

#include <jni.h>
#include <cstring>

namespace net {

namespace {

// One exchange in flight, handed to Java as a jlong and back to the callbacks below.
struct Exchange {
    const std::function<bool(const HttpHead&)>* onHead = nullptr;
    const std::function<bool(const char*, size_t)>* onBody = nullptr;
    HttpResponse* resp = nullptr;
    size_t received = 0;
    size_t maxBytes = 0;
    bool tooLarge = false;
};

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

std::string hostForUrl(const std::string& host) { return host.find(':') != std::string::npos ? "[" + host + "]" : host; }

// Runs one exchange through Http.run. Sets resp.error / resp.detail on failure.
void exchange(const HttpRequest& req, Exchange& x, CancelToken* cancel) {
    HttpResponse& resp = *x.resp;
    if (!req.tls && !isLoopbackHost(req.host)) {
        resp.error = "insecure";
        return;
    }
    JNIEnv* env = android_plat::threadEnv();
    jclass cls = static_cast<jclass>(android_plat::httpClass());
    if (!env || !cls) {
        resp.error = "unavailable";
        resp.detail = "no JNI environment or no Http class";
        return;
    }
    static jmethodID ctor = env->GetMethodID(cls, "<init>", "()V");
    static jmethodID run = env->GetMethodID(cls, "run", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BILjava/lang/String;J)V");
    static jmethodID abort = env->GetMethodID(cls, "abort", "()V");
    static jfieldID resultField = env->GetFieldID(cls, "result", "[Ljava/lang/String;");
    if (!ctor || !run || !abort || !resultField) {
        env->ExceptionClear();
        resp.error = "unavailable";
        resp.detail = "Http class without the expected members";
        return;
    }
    jobject local = env->NewObject(cls, ctor);
    jobject http = local ? env->NewGlobalRef(local) : nullptr;
    if (local) env->DeleteLocalRef(local);
    if (!http) {
        env->ExceptionClear();
        resp.error = "unavailable";
        return;
    }

    const std::string url = std::string(req.tls ? "https://" : "http://") + hostForUrl(req.host) + ":" +
                            std::to_string(req.port) + req.path;
    std::vector<std::pair<std::string, std::string>> hs;
    hs.emplace_back("Accept", req.accept);
    const bool hasBody = !req.body.empty() || req.method == "POST" || req.method == "PUT";
    if (hasBody) hs.emplace_back("Content-Type", "application/json");
    for (const auto& h : req.headers) hs.push_back(h);
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray jh = env->NewObjectArray(jsize(hs.size() * 2), stringClass, nullptr);
    for (size_t i = 0; i < hs.size(); ++i) {
        jstring k = env->NewStringUTF(hs[i].first.c_str()), v = env->NewStringUTF(hs[i].second.c_str());
        env->SetObjectArrayElement(jh, jsize(2 * i), k);
        env->SetObjectArrayElement(jh, jsize(2 * i + 1), v);
        env->DeleteLocalRef(k);
        env->DeleteLocalRef(v);
    }
    jbyteArray jbody = nullptr;
    if (hasBody) {
        jbody = env->NewByteArray(jsize(req.body.size()));
        env->SetByteArrayRegion(jbody, 0, jsize(req.body.size()), reinterpret_cast<const jbyte*>(req.body.data()));
    }
    jstring jurl = env->NewStringUTF(url.c_str()), jmethod = env->NewStringUTF(req.method.c_str());
    jstring jpin = env->NewStringUTF(req.pinnedSha256.c_str());

    {
        // A cancel from another thread closes the connection under the blocked read.
        AbortGuard guard(cancel, [http] {
            if (JNIEnv* e = android_plat::threadEnv()) {
                e->CallVoidMethod(http, abort);
                if (e->ExceptionCheck()) e->ExceptionClear();
            }
        });
        env->CallVoidMethod(http, run, jurl, jmethod, jh, jbody, jint(req.timeoutMs), jpin, jlong(reinterpret_cast<intptr_t>(&x)));
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        resp.error = "network";
        resp.detail = "exception in Http.run";
    } else {
        jobjectArray r = static_cast<jobjectArray>(env->GetObjectField(http, resultField));
        jstring e0 = static_cast<jstring>(env->GetObjectArrayElement(r, 0));
        jstring e1 = static_cast<jstring>(env->GetObjectArrayElement(r, 1));
        std::string err = jstr(env, e0), detail = jstr(env, e1);
        env->DeleteLocalRef(e0);
        env->DeleteLocalRef(e1);
        env->DeleteLocalRef(r);
        if (x.tooLarge) err = "too_large";
        else if (err == "cancelled" || (cancel && cancel->cancelled() && !err.empty())) err = "cancelled";
        if (resp.error.empty()) {
            resp.error = err;
            if (!detail.empty()) resp.detail = detail;
        }
    }
    for (jobject o : {static_cast<jobject>(jurl), static_cast<jobject>(jmethod), static_cast<jobject>(jpin),
                      static_cast<jobject>(jh), static_cast<jobject>(jbody), static_cast<jobject>(stringClass)})
        if (o) env->DeleteLocalRef(o);
    env->DeleteGlobalRef(http);
    if (!resp.error.empty() && resp.error != "aborted")
        LOGW("http: %s %s%s: %s (%s)", req.method.c_str(), req.host.c_str(), req.path.c_str(), resp.error.c_str(), resp.detail.c_str());
}

}  // namespace

bool transportAvailable() { return android_plat::httpClass() != nullptr; }

void httpRequest(const HttpRequest& req, HttpResponse& resp, CancelToken* cancel) {
    resp = HttpResponse();
    Exchange x;
    x.resp = &resp;
    x.maxBytes = req.maxResponseBytes;
    std::function<bool(const HttpHead&)> onHead = [](const HttpHead&) { return true; };
    std::function<bool(const char*, size_t)> onBody = [&](const char* p, size_t n) {
        resp.body.append(p, n);
        return true;
    };
    x.onHead = &onHead;
    x.onBody = &onBody;
    exchange(req, x, cancel);
}

void httpStream(const HttpRequest& req, const std::function<bool(const HttpHead&)>& onHead,
                const std::function<bool(const char*, size_t)>& onBody, HttpResponse& resp, CancelToken* cancel) {
    resp = HttpResponse();
    Exchange x;
    x.resp = &resp;
    x.maxBytes = req.maxResponseBytes;
    x.onHead = &onHead;
    x.onBody = &onBody;
    exchange(req, x, cancel);
}

std::unique_ptr<WebSocket> wsConnect(const WsParams& p, std::string& error, WsAnswer& answer, CancelToken*) {
    (void)p;
    answer.status = 0;
    error = "unavailable";
    LOGW("networking: no WebSocket client on Android yet (live online games are off)");
    return nullptr;
}

}  // namespace net

// ---- the callbacks of Http.run (Native.nativeHttpHead / nativeHttpBody) ----
extern "C" {

JNIEXPORT jboolean JNICALL Java_com_scacelith_game_Native_nativeHttpHead(JNIEnv* env, jclass, jlong ctx, jint status,
                                                                         jobjectArray headers) {
    auto* x = reinterpret_cast<net::Exchange*>(intptr_t(ctx));
    net::HttpHead head;
    head.status = int(status);
    const jsize n = headers ? env->GetArrayLength(headers) : 0;
    for (jsize i = 0; i + 1 < n; i += 2) {
        jstring k = static_cast<jstring>(env->GetObjectArrayElement(headers, i));
        jstring v = static_cast<jstring>(env->GetObjectArrayElement(headers, i + 1));
        head.headers.emplace_back(net::jstr(env, k), net::jstr(env, v));
        env->DeleteLocalRef(k);
        env->DeleteLocalRef(v);
    }
    x->resp->status = head.status;
    x->resp->retryAfter = head.get("retry-after");
    return (*x->onHead)(head) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_scacelith_game_Native_nativeHttpBody(JNIEnv* env, jclass, jlong ctx, jbyteArray buf, jint n) {
    auto* x = reinterpret_cast<net::Exchange*>(intptr_t(ctx));
    if (n <= 0) return JNI_TRUE;
    if (x->maxBytes && x->received + size_t(n) > x->maxBytes) {
        x->tooLarge = true;
        return JNI_FALSE;
    }
    x->received += size_t(n);
    jbyte* p = env->GetByteArrayElements(buf, nullptr);
    if (!p) return JNI_FALSE;
    const bool keep = (*x->onBody)(reinterpret_cast<const char*>(p), size_t(n));
    env->ReleaseByteArrayElements(buf, p, JNI_ABORT);
    return keep ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"

#endif  // __ANDROID__

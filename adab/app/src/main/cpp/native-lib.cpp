#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <string>

#include "dab/scan.h"

namespace {

std::atomic_bool g_cancelScan{false};

struct JniScanContext {
    JNIEnv* env;
    jobject callback;
    jmethodID onProgress;
    jmethodID onLock;
    jmethodID onService;
    jmethodID onComplete;
    jmethodID onError;
};

jstring newString(JNIEnv* env, const char* value) {
    return env->NewStringUTF(value != nullptr ? value : "");
}

int scanCancelRequested(void*) {
    return g_cancelScan.load() ? 1 : 0;
}

int scanCallback(const dab_scan_event_t* event, void* user) {
    auto* context = static_cast<JniScanContext*>(user);
    JNIEnv* env;

    if (event == nullptr || context == nullptr || context->env == nullptr || context->callback == nullptr) {
        return -1;
    }

    env = context->env;
    switch (event->type) {
        case DAB_SCAN_EVENT_PROGRESS: {
            jstring channel = newString(env, event->channel);
            env->CallVoidMethod(context->callback,
                                context->onProgress,
                                static_cast<jint>(event->index),
                                static_cast<jint>(event->total),
                                channel,
                                static_cast<jlong>(event->frequency_hz));
            env->DeleteLocalRef(channel);
            break;
        }
        case DAB_SCAN_EVENT_LOCK: {
            jstring channel = newString(env, event->channel);
            env->CallVoidMethod(context->callback,
                                context->onLock,
                                static_cast<jint>(event->index),
                                static_cast<jint>(event->total),
                                channel,
                                static_cast<jlong>(event->frequency_hz),
                                static_cast<jint>(event->service_count),
                                static_cast<jint>(event->label_count));
            env->DeleteLocalRef(channel);
            break;
        }
        case DAB_SCAN_EVENT_SERVICE: {
            jstring channel = newString(env, event->channel);
            jstring label = newString(env, event->label);
            env->CallVoidMethod(context->callback,
                                context->onService,
                                channel,
                                static_cast<jlong>(event->frequency_hz),
                                static_cast<jint>(event->service_id),
                                label);
            env->DeleteLocalRef(label);
            env->DeleteLocalRef(channel);
            break;
        }
        case DAB_SCAN_EVENT_COMPLETE:
            env->CallVoidMethod(context->callback,
                                context->onComplete,
                                static_cast<jint>(event->service_count));
            break;
        case DAB_SCAN_EVENT_ERROR: {
            jstring message = newString(env, event->message);
            env->CallVoidMethod(context->callback, context->onError, message);
            env->DeleteLocalRef(message);
            break;
        }
        default:
            break;
    }

    if (env->ExceptionCheck()) {
        __android_log_write(ANDROID_LOG_ERROR, "adab", "scan callback threw an exception");
        return -1;
    }

    return scanCancelRequested(nullptr);
}

bool initScanContext(JNIEnv* env, jobject callback, JniScanContext* context) {
    jclass callbackClass;

    if (env == nullptr || callback == nullptr || context == nullptr) {
        return false;
    }

    callbackClass = env->GetObjectClass(callback);
    if (callbackClass == nullptr) {
        return false;
    }

    context->env = env;
    context->callback = callback;
    context->onProgress = env->GetMethodID(callbackClass, "onScanProgress", "(IILjava/lang/String;J)V");
    context->onLock = env->GetMethodID(callbackClass, "onScanLock", "(IILjava/lang/String;JII)V");
    context->onService = env->GetMethodID(callbackClass, "onScanService", "(Ljava/lang/String;JILjava/lang/String;)V");
    context->onComplete = env->GetMethodID(callbackClass, "onScanComplete", "(I)V");
    context->onError = env->GetMethodID(callbackClass, "onScanError", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(callbackClass);

    return context->onProgress != nullptr &&
           context->onLock != nullptr &&
           context->onService != nullptr &&
           context->onComplete != nullptr &&
           context->onError != nullptr;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_site_valron_adab_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "DAB native layer ready";
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_site_valron_adab_MainActivity_startRtlsdrScan(
        JNIEnv* env,
        jobject /* this */,
        jobject callback,
        jint perFrequencySeconds) {
    dab_scan_config_t config;
    JniScanContext context{};

    if (!initScanContext(env, callback, &context)) {
        return -1;
    }

    g_cancelScan.store(false);
    dab_scan_default_config(&config);
    config.driver = "rtlsdr";
    config.per_frequency_seconds = perFrequencySeconds > 0 ? static_cast<uint32_t>(perFrequencySeconds) : 4u;
    config.agc_enabled = 1;
    config.gain_db = 40;
    config.use_real_device = 1;
    config.cancel = scanCancelRequested;

    return static_cast<jint>(dab_scan_band_iii(&config, scanCallback, &context));
}

extern "C" JNIEXPORT void JNICALL
Java_site_valron_adab_MainActivity_cancelScan(
        JNIEnv* /* env */,
        jobject /* this */) {
    g_cancelScan.store(true);
}

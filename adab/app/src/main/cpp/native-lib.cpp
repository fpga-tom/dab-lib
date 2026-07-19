#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <dlfcn.h>
#include <string>

#include "dab/receive.h"
#include "dab/scan.h"

namespace {

std::atomic_bool g_cancelScan{false};
std::atomic_bool g_cancelPlayback{false};

struct JniScanContext {
    JNIEnv* env;
    jobject callback;
    jmethodID onProgress;
    jmethodID onLock;
    jmethodID onService;
    jmethodID onComplete;
    jmethodID onError;
};

struct JniPlaybackContext {
    JNIEnv* env;
    jobject callback;
    jmethodID onStatus;
    jmethodID onFormat;
    jmethodID onAudio;
    jmethodID onError;
};

jstring newString(JNIEnv* env, const char* value) {
    return env->NewStringUTF(value != nullptr ? value : "");
}

int scanCancelRequested(void*) {
    return g_cancelScan.load() ? 1 : 0;
}

int playbackCancelRequested(void*) {
    return g_cancelPlayback.load() ? 1 : 0;
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

jbyteArray newByteArray(JNIEnv* env, const uint8_t* data, uint32_t length) {
    jbyteArray array = env->NewByteArray(static_cast<jsize>(length));
    if (array == nullptr || data == nullptr || length == 0u) {
        return array;
    }

    env->SetByteArrayRegion(array, 0, static_cast<jsize>(length),
                            reinterpret_cast<const jbyte*>(data));
    return array;
}

int playbackCallback(const dab_receive_event_t* event, void* user) {
    auto* context = static_cast<JniPlaybackContext*>(user);
    JNIEnv* env;

    if (event == nullptr || context == nullptr || context->env == nullptr || context->callback == nullptr) {
        return -1;
    }

    env = context->env;
    switch (event->type) {
        case DAB_RECEIVE_EVENT_STATUS: {
            jstring message = newString(env, event->message);
            env->CallVoidMethod(context->callback, context->onStatus, message);
            env->DeleteLocalRef(message);
            break;
        }
        case DAB_RECEIVE_EVENT_AUDIO_FORMAT: {
            jbyteArray asc = newByteArray(env, event->asc, event->asc_length);
            env->CallVoidMethod(context->callback,
                                context->onFormat,
                                static_cast<jint>(event->sample_rate),
                                static_cast<jint>(event->channels),
                                asc);
            env->DeleteLocalRef(asc);
            break;
        }
        case DAB_RECEIVE_EVENT_AUDIO_AU: {
            jbyteArray au = newByteArray(env, event->audio_data, event->audio_length);
            env->CallVoidMethod(context->callback, context->onAudio, au);
            env->DeleteLocalRef(au);
            break;
        }
        case DAB_RECEIVE_EVENT_ERROR: {
            jstring message = newString(env, event->message);
            env->CallVoidMethod(context->callback, context->onError, message);
            env->DeleteLocalRef(message);
            break;
        }
        default:
            break;
    }

    if (env->ExceptionCheck()) {
        __android_log_write(ANDROID_LOG_ERROR, "adab", "playback callback threw an exception");
        return -1;
    }

    return playbackCancelRequested(nullptr);
}

bool initPlaybackContext(JNIEnv* env, jobject callback, JniPlaybackContext* context) {
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
    context->onStatus = env->GetMethodID(callbackClass, "onPlaybackStatus", "(Ljava/lang/String;)V");
    context->onFormat = env->GetMethodID(callbackClass, "onPlaybackFormat", "(II[B)V");
    context->onAudio = env->GetMethodID(callbackClass, "onPlaybackAudio", "([B)V");
    context->onError = env->GetMethodID(callbackClass, "onPlaybackError", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(callbackClass);

    return context->onStatus != nullptr &&
           context->onFormat != nullptr &&
           context->onAudio != nullptr &&
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
        jint perFrequencySeconds,
        jint usbFileDescriptor) {
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
    config.android_usb_fd = usbFileDescriptor;
    config.cancel = scanCancelRequested;

    return static_cast<jint>(dab_scan_band_iii(&config, scanCallback, &context));
}

extern "C" JNIEXPORT void JNICALL
Java_site_valron_adab_MainActivity_cancelScan(
        JNIEnv* /* env */,
        jobject /* this */) {
    g_cancelScan.store(true);
}

extern "C" JNIEXPORT jint JNICALL
Java_site_valron_adab_MainActivity_startRtlsdrPlayback(
        JNIEnv* env,
        jobject /* this */,
        jobject callback,
        jlong frequencyHz,
        jint serviceId,
        jint usbFileDescriptor) {
    dab_receive_config_t config;
    JniPlaybackContext context{};

    if (!initPlaybackContext(env, callback, &context)) {
        return -1;
    }

    g_cancelPlayback.store(false);
    dab_receive_default_config(&config);
    config.driver = "rtlsdr";
    config.frequency_hz = static_cast<uint64_t>(frequencyHz);
    config.service_id = static_cast<uint32_t>(serviceId);
    config.agc_enabled = 1;
    config.gain_db = 40;
    config.use_real_device = 1;
    config.android_usb_fd = usbFileDescriptor;
    config.cancel = playbackCancelRequested;

    return static_cast<jint>(dab_receive_service(&config, playbackCallback, &context));
}

extern "C" JNIEXPORT jint JNICALL
Java_site_valron_adab_MainActivity_preflightRtlsdrFrequency(
        JNIEnv* env,
        jobject /* this */,
        jobject callback,
        jstring channelName,
        jlong frequencyHz,
        jint usbFileDescriptor) {
    dab_scan_config_t config;
    JniScanContext context{};
    const char* channel = nullptr;
    jint rc;

    if (!initScanContext(env, callback, &context)) {
        return -1;
    }

    if (channelName != nullptr) {
        channel = env->GetStringUTFChars(channelName, nullptr);
    }

    g_cancelScan.store(false);
    dab_scan_default_config(&config);
    config.driver = "rtlsdr";
    config.per_frequency_seconds = 5u;
    config.agc_enabled = 1;
    config.gain_db = 40;
    config.use_real_device = 1;
    config.android_usb_fd = usbFileDescriptor;
    config.cancel = scanCancelRequested;

    rc = static_cast<jint>(dab_scan_frequency_hz(&config,
                                                 channel != nullptr ? channel : "selected",
                                                 static_cast<uint64_t>(frequencyHz),
                                                 scanCallback,
                                                 &context));

    if (channel != nullptr) {
        env->ReleaseStringUTFChars(channelName, channel);
    }
    return rc;
}

extern "C" JNIEXPORT void JNICALL
Java_site_valron_adab_MainActivity_stopPlayback(
        JNIEnv* /* env */,
        jobject /* this */) {
    g_cancelPlayback.store(true);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_site_valron_adab_MainActivity_isRtlsdrLibraryAvailable(
        JNIEnv* /* env */,
        jobject /* this */) {
    void* handle = dlopen("librtlsdr.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "adab", "librtlsdr.so not loadable: %s", dlerror());
        return JNI_FALSE;
    }
    dlclose(handle);
    return JNI_TRUE;
}

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 ducttape3
#include <jni.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include "policy.hpp"
#include "zygisk.hpp"

namespace {

constexpr const char *kVaultClass =
    "com/samsung/android/service/vaultkeeper/VaultKeeperManager";

bool clear_exception(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

void sleep_seconds(unsigned seconds) {
    timespec remaining{static_cast<time_t>(seconds), 0};
    while (nanosleep(&remaining, &remaining) != 0) {}
}

bool boot_completed() {
    char value[PROP_VALUE_MAX]{};
    __system_property_get("sys.boot_completed", value);
    return value[0] == '1' && value[1] == '\0';
}

// Every JNI reference here belongs to the worker's local frame. A fresh manager
// is acquired on each retry because its constructor captures the Binder service.
struct JniVault {
    JNIEnv *env;
    jobject manager = nullptr;
    jmethodID read_method = nullptr;
    jmethodID write_method = nullptr;

    bool initialize() {
        jclass cls = env->FindClass(kVaultClass);
        if (clear_exception(env) || !cls) return false;
        jmethodID get_instance = env->GetStaticMethodID(
            cls, "getInstance",
            "(Ljava/lang/String;)Lcom/samsung/android/service/vaultkeeper/VaultKeeperManager;");
        if (clear_exception(env) || !get_instance) return false;
        read_method = env->GetMethodID(cls, "read", "(I)[B");
        if (clear_exception(env) || !read_method) return false;
        write_method = env->GetMethodID(cls, "write", "(I[B)I");
        if (clear_exception(env) || !write_method) return false;
        jstring name = env->NewStringUTF("DMC");
        if (clear_exception(env) || !name) return false;
        manager = env->CallStaticObjectMethod(cls, get_instance, name);
        return !clear_exception(env) && manager;
    }

    static bool read(void *context, dmc::Record &record) {
        auto &self = *static_cast<JniVault *>(context);
        auto data = static_cast<jbyteArray>(
            self.env->CallObjectMethod(self.manager, self.read_method, jint{1}));
        if (clear_exception(self.env) || !data) return false;
        const jsize length = self.env->GetArrayLength(data);
        record.size = static_cast<size_t>(length);
        if (record.size == dmc::kRecordSize) {
            self.env->GetByteArrayRegion(data, 0, length,
                                       reinterpret_cast<jbyte *>(record.data));
        }
        self.env->DeleteLocalRef(data);
        return !clear_exception(self.env);
    }

    static int write(void *context, const dmc::Record &record) {
        auto &self = *static_cast<JniVault *>(context);
        if (!dmc::supported(record)) return -102;
        jbyteArray data = self.env->NewByteArray(dmc::kRecordSize);
        if (clear_exception(self.env) || !data) return -103;
        self.env->SetByteArrayRegion(data, 0, dmc::kRecordSize,
                                    reinterpret_cast<const jbyte *>(record.data));
        if (clear_exception(self.env)) {
            self.env->DeleteLocalRef(data);
            return -103;
        }
        const jint code = self.env->CallIntMethod(
            self.manager, self.write_method, jint{1}, data);
        self.env->DeleteLocalRef(data);
        return clear_exception(self.env) ? -103 : code;
    }
};

void *worker(void *argument) {
    auto *vm = static_cast<JavaVM *>(argument);
    pthread_setname_np(pthread_self(), "backdownload");

    // DmcService resets the AT byte in PHASE_BOOT_COMPLETED. Wait for a normal
    // Android boot and let that initialization finish before touching the vault.
    unsigned wait = 0;
    while (!boot_completed() && wait < 600) {
        sleep_seconds(2);
        wait += 2;
    }
    if (!boot_completed()) return nullptr;
    sleep_seconds(10);

    JNIEnv *env = nullptr;
    JavaVMAttachArgs attach{JNI_VERSION_1_6, const_cast<char *>("backdownload"), nullptr};
    if (vm->AttachCurrentThreadAsDaemon(&env, &attach) != JNI_OK || !env) {
        return nullptr;
    }

    // Only this short startup window is monitored. Ordinary credential and
    // Maintenance Mode setters preserve byte 2, so continuous polling is needless.
    for (unsigned attempt = 0; attempt < 36; ++attempt) {
        if (env->PushLocalFrame(16) != JNI_OK) {
            clear_exception(env);
            break;
        }
        JniVault access{env};
        dmc::Outcome outcome{dmc::Result::ReadFailed};
        if (access.initialize()) {
            const dmc::Vault vault{&access, JniVault::read, JniVault::write};
            outcome = dmc::authorize_at(vault);
        }
        env->PopLocalFrame(nullptr);

        if (outcome.result == dmc::Result::UnsupportedRecord) {
            vm->DetachCurrentThread();
            return nullptr;
        }
        if (attempt + 1 < 36) sleep_seconds(5);
    }
    vm->DetachCurrentThread();
    return nullptr;
}

class BackDownloadModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        if (env->GetJavaVM(&vm_) != JNI_OK) vm_ = nullptr;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *) override {
        // No callbacks, hooks, or background work are retained in app processes.
        api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postServerSpecialize(const zygisk::ServerSpecializeArgs *) override {
        if (!vm_ || getuid() != 1000) return;
        pthread_t thread;
        const int error = pthread_create(&thread, nullptr, worker, vm_);
        if (error != 0) return;
        pthread_detach(thread);
    }

private:
    zygisk::Api *api_ = nullptr;
    JavaVM *vm_ = nullptr;
};

} // namespace

REGISTER_ZYGISK_MODULE(BackDownloadModule)

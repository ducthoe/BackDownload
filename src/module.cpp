// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 ducttape3
#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "policy.hpp"
#include "status.hpp"
#include "zygisk.hpp"

namespace {

constexpr const char *kVaultClass =
    "com/samsung/android/service/vaultkeeper/VaultKeeperManager";

void status_service() {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return;
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return;
    }
    char *args[] = {const_cast<char *>("/system/bin/sh"),
        const_cast<char *>("/data/adb/modules/dmc_at_zygisk/service.sh"), nullptr};
    pid_t child;
    const int error = posix_spawn(&child, args[0], &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error == 0) {
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
}
void status_companion([[maybe_unused]] int client) {
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mutex);
    const char *paths[] = {status::kPolicyFile, status::kRequestFile};
    for (const char *path : paths) {
        const int file = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat metadata{};
        if (file >= 0 && fstat(file, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
            fchown(file, 1000, 1000) == 0 && fchmod(file, 0600) == 0) {
            ftruncate(file, 0);
        }
        if (file >= 0) close(file);
    }
    pthread_mutex_unlock(&mutex);
    // Also start the monitor when Android is soft-restarted after installation.
    // Its boot/PID lock handles a monitor already started by the root manager.
    status_service();
}

struct WorkerArgs {
    JavaVM *vm = nullptr;
};

bool clear_exception(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

struct Requests {
    int events = -1;
    Requests() {
        events = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (events >= 0 && inotify_add_watch(events, status::kRequestFile,
                IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF) < 0) {
            close(events);
            events = -1;
        }
    }
    ~Requests() { if (events >= 0) close(events); }
    void wait(unsigned milliseconds) const {
        pollfd event{events, POLLIN, 0};
        poll(&event, events >= 0 ? 1 : 0,
             static_cast<int>(events >= 0 ? milliseconds : 1000));
        if (events >= 0) {
            char buffer[512];
            while (read(events, buffer, sizeof(buffer)) > 0) {}
        }
    }
};

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
    const auto &args = *static_cast<WorkerArgs *>(argument);
    auto *vm = args.vm;
    status::Writer writer;
    Requests requests;
    char token[status::kTokenSize]{};
    pthread_setname_np(pthread_self(), "backdownload");

    // DmcService resets the AT byte in PHASE_BOOT_COMPLETED. Wait for a normal
    // Android boot and let that initialization finish before touching the vault.
    const auto boot_deadline = status::uptime() + 600;
    while (!boot_completed() && status::uptime() < boot_deadline) {
        status::request_token(status::kRequestFile, token);
        if (strcmp(token, "stop") == 0) return nullptr;
        writer.report(token, {"starting", {}});
        requests.wait(2000);
    }
    if (!boot_completed()) return nullptr;
    const auto settle_deadline = status::uptime() + 10;
    while (status::uptime() < settle_deadline) {
        status::request_token(status::kRequestFile, token);
        if (strcmp(token, "stop") == 0) return nullptr;
        writer.report(token, {"starting", {}});
        requests.wait(1000);
    }

    JNIEnv *env = nullptr;
    JavaVMAttachArgs attach{JNI_VERSION_1_6, const_cast<char *>("backdownload"), nullptr};
    if (vm->AttachCurrentThreadAsDaemon(&env, &attach) != JNI_OK || !env) {
        writer.report(token, {"read_failed", {}});
        return nullptr;
    }

    // Startup retries retain the original patch window. Later reads and Action
    // requests inspect the policy without extending that write window.
    unsigned attempts = 0;
    auto next_patch = status::uptime();
    auto next_check = next_patch;
    char last_token[status::kTokenSize]{};
    for (;;) {
        status::request_token(status::kRequestFile, token);
        if (strcmp(token, "stop") == 0) break;
        const auto now = status::uptime();
        const bool patch = attempts < 36 && now >= next_patch;
        if (patch || now >= next_check || strcmp(token, last_token) != 0) {
            status::Sample sample;
            if (env->PushLocalFrame(16) == JNI_OK) {
                JniVault access{env};
                if (access.initialize()) {
                    const dmc::Vault vault{&access, JniVault::read, JniVault::write};
                    if (patch && dmc::authorize_at(vault).result == dmc::Result::UnsupportedRecord) {
                        attempts = 36;
                    }
                    sample = status::Sample::inspect(vault);
                }
                env->PopLocalFrame(nullptr);
            } else {
                clear_exception(env);
            }
            writer.report(token, sample);
            strcpy(last_token, token);
            next_check = status::uptime() + 30;
            if (patch && attempts < 36) {
                ++attempts;
                next_patch = status::uptime() + 5;
            }
        }
        const auto deadline = attempts < 36 && next_patch < next_check ? next_patch : next_check;
        const auto current = status::uptime();
        requests.wait(deadline > current ? static_cast<unsigned>((deadline - current) * 1000) : 1);
    }
    vm->DetachCurrentThread();
    return nullptr;
}

class BackDownloadModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        if (env->GetJavaVM(&worker_args_.vm) != JNI_OK) worker_args_.vm = nullptr;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *) override {
        // No callbacks, hooks, or background work are retained in app processes.
        api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        const int companion = api_->connectCompanion();
        if (companion >= 0) {
            const unsigned char start = 0;
            send(companion, &start, 1, MSG_NOSIGNAL);
            close(companion);
        }
    }

    void postServerSpecialize(const zygisk::ServerSpecializeArgs *) override {
        if (!worker_args_.vm || getuid() != 1000) return;
        pthread_t thread;
        const int error = pthread_create(&thread, nullptr, worker, &worker_args_);
        if (error != 0) return;
        pthread_detach(thread);
    }

private:
    zygisk::Api *api_ = nullptr;
    WorkerArgs worker_args_;
};

} // namespace

REGISTER_ZYGISK_MODULE(BackDownloadModule)
REGISTER_ZYGISK_COMPANION(status_companion)

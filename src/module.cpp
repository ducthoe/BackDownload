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
#include "zygisk.hpp"

namespace {

constexpr const char *kVaultClass =
    "com/samsung/android/service/vaultkeeper/VaultKeeperManager";
constexpr const char *kStatusFile = "/data/system/backdownload.status";

void update_status(bool patched) {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return;
    if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return;
    }
    char *args[] = {const_cast<char *>("/system/bin/sh"),
                   const_cast<char *>("/data/adb/modules/dmc_at_zygisk/update-status.sh"),
                   const_cast<char *>(patched ? "1" : "0"), nullptr};
    pid_t child;
    const int error = posix_spawn(&child, args[0], &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error == 0) {
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
}

void status_companion([[maybe_unused]] int client) {
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static unsigned generation = 0;
    pthread_mutex_lock(&mutex);
    const unsigned current = ++generation;
    const int file = open(kStatusFile, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat metadata{};
    if (file < 0 || fstat(file, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        fchown(file, 1000, 1000) != 0 || fchmod(file, 0600) != 0 || ftruncate(file, 1) != 0) {
        if (file >= 0) close(file);
        pthread_mutex_unlock(&mutex);
        return;
    }
    const unsigned char pending = 0;
    if (pwrite(file, &pending, 1, 0) != 1) {
        close(file);
        pthread_mutex_unlock(&mutex);
        return;
    }
    const int events = inotify_init1(IN_CLOEXEC);
    if (events < 0 || inotify_add_watch(events, kStatusFile, IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF) < 0) {
        if (events >= 0) close(events);
        close(file);
        pthread_mutex_unlock(&mutex);
        return;
    }
    update_status(false);
    pthread_mutex_unlock(&mutex);

    unsigned char previous = 0;
    for (unsigned wait = 0; wait < 120; ++wait) {
        pollfd event{events, POLLIN, 0};
        const int ready = poll(&event, 1, 10000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        pthread_mutex_lock(&mutex);
        if (current != generation) {
            pthread_mutex_unlock(&mutex);
            break;
        }
        unsigned char status = 0;
        if (ready > 0) {
            char buffer[512];
            if (read(events, buffer, sizeof(buffer)) <= 0 || pread(file, &status, 1, 0) != 1 || status > 3) {
                pthread_mutex_unlock(&mutex);
                break;
            }
            const unsigned char patched = status & 1;
            if (patched != previous) update_status(patched == 1);
            previous = patched;
        }
        pthread_mutex_unlock(&mutex);
        if (status >= 2) break;
    }
    close(events);
    close(file);
}

struct WorkerArgs {
    JavaVM *vm = nullptr;
};

struct StatusWriter {
    unsigned char verified = 0;
    ~StatusWriter() { write(verified | 2); }
    static void write(unsigned char status) {
        const int fd = open(kStatusFile, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            while (pwrite(fd, &status, 1, 0) < 0 && errno == EINTR) {}
            close(fd);
        }
    }
    void report(dmc::Result result) {
        verified = result == dmc::Result::AlreadyAuthorized ||
                   result == dmc::Result::WrittenAndVerified;
        write(verified);
    }
};

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
    const auto &args = *static_cast<WorkerArgs *>(argument);
    auto *vm = args.vm;
    StatusWriter status;
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

        status.report(outcome.result);

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

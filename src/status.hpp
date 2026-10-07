// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 ducttape3
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "policy.hpp"

namespace status {

constexpr const char *kPolicyFile = "/data/system/backdownload.status";
constexpr const char *kRequestFile = "/data/system/backdownload.request";
constexpr size_t kPacketSize = 256;
constexpr size_t kTokenSize = 64;

inline unsigned long long uptime() {
    timespec now{};
    return clock_gettime(CLOCK_BOOTTIME, &now) == 0
        ? static_cast<unsigned long long>(now.tv_sec) : 0;
}

inline bool read_text(const char *path, char *buffer, size_t size) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat metadata{};
    const bool regular = fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode);
    ssize_t length = -1;
    if (regular) {
        do { length = read(fd, buffer, size - 1); } while (length < 0 && errno == EINTR);
    }
    close(fd);
    if (length <= 0) return false;
    buffer[length] = '\0';
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return true;
}

inline void request_token(const char *path, char (&token)[kTokenSize]) {
    strcpy(token, "0");
    char input[kTokenSize]{};
    if (!read_text(path, input, sizeof(input))) return;
    if (strcmp(input, "stop") == 0) {
        strcpy(token, input);
        return;
    }
    if (strlen(input) != 36 || strspn(input, "0123456789abcdef-") != 36) return;
    strcpy(token, input);
}

struct Sample {
    const char *state = "read_failed";
    dmc::Record record{};

    static Sample inspect(const dmc::Vault &vault) {
        Sample result;
        if (vault.read(vault.context, result.record)) {
            result.state = dmc::supported(result.record) ? "valid" : "unsupported";
        }
        return result;
    }
};

struct Writer {
    const char *path = kPolicyFile;
    char boot_id[kTokenSize]{};

    Writer() { read_text("/proc/sys/kernel/random/boot_id", boot_id, sizeof(boot_id)); }

    bool report(const char *token, const Sample &sample) const {
        if (strlen(boot_id) != 36) return false;
        const bool valid = strcmp(sample.state, "valid") == 0 && dmc::supported(sample.record);
        char packet[kPacketSize]{};
        const int length = snprintf(packet, sizeof(packet), "BD1 %s %d %s %llu %s %d %d %d\n",
            boot_id, getpid(), token, uptime(), sample.state,
            valid ? sample.record.data[0] : -1,
            valid ? sample.record.data[1] : -1,
            valid ? sample.record.data[2] : -1);
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(packet)) return false;
        const int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        struct stat metadata{};
        ssize_t written = -1;
        if (fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode)) {
            do { written = pwrite(fd, packet, sizeof(packet), 0); }
            while (written < 0 && errno == EINTR);
        }
        close(fd);
        return written == sizeof(packet);
    }
};

} // namespace status

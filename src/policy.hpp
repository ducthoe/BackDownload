// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 ducttape3
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace dmc {

constexpr size_t kRecordSize = 32;
constexpr size_t kAtByte = 2;

struct Record {
    uint8_t data[kRecordSize]{};
    size_t size = 0;
};

struct Vault {
    void *context;
    bool (*read)(void *, Record &);
    int (*write)(void *, const Record &);
};

enum class Result {
    AlreadyAuthorized,
    WrittenAndVerified,
    ReadFailed,
    UnsupportedRecord,
    WriteFailed,
    VerificationFailed,
};

struct Outcome {
    Result result;
    int write_code = 0;
};

inline bool supported(const Record &record) {
    return record.size == kRecordSize && record.data[0] <= 1 &&
           record.data[1] <= 1 && record.data[kAtByte] <= 1;
}

// Only the AT authorization byte is changed. Never construct an empty policy
// when the existing vault cannot be read, and never write an unknown layout.
inline Outcome authorize_at(const Vault &vault) {
    Record record;
    if (!vault.read(vault.context, record)) return {Result::ReadFailed};
    if (!supported(record)) return {Result::UnsupportedRecord};
    if (record.data[kAtByte] == 1) {
        return {Result::AlreadyAuthorized};
    }

    record.data[kAtByte] = 1;
    const int code = vault.write(vault.context, record);
    if (code != 0) return {Result::WriteFailed, code};

    Record verified;
    if (!vault.read(vault.context, verified) || !supported(verified) ||
        verified.data[kAtByte] != 1) {
        return {Result::VerificationFailed};
    }
    // Screen-lock/Maintenance Mode may legitimately change during read-back.
    return {Result::WrittenAndVerified};
}

} // namespace dmc

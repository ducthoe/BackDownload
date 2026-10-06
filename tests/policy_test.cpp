// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 ducttape3
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "policy.hpp"

struct FakeVault {
    dmc::Record record;
    bool readable = true;
    bool retain_write = true;
    bool fail_readback = false;
    bool change_lock_on_readback = false;
    int write_code = 0;
    unsigned reads = 0;
    unsigned writes = 0;

    FakeVault() {
        record.size = dmc::kRecordSize;
        for (size_t i = 3; i < record.size; ++i) record.data[i] = uint8_t(i * 7);
    }

    dmc::Vault api() { return {this, read, write}; }

    static bool read(void *context, dmc::Record &out) {
        auto &self = *static_cast<FakeVault *>(context);
        ++self.reads;
        if (!self.readable || (self.fail_readback && self.writes)) return false;
        if (self.change_lock_on_readback && self.writes) self.record.data[0] ^= 1;
        out = self.record;
        return true;
    }

    static int write(void *context, const dmc::Record &in) {
        auto &self = *static_cast<FakeVault *>(context);
        ++self.writes;
        if (self.write_code == 0 && self.retain_write) self.record = in;
        return self.write_code;
    }
};

int main() {
    for (uint8_t screen = 0; screen <= 1; ++screen) {
        for (uint8_t maintenance = 0; maintenance <= 1; ++maintenance) {
            for (uint8_t at = 0; at <= 1; ++at) {
                FakeVault vault;
                vault.record.data[0] = screen;
                vault.record.data[1] = maintenance;
                vault.record.data[2] = at;
                auto original = vault.record;
                auto result = dmc::authorize_at(vault.api());
                assert(result.result == (at ? dmc::Result::AlreadyAuthorized
                                           : dmc::Result::WrittenAndVerified));
                original.data[2] = 1;
                assert(memcmp(original.data, vault.record.data, 32) == 0);
                assert(vault.writes == (at ? 0u : 1u));
                const unsigned writes = vault.writes;
                assert(dmc::authorize_at(vault.api()).result == dmc::Result::AlreadyAuthorized);
                assert(vault.writes == writes);
            }
        }
    }

    const size_t invalid_sizes[] = {0, 3, 31, 33};
    for (size_t size : invalid_sizes) {
        FakeVault vault;
        vault.record.size = size;
        assert(dmc::authorize_at(vault.api()).result == dmc::Result::UnsupportedRecord);
        assert(vault.writes == 0);
    }
    for (size_t index = 0; index < 3; ++index) {
        FakeVault vault;
        vault.record.data[index] = 255;
        assert(dmc::authorize_at(vault.api()).result == dmc::Result::UnsupportedRecord);
        assert(vault.writes == 0);
    }

    FakeVault unavailable;
    unavailable.readable = false;
    assert(dmc::authorize_at(unavailable.api()).result == dmc::Result::ReadFailed);
    assert(unavailable.writes == 0);

    FakeVault refused;
    refused.write_code = -106;
    auto failed = dmc::authorize_at(refused.api());
    assert(failed.result == dmc::Result::WriteFailed && failed.write_code == -106);
    assert(refused.record.data[2] == 0);
    refused.write_code = 0;
    assert(dmc::authorize_at(refused.api()).result == dmc::Result::WrittenAndVerified);

    FakeVault dropped;
    dropped.retain_write = false;
    assert(dmc::authorize_at(dropped.api()).result == dmc::Result::VerificationFailed);

    FakeVault failed_readback;
    failed_readback.fail_readback = true;
    assert(dmc::authorize_at(failed_readback.api()).result == dmc::Result::VerificationFailed);

    FakeVault changed;
    changed.change_lock_on_readback = true;
    assert(dmc::authorize_at(changed.api()).result == dmc::Result::WrittenAndVerified);

    // Samsung's boot initializer resets AT, while later setPolicy updates keep it.
    FakeVault boot;
    boot.record.data[0] = 1;
    assert(dmc::authorize_at(boot.api()).result == dmc::Result::WrittenAndVerified);
    boot.record.data[0] = 0;
    boot.record.data[1] = 1;
    assert(dmc::authorize_at(boot.api()).result == dmc::Result::AlreadyAuthorized);
    boot.record.data[0] = 1;
    boot.record.data[1] = 0;
    boot.record.data[2] = 0;
    assert(dmc::authorize_at(boot.api()).result == dmc::Result::WrittenAndVerified);

    puts("PASS: authorization, byte preservation, unsupported layouts, errors, retries, read-back, boot reset");
}

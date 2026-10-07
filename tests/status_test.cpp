// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 ducttape3
#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include "status.hpp"

struct Fixture {
    dmc::Record record{};
    bool readable = true;
    static bool read(void *context, dmc::Record &record) {
        const auto &fixture = *static_cast<Fixture *>(context);
        record = fixture.record;
        return fixture.readable;
    }
    dmc::Vault api() { return {this, read, nullptr}; }
};

int main() {
    Fixture fixture;
    fixture.record.size = dmc::kRecordSize;
    for (unsigned flags = 0; flags < 8; ++flags) {
        for (unsigned i = 0; i < 3; ++i) fixture.record.data[i] = (flags >> i) & 1;
        const auto sample = status::Sample::inspect(fixture.api());
        assert(strcmp(sample.state, "valid") == 0);
        assert(memcmp(sample.record.data, fixture.record.data, 32) == 0);
    }
    fixture.readable = false;
    assert(strcmp(status::Sample::inspect(fixture.api()).state, "read_failed") == 0);
    fixture.readable = true;
    fixture.record.size = 3;
    assert(strcmp(status::Sample::inspect(fixture.api()).state, "unsupported") == 0);
    fixture.record.size = 32;
    fixture.record.data[2] = 2;
    assert(strcmp(status::Sample::inspect(fixture.api()).state, "unsupported") == 0);

    char path[PATH_MAX];
    const char *temp = getenv("TMPDIR");
    snprintf(path, sizeof(path), "%s/backdownload-status-XXXXXX", temp ? temp : "/tmp");
    const int file = mkstemp(path);
    assert(file >= 0);
    status::Writer writer;
    writer.path = path;
    fixture.record.data[0] = 1;
    fixture.record.data[1] = 0;
    fixture.record.data[2] = 1;
    const char *token = "11111111-2222-3333-4444-555555555555";
    assert(writer.report(token, status::Sample::inspect(fixture.api())));
    char packet[status::kPacketSize]{};
    assert(pread(file, packet, sizeof(packet), 0) == sizeof(packet));
    assert(strstr(packet, token) != nullptr);
    assert(strstr(packet, " valid 1 0 1\n") != nullptr);
    fixture.readable = false;
    assert(writer.report(token, status::Sample::inspect(fixture.api())));
    assert(pread(file, packet, sizeof(packet), 0) == sizeof(packet));
    assert(strstr(packet, " read_failed -1 -1 -1\n") != nullptr);
    assert(strstr(packet, " valid ") == nullptr);

    assert(ftruncate(file, 0) == 0);
    assert(pwrite(file, token, strlen(token), 0) == static_cast<ssize_t>(strlen(token)));
    char query[status::kTokenSize]{};
    status::request_token(path, query);
    assert(strcmp(query, token) == 0);
    assert(ftruncate(file, 0) == 0);
    assert(pwrite(file, "bad request", 11, 0) == 11);
    status::request_token(path, query);
    assert(strcmp(query, "0") == 0);
    assert(ftruncate(file, 0) == 0);
    assert(pwrite(file, "stop\n", 5, 0) == 5);
    status::request_token(path, query);
    assert(strcmp(query, "stop") == 0);
    close(file);
    unlink(path);
    puts("PASS: policy snapshots, read failures, unsupported layouts, native status packets, request tokens");
}

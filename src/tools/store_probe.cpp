// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>

#include <cstdio>

#include "signal_store.h"

using namespace cutefish::messages;

int main()
{
    const char *path = "/tmp/signal-store-probe.db";
    ::unlink(path);

    SignalStore store;
    std::string error;
    if (!store.open(path, error)) {
        std::fprintf(stderr, "open failed: %s\n", error.c_str());
        return 1;
    }

    Thread thread;
    thread.key = "signal:+15550001";
    thread.name = "Avery";
    thread.address = "+15550001";
    thread.snippet = "Hello";
    thread.lastTimestamp = "2026-11-22T10:01:00";
    thread.unread = 1;
    store.saveThread(thread);

    Message incoming;
    incoming.id = 1000;
    incoming.handle = "+15550001";
    incoming.body = "Hi";
    incoming.timestamp = "2026-11-22T10:00:00";
    store.saveMessage(thread.key, incoming);

    Message outgoing;
    outgoing.id = 1001;
    outgoing.handle = "+15550001";
    outgoing.body = "Hello";
    outgoing.timestamp = "2026-11-22T10:01:00";
    outgoing.outgoing = true;
    outgoing.read = false;
    store.saveMessage(thread.key, outgoing);

    store.setMessageRead(thread.key, outgoing.id);
    const std::vector<Thread> loaded = store.load();
    if (loaded.size() != 1 || loaded[0].messages.size() != 2) {
        std::fprintf(stderr, "load mismatch: %zu threads\n", loaded.size());
        return 1;
    }
    const Thread &back = loaded[0];
    if (back.name != "Avery" || back.messages[0].body != "Hi"
        || !back.messages[1].outgoing || !back.messages[1].read) {
        std::fprintf(stderr, "round trip mismatch\n");
        return 1;
    }
    std::printf("store ok: %s, %zu messages, unread %d\n",
                back.name.c_str(), back.messages.size(), back.unread);
    return 0;
}

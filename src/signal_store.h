// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_SIGNAL_STORE_H_
#define CUTEFISH_MESSAGES_SIGNAL_STORE_H_

#include <string>
#include <vector>

#include "messages_client.h"

struct sqlite3;

namespace cutefish::messages {

// The conversations signal-cli does not keep: threads and messages, so the
// window has history the moment it opens. Plain SQLite; one file.
class SignalStore
{
public:
    SignalStore() = default;
    ~SignalStore();

    bool open(const std::string &path, std::string &error);

    std::vector<Thread> load();

    void saveThread(const Thread &thread);
    void saveMessage(const std::string &threadKey, const Message &message);
    void setMessageRead(const std::string &threadKey, int64_t messageId);
    void markThreadRead(const std::string &threadKey);

private:
    SignalStore(const SignalStore &) = delete;
    SignalStore &operator=(const SignalStore &) = delete;

    void exec(const char *sql);

    sqlite3 *m_db = nullptr;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_SIGNAL_STORE_H_

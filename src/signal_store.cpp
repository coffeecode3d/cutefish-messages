// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "signal_store.h"

#include <sqlite3.h>

#include <map>

namespace cutefish::messages {

namespace {

std::string text(sqlite3_stmt *statement, int column)
{
    const unsigned char *value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char *>(value) : std::string();
}

} // namespace

SignalStore::~SignalStore()
{
    if (m_db)
        sqlite3_close(m_db);
}

void SignalStore::exec(const char *sql)
{
    if (m_db)
        sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr);
}

bool SignalStore::open(const std::string &path, std::string &error)
{
    if (sqlite3_open(path.c_str(), &m_db) != SQLITE_OK) {
        error = sqlite3_errmsg(m_db);
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(m_db, 2000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("CREATE TABLE IF NOT EXISTS threads ("
         "key TEXT PRIMARY KEY,"
         "name TEXT NOT NULL DEFAULT '',"
         "address TEXT NOT NULL DEFAULT '',"
         "is_group INTEGER NOT NULL DEFAULT 0,"
         "snippet TEXT NOT NULL DEFAULT '',"
         "last_timestamp TEXT NOT NULL DEFAULT '',"
         "unread INTEGER NOT NULL DEFAULT 0)");
    exec("CREATE TABLE IF NOT EXISTS messages ("
         "thread_key TEXT NOT NULL,"
         "id INTEGER NOT NULL,"
         "handle TEXT NOT NULL DEFAULT '',"
         "body TEXT NOT NULL DEFAULT '',"
         "timestamp TEXT NOT NULL DEFAULT '',"
         "sender TEXT NOT NULL DEFAULT '',"
         "outgoing INTEGER NOT NULL DEFAULT 0,"
         "read INTEGER NOT NULL DEFAULT 1,"
         "attachment TEXT NOT NULL DEFAULT '',"
         "PRIMARY KEY (thread_key, id))");
    return true;
}

std::vector<Thread> SignalStore::load()
{
    std::vector<Thread> threads;
    if (!m_db)
        return threads;

    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "SELECT key, name, address, is_group, snippet, last_timestamp, unread"
                           " FROM threads ORDER BY last_timestamp DESC",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return threads;
    }
    std::map<std::string, size_t> index;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        Thread thread;
        thread.key = text(statement, 0);
        thread.name = text(statement, 1);
        thread.address = text(statement, 2);
        thread.group = sqlite3_column_int(statement, 3) != 0;
        thread.snippet = text(statement, 4);
        thread.lastTimestamp = text(statement, 5);
        thread.unread = sqlite3_column_int(statement, 6);
        thread.service = "signal";
        thread.serviceName = "Signal";
        index[thread.key] = threads.size();
        threads.push_back(std::move(thread));
    }
    sqlite3_finalize(statement);

    if (sqlite3_prepare_v2(m_db,
                           "SELECT thread_key, id, handle, body, timestamp, sender, outgoing, read,"
                           " attachment FROM messages ORDER BY thread_key, id",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return threads;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto found = index.find(text(statement, 0));
        if (found == index.end())
            continue;
        Message message;
        message.id = sqlite3_column_int64(statement, 1);
        message.handle = text(statement, 2);
        message.body = text(statement, 3);
        message.timestamp = text(statement, 4);
        message.sender = text(statement, 5);
        message.outgoing = sqlite3_column_int(statement, 6) != 0;
        message.read = sqlite3_column_int(statement, 7) != 0;
        message.attachmentPath = text(statement, 8);
        threads[found->second].messages.push_back(std::move(message));
    }
    sqlite3_finalize(statement);
    return threads;
}

void SignalStore::saveThread(const Thread &thread)
{
    if (!m_db)
        return;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "INSERT INTO threads (key, name, address, is_group, snippet,"
                           " last_timestamp, unread) VALUES (?, ?, ?, ?, ?, ?, ?)"
                           " ON CONFLICT(key) DO UPDATE SET name=excluded.name,"
                           " address=excluded.address, is_group=excluded.is_group,"
                           " snippet=excluded.snippet, last_timestamp=excluded.last_timestamp,"
                           " unread=excluded.unread",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(statement, 1, thread.key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, thread.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, thread.address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, thread.group ? 1 : 0);
    sqlite3_bind_text(statement, 5, thread.snippet.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, thread.lastTimestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 7, thread.unread);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

void SignalStore::saveMessage(const std::string &threadKey, const Message &message)
{
    if (!m_db)
        return;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "INSERT INTO messages (thread_key, id, handle, body, timestamp,"
                           " sender, outgoing, read, attachment) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
                           " ON CONFLICT(thread_key, id) DO UPDATE SET body=excluded.body,"
                           " sender=excluded.sender, outgoing=excluded.outgoing,"
                           " read=excluded.read, attachment=excluded.attachment",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(statement, 1, threadKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, message.id);
    sqlite3_bind_text(statement, 3, message.handle.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, message.body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, message.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, message.sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 7, message.outgoing ? 1 : 0);
    sqlite3_bind_int(statement, 8, message.read ? 1 : 0);
    sqlite3_bind_text(statement, 9, message.attachmentPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

void SignalStore::setMessageRead(const std::string &threadKey, int64_t messageId)
{
    if (!m_db)
        return;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "UPDATE messages SET read=1 WHERE thread_key=? AND id=?",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(statement, 1, threadKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, messageId);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

void SignalStore::markThreadRead(const std::string &threadKey)
{
    if (!m_db)
        return;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(m_db, "UPDATE threads SET unread=0 WHERE key=?",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(statement, 1, threadKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

} // namespace cutefish::messages

// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_TELEGRAM_BACKEND_H_
#define CUTEFISH_MESSAGES_TELEGRAM_BACKEND_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "backend.h"
#include "base/event_loop.h"
#include "base/json.h"
#include "base/timer.h"

namespace cutefish::messages {

// Telegram through TDLib's JSON client. TDLib keeps its own database, so the
// backend does not need a store of its own.
class TelegramBackend : public Backend
{
public:
    enum class AuthKind {
        None,
        Phone,
        Code,
        Password,
        Other,
    };

    explicit TelegramBackend(base::EventLoop *loop);
    ~TelegramBackend() override;

    ServiceAccount account() const override;
    bool ready() const override { return m_authorized; }
    std::string statusLine() const override;
    bool start(std::string &error) override;
    void refresh() override;

    std::vector<Thread> threads() const override;
    void loadThread(const Thread &thread) override;
    void markRead(const Thread &thread) override;
    bool send(const Thread &thread, const std::string &body, std::string &error) override;
    bool sendFile(const Thread &thread, const std::string &path, const std::string &caption,
                  std::string &error) override;
    void sendTyping(const Thread &thread, bool typing) override;
    void fetchAttachment(int64_t messageId) override { (void)messageId; }

    // Signing in is a few questions: phone number, code, then two-step
    // password if the account has one.
    bool authenticating() const { return m_authKind != AuthKind::None; }
    bool needsAuth() const
    {
        return m_authKind == AuthKind::Phone || m_authKind == AuthKind::Code
                || m_authKind == AuthKind::Password;
    }
    AuthKind authKind() const { return m_authKind; }
    std::string authPrompt() const { return m_authPrompt; }
    std::string authDetail() const { return m_authDetail; }
    void submitAuth(const std::string &value);

    base::Signal<> authChanged;

private:
    struct PendingFile
    {
        int64_t chatId = 0;
        int64_t messageId = 0;
        bool avatar = false;
    };

    void sendJson(const base::json::Value &request);
    void poll();
    void handle(const base::json::Value &object);
    void handleAuthState(const base::json::Value &state);
    void setAuth(AuthKind kind, std::string prompt, std::string detail);
    void loadChats();
    void syncChat(const base::json::Value &chat);
    void requestUser(int64_t userId);
    void requestAvatar(int64_t chatId, const base::json::Value &photo);
    void loadHistory(int64_t chatId);
    void addMessage(const base::json::Value &message);
    Message messageFrom(const base::json::Value &message, bool group) const;
    std::string senderName(const base::json::Value &sender) const;
    std::string bodyOf(const base::json::Value &content, bool &image,
                       base::json::Value &file, std::string &fileName) const;
    void requestFile(const base::json::Value &file, int64_t chatId, int64_t messageId,
                     bool avatar);
    void viewChat(int64_t chatId);
    Thread *threadFor(int64_t chatId);
    std::string timestampOf(int64_t seconds) const;

    base::EventLoop *m_loop;
    void *m_client = nullptr;
    base::Timer m_poll{base::EventLoop::current()};
    bool m_authorized = false;
    bool m_chatsRequested = false;
    bool m_parametersSet = false;
    AuthKind m_authKind = AuthKind::None;
    std::string m_authPrompt;
    std::string m_authDetail;
    std::string m_apiId;
    std::string m_apiHash;
    std::string m_dataDir;
    int64_t m_openChat = 0;
    std::map<int64_t, Thread> m_threads;
    std::set<int64_t> m_loadedChats;
    std::set<int64_t> m_usersRequested;
    std::map<int64_t, std::string> m_userNames;
    std::map<int64_t, int64_t> m_privateChatByUser;
    std::map<int64_t, int64_t> m_lastReadOutbox;
    std::map<int64_t, PendingFile> m_pendingFiles;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_TELEGRAM_BACKEND_H_

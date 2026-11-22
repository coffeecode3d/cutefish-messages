// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_SMS_BACKEND_H_
#define CUTEFISH_MESSAGES_SMS_BACKEND_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "backend.h"
#include "phone/link.h"

namespace cutefish::messages {

// The phone's SMS and MMS, over our own KDE Connect protocol link.
class SmsBackend : public Backend
{
public:
    explicit SmsBackend(base::EventLoop *loop);
    ~SmsBackend() override;

    ServiceAccount account() const override;
    bool ready() const override;
    std::string statusLine() const override;
    bool start(std::string &error) override;
    void refresh() override;

    std::vector<Thread> threads() const override { return m_threads; }
    void loadThread(const Thread &thread) override;
    void markRead(const Thread &thread) override;
    void setUnread(const Thread &thread, bool unread) override;
    bool send(const Thread &thread, const std::string &body, std::string &error) override;
    bool sendFile(const Thread &thread, const std::string &path, const std::string &caption,
                  std::string &error) override;
    void fetchAttachment(int64_t messageId) override;

private:
    void rebuild();
    std::string nameOf(const std::string &address) const;
    std::string groupName(const std::vector<std::string> &addresses) const;

    phone::PhoneLink m_phone;
    std::vector<phone::Conversation> m_conversations;
    std::map<int64_t, std::vector<phone::SmsMessage>> m_messages;
    std::vector<Thread> m_threads;
    std::set<int64_t> m_requested;
    std::set<int64_t> m_read;
    // A burst of summaries rebuilds the list once.
    base::Timer m_coalesce;
    std::vector<base::Subscription> m_subscriptions;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_SMS_BACKEND_H_

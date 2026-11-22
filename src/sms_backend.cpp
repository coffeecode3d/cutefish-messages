// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "sms_backend.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace cutefish::messages {

namespace {

std::string prettyAddress(const std::string &address)
{
    std::string digits;
    for (const char c : address) {
        if (std::isdigit(static_cast<unsigned char>(c)))
            digits.push_back(c);
    }
    if (address.size() > 1 && address[0] == '+' && digits.size() == 11 && digits[0] == '1')
        return "+1 (" + digits.substr(1, 3) + ") " + digits.substr(4, 3) + "-" + digits.substr(7);
    if (digits.size() == 10 && digits == address)
        return "(" + digits.substr(0, 3) + ") " + digits.substr(3, 3) + "-" + digits.substr(6);
    return address;
}

// The daemon's timestamps are text; the phone's are epoch milliseconds.
std::string timestampOf(int64_t milliseconds)
{
    const std::time_t seconds = std::time_t(milliseconds / 1000);
    std::tm local {};
    localtime_r(&seconds, &local);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}

int64_t nowMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

Message messageOf(const phone::SmsMessage &sms)
{
    Message message;
    message.id = sms.id;
    message.handle = sms.address;
    message.body = sms.body;
    message.timestamp = timestampOf(sms.date);
    message.outgoing = sms.type == 2;
    message.read = sms.read;
    const bool playable = sms.fullPath.ends_with(".mp4") || sms.fullPath.ends_with(".mp3")
            || sms.fullPath.ends_with(".bin");
    if (!sms.fullPath.empty() && !playable) {
        message.attachmentPath = sms.fullPath;
    } else {
        for (const phone::SmsAttachment &attachment : sms.attachments) {
            if (!attachment.thumbnailPath.empty()) {
                message.attachmentPath = attachment.thumbnailPath;
                break;
            }
        }
    }
    return message;
}

} // namespace

SmsBackend::SmsBackend(base::EventLoop *loop)
    : m_phone(loop)
    , m_coalesce(loop)
{
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(std::chrono::milliseconds(60));
    m_coalesce.onTimeout = [this] {
        rebuild();
        // A thread we already hold that the phone says is newer is fetched again.
        for (const phone::Conversation &conversation : m_conversations) {
            const auto held = m_messages.find(conversation.threadId);
            if (held == m_messages.end() || held->second.empty())
                continue;
            if (conversation.date <= held->second.back().date)
                continue;
            if (m_requested.count(conversation.threadId))
                continue;
            m_requested.insert(conversation.threadId);
            m_phone.requestThread(conversation.threadId);
        }
        threadsChanged();
    };
    m_subscriptions.push_back(m_phone.stateChanged.connect([this] { statusChanged(); }));
    m_subscriptions.push_back(m_phone.conversationsReceived.connect(
            [this](std::vector<phone::Conversation> list) {
        m_conversations = std::move(list);
        m_coalesce.start();
    }));
    m_subscriptions.push_back(m_phone.threadReceived.connect(
            [this](int64_t threadId, std::vector<phone::SmsMessage> messages) {
        m_requested.erase(threadId);
        std::sort(messages.begin(), messages.end(),
                  [](const phone::SmsMessage &left, const phone::SmsMessage &right) {
                      return left.date < right.date;
                  });
        m_messages[threadId] = std::move(messages);
        rebuild();
        threadsChanged();
    }));
    m_subscriptions.push_back(m_phone.attachmentReady.connect(
            [this](int64_t messageId, std::string path) {
        for (auto &[threadId, messages] : m_messages) {
            for (phone::SmsMessage &sms : messages) {
                if (sms.id == messageId)
                    sms.fullPath = path;
            }
        }
        for (Thread &thread : m_threads) {
            for (Message &message : thread.messages) {
                if (message.id == messageId)
                    message.attachmentPath = path;
            }
        }
        attachmentReady(messageId, std::move(path));
    }));
    m_subscriptions.push_back(m_phone.contactsChanged.connect([this] {
        rebuild();
        threadsChanged();
    }));
}

SmsBackend::~SmsBackend() = default;

ServiceAccount SmsBackend::account() const
{
    return {"sms", "Phone", "SMS & MMS", true};
}

bool SmsBackend::ready() const
{
    return m_phone.isConnected();
}

std::string SmsBackend::statusLine() const
{
    if (!m_phone.isConnected())
        return "Waiting for your phone. Keep KDE Connect open.";
    std::string text = m_phone.phoneName().empty() ? "Phone" : m_phone.phoneName();
    if (m_phone.battery() >= 0)
        text += " \u00b7 " + std::to_string(m_phone.battery()) + "%";
    return text;
}

bool SmsBackend::start(std::string &error)
{
    return m_phone.start(error);
}

void SmsBackend::refresh()
{
    if (m_phone.isConnected())
        m_phone.requestConversations();
}

void SmsBackend::rebuild()
{
    std::sort(m_conversations.begin(), m_conversations.end(),
              [](const phone::Conversation &left, const phone::Conversation &right) {
                  return left.date > right.date;
              });
    m_threads.clear();
    for (const phone::Conversation &conversation : m_conversations) {
        Thread thread;
        thread.key = "sms:" + std::to_string(conversation.threadId);
        thread.service = "sms";
        thread.serviceName = "Phone";
        thread.threadId = conversation.threadId;
        thread.address = conversation.address;
        thread.addresses = conversation.addresses;
        if (thread.addresses.empty() && !conversation.address.empty())
            thread.addresses.push_back(conversation.address);
        thread.group = thread.addresses.size() > 1;
        thread.name = thread.group ? groupName(thread.addresses) : nameOf(conversation.address);
        thread.snippet = conversation.snippet;
        thread.lastTimestamp = timestampOf(conversation.date);
        thread.unread = conversation.read || m_read.count(conversation.threadId) ? 0 : 1;
        const auto held = m_messages.find(conversation.threadId);
        if (held != m_messages.end()) {
            for (const phone::SmsMessage &sms : held->second) {
                Message message = messageOf(sms);
                if (thread.group && !message.outgoing)
                    message.sender = nameOf(message.handle);
                thread.messages.push_back(std::move(message));
            }
        }
        m_threads.push_back(std::move(thread));
    }
}

std::string SmsBackend::nameOf(const std::string &address) const
{
    const std::string known = m_phone.nameFor(address);
    return known.empty() ? prettyAddress(address) : known;
}

std::string SmsBackend::groupName(const std::vector<std::string> &addresses) const
{
    std::vector<std::string> names;
    for (const std::string &address : addresses) {
        const std::string name = nameOf(address);
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }
    if (names.empty())
        return "Group conversation";
    if (names.size() <= 2) {
        std::string joined = names[0];
        for (size_t i = 1; i < names.size(); ++i)
            joined += " & " + names[i];
        return joined;
    }
    return names[0] + ", " + names[1] + " +" + std::to_string(names.size() - 2);
}

void SmsBackend::loadThread(const Thread &thread)
{
    if (thread.threadId == 0 || m_requested.count(thread.threadId))
        return;
    const auto held = m_messages.find(thread.threadId);
    if (held != m_messages.end() && !held->second.empty())
        return;
    m_requested.insert(thread.threadId);
    m_phone.requestThread(thread.threadId);
}

void SmsBackend::markRead(const Thread &thread)
{
    if (thread.threadId == 0)
        return;
    m_read.insert(thread.threadId);
    for (phone::Conversation &conversation : m_conversations) {
        if (conversation.threadId == thread.threadId)
            conversation.read = true;
    }
    for (Thread &known : m_threads) {
        if (known.threadId == thread.threadId)
            known.unread = 0;
    }
}

bool SmsBackend::send(const Thread &thread, const std::string &body, std::string &error)
{
    std::vector<std::string> recipients = thread.addresses;
    if (recipients.empty())
        recipients.push_back(thread.address);
    if (!m_phone.sendSms(recipients, body, error))
        return false;

    phone::SmsMessage sent;
    sent.threadId = thread.threadId;
    sent.address = thread.address;
    sent.addresses = recipients;
    sent.body = body;
    sent.date = nowMilliseconds();
    sent.type = 2;
    sent.read = true;
    m_messages[thread.threadId].push_back(sent);
    m_phone.noteSent(thread.threadId, body, sent.date);
    for (phone::Conversation &conversation : m_conversations) {
        if (conversation.threadId == thread.threadId) {
            conversation.date = sent.date;
            conversation.snippet = body;
            conversation.type = 2;
        }
    }
    rebuild();
    threadsChanged();
    return true;
}

void SmsBackend::fetchAttachment(int64_t messageId)
{
    m_phone.requestAttachmentNow(messageId);
}

} // namespace cutefish::messages

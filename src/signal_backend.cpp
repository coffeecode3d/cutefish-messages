// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "signal_backend.h"

#include "avatar_cache.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace cutefish::messages {

namespace {

std::string homePath(const char *environment, const char *fallback)
{
    if (const char *value = std::getenv(environment))
        return value;
    if (const char *home = std::getenv("HOME"))
        return std::string(home) + fallback;
    return "/tmp";
}

std::string timestampOf(int64_t milliseconds)
{
    const std::time_t seconds = std::time_t(milliseconds / 1000);
    std::tm local {};
    localtime_r(&seconds, &local);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}

std::string extensionFor(const std::string &contentType, const std::string &filename)
{
    const auto dot = filename.rfind('.');
    if (dot != std::string::npos && dot + 1 < filename.size() && filename.size() - dot <= 6)
        return filename.substr(dot);
    if (contentType.find("jpeg") != std::string::npos || contentType.find("jpg") != std::string::npos)
        return ".jpg";
    if (contentType.find("png") != std::string::npos)
        return ".png";
    if (contentType.find("webp") != std::string::npos)
        return ".webp";
    if (contentType.find("gif") != std::string::npos)
        return ".gif";
    if (contentType.find("mp4") != std::string::npos || contentType.find("video") != std::string::npos)
        return ".mp4";
    return ".bin";
}

std::string stringOr(const base::json::Value &value, std::string fallback = {})
{
    return value.isString() ? value.toString() : std::move(fallback);
}

} // namespace

SignalBackend::SignalBackend(base::EventLoop *loop)
    : m_loop(loop)
    , m_rpc(loop)
{
    const char *override = std::getenv("CUTEFISH_SIGNAL_CLI");
    m_binary = override ? override : "signal-cli";
    const std::string root = homePath("XDG_DATA_HOME", "/.local/share") + "/cutefish/messages";
    m_dataDir = root + "/signal";
    m_storePath = root + "/signal.db";
    m_socket = homePath("XDG_RUNTIME_DIR", "/tmp") + "/cutefish-messages-signal.sock";

    std::error_code code;
    std::filesystem::create_directories(root, code);
    std::string storeError;
    m_store.open(m_storePath, storeError);

    m_subscriptions.push_back(m_rpc.notification.connect(
            [this](const base::json::Value &params) { handleNotification(params); }));
}

SignalBackend::~SignalBackend() = default;

ServiceAccount SignalBackend::account() const
{
    return {"signal", "Signal", "Linked device", true};
}

std::string SignalBackend::statusLine() const
{
    return m_linked ? "Signal" : "Signal \u00b7 not linked";
}

bool SignalBackend::ensureDaemon(std::string &error)
{
    if (m_rpc.isRunning())
        return true;
    std::error_code code;
    std::filesystem::create_directories(m_dataDir, code);
    return m_rpc.start(m_binary, m_dataDir, m_socket, error);
}

bool SignalBackend::start(std::string &error)
{
    // History is ours; signal-cli keeps none. Load it before the helper, so
    // the list is there even when no account is linked.
    for (Thread &thread : m_store.load()) {
        for (Message &message : thread.messages) {
            if (message.outgoing)
                message.receipt = message.read ? "Read" : "Sent";
            // An old picture whose bytes never landed is still a message.
            if (message.body.empty() && message.attachmentPath.empty())
                message.body = "Attachment";
        }
        thread.avatarPath = avatarFor(thread.key);
        m_threads[thread.key] = std::move(thread);
    }

    // Only start the helper when there is an account to serve; otherwise the
    // service stays offered in the sheet and the daemon starts on linking.
    std::error_code code;
    if (!std::filesystem::exists(m_dataDir, code)
        || std::filesystem::is_empty(m_dataDir, code)) {
        m_linked = false;
        return true;
    }
    if (!ensureDaemon(error))
        return false;

    m_rpc.call("listAccounts", [this](bool ok, const base::json::Value &result,
                                      const std::string &error) {
        if (!ok) {
            (void)error;
            return;
        }
        if (result.isArray() && result.size() > 0) {
            const base::json::Value &first = result[size_t(0)];
            m_account = stringOr(first["number"]);
            if (m_account.empty())
                m_account = stringOr(first["aci"]);
            m_linked = !m_account.empty();
            if (m_linked) {
                for (auto &[key, thread] : m_threads)
                    requestAvatar(thread);
                loadContacts();
                beginReceive();
                statusChanged();
            }
        }
    });
    return true;
}

void SignalBackend::link(LinkUri onUri, LinkDone done)
{
    std::string error;
    if (!ensureDaemon(error)) {
        done(false, error);
        return;
    }
    m_rpc.call("startLink", [this, onUri, done](bool ok, const base::json::Value &result,
                                                const std::string &error) {
        if (!ok) {
            done(false, error);
            return;
        }
        const std::string uri = stringOr(result["deviceLinkUri"]);
        if (uri.empty()) {
            done(false, "Signal did not return a linking code.");
            return;
        }
        onUri(uri);

        base::json::Value params = base::json::Value::object();
        params.insert("deviceLinkUri", uri);
        params.insert("deviceName", "Cutefish Messages");
        m_rpc.call("finishLink", std::move(params),
                   [this, done](bool linked, const base::json::Value &account,
                                const std::string &error) {
            if (!linked) {
                done(false, error);
                return;
            }
            m_account = stringOr(account["number"]);
            if (m_account.empty())
                m_account = stringOr(account["aci"]);
            m_linked = !m_account.empty();
            if (m_linked) {
                loadContacts();
                beginReceive();
                statusChanged();
            }
            done(m_linked, m_linked ? std::string() : "Signal did not return an account.");
        });
    });
}

void SignalBackend::fetchImage(const std::string &threadKey, int64_t messageId,
                               const std::string &groupId, const std::string &recipient,
                               const base::json::Value &attachments)
{
    if (!attachments.isArray())
        return;
    for (size_t i = 0; i < attachments.size(); ++i) {
        const base::json::Value &attachment = attachments[i];
        const std::string type = stringOr(attachment["contentType"]);
        if (type.rfind("image/", 0) != 0)
            continue;
        const std::string id = stringOr(attachment["id"]);
        if (id.empty())
            return;
        const std::string extension = extensionFor(type, stringOr(attachment["filename"]));

        base::json::Value params = base::json::Value::object();
        params.insert("account", m_account);
        params.insert("id", id);
        if (!groupId.empty())
            params.insert("groupId", groupId);
        else
            params.insert("recipient", recipient);

        m_rpc.call("getAttachment", std::move(params),
                   [this, threadKey, messageId, extension](bool ok,
                                                           const base::json::Value &result,
                                                           const std::string &) {
            if (!ok)
                return;
            const std::string data = stringOr(result["data"]);
            if (data.empty())
                return;
            const std::string path = saveAttachmentBase64(threadKey, messageId, 0, extension, data);
            if (path.empty())
                return;
            const auto found = m_threads.find(threadKey);
            if (found == m_threads.end())
                return;
            for (Message &message : found->second.messages) {
                if (message.id != messageId)
                    continue;
                message.attachmentPath = path;
                m_store.saveMessage(threadKey, message);
                attachmentReady(messageId, path);
                return;
            }
        });
        return;
    }
}

void SignalBackend::requestAvatar(Thread &thread)
{
    if (thread.avatarPath.empty())
        thread.avatarPath = avatarFor(thread.key);
    if (!thread.avatarPath.empty())
        return;
    // Ask once a session; a missing picture is not worth a poll every minute.
    if (!m_avatarRequested.insert(thread.key).second)
        return;
    base::json::Value params = base::json::Value::object();
    params.insert("account", m_account);
    if (thread.group)
        params.insert("groupId", thread.address);
    else
        params.insert("contact", thread.address);
    const std::string key = thread.key;
    m_rpc.call("getAvatar", std::move(params),
               [this, key](bool ok, const base::json::Value &result, const std::string &) {
        if (!ok)
            return;
        const std::string data = stringOr(result["data"]);
        if (data.empty())
            return;
        const std::string path = saveAvatarBase64(key, data);
        if (path.empty())
            return;
        const auto found = m_threads.find(key);
        if (found == m_threads.end())
            return;
        found->second.avatarPath = path;
        m_store.saveThread(found->second);
        threadsChanged();
    });
}

void SignalBackend::loadContacts()
{
    base::json::Value params = base::json::Value::object();
    params.insert("account", m_account);
    params.insert("allRecipients", true);
    m_rpc.call("listContacts", std::move(params),
               [this](bool ok, const base::json::Value &contacts, const std::string &) {
        if (!ok || !contacts.isArray())
            return;
        for (size_t i = 0; i < contacts.size(); ++i) {
            const base::json::Value &contact = contacts[i];
            const std::string number = stringOr(contact["number"]);
            if (number.empty())
                continue;
            // Signal adds a nickname of our own, which wins over the name the
            // contact saved and over their profile name.
            std::string name = stringOr(contact["nickName"]);
            if (name.empty()) {
                const std::string given = stringOr(contact["nickGivenName"]);
                const std::string family = stringOr(contact["nickFamilyName"]);
                if (!given.empty())
                    name = family.empty() ? given : given + " " + family;
            }
            if (name.empty())
                name = stringOr(contact["name"]);
            if (name.empty()) {
                const base::json::Value &profile = contact["profile"];
                const std::string given = stringOr(profile["givenName"]);
                const std::string family = stringOr(profile["familyName"]);
                if (!given.empty())
                    name = family.empty() ? given : given + " " + family;
            }
            if (name.empty())
                name = stringOr(contact["profileName"]);
            if (!name.empty())
                m_contactNames[number] = name;
        }
        // A stored thread may still carry the number it was created with.
        bool changed = false;
        for (auto &[key, thread] : m_threads) {
            if (thread.group)
                continue;
            const auto found = m_contactNames.find(thread.address);
            if (found == m_contactNames.end() || thread.name == found->second)
                continue;
            thread.name = found->second;
            m_store.saveThread(thread);
            changed = true;
        }
        if (changed)
            threadsChanged();
    });
}

void SignalBackend::beginReceive()
{
    if (m_receiving)
        return;
    m_receiving = true;
    refresh();
}

void SignalBackend::refresh()
{
    if (!m_linked)
        return;
    // The daemon streams receive notifications by default; asking for groups
    // names the ones that have no message yet.
    base::json::Value groupParams = base::json::Value::object();
    groupParams.insert("account", m_account);
    m_rpc.call("listGroups", std::move(groupParams),
               [this](bool ok, const base::json::Value &groups, const std::string &) {
        if (!ok || !groups.isArray())
            return;
        for (size_t i = 0; i < groups.size(); ++i) {
            const base::json::Value &group = groups[i];
            const std::string id = stringOr(group["id"]);
            if (id.empty() || group["isMember"].toBool(false) == false)
                continue;
            Thread &thread = threadFor("signal:g:" + id, stringOr(group["name"], "Group"), true);
            thread.group = true;
            requestAvatar(thread);
            m_store.saveThread(thread);
        }
        threadsChanged();
    });
}

void SignalBackend::handleNotification(const base::json::Value &params)
{
    if (!m_linked)
        return;
    const base::json::Value &envelope = params["envelope"];
    if (envelope.isObject())
        handleEnvelope(envelope);
}

void SignalBackend::handleEnvelope(const base::json::Value &envelope)
{
    const std::string source = stringOr(envelope["sourceNumber"]);
    const std::string sourceAci = stringOr(envelope["sourceUuid"]);
    const std::string from = source.empty() ? sourceAci : source;
    const std::string name = stringOr(envelope["sourceName"]);
    const int64_t sent = envelope["timestamp"].toInt();

    const base::json::Value &typing = envelope["typingMessage"];
    if (typing.isObject()) {
        const std::string groupId = stringOr(typing["groupId"]);
        const std::string key = groupId.empty() ? "signal:" + from : "signal:g:" + groupId;
        const std::string action = stringOr(typing["action"]);
        typingChanged(key, action == "STARTED" || action == "started");
        return;
    }

    const base::json::Value &data = envelope["dataMessage"];
    if (data.isObject()) {
        std::string body = stringOr(data["message"]);
        const base::json::Value &groupIdValue = data["groupInfo"]["groupId"];
        const std::string groupId = groupIdValue.isString() ? groupIdValue.toString() : std::string();
        const int64_t when = data["timestamp"].toInt(sent);
        std::string key;
        std::string title;
        bool group = false;
        if (!groupId.empty()) {
            key = "signal:g:" + groupId;
            title = stringOr(data["groupInfo"]["groupName"], "Group");
            group = true;
        } else {
            key = "signal:" + from;
            title = name.empty() ? from : name;
        }
        Thread &thread = threadFor(key, title, group);
        if (!group && !name.empty() && m_contactNames.find(from) == m_contactNames.end()
            && thread.name == from) {
            thread.name = name;
            m_store.saveThread(thread);
        }
        requestAvatar(thread);
        if (hasMessage(thread, when))
            return;
        const base::json::Value &attachments = data["attachments"];
        if (body.empty() && attachments.isArray() && attachments.size() > 0
            && stringOr(attachments[0]["contentType"]).rfind("image/", 0) != 0) {
            body = stringOr(attachments[0]["filename"]);
            if (body.empty())
                body = "Attachment";
        }
        Message message;
        message.id = when;
        message.handle = from;
        message.body = body;
        message.timestamp = timestampOf(when);
        if (thread.group && !name.empty())
            message.sender = name;
        thread.messages.push_back(message);
        thread.snippet = body.empty() && attachments.isArray() && attachments.size() > 0
                ? std::string("Photo")
                : body;
        thread.lastTimestamp = message.timestamp;
        thread.unread = key == m_openKey ? 0 : 1;
        m_store.saveThread(thread);
        m_store.saveMessage(thread.key, message);
        fetchImage(thread.key, message.id, groupId, from, attachments);
        threadsChanged();
        return;
    }

    const base::json::Value &receipt = envelope["receiptMessage"];
    if (receipt.isObject() && receipt["isRead"].toBool(false)) {
        const base::json::Value &timestamps = receipt["timestamps"];
        for (auto &[key, thread] : m_threads) {
            for (Message &message : thread.messages) {
                if (!message.outgoing)
                    continue;
                for (size_t i = 0; i < timestamps.size(); ++i) {
                    if (timestamps[i].toInt() != message.id)
                        continue;
                    message.read = true;
                    message.receipt = "Read";
                    m_store.setMessageRead(key, message.id);
                }
            }
        }
        threadsChanged();
        return;
    }

    // A message this account sent from another device.
    const base::json::Value &sentMessage = envelope["syncMessage"]["sentMessage"];
    if (sentMessage.isObject()) {
        const std::string destination = stringOr(sentMessage["destinationNumber"]);
        const std::string groupId = stringOr(sentMessage["groupInfo"]["groupId"]);
        const int64_t when = sentMessage["timestamp"].toInt(sent);
        std::string body = stringOr(sentMessage["message"]);
        const base::json::Value &attachments = sentMessage["attachments"];
        if (body.empty() && attachments.isArray() && attachments.size() > 0
            && stringOr(attachments[0]["contentType"]).rfind("image/", 0) != 0) {
            body = stringOr(attachments[0]["filename"]);
            if (body.empty())
                body = "Attachment";
        }
        std::string key;
        std::string title;
        bool group = false;
        if (!groupId.empty()) {
            key = "signal:g:" + groupId;
            title = stringOr(sentMessage["groupInfo"]["groupName"], "Group");
            group = true;
        } else {
            key = "signal:" + destination;
            title = destination;
        }
        Thread &thread = threadFor(key, title, group);
        requestAvatar(thread);
        if (hasMessage(thread, when))
            return;
        Message message;
        message.id = when;
        message.handle = destination;
        message.body = body;
        message.timestamp = timestampOf(when);
        message.outgoing = true;
        message.read = false;
        message.receipt = "Sent";
        thread.messages.push_back(message);
        thread.snippet = body.empty() && attachments.isArray() && attachments.size() > 0
                ? std::string("Photo")
                : body;
        thread.lastTimestamp = message.timestamp;
        thread.unread = 0;
        m_store.saveThread(thread);
        m_store.saveMessage(thread.key, message);
        fetchImage(thread.key, message.id, groupId, destination, attachments);
        threadsChanged();
    }
}

bool SignalBackend::hasMessage(const Thread &thread, int64_t id) const
{
    for (const Message &message : thread.messages) {
        if (message.id == id)
            return true;
    }
    return false;
}

Thread &SignalBackend::threadFor(const std::string &key, const std::string &name, bool group)
{
    Thread &thread = m_threads[key];
    if (thread.key.empty()) {
        thread.key = key;
        thread.service = "signal";
        thread.serviceName = "Signal";
        thread.group = group;
        thread.name = name.empty() ? key : name;
        thread.address = key.starts_with("signal:g:") ? key.substr(9) : key.substr(7);
        if (!group) {
            const auto known = m_contactNames.find(thread.address);
            if (known != m_contactNames.end())
                thread.name = known->second;
        }
    }
    return thread;
}

void SignalBackend::ensureThreadsSorted()
{
}

std::vector<Thread> SignalBackend::threads() const
{
    std::vector<Thread> list;
    list.reserve(m_threads.size());
    for (const auto &[key, thread] : m_threads) {
        if (!thread.messages.empty())
            list.push_back(thread);
    }
    std::sort(list.begin(), list.end(), [](const Thread &left, const Thread &right) {
        return left.lastTimestamp > right.lastTimestamp;
    });
    return list;
}

void SignalBackend::setUnread(const Thread &thread, bool unread)
{
    const auto found = m_threads.find(thread.key);
    if (found == m_threads.end())
        return;
    found->second.unread = unread ? 1 : 0;
    m_store.saveThread(found->second);
    threadsChanged();
}

bool SignalBackend::removeThread(const Thread &thread)
{
    const auto found = m_threads.find(thread.key);
    if (found == m_threads.end())
        return false;
    m_threads.erase(found);
    m_store.deleteThread(thread.key);
    threadsChanged();
    return true;
}

void SignalBackend::loadThread(const Thread &thread)
{
    m_openKey = thread.key;
}

void SignalBackend::markRead(const Thread &thread)
{
    m_openKey = thread.key;
    const auto found = m_threads.find(thread.key);
    if (found != m_threads.end()) {
        found->second.unread = 0;
        m_store.markThreadRead(thread.key);
    }
    if (!m_linked || thread.group)
        return;
    int64_t newest = 0;
    for (const Message &message : thread.messages) {
        if (!message.outgoing && message.id > newest)
            newest = message.id;
    }
    if (newest == 0)
        return;
    const auto sent = m_receiptSent.find(thread.key);
    if (sent != m_receiptSent.end() && sent->second >= newest)
        return;
    m_receiptSent[thread.key] = newest;
    base::json::Value params = base::json::Value::object();
    params.insert("account", m_account);
    base::json::Value::Array recipients;
    recipients.push_back(base::json::Value(thread.address));
    params.insert("recipient", base::json::Value(std::move(recipients)));
    params.insert("targetTimestamp", newest);
    params.insert("type", "read");
    m_rpc.call("sendReceipt", std::move(params), nullptr);
}

void SignalBackend::sendTyping(const Thread &thread, bool typing)
{
    if (!m_linked)
        return;
    base::json::Value params = base::json::Value::object();
    params.insert("account", m_account);
    if (thread.group) {
        params.insert("groupId", thread.address);
    } else {
        base::json::Value::Array recipients;
        recipients.push_back(base::json::Value(thread.address));
        params.insert("recipient", base::json::Value(std::move(recipients)));
    }
    if (!typing)
        params.insert("stop", true);
    m_rpc.call("sendTyping", std::move(params), nullptr);
}

bool SignalBackend::sendFile(const Thread &thread, const std::string &path,
                             const std::string &caption, std::string &error)
{
    if (!m_linked) {
        error = "Signal is not linked.";
        return false;
    }
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        error = "That file could not be read.";
        return false;
    }

    base::json::Value params = base::json::Value::object();
    params.insert("account", m_account);
    if (thread.group) {
        params.insert("groupId", thread.address);
    } else {
        base::json::Value::Array recipients;
        recipients.push_back(base::json::Value(thread.address));
        params.insert("recipient", base::json::Value(std::move(recipients)));
    }
    if (!caption.empty())
        params.insert("message", caption);
    base::json::Value::Array attachments;
    attachments.push_back(base::json::Value(path));
    params.insert("attachments", base::json::Value(std::move(attachments)));

    std::string filename = std::filesystem::path(path).filename().string();
    std::string extension = std::filesystem::path(path).extension().string();
    for (char &c : extension)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    const bool image = extension == ".jpg" || extension == ".jpeg" || extension == ".png"
            || extension == ".gif" || extension == ".webp";
    const std::string body = caption.empty() ? filename : caption;
    const bool group = thread.group;
    const std::string address = thread.address;

    m_rpc.call("send", std::move(params),
               [this, group, address, body, image, filename, path](
                       bool ok, const base::json::Value &result, const std::string &) {
        if (!ok) {
            statusChanged();
            return;
        }
        const int64_t when = result["timestamp"].toInt(
                int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()));
        const std::string key = group ? "signal:g:" + address : "signal:" + address;
        Thread &target = threadFor(key, address, group);
        Message message;
        message.id = when;
        message.handle = address;
        message.body = body;
        message.timestamp = timestampOf(when);
        message.outgoing = true;
        message.read = false;
        message.receipt = "Sent";
        if (image)
            message.attachmentPath = path;
        else
            message.body = filename;
        target.messages.push_back(message);
        target.snippet = body;
        target.lastTimestamp = message.timestamp;
        target.unread = 0;
        m_store.saveThread(target);
        m_store.saveMessage(target.key, message);
        threadsChanged();
    });
    return true;
}

bool SignalBackend::send(const Thread &thread, const std::string &body, std::string &error)
{
    if (!m_linked) {
        error = "Signal is not linked.";
        return false;
    }
    base::json::Value params = base::json::Value::object();
    params.insert("account", m_account);
    if (thread.group) {
        params.insert("groupId", thread.address);
    } else {
        base::json::Value::Array recipients;
        recipients.push_back(base::json::Value(thread.address));
        params.insert("recipient", base::json::Value(std::move(recipients)));
    }
    params.insert("message", body);

    const bool group = thread.group;
    const std::string address = thread.address;
    m_rpc.call("send", std::move(params),
               [this, group, address, body](bool ok, const base::json::Value &result,
                                            const std::string &error) {
        if (!ok) {
            statusChanged();
            return;
        }
        const int64_t when = result["timestamp"].toInt(
                int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()));
        const std::string key = group ? "signal:g:" + address : "signal:" + address;
        Thread &target = threadFor(key, address, group);
        Message message;
        message.id = when;
        message.handle = address;
        message.body = body;
        message.timestamp = timestampOf(when);
        message.outgoing = true;
        message.read = false;
        message.receipt = "Sent";
        target.messages.push_back(message);
        target.snippet = body;
        target.lastTimestamp = message.timestamp;
        target.unread = 0;
        m_store.saveThread(target);
        m_store.saveMessage(target.key, message);
        threadsChanged();
    });
    return true;
}

} // namespace cutefish::messages

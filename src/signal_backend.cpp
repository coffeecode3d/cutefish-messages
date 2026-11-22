// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "signal_backend.h"

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
    m_dataDir = homePath("XDG_DATA_HOME", "/.local/share") + "/cutefish/messages/signal";
    m_socket = homePath("XDG_RUNTIME_DIR", "/tmp") + "/cutefish-messages-signal.sock";

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
                beginReceive();
                statusChanged();
            }
            done(m_linked, m_linked ? std::string() : "Signal did not return an account.");
        });
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

    const base::json::Value &data = envelope["dataMessage"];
    if (data.isObject()) {
        const std::string body = stringOr(data["message"]);
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
        if (hasMessage(thread, when))
            return;
        Message message;
        message.id = when;
        message.handle = from;
        message.body = body;
        message.timestamp = timestampOf(when);
        if (thread.group && !name.empty())
            message.sender = name;
        thread.messages.push_back(std::move(message));
        thread.snippet = body;
        thread.lastTimestamp = timestampOf(when);
        thread.unread = 1;
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
                    if (timestamps[i].toInt() == message.id)
                        message.read = true;
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
        const std::string body = stringOr(sentMessage["message"]);
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
        if (hasMessage(thread, when))
            return;
        Message message;
        message.id = when;
        message.handle = destination;
        message.body = body;
        message.timestamp = timestampOf(when);
        message.outgoing = true;
        thread.messages.push_back(std::move(message));
        thread.snippet = body;
        thread.lastTimestamp = timestampOf(when);
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

void SignalBackend::markRead(const Thread &thread)
{
    if (!m_linked || thread.group)
        return;
    int64_t newest = 0;
    for (const Message &message : thread.messages) {
        if (!message.outgoing && message.id > newest)
            newest = message.id;
    }
    if (newest == 0)
        return;
    base::json::Value params = base::json::Value::object();
    params.insert("account", m_account);
    base::json::Value::Array recipients;
    recipients.push_back(base::json::Value(thread.address));
    params.insert("recipient", base::json::Value(std::move(recipients)));
    params.insert("targetTimestamp", newest);
    params.insert("type", "read");
    m_rpc.call("sendReceipt", std::move(params), nullptr);
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
        target.messages.push_back(std::move(message));
        target.snippet = body;
        target.lastTimestamp = timestampOf(when);
        threadsChanged();
    });
    return true;
}

} // namespace cutefish::messages

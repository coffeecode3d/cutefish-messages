// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "telegram_backend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#ifdef CUTEFISH_HAVE_TDLIB
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#endif

namespace cutefish::messages {

#ifdef CUTEFISH_HAVE_TDLIB
namespace {

using TdCreate = void *(*)();
using TdSend = void (*)(void *, const char *);
using TdReceive = const char *(*)(void *, double);
using TdExecute = const char *(*)(void *, const char *);
using TdDestroy = void (*)(void *);

void *g_library = nullptr;
TdCreate g_create = nullptr;
TdSend g_send = nullptr;
TdReceive g_receive = nullptr;
TdExecute g_execute = nullptr;
TdDestroy g_destroy = nullptr;

bool loadTdlib(std::string &error)
{
    if (g_create)
        return true;
    std::vector<std::string> paths;
    if (const char *chosen = std::getenv("CUTEFISH_TDLIB"))
        paths.push_back(chosen);
#ifdef CUTEFISH_TDLIB_PATH
    paths.push_back(CUTEFISH_TDLIB_PATH);
#endif
    paths.push_back("libtdjson.so.1.8.38");
    paths.push_back("libtdjson.so");
    for (const std::string &path : paths) {
        // Deep bind: TDLib carries its own SQLCipher, and the system SQLite
        // this program links for the Signal store must not interpose on it.
        g_library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        if (g_library)
            break;
    }
    if (!g_library) {
        error = "TDLib (libtdjson) is not installed.";
        return false;
    }
    g_create = reinterpret_cast<TdCreate>(dlsym(g_library, "td_json_client_create"));
    g_send = reinterpret_cast<TdSend>(dlsym(g_library, "td_json_client_send"));
    g_receive = reinterpret_cast<TdReceive>(dlsym(g_library, "td_json_client_receive"));
    g_execute = reinterpret_cast<TdExecute>(dlsym(g_library, "td_json_client_execute"));
    g_destroy = reinterpret_cast<TdDestroy>(dlsym(g_library, "td_json_client_destroy"));
    if (!g_create || !g_send || !g_receive || !g_execute || !g_destroy) {
        error = "TDLib is missing its JSON interface.";
        return false;
    }
    return true;
}

} // namespace
#endif

namespace {

std::string environment(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : std::string();
}

std::string homePath(const char *variable, const char *fallback)
{
    if (const char *value = std::getenv(variable))
        return value;
    if (const char *home = std::getenv("HOME"))
        return std::string(home) + fallback;
    return "/tmp";
}

std::string trim(std::string text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    return text.substr(start);
}

std::string configValue(const std::string &key)
{
    if (key == "api_id") {
        const std::string value = environment("CUTEFISH_TELEGRAM_API_ID");
        if (!value.empty())
            return value;
    } else if (key == "api_hash") {
        const std::string value = environment("CUTEFISH_TELEGRAM_API_HASH");
        if (!value.empty())
            return value;
    }

    const std::string path =
            homePath("XDG_CONFIG_HOME", "/.config") + "/cutefish/messages/telegram.conf";
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        if (trim(line.substr(0, equals)) == key)
            return trim(line.substr(equals + 1));
    }
    return {};
}

std::string fileNameOf(const std::string &path)
{
    return std::filesystem::path(path).filename().string();
}

std::string lowerExtension(const std::string &path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    for (char &c : extension)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

bool isPicture(const std::string &path)
{
    const std::string extension = lowerExtension(path);
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png"
            || extension == ".gif" || extension == ".webp";
}

} // namespace

TelegramBackend::TelegramBackend(base::EventLoop *loop)
    : m_loop(loop)
{
    m_apiId = configValue("api_id");
    m_apiHash = configValue("api_hash");
    const std::string root = homePath("XDG_DATA_HOME", "/.local/share") + "/cutefish/messages";
    m_dataDir = root + "/telegram";
}

TelegramBackend::~TelegramBackend()
{
    m_poll.stop();
#ifdef CUTEFISH_HAVE_TDLIB
    if (m_client) {
        g_send(m_client, "{\"@type\":\"close\"}");
        for (int i = 0; i < 20; ++i) {
            if (!g_receive(m_client, 0.1))
                continue;
        }
        g_destroy(m_client);
        m_client = nullptr;
    }
#endif
}

bool TelegramBackend::saveCredentials(const std::string &id, const std::string &hash,
                                      std::string &error)
{
    const std::string apiId = trim(id);
    const std::string apiHash = trim(hash);
    if (apiId.empty() || apiHash.empty()
        || !std::all_of(apiId.begin(), apiId.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
        error = "Enter the numeric API id and the API hash.";
        return false;
    }
    const std::string directory =
            homePath("XDG_CONFIG_HOME", "/.config") + "/cutefish/messages";
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    const std::string path = directory + "/telegram.conf";
    {
        std::ofstream file(path, std::ios::trunc);
        if (!file) {
            error = "The credentials could not be saved.";
            return false;
        }
        file << "api_id=" << apiId << "\napi_hash=" << apiHash << "\n";
        if (!file) {
            error = "The credentials could not be saved.";
            return false;
        }
    }
#ifdef __unix__
    std::filesystem::permissions(path, std::filesystem::perms::owner_read
                                         | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
#endif
    return true;
}

ServiceAccount TelegramBackend::account() const
{
    return {"telegram", "Telegram", m_authorized ? "Signed in" : "Not signed in", true};
}

std::string TelegramBackend::statusLine() const
{
    return m_authorized ? "Telegram" : "Telegram \u00b7 not signed in";
}

bool TelegramBackend::start(std::string &error)
{
#ifdef CUTEFISH_HAVE_TDLIB
    if (!loadTdlib(error))
        return false;
    if (m_apiId.empty() || m_apiHash.empty()) {
        error = "Telegram needs api_id and api_hash in "
                "~/.config/cutefish/messages/telegram.conf.";
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(m_dataDir, code);

    m_client = g_create();
    if (!m_client) {
        error = "Telegram could not start.";
        return false;
    }
    g_execute(
            m_client,
            "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":0}");
    m_poll.setInterval(std::chrono::milliseconds(50));
    m_poll.onTimeout = [this] { poll(); };
    m_poll.start();
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "getAuthorizationState");
    sendJson(request);
    return true;
#else
    error = "This build has no Telegram support; install libtdjson and rebuild.";
    return false;
#endif
}

void TelegramBackend::sendJson(const base::json::Value &request)
{
#ifdef CUTEFISH_HAVE_TDLIB
    if (!m_client)
        return;
    const std::string line = request.serialize();
    g_send(m_client, line.c_str());
#else
    (void)request;
#endif
}

void TelegramBackend::poll()
{
#ifdef CUTEFISH_HAVE_TDLIB
    if (!m_client)
        return;
    for (;;) {
        const char *raw = g_receive(m_client, 0.0);
        if (!raw)
            break;
        const auto parsed = base::json::Value::parse(std::string(raw));
        if (parsed && parsed->isObject())
            handle(*parsed);
    }
#endif
}

void TelegramBackend::setAuth(AuthKind kind, std::string prompt, std::string detail)
{
    m_authKind = kind;
    m_authPrompt = std::move(prompt);
    m_authDetail = std::move(detail);
    authChanged();
}

void TelegramBackend::handleAuthState(const base::json::Value &state)
{
    const std::string type = state["@type"].toString();
    if (type == "authorizationStateWaitTdlibParameters") {
        if (m_parametersSet)
            return;
        m_parametersSet = true;
        base::json::Value request = base::json::Value::object();
        request.insert("@type", "setTdlibParameters");
        request.insert("database_directory", m_dataDir + "/db");
        request.insert("files_directory", m_dataDir + "/files");
        request.insert("use_file_database", true);
        request.insert("use_chat_info_database", true);
        request.insert("use_message_database", true);
        request.insert("use_secret_chats", false);
        request.insert("api_id", int64_t(std::strtoll(m_apiId.c_str(), nullptr, 10)));
        request.insert("api_hash", m_apiHash);
        request.insert("system_language_code", "en");
        request.insert("device_model", "Desktop");
        request.insert("system_version", "CutefishOS");
        request.insert("application_version", "0.1");
        request.insert("enable_storage_optimizer", true);
        request.insert("ignore_file_names", false);
        sendJson(request);
    } else if (type == "authorizationStateWaitPhoneNumber") {
        // QR first: scanning skips the code, the same idea as Signal's link.
        if (!m_qrRequested) {
            m_qrRequested = true;
            base::json::Value request = base::json::Value::object();
            request.insert("@type", "requestQrCodeAuthentication");
            request.insert("other_user_ids", base::json::Value::Array());
            request.insert("@extra", "qr");
            sendJson(request);
            setAuth(AuthKind::Other, "Scan the code from Telegram", "");
        } else {
            setAuth(AuthKind::Phone, "Enter your phone number",
                    "Include the country code, for example +1 555 010 0000.");
        }
    } else if (type == "authorizationStateWaitOtherDeviceConfirmation") {
        m_qrLink = state["link"].toString();
        setAuth(AuthKind::Other, "Scan the code from Telegram", "");
    } else if (type == "authorizationStateWaitCode") {
        m_qrLink.clear();
        setAuth(AuthKind::Code, "Enter the code Telegram sent you", "Check Telegram or your SMS.");
    } else if (type == "authorizationStateWaitPassword") {
        m_qrLink.clear();
        setAuth(AuthKind::Password, "Enter your two-step verification password", "");
    } else if (type == "authorizationStateReady") {
        m_authorized = true;
        m_qrLink.clear();
        m_chatsRequested = false;
        setAuth(AuthKind::None, {}, {});
        statusChanged();
        loadChats();
    } else if (type == "authorizationStateClosed") {
        m_authorized = false;
        m_authKind = AuthKind::None;
        authChanged();
        statusChanged();
    } else if (type == "authorizationStateWaitRegistration") {
        setAuth(AuthKind::Other, "Create a Telegram account on your phone first", "");
    } else if (type == "authorizationStateWaitEmailAddress"
               || type == "authorizationStateWaitEmailCode") {
        setAuth(AuthKind::Other, "Confirm the email address in Telegram", "");
    }
}

void TelegramBackend::submitAuth(const std::string &value)
{
    base::json::Value request = base::json::Value::object();
    if (m_authKind == AuthKind::Phone) {
        request.insert("@type", "setAuthenticationPhoneNumber");
        request.insert("phone_number", value);
        request.insert("settings", base::json::Value());
        setAuth(AuthKind::Other, "Signing in\u2026", "");
    } else if (m_authKind == AuthKind::Code) {
        request.insert("@type", "checkAuthenticationCode");
        request.insert("code", value);
        setAuth(AuthKind::Other, "Signing in\u2026", "");
    } else if (m_authKind == AuthKind::Password) {
        request.insert("@type", "checkAuthenticationPassword");
        request.insert("password", value);
        setAuth(AuthKind::Other, "Signing in\u2026", "");
    } else {
        return;
    }
    sendJson(request);
}

void TelegramBackend::handle(const base::json::Value &object)
{
    const std::string type = object["@type"].toString();
    const std::string extra = object["@extra"].toString();

    if (type == "updateAuthorizationState") {
        handleAuthState(object["authorization_state"]);
        return;
    }
    if (type.rfind("authorizationState", 0) == 0) {
        // The answer to getAuthorizationState when it arrives before updates.
        handleAuthState(object);
        return;
    }
    if (type == "updateNewChat") {
        syncChat(object["chat"]);
        threadsChanged();
        return;
    }
    if (type == "updateChatTitle") {
        if (Thread *thread = threadFor(object["chat_id"].toInt()))
            thread->name = object["title"].toString();
        threadsChanged();
        return;
    }
    if (type == "updateChatPhoto") {
        if (Thread *thread = threadFor(object["chat_id"].toInt()))
            requestAvatar(thread->threadId, object["photo"]);
        return;
    }
    if (type == "updateChatLastMessage") {
        const int64_t chatId = object["chat_id"].toInt();
        Thread *thread = threadFor(chatId);
        const base::json::Value &message = object["last_message"];
        if (thread && message.isObject()) {
            bool image = false;
            base::json::Value file;
            std::string fileName;
            const std::string body = bodyOf(message["content"], image, file, fileName);
            thread->snippet = body.empty() ? (image ? "Photo" : fileName) : body;
            thread->lastTimestamp = timestampOf(message["date"].toInt());
        }
        threadsChanged();
        return;
    }
    if (type == "updateChatReadInbox") {
        if (Thread *thread = threadFor(object["chat_id"].toInt()))
            thread->unread = int(object["unread_count"].toInt());
        threadsChanged();
        return;
    }
    if (type == "updateChatReadOutbox") {
        const int64_t chatId = object["chat_id"].toInt();
        const int64_t last = object["last_read_outbox_message_id"].toInt();
        m_lastReadOutbox[chatId] = last;
        if (Thread *thread = threadFor(chatId)) {
            for (Message &message : thread->messages) {
                if (!message.outgoing || message.id > last)
                    continue;
                message.read = true;
                message.receipt = "Read";
            }
        }
        threadsChanged();
        return;
    }
    if (type == "updateNewMessage" || type == "updateMessageSendSucceeded") {
        addMessage(object["message"]);
        return;
    }
    if (type == "updateChatAction" || type == "updateUserTyping") {
        int64_t chatId = object["chat_id"].toInt();
        if (type == "updateUserTyping") {
            const auto found = m_privateChatByUser.find(object["user_id"].toInt());
            if (found != m_privateChatByUser.end())
                chatId = found->second;
        }
        const std::string action = object["action"]["@type"].toString();
        if (Thread *thread = threadFor(chatId)) {
            if (action == "chatActionTyping")
                typingChanged(thread->key, true);
            else if (action == "chatActionCancel")
                typingChanged(thread->key, false);
        }
        return;
    }
    if (type == "updateFile") {
        const base::json::Value &file = object["file"];
        const auto found = m_pendingFiles.find(file["id"].toInt());
        if (found == m_pendingFiles.end())
            return;
        if (!file["local"]["is_downloading_completed"].toBool(false))
            return;
        const std::string path = file["local"]["path"].toString();
        if (path.empty())
            return;
        const PendingFile pending = found->second;
        m_pendingFiles.erase(found);
        if (pending.avatar) {
            if (Thread *thread = threadFor(pending.chatId))
                thread->avatarPath = path;
            threadsChanged();
        } else {
            if (Thread *thread = threadFor(pending.chatId)) {
                for (Message &message : thread->messages) {
                    if (message.id != pending.messageId)
                        continue;
                    message.attachmentPath = path;
                    attachmentReady(message.id, path);
                    break;
                }
            }
        }
        return;
    }
    if (type == "user" && extra.starts_with("user:")) {
        const int64_t userId = object["id"].toInt();
        std::string name = object["first_name"].toString();
        const std::string last = object["last_name"].toString();
        if (!last.empty())
            name = name.empty() ? last : name + " " + last;
        if (name.empty())
            name = object["usernames"]["active_usernames"].toString();
        if (!name.empty())
            m_userNames[userId] = name;
        bool changed = false;
        for (auto &[chatId, thread] : m_threads) {
            for (Message &message : thread.messages) {
                const auto known = m_senderIds.find(message.id);
                if (known == m_senderIds.end() || known->second != userId)
                    continue;
                message.sender = name;
                changed = true;
            }
        }
        if (changed)
            threadsChanged();
        return;
    }
    if (type == "messages" && extra.starts_with("history:")) {
        const int64_t chatId = std::strtoll(extra.c_str() + 8, nullptr, 10);
        Thread *thread = threadFor(chatId);
        if (!thread)
            return;
        const base::json::Value &messages = object["messages"];
        for (size_t i = 0; i < messages.size(); ++i)
            addMessage(messages[i], true);
        std::sort(thread->messages.begin(), thread->messages.end(),
                  [](const Message &left, const Message &right) { return left.id < right.id; });
        std::fprintf(stderr, "telegram: history %zu messages\n", messages.size());
        if (!thread->messages.empty()) {
            const Message &last = thread->messages.back();
            thread->snippet = last.body.empty()
                    ? (last.attachmentPath.empty() ? "Attachment" : "Photo")
                    : last.body;
            thread->lastTimestamp = last.timestamp;
        }
        if (m_openChat == chatId)
            viewChat(chatId);
        threadsChanged();
        return;
    }
    if (type == "chat" && extra.starts_with("chat:")) {
        syncChat(object);
        threadsChanged();
        return;
    }
    if (type == "ok" && extra == "loadchats") {
        loadChatsPage();
        return;
    }
    if (type == "chats" && extra == "chats") {
        const base::json::Value &ids = object["chat_ids"];
        for (size_t i = 0; i < ids.size(); ++i) {
            base::json::Value request = base::json::Value::object();
            request.insert("@type", "getChat");
            request.insert("chat_id", ids[i].toInt());
            request.insert("@extra", "chat:" + std::to_string(ids[i].toInt()));
            sendJson(request);
        }
        return;
    }
    if (type == "error") {
        const std::string message = object["message"].toString();
        std::fprintf(stderr, "telegram: %s\n", message.c_str());
        if (extra == "qr") {
            m_qrRequested = false;
            setAuth(AuthKind::Phone, "Enter your phone number",
                    "Include the country code, for example +1 555 010 0000.");
        }
        if (extra == "loadchats") {
            // 404 means every chat list page was loaded; either way list them.
            requestChats();
        }
        if (extra.starts_with("history:")) {
            const int64_t chatId = std::strtoll(extra.c_str() + 8, nullptr, 10);
            m_loadedChats.erase(chatId);
        }
        return;
    }
}

void TelegramBackend::loadChats()
{
    if (m_chatsRequested || !m_authorized)
        return;
    m_chatsRequested = true;
    m_chatLoadTries = 0;
    // Show whatever is already cached, then load the rest in the background.
    requestChats();
    loadChatsPage();
}

void TelegramBackend::loadChatsPage()
{
    if (++m_chatLoadTries > 20) {
        requestChats();
        return;
    }
    base::json::Value list = base::json::Value::object();
    list.insert("@type", "chatListMain");
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "loadChats");
    request.insert("chat_list", list);
    request.insert("limit", 200);
    request.insert("@extra", "loadchats");
    sendJson(request);
}

void TelegramBackend::requestChats()
{
    base::json::Value list = base::json::Value::object();
    list.insert("@type", "chatListMain");
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "getChats");
    request.insert("chat_list", list);
    request.insert("limit", 200);
    request.insert("@extra", "chats");
    sendJson(request);
}

void TelegramBackend::refresh()
{
    if (m_authorized && m_threads.empty())
        loadChats();
}

void TelegramBackend::syncChat(const base::json::Value &chat)
{
    const int64_t chatId = chat["id"].toInt();
    if (chatId == 0)
        return;
    Thread *thread = threadFor(chatId);
    if (!thread)
        return;
    const std::string title = chat["title"].toString();
    if (!title.empty())
        thread->name = title;
    const std::string type = chat["type"]["@type"].toString();
    thread->group = type == "chatTypeBasicGroup" || type == "chatTypeSupergroup";
    thread->channel = type == "chatTypeSupergroup"
            && chat["type"]["is_channel"].toBool(false);
    thread->unread = int(chat["unread_count"].toInt());
    if (type == "chatTypePrivate" || type == "chatTypeSecret") {
        const int64_t userId = chat["type"]["user_id"].toInt();
        m_privateChatByUser[userId] = chatId;
        if (m_userNames.find(userId) == m_userNames.end())
            requestUser(userId);
    }
    requestAvatar(chatId, chat["photo"]["small"]);

    const base::json::Value &last = chat["last_message"];
    if (last.isObject()) {
        bool image = false;
        base::json::Value file;
        std::string fileName;
        const std::string body = bodyOf(last["content"], image, file, fileName);
        thread->snippet = body.empty() ? (image ? "Photo" : fileName) : body;
        thread->lastTimestamp = timestampOf(last["date"].toInt());
    }
}

void TelegramBackend::requestUser(int64_t userId)
{
    if (userId == 0 || !m_usersRequested.insert(userId).second)
        return;
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "getUser");
    request.insert("user_id", userId);
    request.insert("@extra", "user:" + std::to_string(userId));
    sendJson(request);
}

void TelegramBackend::requestAvatar(int64_t chatId, const base::json::Value &photo)
{
    if (!photo.isObject())
        return;
    Thread *thread = threadFor(chatId);
    if (!thread)
        return;
    const std::string path = photo["local"]["path"].toString();
    if (!path.empty()) {
        if (thread->avatarPath != path) {
            thread->avatarPath = path;
            threadsChanged();
        }
        return;
    }
    requestFile(photo, chatId, 0, true);
}

void TelegramBackend::requestFile(const base::json::Value &file, int64_t chatId,
                                  int64_t messageId, bool avatar)
{
    const int64_t fileId = file["id"].toInt();
    if (fileId == 0 || m_pendingFiles.count(fileId))
        return;
    m_pendingFiles[fileId] = {chatId, messageId, avatar};
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "downloadFile");
    request.insert("file_id", fileId);
    request.insert("priority", 1);
    request.insert("offset", 0);
    request.insert("limit", 0);
    request.insert("synchronous", false);
    sendJson(request);
}

void TelegramBackend::loadThread(const Thread &thread)
{
    m_openChat = thread.threadId;
    base::json::Value open = base::json::Value::object();
    open.insert("@type", "openChat");
    open.insert("chat_id", thread.threadId);
    sendJson(open);

    if (m_loadedChats.contains(thread.threadId))
        return;
    m_loadedChats.insert(thread.threadId);
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "getChatHistory");
    request.insert("chat_id", thread.threadId);
    request.insert("from_message_id", int64_t(0));
    request.insert("offset", 0);
    request.insert("limit", 100);
    request.insert("only_local", false);
    request.insert("@extra", "history:" + std::to_string(thread.threadId));
    sendJson(request);
}

void TelegramBackend::markRead(const Thread &thread)
{
    m_openChat = thread.threadId;
    viewChat(thread.threadId);
    Thread *known = threadFor(thread.threadId);
    if (known)
        known->unread = 0;
}

void TelegramBackend::viewChat(int64_t chatId)
{
    Thread *thread = threadFor(chatId);
    if (!thread)
        return;
    base::json::Value::Array ids;
    for (auto it = thread->messages.rbegin(); it != thread->messages.rend(); ++it) {
        if (it->outgoing)
            continue;
        ids.push_back(base::json::Value(it->id));
        if (ids.size() >= 100)
            break;
    }
    if (ids.empty())
        return;
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "viewMessages");
    request.insert("chat_id", chatId);
    request.insert("message_ids", base::json::Value(std::move(ids)));
    request.insert("force_read", true);
    sendJson(request);
}

std::string TelegramBackend::senderName(const base::json::Value &sender) const
{
    const std::string type = sender["@type"].toString();
    if (type == "messageSenderUser") {
        const auto found = m_userNames.find(sender["user_id"].toInt());
        return found == m_userNames.end() ? std::string("Member") : found->second;
    }
    if (type == "messageSenderChat") {
        const int64_t chatId = sender["chat_id"].toInt();
        for (const auto &[id, thread] : m_threads) {
            if (id == chatId)
                return thread.name;
        }
    }
    return {};
}

std::string TelegramBackend::bodyOf(const base::json::Value &content, bool &image,
                                    base::json::Value &file, std::string &fileName) const
{
    const std::string type = content["@type"].toString();
    if (type == "messageText")
        return content["text"]["text"].toString();
    if (type == "messagePhoto") {
        image = true;
        const base::json::Value &sizes = content["photo"]["sizes"];
        if (sizes.size() > 0)
            file = sizes[sizes.size() - 1]["photo"];
        return content["caption"]["text"].toString();
    }
    if (type == "messageDocument") {
        fileName = content["document"]["file_name"].toString();
        return content["caption"]["text"].toString();
    }
    if (type == "messageVideo") {
        fileName = content["video"]["file_name"].toString();
        file = content["video"]["video"];
        return content["caption"]["text"].toString();
    }
    if (type == "messageAnimation") {
        fileName = content["animation"]["file_name"].toString();
        file = content["animation"]["animation"];
        return content["caption"]["text"].toString();
    }
    if (type == "messageVoiceNote") {
        file = content["voice_note"]["voice"];
        return "Voice message";
    }
    if (type == "messageVideoNote") {
        file = content["video_note"]["video"];
        return "Video message";
    }
    if (type == "messageSticker")
        return "Sticker";
    if (type == "messageLocation")
        return "Location";
    if (type == "messageContact")
        return "Contact";
    if (type == "messagePoll")
        return "Poll";
    return {};
}

Message TelegramBackend::messageFrom(const base::json::Value &message, bool group) const
{
    Message result;
    result.id = message["id"].toInt();
    result.outgoing = message["is_outgoing"].toBool(false);
    result.timestamp = timestampOf(message["date"].toInt());
    const int64_t chatId = message["chat_id"].toInt();

    bool image = false;
    base::json::Value file;
    std::string fileName;
    std::string body = bodyOf(message["content"], image, file, fileName);
    if (body.empty() && !image)
        body = fileName;
    result.body = body;
    if (group && !result.outgoing)
        result.sender = senderName(message["sender_id"]);
    if (image && file.isObject()) {
        const std::string path = file["local"]["path"].toString();
        if (!path.empty())
            result.attachmentPath = path;
    }
    const auto read = m_lastReadOutbox.find(chatId);
    if (result.outgoing) {
        result.read = read != m_lastReadOutbox.end() && result.id <= read->second;
        result.receipt = result.read ? "Read" : "Sent";
    }
    return result;
}

void TelegramBackend::addMessage(const base::json::Value &message, bool history)
{
    const int64_t chatId = message["chat_id"].toInt();
    Thread *thread = threadFor(chatId);
    if (!thread)
        return;
    Message parsed = messageFrom(message, thread->group);
    if (thread->channel)
        parsed.sender.clear();
    const std::string type = message["content"]["@type"].toString();
    if (type.starts_with("messageChat") || type == "messageCustomServiceAction")
        return;

    bool image = false;
    base::json::Value file;
    std::string fileName;
    bodyOf(message["content"], image, file, fileName);
    // A picture with no caption is still a message, even before it downloads.
    if (parsed.body.empty() && !image && fileName.empty())
        return;

    const bool had = std::any_of(thread->messages.begin(), thread->messages.end(),
                                 [&](const Message &known) { return known.id == parsed.id; });
    if (!had)
        thread->messages.push_back(parsed);
    if (had) {
        for (Message &known : thread->messages) {
            if (known.id != parsed.id)
                continue;
            // A later update may not carry the downloaded file's path.
            if (parsed.attachmentPath.empty())
                parsed.attachmentPath = known.attachmentPath;
            known = parsed;
        }
    }

    const base::json::Value &sender = message["sender_id"];
    if (sender["@type"].toString() == "messageSenderUser") {
        const int64_t userId = sender["user_id"].toInt();
        m_senderIds[parsed.id] = userId;
        requestUser(userId);
    }

    if (image && file.isObject() && parsed.attachmentPath.empty())
        requestFile(file, chatId, parsed.id, false);
    std::sort(thread->messages.begin(), thread->messages.end(),
              [](const Message &left, const Message &right) { return left.id < right.id; });
    if (parsed.body.empty())
        thread->snippet = image ? "Photo" : fileName;
    else
        thread->snippet = parsed.body;
    thread->lastTimestamp = parsed.timestamp;

    if (!history && !parsed.outgoing) {
        if (chatId == m_openChat) {
            base::json::Value::Array ids;
            ids.push_back(base::json::Value(parsed.id));
            base::json::Value request = base::json::Value::object();
            request.insert("@type", "viewMessages");
            request.insert("chat_id", chatId);
            request.insert("message_ids", base::json::Value(std::move(ids)));
            request.insert("force_read", true);
            sendJson(request);
            thread->unread = 0;
        } else {
            thread->unread = std::max(1, thread->unread + 1);
        }
    }
    if (!history)
        threadsChanged();
}

bool TelegramBackend::send(const Thread &thread, const std::string &body, std::string &error)
{
    if (!m_authorized) {
        error = "Telegram is not signed in.";
        return false;
    }
    base::json::Value text = base::json::Value::object();
    text.insert("@type", "formattedText");
    text.insert("text", body);
    base::json::Value content = base::json::Value::object();
    content.insert("@type", "inputMessageText");
    content.insert("text", std::move(text));
    content.insert("disable_web_page_preview", false);
    content.insert("clear_draft", true);

    base::json::Value request = base::json::Value::object();
    request.insert("@type", "sendMessage");
    request.insert("chat_id", thread.threadId);
    request.insert("message_thread_id", 0);
    request.insert("input_message_content", std::move(content));
    sendJson(request);
    return true;
}

bool TelegramBackend::sendFile(const Thread &thread, const std::string &path,
                               const std::string &caption, std::string &error)
{
    if (!m_authorized) {
        error = "Telegram is not signed in.";
        return false;
    }
    base::json::Value file = base::json::Value::object();
    file.insert("@type", "inputFileLocal");
    file.insert("path", path);
    base::json::Value text = base::json::Value::object();
    text.insert("@type", "formattedText");
    text.insert("text", caption);

    base::json::Value content = base::json::Value::object();
    if (isPicture(path)) {
        content.insert("@type", "inputMessagePhoto");
        content.insert("photo", std::move(file));
        content.insert("thumbnail", base::json::Value());
        content.insert("added_sticker_file_ids", base::json::Value::Array());
        content.insert("width", 0);
        content.insert("height", 0);
        content.insert("caption", std::move(text));
        content.insert("ttl", 0);
        content.insert("has_spoiler", false);
    } else {
        content.insert("@type", "inputMessageDocument");
        content.insert("document", std::move(file));
        content.insert("thumbnail", base::json::Value());
        content.insert("caption", std::move(text));
        content.insert("disable_content_type_detection", false);
    }
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "sendMessage");
    request.insert("chat_id", thread.threadId);
    request.insert("message_thread_id", 0);
    request.insert("input_message_content", std::move(content));
    sendJson(request);
    return true;
}

void TelegramBackend::sendTyping(const Thread &thread, bool typing)
{
    if (!m_authorized)
        return;
    base::json::Value action = base::json::Value::object();
    action.insert("@type", typing ? "chatActionTyping" : "chatActionCancel");
    base::json::Value request = base::json::Value::object();
    request.insert("@type", "sendChatAction");
    request.insert("chat_id", thread.threadId);
    request.insert("message_thread_id", 0);
    request.insert("action", std::move(action));
    sendJson(request);
}

std::string TelegramBackend::timestampOf(int64_t seconds) const
{
    const std::time_t when = std::time_t(seconds);
    std::tm local {};
    localtime_r(&when, &local);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}

Thread *TelegramBackend::threadFor(int64_t chatId)
{
    if (chatId == 0)
        return nullptr;
    const auto found = m_threads.find(chatId);
    if (found != m_threads.end())
        return &found->second;
    Thread thread;
    thread.key = "tg:" + std::to_string(chatId);
    thread.service = "telegram";
    thread.serviceName = "Telegram";
    thread.threadId = chatId;
    thread.replyReady = true;
    const auto inserted = m_threads.emplace(chatId, std::move(thread));
    return &inserted.first->second;
}

std::vector<Thread> TelegramBackend::threads() const
{
    std::vector<Thread> list;
    list.reserve(m_threads.size());
    for (const auto &[chatId, thread] : m_threads)
        list.push_back(thread);
    std::sort(list.begin(), list.end(), [](const Thread &left, const Thread &right) {
        return left.lastTimestamp > right.lastTimestamp;
    });
    return list;
}

} // namespace cutefish::messages

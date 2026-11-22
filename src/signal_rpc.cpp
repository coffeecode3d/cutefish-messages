// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "signal_rpc.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace cutefish::messages {

namespace {

bool connectSocket(const std::string &path, int &fd)
{
    fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        fd = -1;
        return false;
    }
    return true;
}

bool writeAll(int fd, const std::string &text)
{
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t written = ::write(fd, text.data() + done, text.size() - done);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += size_t(written);
    }
    return true;
}

} // namespace

SignalRpc::SignalRpc(base::EventLoop *loop)
    : m_loop(loop)
{
}

SignalRpc::~SignalRpc()
{
    stop();
}

bool SignalRpc::start(const std::string &binary, const std::string &dataDir,
                      const std::string &socketPath, std::string &error)
{
    stop();
    ::unlink(socketPath.c_str());

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "The Signal helper could not be started.";
        return false;
    }
    if (pid == 0) {
        ::setsid();
        const std::string socket = "--socket=" + socketPath;
        ::execlp(binary.c_str(), binary.c_str(), "--data-dir", dataDir.c_str(), "daemon",
                 socket.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    m_process = pid;

    // Wait for the socket, up to five seconds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (connectSocket(socketPath, m_fd))
            break;
        ::usleep(100 * 1000);
    }
    if (m_fd < 0) {
        error = "The Signal helper did not start.";
        stop();
        return false;
    }

    int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
    m_watch = m_loop->watch(m_fd, POLLIN, [this](uint32_t) { onReadable(); });
    return true;
}

void SignalRpc::stop()
{
    if (m_watch) {
        m_loop->unwatch(m_watch);
        m_watch = 0;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_process > 0) {
        ::kill(pid_t(m_process), SIGTERM);
        int status = 0;
        ::waitpid(pid_t(m_process), &status, 0);
        m_process = 0;
    }
    failAll("The Signal helper stopped.");
}

void SignalRpc::call(const std::string &method, base::json::Value params, Reply done)
{
    if (m_fd < 0) {
        if (done)
            done(false, {}, "The Signal helper is not running.");
        return;
    }
    const int64_t id = m_nextId++;
    base::json::Value request = base::json::Value::object();
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", method);
    request.insert("params", std::move(params));
    m_pending[id] = std::move(done);

    std::string line = request.serialize();
    line += '\n';
    if (!writeAll(m_fd, line)) {
        failAll("The Signal helper did not take the request.");
    }
}

void SignalRpc::onReadable()
{
    char buffer[65536];
    for (;;) {
        const ssize_t got = ::read(m_fd, buffer, sizeof(buffer));
        if (got > 0) {
            m_buffer.append(buffer, size_t(got));
            continue;
        }
        if (got == 0) {
            failAll("The Signal helper closed the connection.");
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        failAll("The Signal helper's socket failed.");
        return;
    }
    consume();
}

void SignalRpc::consume()
{
    for (;;) {
        const auto newline = m_buffer.find('\n');
        if (newline == std::string::npos)
            return;
        std::string line = m_buffer.substr(0, newline);
        m_buffer.erase(0, newline + 1);
        const auto parsed = base::json::Value::parse(line);
        if (!parsed || !parsed->isObject())
            continue;

        if ((*parsed)["method"].isString()) {
            // A notification, such as `receive`.
            notification((*parsed)["params"]);
            continue;
        }
        const int64_t id = (*parsed)["id"].toInt();
        const auto found = m_pending.find(id);
        if (found == m_pending.end())
            continue;
        const Reply reply = std::move(found->second);
        m_pending.erase(found);
        if ((*parsed)["error"].isObject()) {
            std::string message = (*parsed)["error"]["message"].toString("request failed");
            if (reply)
                reply(false, {}, message);
        } else if (reply) {
            reply(true, (*parsed)["result"], {});
        }
    }
}

void SignalRpc::failAll(const std::string &error)
{
    auto pending = std::move(m_pending);
    m_pending.clear();
    for (auto &[id, reply] : pending) {
        if (reply)
            reply(false, {}, error);
    }
}

} // namespace cutefish::messages

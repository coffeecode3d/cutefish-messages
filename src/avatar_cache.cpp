// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "avatar_cache.h"

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace cutefish::messages {

namespace {

std::string cacheDirectory()
{
    std::string root;
    if (const char *value = std::getenv("XDG_CACHE_HOME"))
        root = value;
    else if (const char *home = std::getenv("HOME"))
        root = std::string(home) + "/.cache";
    else
        root = "/tmp";
    return root + "/cutefish/messages/avatars";
}

std::string fileFor(const std::string &key)
{
    // FNV-1a, so the name is stable across runs and versions.
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char name[32] = {};
    std::snprintf(name, sizeof(name), "%016llx.img", static_cast<unsigned long long>(hash));
    return cacheDirectory() + "/" + name;
}

bool isPicture(const std::string &bytes)
{
    if (bytes.size() < 12)
        return false;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    if (data[0] == 0xFF && data[1] == 0xD8)
        return true; // JPEG
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return true; // PNG
    if (bytes.rfind("RIFF", 0) == 0 && bytes.rfind("WEBP", 8) == 8)
        return true;
    if (bytes.rfind("GIF8", 0) == 0)
        return true;
    return false;
}

int base64Value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

std::string decodeBase64(const std::string &text)
{
    std::string out;
    out.reserve(text.size() * 3 / 4);
    uint32_t bits = 0;
    int count = 0;
    for (unsigned char c : text) {
        if (c == '=' || std::isspace(c))
            continue;
        const int value = base64Value(c);
        if (value < 0)
            continue;
        bits = (bits << 6) | uint32_t(value);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(char((bits >> count) & 0xFF));
        }
    }
    return out;
}

} // namespace

std::string avatarFor(const std::string &key)
{
    const std::string path = fileFor(key);
    std::error_code code;
    if (!std::filesystem::exists(path, code))
        return {};
    return path;
}

std::string saveAvatar(const std::string &key, const std::string &bytes)
{
    if (!isPicture(bytes))
        return {};
    const std::string directory = cacheDirectory();
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    const std::string path = fileFor(key);
    const std::string temporary = path + ".part";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return {};
        file.write(bytes.data(), std::streamsize(bytes.size()));
        if (!file)
            return {};
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        std::error_code ignored;
        std::filesystem::rename(temporary, path, ignored);
    }
    return std::filesystem::exists(path) ? path : std::string();
}

std::string base64Encode(const std::string &bytes)
{
    static const char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < bytes.size()) {
        const uint32_t chunk = (uint32_t(uint8_t(bytes[i])) << 16)
                | (uint32_t(uint8_t(bytes[i + 1])) << 8) | uint32_t(uint8_t(bytes[i + 2]));
        out.push_back(alphabet[(chunk >> 18) & 63]);
        out.push_back(alphabet[(chunk >> 12) & 63]);
        out.push_back(alphabet[(chunk >> 6) & 63]);
        out.push_back(alphabet[chunk & 63]);
        i += 3;
    }
    if (i < bytes.size()) {
        uint32_t chunk = uint32_t(uint8_t(bytes[i])) << 16;
        if (i + 1 < bytes.size())
            chunk |= uint32_t(uint8_t(bytes[i + 1])) << 8;
        out.push_back(alphabet[(chunk >> 18) & 63]);
        out.push_back(alphabet[(chunk >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string saveAttachmentBase64(const std::string &threadKey, int64_t messageId,
                                 size_t index, const std::string &extension,
                                 const std::string &base64)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : threadKey) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char name[64] = {};
    std::snprintf(name, sizeof(name), "%016llx-%lld-%zu%s",
                  static_cast<unsigned long long>(hash), static_cast<long long>(messageId),
                  index, extension.c_str());
    std::string root;
    if (const char *value = std::getenv("XDG_CACHE_HOME"))
        root = value;
    else if (const char *home = std::getenv("HOME"))
        root = std::string(home) + "/.cache";
    else
        root = "/tmp";
    const std::string directory = root + "/cutefish/messages/attachments";
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    const std::string path = directory + "/" + name;
    const std::string temporary = path + ".part";

    const auto comma = base64.find(',');
    const std::string body = comma == std::string::npos ? base64 : base64.substr(comma + 1);
    const std::string bytes = decodeBase64(body);
    if (bytes.empty())
        return {};
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return {};
        file.write(bytes.data(), std::streamsize(bytes.size()));
        if (!file)
            return {};
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::rename(temporary, path, ignored);
    }
    return std::filesystem::exists(path) ? path : std::string();
}

std::string saveAvatarBase64(const std::string &key, const std::string &base64)
{
    // Some sources prefix a data: URL; the pixels follow the comma.
    const auto comma = base64.find(',');
    const std::string body = comma == std::string::npos ? base64 : base64.substr(comma + 1);
    return saveAvatar(key, decodeBase64(body));
}

} // namespace cutefish::messages

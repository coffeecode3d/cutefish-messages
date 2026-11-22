// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_AVATAR_CACHE_H_
#define CUTEFISH_MESSAGES_AVATAR_CACHE_H_

#include <cstdint>
#include <string>

namespace cutefish::messages {

// Profile pictures kept under ~/.cache/cutefish/messages/avatars, named by a
// hash of the service and address so a thread can find its picture without a
// database column.
std::string avatarFor(const std::string &key);
// Writes a picture for the key and returns its path, or empty on failure.
std::string saveAvatar(const std::string &key, const std::string &bytes);
std::string saveAvatarBase64(const std::string &key, const std::string &base64);

// Message attachments, named by the conversation, message and part so a
// restart finds the file again.
std::string base64Encode(const std::string &bytes);
std::string saveAttachmentBase64(const std::string &threadKey, int64_t messageId,
                                 size_t index, const std::string &extension,
                                 const std::string &base64);

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_AVATAR_CACHE_H_

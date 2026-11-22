// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_PHONE_CERTIFICATE_H_
#define CUTEFISH_MESSAGES_PHONE_CERTIFICATE_H_

#include <string>

namespace cutefish::phone {

// The device's TLS identity: made once, kept in
// ~/.config/cutefish/messages, and the thing the phone pins at pairing.
struct PhoneCertificate
{
    std::string certificate;
    std::string key;
    // SHA-256 of the DER certificate, uppercase hex.
    std::string fingerprint;
};

PhoneCertificate loadOrCreatePhoneCertificate();

} // namespace cutefish::phone

#endif // CUTEFISH_MESSAGES_PHONE_CERTIFICATE_H_

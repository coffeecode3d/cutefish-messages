// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "phone/certificate.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "base/paths.h"

namespace cutefish::phone {

namespace {

std::string toHex(const unsigned char *bytes, size_t size)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text(size * 2, '\0');
    for (size_t i = 0; i < size; ++i) {
        text[i * 2] = kDigits[bytes[i] >> 4];
        text[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    return text;
}

std::string pemOf(X509 *certificate)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    PEM_write_bio_X509(bio, certificate);
    char *data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string text(data ? std::string(data, size_t(size)) : std::string());
    BIO_free(bio);
    return text;
}

std::string pemOf(EVP_PKEY *key)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char *data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string text(data ? std::string(data, size_t(size)) : std::string());
    BIO_free(bio);
    return text;
}

bool writePrivate(const std::filesystem::path &path, const std::string &text)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t written = ::write(fd, text.data() + done, text.size() - done);
        if (written <= 0) {
            ::close(fd);
            return false;
        }
        done += size_t(written);
    }
    ::close(fd);
    return true;
}

std::string readAll(const std::filesystem::path &path)
{
    std::ifstream file(path);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string makeCertificate(std::string &keyPem)
{
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!context)
        return {};
    EVP_PKEY *key = nullptr;
    if (EVP_PKEY_keygen_init(context) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) > 0)
        EVP_PKEY_keygen(context, &key);
    EVP_PKEY_CTX_free(context);
    if (!key)
        return {};

    X509 *certificate = X509_new();
    if (!certificate) {
        EVP_PKEY_free(key);
        return {};
    }
    std::array<unsigned char, 16> serial {};
    RAND_bytes(serial.data(), int(serial.size()));
    serial[0] &= 0x7F;
    BIGNUM *number = BN_bin2bn(serial.data(), int(serial.size()), nullptr);
    if (number) {
        BN_to_ASN1_INTEGER(number, X509_get_serialNumber(certificate));
        BN_free(number);
    }
    X509_set_version(certificate, 2);
    X509_gmtime_adj(X509_get_notBefore(certificate), -365L * 24 * 60 * 60);
    X509_gmtime_adj(X509_get_notAfter(certificate), 10L * 365 * 24 * 60 * 60);
    X509_set_pubkey(certificate, key);
    X509_NAME *name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char *>("Cutefish Messages"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char *>("Messages"), -1, -1, 0);
    X509_set_issuer_name(certificate, name);

    std::string result;
    if (X509_sign(certificate, key, EVP_sha256()) > 0) {
        result = pemOf(certificate);
        keyPem = pemOf(key);
    }
    X509_free(certificate);
    EVP_PKEY_free(key);
    return result;
}

} // namespace

PhoneCertificate loadOrCreatePhoneCertificate()
{
    const std::filesystem::path directory(base::paths::cutefishConfig("messages") + ".d");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path certPath = directory / "cert.pem";
    const std::filesystem::path keyPath = directory / "key.pem";

    PhoneCertificate certificate;
    certificate.certificate = readAll(certPath);
    certificate.key = readAll(keyPath);
    if (certificate.certificate.empty() || certificate.key.empty()) {
        certificate.certificate = makeCertificate(certificate.key);
        if (certificate.certificate.empty())
            return {};
        writePrivate(certPath, certificate.certificate);
        writePrivate(keyPath, certificate.key);
    }

    BIO *bio = BIO_new_mem_buf(certificate.certificate.data(), int(certificate.certificate.size()));
    X509 *x509 = bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
    if (bio)
        BIO_free(bio);
    if (!x509)
        return {};
    unsigned char *der = nullptr;
    const int size = i2d_X509(x509, &der);
    if (size > 0) {
        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
        SHA256(der, size_t(size), digest.data());
        certificate.fingerprint = toHex(digest.data(), digest.size());
    }
    if (der)
        OPENSSL_free(der);
    X509_free(x509);
    return certificate;
}

} // namespace cutefish::phone

/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CertificateInspector.h"

#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace LadybirdSDE {

static String x509_name_to_string(X509_NAME* name)
{
    if (!name)
        return {};
    char buffer[1024] {};
    if (!X509_NAME_oneline(name, buffer, sizeof(buffer)))
        return {};
    return String::from_utf8_with_replacement_character(StringView { buffer, strlen(buffer) });
}

static String asn1_time_to_string(ASN1_TIME const* time)
{
    if (!time)
        return {};
    auto* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    String result;
    if (ASN1_TIME_print(bio, time) == 1) {
        char* data = nullptr;
        auto length = BIO_get_mem_data(bio, &data);
        if (length > 0)
            result = String::from_utf8_with_replacement_character(StringView { data, static_cast<size_t>(length) });
    }
    BIO_free(bio);
    return result;
}

static String asn1_integer_to_hex(ASN1_INTEGER const* integer)
{
    if (!integer)
        return {};
    auto* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    String result;
    if (i2a_ASN1_INTEGER(bio, integer) > 0) {
        char* data = nullptr;
        auto length = BIO_get_mem_data(bio, &data);
        if (length > 0)
            result = String::from_utf8_with_replacement_character(StringView { data, static_cast<size_t>(length) });
    }
    BIO_free(bio);
    return result;
}

static String x509_fingerprint(X509* certificate)
{
    unsigned char digest[EVP_MAX_MD_SIZE] {};
    unsigned int digest_length = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &digest_length) != 1)
        return {};
    StringBuilder builder;
    for (unsigned int i = 0; i < digest_length; ++i) {
        if (i)
            builder.append(':');
        builder.appendff("{:02X}", digest[i]);
    }
    return builder.to_string().release_value();
}

static int connect_with_timeout(ByteString const& host, u16 port, int timeout_ms)
{
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    auto port_str = ByteString::number(port);
    struct addrinfo* results = nullptr;
    if (::getaddrinfo(host.characters(), port_str.characters(), &hints, &results) != 0 || !results)
        return -1;

    int connected_fd = -1;
    for (auto* addr = results; addr; addr = addr->ai_next) {
        int fd = ::socket(addr->ai_family, addr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, addr->ai_protocol);
        if (fd < 0)
            continue;

        int rc = ::connect(fd, addr->ai_addr, addr->ai_addrlen);
        if (rc == 0) {
            connected_fd = fd;
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            if (::poll(&pfd, 1, timeout_ms) > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                    connected_fd = fd;
                    break;
                }
            }
        }
        ::close(fd);
    }

    ::freeaddrinfo(results);

    if (connected_fd >= 0) {
        int flags = ::fcntl(connected_fd, F_GETFL, 0);
        if (flags >= 0)
            (void)::fcntl(connected_fd, F_SETFL, flags & ~O_NONBLOCK);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        (void)::setsockopt(connected_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(connected_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    return connected_fd;
}

static CertificateInspectionResult inspect_certificate_sync(ByteString const& host, u16 port)
{
    int fd = connect_with_timeout(host, port, 5000);
    if (fd < 0)
        return { {}, "Could not reach the site."_string };

    auto* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ::close(fd);
        return { {}, "Could not initialize TLS context."_string };
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    auto* ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        ::close(fd);
        return { {}, "Could not initialize TLS session."_string };
    }

    SSL_set_tlsext_host_name(ssl, host.characters());
    SSL_set_fd(ssl, fd);

    CertificateInspectionResult result;
    if (SSL_connect(ssl) == 1) {
        if (auto* certificate = SSL_get0_peer_certificate(ssl)) {
            auto const* version = SSL_get_version(ssl);
            auto const* cipher = SSL_get_cipher_name(ssl);
            result.details = CertificateDetails {
                .subject = x509_name_to_string(X509_get_subject_name(certificate)),
                .issuer = x509_name_to_string(X509_get_issuer_name(certificate)),
                .valid_from = asn1_time_to_string(X509_get0_notBefore(certificate)),
                .valid_until = asn1_time_to_string(X509_get0_notAfter(certificate)),
                .serial_number = asn1_integer_to_hex(X509_get_serialNumber(certificate)),
                .sha256_fingerprint = x509_fingerprint(certificate),
                .protocol = version ? String::from_utf8_with_replacement_character(StringView { version, strlen(version) }) : String {},
                .cipher = cipher ? String::from_utf8_with_replacement_character(StringView { cipher, strlen(cipher) }) : String {},
            };
        } else {
            result.error = "The server did not present a certificate."_string;
        }
        SSL_shutdown(ssl);
    } else {
        result.error = "Could not complete the TLS handshake."_string;
    }

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return result;
}

void inspect_certificate_async(URL::URL url, Function<void(CertificateInspectionResult)> callback)
{
    if (url.scheme() != "https"sv) {
        callback({ {}, "Certificate details are only available for secure pages."_string });
        return;
    }

    auto host = url.serialized_host().to_byte_string();
    if (host.is_empty()) {
        callback({ {}, "Certificate details are not available for this address."_string });
        return;
    }
    if (host.starts_with('[') && host.ends_with(']') && host.length() > 2)
        host = host.substring(1, host.length() - 2);

    u16 port = url.port().value_or(443);
    auto* loop = &Core::EventLoop::current();

    std::thread([loop, host = move(host), port, callback = move(callback)]() mutable {
        auto result = inspect_certificate_sync(host, port);
        loop->deferred_invoke([callback = move(callback), result = move(result)]() mutable {
            callback(move(result));
        });
    }).detach();
}

}

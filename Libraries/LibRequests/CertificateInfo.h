/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibIPC/Forward.h>

namespace Requests {

// Details of the certificate the TLS handshake for a request negotiated. RequestServer fills this in from the live
// OpenSSL session, so the chrome can show the same certificate the page was actually served under.
struct CertificateInfo {
    String subject;
    String issuer;
    String valid_from;
    String valid_until;
    String serial_number;
    String sha256_fingerprint;
    String protocol;
    String cipher;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::CertificateInfo const&);

template<>
ErrorOr<Requests::CertificateInfo> decode(Decoder&);

}

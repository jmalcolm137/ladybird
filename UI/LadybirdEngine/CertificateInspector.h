/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/URL.h>

namespace LadybirdSDE {

struct CertificateDetails {
    String subject;
    String issuer;
    String valid_from;
    String valid_until;
    String serial_number;
    String sha256_fingerprint;
    String protocol;
    String cipher;
};

struct CertificateInspectionResult {
    Optional<CertificateDetails> details;
    String error;
};

// Inspects the TLS peer certificate for an https:// URL directly via OpenSSL
// on a worker thread, posting the result back to the caller's Core::EventLoop.
// Keeping this self-contained in UI/LadybirdEngine avoids modifying upstream
// LibRequests and RequestServer.
void inspect_certificate_async(URL::URL url, Function<void(CertificateInspectionResult)> callback);

}

/*
 * Copyright (c) 2026, Ladybird SDE contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/CertificateInfo.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::CertificateInfo const& info)
{
    TRY(encoder.encode(info.subject));
    TRY(encoder.encode(info.issuer));
    TRY(encoder.encode(info.valid_from));
    TRY(encoder.encode(info.valid_until));
    TRY(encoder.encode(info.serial_number));
    TRY(encoder.encode(info.sha256_fingerprint));
    TRY(encoder.encode(info.protocol));
    TRY(encoder.encode(info.cipher));
    return {};
}

template<>
ErrorOr<Requests::CertificateInfo> decode(Decoder& decoder)
{
    auto subject = TRY(decoder.decode<String>());
    auto issuer = TRY(decoder.decode<String>());
    auto valid_from = TRY(decoder.decode<String>());
    auto valid_until = TRY(decoder.decode<String>());
    auto serial_number = TRY(decoder.decode<String>());
    auto sha256_fingerprint = TRY(decoder.decode<String>());
    auto protocol = TRY(decoder.decode<String>());
    auto cipher = TRY(decoder.decode<String>());

    return Requests::CertificateInfo {
        .subject = move(subject),
        .issuer = move(issuer),
        .valid_from = move(valid_from),
        .valid_until = move(valid_until),
        .serial_number = move(serial_number),
        .sha256_fingerprint = move(sha256_fingerprint),
        .protocol = move(protocol),
        .cipher = move(cipher),
    };
}

}

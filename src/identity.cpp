// Thermal Control Plane — identity rendering and key validation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/identity.hpp"

namespace thermal_control_plane {
namespace {

void append_u64(std::string& out, std::uint64_t value) {
    char buffer[21];
    std::size_t index = sizeof(buffer);
    if (value == 0) {
        out.push_back('0');
        return;
    }
    while (value != 0) {
        --index;
        buffer[index] = static_cast<char>('0' + static_cast<int>(value % 10U));
        value /= 10U;
    }
    out.append(buffer + index, sizeof(buffer) - index);
}

}  // namespace

std::string to_string(const ScopeRef& scope) {
    std::string out;
    out.reserve(48);
    out.push_back('f');
    append_u64(out, scope.facility.value);
    out.append("/z");
    append_u64(out, scope.zone.value);
    out.append("/d");
    append_u64(out, scope.domain.value);
    return out;
}

ScopeRef facility_scope(FacilityId facility) noexcept {
    return ScopeRef{facility, ZoneRef{0}, DomainRef{0}};
}

Result<IdempotencyKey> IdempotencyKey::parse(std::string_view text) {
    if (text.empty()) {
        return Error{ErrorCode::INVALID_KEY, "idempotency key must not be empty"};
    }
    if (text.size() > kMaxLength) {
        return Error{ErrorCode::INVALID_KEY, "idempotency key exceeds " + std::to_string(kMaxLength) + " bytes"};
    }
    IdempotencyKey key;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto raw = static_cast<unsigned char>(text[i]);
        if (raw < 0x21U || raw > 0x7EU) {
            return Error{ErrorCode::INVALID_KEY, "idempotency key must contain printable ASCII only"};
        }
        key.bytes_[i] = text[i];
    }
    key.length_ = static_cast<std::uint8_t>(text.size());
    return key;
}

}  // namespace thermal_control_plane

// Thermal Control Plane — typed error semantics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_ERROR_HPP
#define THERMAL_CONTROL_PLANE_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace thermal_control_plane {

/// Typed thermal-authority error codes.
///
/// Every refusal is a value, never an exception and never a bare bool. The
/// numeric value of each enumerator is part of the machine contract and is
/// stable across releases; the human detail string is explanatory only.
enum class ErrorCode : std::uint32_t {
    NONE = 0,

    // Structural validation of caller-supplied values.
    INVALID_ARGUMENT = 1,
    INVALID_KEY = 2,
    OUT_OF_RANGE = 3,
    ARITHMETIC_OVERFLOW = 4,

    // Generation and authority fencing.
    STALE_EPOCH = 20,
    FUTURE_EPOCH = 21,
    STALE_POLICY_GENERATION = 22,
    FUTURE_POLICY_GENERATION = 23,
    STALE_TOPOLOGY_GENERATION = 24,
    FUTURE_TOPOLOGY_GENERATION = 25,
    STALE_EVIDENCE_GENERATION = 26,
    FUTURE_EVIDENCE_GENERATION = 27,
    STALE_OBJECT_GENERATION = 28,
    FUTURE_OBJECT_GENERATION = 29,
    STALE_STATE_REVISION = 30,
    FUTURE_STATE_REVISION = 31,
    STALE_PROCESS_INCARNATION = 32,
    STALE_DIRECTIVE = 33,
    STALE_ATTEMPT = 34,

    // Identity resolution.
    UNKNOWN_SCOPE = 40,
    UNKNOWN_SENSOR = 41,
    UNKNOWN_POLICY = 42,
    UNKNOWN_LIMIT_SET = 43,
    UNKNOWN_DIRECTIVE = 44,
    UNKNOWN_INTERLOCK = 45,
    DUPLICATE_SCOPE = 46,
    DUPLICATE_SENSOR = 47,
    DUPLICATE_INTERLOCK = 48,

    // Evidence quality and ordering.
    EVIDENCE_UNKNOWN = 60,
    EVIDENCE_STALE = 61,
    EVIDENCE_FUTURE = 62,
    EVIDENCE_INDETERMINATE = 63,
    EVIDENCE_CONFLICT = 64,
    EVIDENCE_SUPERSEDED = 65,
    EVIDENCE_UNSUPPORTED = 66,
    EVIDENCE_DUPLICATE = 67,
    EVIDENCE_REQUIRED = 68,
    EVIDENCE_NOT_REVALIDATED = 69,

    // Limit and policy definitions.
    LIMIT_UNDEFINED = 80,
    LIMIT_ORDER_INVALID = 81,
    POLICY_INVALID = 82,
    POLICY_NON_MONOTONE_DERATING = 83,
    POLICY_LIMIT_EXCEEDED = 84,
    LIMIT_SET_CONFLICT = 85,

    // Authority outcomes.
    INTERLOCK_ASSERTED = 100,
    AUTHORITY_INSUFFICIENT = 101,
    DERATE_OUT_OF_BOUNDS = 102,
    DERATE_INSUFFICIENT = 103,
    MODE_TRANSITION_INVALID = 104,
    RECOVERY_NOT_ELIGIBLE = 105,
    ESCALATION_REQUIRED = 106,
    COORDINATION_UNAVAILABLE = 107,
    AUTHORITY_REFUSED = 108,
    VERIFICATION_UNPROVEN = 109,
    VERIFICATION_CONTRADICTED = 110,
    IDEMPOTENCY_CONFLICT = 111,

    // Runtime lifecycle and resource bounds.
    RUNTIME_CLOSED = 120,
    RESOURCE_EXHAUSTED = 121,
    CONCURRENCY_CONFLICT = 122,

    // Durable store.
    STORE_LOCKED = 140,
    STORE_IO = 141,
    STORE_CORRUPT = 142,
    STORE_TRUNCATED = 143,
    STORE_TRAILING_BYTES = 144,
    STORE_INTEGRITY = 145,
    STORE_UNSUPPORTED_VERSION = 146,
    STORE_RESERVED_FIELD = 147,
    STORE_SLOT_UNAVAILABLE = 148,
    STORE_PATH_INVALID = 149,
    STORE_READBACK_MISMATCH = 150,

    // Canonical encoding.
    ENCODING_INVALID = 170,
    ENCODING_TRUNCATED = 171,
    ENCODING_TOO_LARGE = 172,
    ENCODING_TRAILING_BYTES = 173,
    ENCODING_UNKNOWN_ENUM = 174,

    INTERNAL = 200,
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::NONE: return "NONE";
        case ErrorCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case ErrorCode::INVALID_KEY: return "INVALID_KEY";
        case ErrorCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case ErrorCode::ARITHMETIC_OVERFLOW: return "ARITHMETIC_OVERFLOW";
        case ErrorCode::STALE_EPOCH: return "STALE_EPOCH";
        case ErrorCode::FUTURE_EPOCH: return "FUTURE_EPOCH";
        case ErrorCode::STALE_POLICY_GENERATION: return "STALE_POLICY_GENERATION";
        case ErrorCode::FUTURE_POLICY_GENERATION: return "FUTURE_POLICY_GENERATION";
        case ErrorCode::STALE_TOPOLOGY_GENERATION: return "STALE_TOPOLOGY_GENERATION";
        case ErrorCode::FUTURE_TOPOLOGY_GENERATION: return "FUTURE_TOPOLOGY_GENERATION";
        case ErrorCode::STALE_EVIDENCE_GENERATION: return "STALE_EVIDENCE_GENERATION";
        case ErrorCode::FUTURE_EVIDENCE_GENERATION: return "FUTURE_EVIDENCE_GENERATION";
        case ErrorCode::STALE_OBJECT_GENERATION: return "STALE_OBJECT_GENERATION";
        case ErrorCode::FUTURE_OBJECT_GENERATION: return "FUTURE_OBJECT_GENERATION";
        case ErrorCode::STALE_STATE_REVISION: return "STALE_STATE_REVISION";
        case ErrorCode::FUTURE_STATE_REVISION: return "FUTURE_STATE_REVISION";
        case ErrorCode::STALE_PROCESS_INCARNATION: return "STALE_PROCESS_INCARNATION";
        case ErrorCode::STALE_DIRECTIVE: return "STALE_DIRECTIVE";
        case ErrorCode::STALE_ATTEMPT: return "STALE_ATTEMPT";
        case ErrorCode::UNKNOWN_SCOPE: return "UNKNOWN_SCOPE";
        case ErrorCode::UNKNOWN_SENSOR: return "UNKNOWN_SENSOR";
        case ErrorCode::UNKNOWN_POLICY: return "UNKNOWN_POLICY";
        case ErrorCode::UNKNOWN_LIMIT_SET: return "UNKNOWN_LIMIT_SET";
        case ErrorCode::UNKNOWN_DIRECTIVE: return "UNKNOWN_DIRECTIVE";
        case ErrorCode::UNKNOWN_INTERLOCK: return "UNKNOWN_INTERLOCK";
        case ErrorCode::DUPLICATE_SCOPE: return "DUPLICATE_SCOPE";
        case ErrorCode::DUPLICATE_SENSOR: return "DUPLICATE_SENSOR";
        case ErrorCode::DUPLICATE_INTERLOCK: return "DUPLICATE_INTERLOCK";
        case ErrorCode::EVIDENCE_UNKNOWN: return "EVIDENCE_UNKNOWN";
        case ErrorCode::EVIDENCE_STALE: return "EVIDENCE_STALE";
        case ErrorCode::EVIDENCE_FUTURE: return "EVIDENCE_FUTURE";
        case ErrorCode::EVIDENCE_INDETERMINATE: return "EVIDENCE_INDETERMINATE";
        case ErrorCode::EVIDENCE_CONFLICT: return "EVIDENCE_CONFLICT";
        case ErrorCode::EVIDENCE_SUPERSEDED: return "EVIDENCE_SUPERSEDED";
        case ErrorCode::EVIDENCE_UNSUPPORTED: return "EVIDENCE_UNSUPPORTED";
        case ErrorCode::EVIDENCE_DUPLICATE: return "EVIDENCE_DUPLICATE";
        case ErrorCode::EVIDENCE_REQUIRED: return "EVIDENCE_REQUIRED";
        case ErrorCode::EVIDENCE_NOT_REVALIDATED: return "EVIDENCE_NOT_REVALIDATED";
        case ErrorCode::LIMIT_UNDEFINED: return "LIMIT_UNDEFINED";
        case ErrorCode::LIMIT_ORDER_INVALID: return "LIMIT_ORDER_INVALID";
        case ErrorCode::POLICY_INVALID: return "POLICY_INVALID";
        case ErrorCode::POLICY_NON_MONOTONE_DERATING: return "POLICY_NON_MONOTONE_DERATING";
        case ErrorCode::POLICY_LIMIT_EXCEEDED: return "POLICY_LIMIT_EXCEEDED";
        case ErrorCode::LIMIT_SET_CONFLICT: return "LIMIT_SET_CONFLICT";
        case ErrorCode::INTERLOCK_ASSERTED: return "INTERLOCK_ASSERTED";
        case ErrorCode::AUTHORITY_INSUFFICIENT: return "AUTHORITY_INSUFFICIENT";
        case ErrorCode::DERATE_OUT_OF_BOUNDS: return "DERATE_OUT_OF_BOUNDS";
        case ErrorCode::DERATE_INSUFFICIENT: return "DERATE_INSUFFICIENT";
        case ErrorCode::MODE_TRANSITION_INVALID: return "MODE_TRANSITION_INVALID";
        case ErrorCode::RECOVERY_NOT_ELIGIBLE: return "RECOVERY_NOT_ELIGIBLE";
        case ErrorCode::ESCALATION_REQUIRED: return "ESCALATION_REQUIRED";
        case ErrorCode::COORDINATION_UNAVAILABLE: return "COORDINATION_UNAVAILABLE";
        case ErrorCode::AUTHORITY_REFUSED: return "AUTHORITY_REFUSED";
        case ErrorCode::VERIFICATION_UNPROVEN: return "VERIFICATION_UNPROVEN";
        case ErrorCode::VERIFICATION_CONTRADICTED: return "VERIFICATION_CONTRADICTED";
        case ErrorCode::IDEMPOTENCY_CONFLICT: return "IDEMPOTENCY_CONFLICT";
        case ErrorCode::RUNTIME_CLOSED: return "RUNTIME_CLOSED";
        case ErrorCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
        case ErrorCode::CONCURRENCY_CONFLICT: return "CONCURRENCY_CONFLICT";
        case ErrorCode::STORE_LOCKED: return "STORE_LOCKED";
        case ErrorCode::STORE_IO: return "STORE_IO";
        case ErrorCode::STORE_CORRUPT: return "STORE_CORRUPT";
        case ErrorCode::STORE_TRUNCATED: return "STORE_TRUNCATED";
        case ErrorCode::STORE_TRAILING_BYTES: return "STORE_TRAILING_BYTES";
        case ErrorCode::STORE_INTEGRITY: return "STORE_INTEGRITY";
        case ErrorCode::STORE_UNSUPPORTED_VERSION: return "STORE_UNSUPPORTED_VERSION";
        case ErrorCode::STORE_RESERVED_FIELD: return "STORE_RESERVED_FIELD";
        case ErrorCode::STORE_SLOT_UNAVAILABLE: return "STORE_SLOT_UNAVAILABLE";
        case ErrorCode::STORE_PATH_INVALID: return "STORE_PATH_INVALID";
        case ErrorCode::STORE_READBACK_MISMATCH: return "STORE_READBACK_MISMATCH";
        case ErrorCode::ENCODING_INVALID: return "ENCODING_INVALID";
        case ErrorCode::ENCODING_TRUNCATED: return "ENCODING_TRUNCATED";
        case ErrorCode::ENCODING_TOO_LARGE: return "ENCODING_TOO_LARGE";
        case ErrorCode::ENCODING_TRAILING_BYTES: return "ENCODING_TRAILING_BYTES";
        case ErrorCode::ENCODING_UNKNOWN_ENUM: return "ENCODING_UNKNOWN_ENUM";
        case ErrorCode::INTERNAL: return "INTERNAL";
    }
    return "UNRECOGNISED_ERROR_CODE";
}

/// Stable machine code plus explanatory detail.
struct Error {
    ErrorCode code = ErrorCode::NONE;
    std::string detail;

    Error() = default;
    explicit Error(ErrorCode c, std::string d = {}) : code(c), detail(std::move(d)) {}

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::NONE; }
    [[nodiscard]] std::string render() const;
};

/// Result carrier used for operations that can fail.
///
/// Policy outcomes (a selected mode, a granted derating, a refusal verdict)
/// are values and travel in the value channel; Error carries only the reason
/// an operation could not produce a value at all.
///
/// Lifetime note: value() returns a reference into the carrier, exactly as
/// std::expected does. Copy the value or keep the carrier alive.
template <class T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<Error>(storage_); }
    [[nodiscard]] Error& error() & { return std::get<Error>(storage_); }

    [[nodiscard]] T value_or(T fallback) const {
        return has_value() ? std::get<T>(storage_) : std::move(fallback);
    }

private:
    std::variant<T, Error> storage_;
};

/// Result carrier for operations with no payload.
class Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }
    [[nodiscard]] ErrorCode code() const noexcept { return error_.code; }

    static Status success() { return Status{}; }
    static Status failure(ErrorCode code, std::string detail = {}) {
        return Status{Error{code, std::move(detail)}};
    }

private:
    Error error_{};
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_ERROR_HPP

// Thermal Control Plane — exact integer quantity arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/quantity.hpp"

#include <string>

namespace thermal_control_plane {
namespace {

constexpr std::int64_t kInt64Max = 9223372036854775807LL;
constexpr std::int64_t kInt64Min = (-9223372036854775807LL - 1LL);

/// Render a signed value as decimal text without locale involvement.
void append_int(std::string& out, std::int64_t value) {
    if (value == 0) {
        out.push_back('0');
        return;
    }
    const bool negative = value < 0;
    std::uint64_t magnitude = 0;
    if (negative) {
        magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1ULL;
    } else {
        magnitude = static_cast<std::uint64_t>(value);
    }
    char buffer[24];
    std::size_t index = sizeof(buffer);
    while (magnitude != 0) {
        --index;
        buffer[index] = static_cast<char>('0' + static_cast<int>(magnitude % 10ULL));
        magnitude /= 10ULL;
    }
    if (negative) {
        out.push_back('-');
    }
    out.append(buffer + index, sizeof(buffer) - index);
}

}  // namespace

Result<std::int64_t> checked_add(std::int64_t a, std::int64_t b) {
    if (b > 0 && a > kInt64Max - b) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "signed addition overflow"};
    }
    if (b < 0 && a < kInt64Min - b) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "signed addition underflow"};
    }
    return a + b;
}

Result<std::int64_t> checked_sub(std::int64_t a, std::int64_t b) {
    if (b == kInt64Min) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "signed subtraction overflow"};
    }
    return checked_add(a, -b);
}

Result<std::int64_t> checked_mul(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) {
        return std::int64_t{0};
    }
    if (a == -1 && b == kInt64Min) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "signed multiplication overflow"};
    }
    if (b == -1 && a == kInt64Min) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "signed multiplication overflow"};
    }
    const std::int64_t product = a * b;
    if (product / b != a) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "signed multiplication overflow"};
    }
    return product;
}

Result<Temperature> Temperature::from_milli_celsius(std::int64_t milli_celsius) {
    if (milli_celsius < static_cast<std::int64_t>(kAbsoluteZeroMilliC)) {
        return Error{ErrorCode::OUT_OF_RANGE, "temperature below absolute zero"};
    }
    if (milli_celsius > static_cast<std::int64_t>(kMaxMilliC)) {
        return Error{ErrorCode::OUT_OF_RANGE, "temperature above representable facility range"};
    }
    return from_milli_celsius_unchecked(static_cast<std::int32_t>(milli_celsius));
}

Result<TemperatureDelta> difference(Temperature a, Temperature b) {
    const auto delta = checked_sub(static_cast<std::int64_t>(a.milli_celsius()),
                                   static_cast<std::int64_t>(b.milli_celsius()));
    if (!delta.has_value()) {
        return delta.error();
    }
    return TemperatureDelta{delta.value()};
}

Result<BasisPoints> BasisPoints::from_value(std::int64_t value) {
    if (value < 0) {
        return Error{ErrorCode::OUT_OF_RANGE, "basis points must not be negative"};
    }
    if (value > static_cast<std::int64_t>(kFull)) {
        return Error{ErrorCode::OUT_OF_RANGE, "basis points must not exceed 10000"};
    }
    return from_value_unchecked(static_cast<std::uint32_t>(value));
}

std::string to_string(Temperature temperature) {
    std::string out;
    out.reserve(12);
    append_int(out, static_cast<std::int64_t>(temperature.milli_celsius()));
    out.append("mC");
    return out;
}

std::string to_string(TemperatureDelta delta) {
    std::string out;
    out.reserve(14);
    append_int(out, delta.milli_celsius);
    out.append("mC");
    return out;
}

std::string to_string(BasisPoints basis_points) {
    std::string out;
    out.reserve(12);
    append_int(out, static_cast<std::int64_t>(basis_points.value()));
    out.append("bp");
    return out;
}

}  // namespace thermal_control_plane

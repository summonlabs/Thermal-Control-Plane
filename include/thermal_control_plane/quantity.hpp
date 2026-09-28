// Thermal Control Plane — exact integer physical quantities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_QUANTITY_HPP
#define THERMAL_CONTROL_PLANE_QUANTITY_HPP

#include <cstdint>
#include <string>

#include "thermal_control_plane/error.hpp"

namespace thermal_control_plane {

/// Checked integer arithmetic helpers.
///
/// Every quantity in this runtime is an exact integer. Nothing that decides
/// authority is computed in floating point, so nothing that decides authority
/// depends on rounding.
[[nodiscard]] Result<std::int64_t> checked_add(std::int64_t a, std::int64_t b);
[[nodiscard]] Result<std::int64_t> checked_sub(std::int64_t a, std::int64_t b);
[[nodiscard]] Result<std::int64_t> checked_mul(std::int64_t a, std::int64_t b);

/// A temperature in exact millidegrees Celsius.
///
/// The accepted range is the physical range this layer is willing to speak
/// about: absolute zero through one thousand degrees Celsius. Values outside
/// it are refused rather than clamped, because a clamped temperature would
/// turn a sensor fault into a plausible reading.
class Temperature {
public:
    static constexpr std::int32_t kAbsoluteZeroMilliC = -273150;
    static constexpr std::int32_t kMaxMilliC = 1000000;

    constexpr Temperature() noexcept = default;

    [[nodiscard]] static Result<Temperature> from_milli_celsius(std::int64_t milli_celsius);
    [[nodiscard]] static constexpr Temperature from_milli_celsius_unchecked(std::int32_t milli_celsius) noexcept {
        Temperature t;
        t.milli_celsius_ = milli_celsius;
        return t;
    }

    [[nodiscard]] constexpr std::int32_t milli_celsius() const noexcept { return milli_celsius_; }

    friend constexpr bool operator==(Temperature a, Temperature b) noexcept {
        return a.milli_celsius_ == b.milli_celsius_;
    }
    friend constexpr bool operator!=(Temperature a, Temperature b) noexcept { return !(a == b); }
    friend constexpr bool operator<(Temperature a, Temperature b) noexcept {
        return a.milli_celsius_ < b.milli_celsius_;
    }
    friend constexpr bool operator>(Temperature a, Temperature b) noexcept { return b < a; }
    friend constexpr bool operator<=(Temperature a, Temperature b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(Temperature a, Temperature b) noexcept { return !(a < b); }

private:
    std::int32_t milli_celsius_ = 0;
};

/// A signed temperature difference in exact millidegrees Celsius.
struct TemperatureDelta {
    std::int64_t milli_celsius = 0;

    friend constexpr bool operator==(TemperatureDelta a, TemperatureDelta b) noexcept {
        return a.milli_celsius == b.milli_celsius;
    }
    friend constexpr bool operator!=(TemperatureDelta a, TemperatureDelta b) noexcept { return !(a == b); }
    friend constexpr bool operator<(TemperatureDelta a, TemperatureDelta b) noexcept {
        return a.milli_celsius < b.milli_celsius;
    }
};

/// Exact difference (a - b) in millidegrees.
[[nodiscard]] Result<TemperatureDelta> difference(Temperature a, Temperature b);

/// A derating or capacity fraction in basis points (1 bp = 0.01 %).
///
/// Basis points keep derating authority exact: no directive is ever derived
/// from a rounded percentage.
class BasisPoints {
public:
    static constexpr std::uint32_t kFull = 10000;

    constexpr BasisPoints() noexcept = default;

    [[nodiscard]] static Result<BasisPoints> from_value(std::int64_t value);
    [[nodiscard]] static constexpr BasisPoints from_value_unchecked(std::uint32_t value) noexcept {
        BasisPoints bp;
        bp.value_ = value;
        return bp;
    }
    [[nodiscard]] static constexpr BasisPoints full() noexcept { return from_value_unchecked(kFull); }
    [[nodiscard]] static constexpr BasisPoints none() noexcept { return from_value_unchecked(0); }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    friend constexpr bool operator==(BasisPoints a, BasisPoints b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(BasisPoints a, BasisPoints b) noexcept { return !(a == b); }
    friend constexpr bool operator<(BasisPoints a, BasisPoints b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator>(BasisPoints a, BasisPoints b) noexcept { return b < a; }
    friend constexpr bool operator<=(BasisPoints a, BasisPoints b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(BasisPoints a, BasisPoints b) noexcept { return !(a < b); }

private:
    std::uint32_t value_ = 0;
};

[[nodiscard]] std::string to_string(Temperature temperature);
[[nodiscard]] std::string to_string(TemperatureDelta delta);
[[nodiscard]] std::string to_string(BasisPoints basis_points);

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_QUANTITY_HPP

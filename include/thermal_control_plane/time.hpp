// Thermal Control Plane — monotonic ticks and freshness windows.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_TIME_HPP
#define THERMAL_CONTROL_PLANE_TIME_HPP

#include <cstdint>
#include <string>

#include "thermal_control_plane/error.hpp"

namespace thermal_control_plane {

/// A monotonic tick supplied by an injected clock.
///
/// Freshness in this runtime is decided by monotonic ticks and observation
/// sequence numbers. Wall-clock time is carried alongside observations as
/// advisory metadata and is never the sole authority for freshness.
struct Tick {
    std::uint64_t value = 0;

    friend constexpr bool operator==(Tick a, Tick b) noexcept { return a.value == b.value; }
    friend constexpr bool operator!=(Tick a, Tick b) noexcept { return a.value != b.value; }
    friend constexpr bool operator<(Tick a, Tick b) noexcept { return a.value < b.value; }
    friend constexpr bool operator>(Tick a, Tick b) noexcept { return b < a; }
    friend constexpr bool operator<=(Tick a, Tick b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(Tick a, Tick b) noexcept { return !(a < b); }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return value == 0; }
};

/// A non-negative elapsed duration in milliseconds.
struct Duration {
    std::int64_t millis = 0;

    [[nodiscard]] static Result<Duration> from_millis(std::int64_t millis);
    [[nodiscard]] static constexpr Duration from_millis_unchecked(std::int64_t millis) noexcept {
        Duration d;
        d.millis = millis;
        return d;
    }

    friend constexpr bool operator==(Duration a, Duration b) noexcept { return a.millis == b.millis; }
    friend constexpr bool operator!=(Duration a, Duration b) noexcept { return !(a == b); }
    friend constexpr bool operator<(Duration a, Duration b) noexcept { return a.millis < b.millis; }
    friend constexpr bool operator>(Duration a, Duration b) noexcept { return b < a; }
    friend constexpr bool operator<=(Duration a, Duration b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(Duration a, Duration b) noexcept { return !(a < b); }
};

/// Elapsed monotonic time between two ticks.
///
/// A tick that appears to move backwards is a clock fault: the elapsed
/// duration is reported as refused rather than silently treated as zero.
[[nodiscard]] Result<Duration> elapsed(Tick from, Tick to);

/// A freshness window expressed in monotonic milliseconds.
struct FreshnessWindow {
    Duration max_age = Duration::from_millis_unchecked(5000);
};

/// Classification of an observation against a freshness window.
enum class FreshnessState : std::uint8_t {
    /// Unset observation: no evidence has ever been supplied.
    Unstamped = 0,
    /// Within the window and not in the future.
    Fresh = 1,
    /// Older than the window.
    Stale = 2,
    /// Stamped ahead of the evaluating clock.
    Future = 3,
};

[[nodiscard]] constexpr std::string_view to_string(FreshnessState state) noexcept {
    switch (state) {
        case FreshnessState::Unstamped: return "UNSTAMPED";
        case FreshnessState::Fresh: return "FRESH";
        case FreshnessState::Stale: return "STALE";
        case FreshnessState::Future: return "FUTURE";
    }
    return "UNRECOGNISED_FRESHNESS_STATE";
}

/// Classify an observation stamp against the evaluating clock.
///
/// A future stamp is never treated as fresh: a producer whose clock runs
/// ahead must not be able to authorise action indefinitely.
[[nodiscard]] FreshnessState classify_freshness(Tick now, Tick observed, FreshnessWindow window) noexcept;

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_TIME_HPP

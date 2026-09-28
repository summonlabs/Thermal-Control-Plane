// Thermal Control Plane — generation-bound thermal limits.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_LIMITS_HPP
#define THERMAL_CONTROL_PLANE_LIMITS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/identity.hpp"
#include "thermal_control_plane/quantity.hpp"

namespace thermal_control_plane {

/// The ordered thermal limit kinds.
///
/// The declaration order is the severity order and the numeric values are
/// part of the encodings, so the ordering can never be silently permuted.
enum class LimitKind : std::uint8_t {
    Advisory = 1,
    Warning = 2,
    Derate = 3,
    Critical = 4,
    Shutdown = 5,
};

inline constexpr std::size_t kLimitKindCount = 5;

[[nodiscard]] constexpr std::size_t limit_index(LimitKind kind) noexcept {
    return static_cast<std::size_t>(kind) - 1U;
}

/// Severity of a limit kind; higher is more severe.
[[nodiscard]] constexpr std::uint8_t limit_severity(LimitKind kind) noexcept {
    return static_cast<std::uint8_t>(kind);
}

[[nodiscard]] constexpr std::string_view to_string(LimitKind kind) noexcept {
    switch (kind) {
        case LimitKind::Advisory: return "ADVISORY";
        case LimitKind::Warning: return "WARNING";
        case LimitKind::Derate: return "DERATE";
        case LimitKind::Critical: return "CRITICAL";
        case LimitKind::Shutdown: return "SHUTDOWN";
    }
    return "UNRECOGNISED_LIMIT_KIND";
}

[[nodiscard]] constexpr bool is_valid_limit_kind(std::uint8_t raw) noexcept {
    return raw >= static_cast<std::uint8_t>(LimitKind::Advisory) &&
           raw <= static_cast<std::uint8_t>(LimitKind::Shutdown);
}

/// A limit threshold that is either explicitly defined or explicitly absent.
///
/// Absent is not zero: a facility that has not published a shutdown limit has
/// no shutdown limit, and this runtime must not invent one. Undefined
/// thresholds encode their value as zero and refuse any other value, so a
/// reserved field that holds data is detected rather than ignored.
struct Threshold {
    bool defined = false;
    std::int32_t milli_celsius = 0;

    friend constexpr bool operator==(const Threshold& a, const Threshold& b) noexcept {
        return a.defined == b.defined && (!a.defined || a.milli_celsius == b.milli_celsius);
    }
    friend constexpr bool operator!=(const Threshold& a, const Threshold& b) noexcept { return !(a == b); }
};

/// The limits published for one thermal scope.
struct ThermalLimitSet {
    LimitSetId id{};
    LimitGeneration generation{};
    ScopeRef scope{};
    ScopeGeneration scope_generation{};
    TopologyGeneration topology_generation{};
    std::array<Threshold, kLimitKindCount> thresholds{};
    BasisPoints max_derate{};

    [[nodiscard]] Threshold& operator[](LimitKind kind) noexcept { return thresholds[limit_index(kind)]; }
    [[nodiscard]] const Threshold& operator[](LimitKind kind) const noexcept {
        return thresholds[limit_index(kind)];
    }
    [[nodiscard]] bool any_defined() const noexcept;
    [[nodiscard]] std::size_t defined_count() const noexcept;
};

/// Structural and ordering validation of a limit set.
///
/// The ordering rule is part of the contract: advisory <= warning <= derate <=
/// critical <= shutdown for every defined pair. A set that violates it is
/// refused rather than repaired, because a repaired ordering would silently
/// change the facility's declared operating envelope.
[[nodiscard]] Status validate(const ThermalLimitSet& limits);

/// A bounded registry of the currently published limit sets.
class LimitRegistry {
public:
    explicit LimitRegistry(std::size_t max_entries = 256);

    /// Publish or replace the limits for a scope.
    ///
    /// Replacing requires a strictly newer limit generation. Re-publishing
    /// identical limits at the same generation is an accepted no-op; publishing
    /// different limits at the same generation is a conflict.
    [[nodiscard]] Status put(const ThermalLimitSet& limits);

    /// Remove the limits for a scope at a strictly newer limit generation.
    [[nodiscard]] Status remove(ScopeRef scope, LimitGeneration generation);

    [[nodiscard]] const ThermalLimitSet* find(ScopeRef scope) const;
    [[nodiscard]] std::vector<ScopeRef> scopes() const;
    [[nodiscard]] std::size_t size() const noexcept { return limits_.size(); }
    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }

private:
    std::size_t max_entries_ = 256;
    std::map<ScopeRef, ThermalLimitSet> limits_;
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_LIMITS_HPP

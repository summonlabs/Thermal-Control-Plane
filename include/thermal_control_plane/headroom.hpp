// Thermal Control Plane — thermal headroom assessment.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_HEADROOM_HPP
#define THERMAL_CONTROL_PLANE_HEADROOM_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/evidence.hpp"
#include "thermal_control_plane/identity.hpp"
#include "thermal_control_plane/limits.hpp"
#include "thermal_control_plane/quantity.hpp"

namespace thermal_control_plane {

/// Whether headroom is an exact number or an explicit absence of one.
enum class HeadroomState : std::uint8_t {
    /// An exact integer number of millidegrees.
    Known = 1,
    /// No usable number exists.
    Unknown = 2,
    /// A source answered, and the answer was that it cannot answer.
    Indeterminate = 3,
};

[[nodiscard]] constexpr std::string_view to_string(HeadroomState state) noexcept {
    switch (state) {
        case HeadroomState::Known: return "KNOWN";
        case HeadroomState::Unknown: return "UNKNOWN";
        case HeadroomState::Indeterminate: return "INDETERMINATE";
    }
    return "UNRECOGNISED_HEADROOM_STATE";
}

/// Why headroom is not known.
///
/// The numeric values are the precedence order used when several scopes
/// disagree: a lower value is reported first. Every reason is an explicit
/// statement about the world; none of them means "assume safe".
enum class HeadroomReason : std::uint8_t {
    Interlocked = 0,
    AwaitingRevalidation = 1,
    SensorAbsent = 2,
    SensorFaulted = 3,
    SourceUnavailable = 4,
    NotSupported = 5,
    EvidenceIndeterminate = 6,
    EvidenceStale = 7,
    EvidenceFuture = 8,
    GenerationMismatch = 9,
    NoEvidence = 10,
    LimitUndefined = 11,
    None = 12,
};

[[nodiscard]] constexpr std::string_view to_string(HeadroomReason reason) noexcept {
    switch (reason) {
        case HeadroomReason::Interlocked: return "INTERLOCKED";
        case HeadroomReason::AwaitingRevalidation: return "AWAITING_REVALIDATION";
        case HeadroomReason::SensorAbsent: return "SENSOR_ABSENT";
        case HeadroomReason::SensorFaulted: return "SENSOR_FAULTED";
        case HeadroomReason::SourceUnavailable: return "SOURCE_UNAVAILABLE";
        case HeadroomReason::NotSupported: return "NOT_SUPPORTED";
        case HeadroomReason::EvidenceIndeterminate: return "EVIDENCE_INDETERMINATE";
        case HeadroomReason::EvidenceStale: return "EVIDENCE_STALE";
        case HeadroomReason::EvidenceFuture: return "EVIDENCE_FUTURE";
        case HeadroomReason::GenerationMismatch: return "GENERATION_MISMATCH";
        case HeadroomReason::NoEvidence: return "NO_EVIDENCE";
        case HeadroomReason::LimitUndefined: return "LIMIT_UNDEFINED";
        case HeadroomReason::None: return "NONE";
    }
    return "UNRECOGNISED_HEADROOM_REASON";
}

[[nodiscard]] constexpr std::uint8_t reason_precedence(HeadroomReason reason) noexcept {
    return static_cast<std::uint8_t>(reason);
}

/// Translate an evidence reason into a headroom reason.
[[nodiscard]] constexpr HeadroomReason to_headroom_reason(EvidenceReason reason) noexcept {
    switch (reason) {
        case EvidenceReason::None: return HeadroomReason::None;
        case EvidenceReason::SensorAbsent: return HeadroomReason::SensorAbsent;
        case EvidenceReason::SensorFaulted: return HeadroomReason::SensorFaulted;
        case EvidenceReason::SourceUnavailable: return HeadroomReason::SourceUnavailable;
        case EvidenceReason::NotSupported: return HeadroomReason::NotSupported;
        case EvidenceReason::Indeterminate: return HeadroomReason::EvidenceIndeterminate;
        case EvidenceReason::AwaitingRevalidation: return HeadroomReason::AwaitingRevalidation;
        case EvidenceReason::StaleObservation: return HeadroomReason::EvidenceStale;
        case EvidenceReason::FutureObservation: return HeadroomReason::EvidenceFuture;
    }
    return HeadroomReason::NoEvidence;
}

/// Headroom against one limit kind.
///
/// The delta is an exact integer count of millidegrees and is only meaningful
/// when the state is Known. When the state is not Known the delta is exactly
/// zero and carries no meaning whatsoever: it is not "no headroom" and it is
/// not "full headroom".
///
/// over_limit is true when the observation has reached or passed the
/// threshold, that is when the delta is zero or negative.
struct Headroom {
    HeadroomState state = HeadroomState::Unknown;
    HeadroomReason reason = HeadroomReason::NoEvidence;
    TemperatureDelta delta{};
    Temperature observed{};
    Threshold limit{};
    LimitKind basis = LimitKind::Warning;
    bool over_limit = false;

    [[nodiscard]] bool known() const noexcept { return state == HeadroomState::Known; }
};

/// Exact headroom of a measured temperature against a defined threshold.
[[nodiscard]] Headroom compute_headroom(Temperature observed, const Threshold& limit, LimitKind basis);

/// Explicit absence of headroom.
[[nodiscard]] Headroom unknown_headroom(HeadroomReason reason, LimitKind basis);

/// Headroom for one scope across every limit kind.
struct ScopeHeadroom {
    ScopeRef scope{};
    bool has_limits = false;
    LimitGeneration limits_generation{};
    std::array<Headroom, kLimitKindCount> by_kind{};
    /// Tightest defined headroom in the scope: the smallest delta across every
    /// defined threshold. It is therefore negative as soon as any declared
    /// threshold, including an advisory one, has been reached. Ties are broken
    /// towards the more severe limit kind.
    Headroom governing{};
    std::size_t sensors = 0;
    std::size_t fresh_measured = 0;
    bool has_hottest = false;
    Temperature hottest{};
    SensorRef hottest_sensor{};
    ObservationSequence hottest_sequence{};
    Tick hottest_at{};
};

/// Compute the headroom of one scope from its limits and its evidence view.
[[nodiscard]] ScopeHeadroom evaluate_headroom(const ThermalLimitSet* limits, const ScopeEvidenceView& view);

/// Facility-wide headroom.
///
/// The facility headroom is Known only when every scope that has published
/// limits has Known headroom. A single scope with missing, stale or
/// indeterminate evidence makes the facility headroom unknown: the least
/// informative scope governs, never the most reassuring one.
struct FacilityHeadroom {
    HeadroomState state = HeadroomState::Unknown;
    HeadroomReason reason = HeadroomReason::NoEvidence;
    bool has_worst = false;
    TemperatureDelta worst_delta{};
    ScopeRef worst_scope{};
    LimitKind worst_basis = LimitKind::Warning;
    Temperature worst_observed{};
    SensorRef worst_sensor{};
    std::size_t scopes_total = 0;
    std::size_t scopes_with_limits = 0;
    std::size_t scopes_known = 0;
    std::size_t scopes_unknown = 0;
    std::size_t scopes_indeterminate = 0;
};

[[nodiscard]] FacilityHeadroom aggregate_headroom(const std::vector<ScopeHeadroom>& scopes);

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_HEADROOM_HPP

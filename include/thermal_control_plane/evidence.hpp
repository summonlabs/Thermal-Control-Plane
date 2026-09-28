// Thermal Control Plane — externally supplied thermal evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_EVIDENCE_HPP
#define THERMAL_CONTROL_PLANE_EVIDENCE_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/identity.hpp"
#include "thermal_control_plane/quantity.hpp"
#include "thermal_control_plane/time.hpp"

namespace thermal_control_plane {

/// How a temperature value came to exist.
///
/// Only Measured is usable as authority. Derived values are carried for
/// explanation and never promote a scope out of an unknown state.
enum class ObservationQuality : std::uint8_t {
    Measured = 1,
    Derived = 2,
    Unavailable = 3,
    Unsupported = 4,
    Indeterminate = 5,
};

[[nodiscard]] constexpr std::string_view to_string(ObservationQuality quality) noexcept {
    switch (quality) {
        case ObservationQuality::Measured: return "MEASURED";
        case ObservationQuality::Derived: return "DERIVED";
        case ObservationQuality::Unavailable: return "UNAVAILABLE";
        case ObservationQuality::Unsupported: return "UNSUPPORTED";
        case ObservationQuality::Indeterminate: return "INDETERMINATE";
    }
    return "UNRECOGNISED_OBSERVATION_QUALITY";
}

/// Why an observation is not a usable measurement.
///
/// The distinction matters: a sensor that does not exist, a sensor that has
/// faulted, and a source that is offline are three different operational
/// facts, and none of them is zero degrees or unlimited headroom.
enum class EvidenceReason : std::uint8_t {
    None = 0,
    SensorAbsent = 1,
    SensorFaulted = 2,
    SourceUnavailable = 3,
    NotSupported = 4,
    Indeterminate = 5,
    AwaitingRevalidation = 6,
    StaleObservation = 7,
    FutureObservation = 8,
};

[[nodiscard]] constexpr std::string_view to_string(EvidenceReason reason) noexcept {
    switch (reason) {
        case EvidenceReason::None: return "NONE";
        case EvidenceReason::SensorAbsent: return "SENSOR_ABSENT";
        case EvidenceReason::SensorFaulted: return "SENSOR_FAULTED";
        case EvidenceReason::SourceUnavailable: return "SOURCE_UNAVAILABLE";
        case EvidenceReason::NotSupported: return "NOT_SUPPORTED";
        case EvidenceReason::Indeterminate: return "INDETERMINATE";
        case EvidenceReason::AwaitingRevalidation: return "AWAITING_REVALIDATION";
        case EvidenceReason::StaleObservation: return "STALE_OBSERVATION";
        case EvidenceReason::FutureObservation: return "FUTURE_OBSERVATION";
    }
    return "UNRECOGNISED_EVIDENCE_REASON";
}

/// Deterministic severity of an evidence reason; lower is more severe.
[[nodiscard]] constexpr std::uint8_t reason_precedence(EvidenceReason reason) noexcept {
    switch (reason) {
        case EvidenceReason::AwaitingRevalidation: return 0;
        case EvidenceReason::SensorAbsent: return 1;
        case EvidenceReason::SensorFaulted: return 2;
        case EvidenceReason::SourceUnavailable: return 3;
        case EvidenceReason::NotSupported: return 4;
        case EvidenceReason::Indeterminate: return 5;
        case EvidenceReason::StaleObservation: return 6;
        case EvidenceReason::FutureObservation: return 7;
        case EvidenceReason::None: return 8;
    }
    return 9;
}

/// A single externally produced temperature observation.
///
/// A temperature observation is evidence, not authority. It carries the
/// generation of the evidence stream it belongs to, the generation of the
/// scope object it was taken against, a per-sensor sequence number and the
/// incarnation of the producer, so that reordering, replay and producer
/// restart are all detectable.
struct TemperatureObservation {
    ObservationId id{};
    SensorRef sensor{};
    ScopeRef scope{};
    ScopeGeneration scope_generation{};
    EvidenceGeneration generation{};
    ObservationSequence sequence{};
    ProcessIncarnation source{};
    Tick observed_at{};
    /// Advisory wall-clock stamp. Never used to decide freshness.
    std::int64_t observed_at_unix_millis = 0;
    Temperature temperature{};
    ObservationQuality quality = ObservationQuality::Measured;
    EvidenceReason reason = EvidenceReason::None;
};

/// A batch of observations from one evidence generation.
struct ObservationBatch {
    EvidenceGeneration generation{};
    TopologyGeneration topology_generation{};
    ProcessIncarnation source{};
    std::vector<TemperatureObservation> observations;
};

/// Result of a successful ingest.
enum class IngestDisposition : std::uint8_t {
    /// At least one new observation advanced the stored evidence.
    Applied = 1,
    /// Every observation was an exact duplicate of stored evidence.
    Duplicate = 2,
};

struct IngestReport {
    IngestDisposition disposition = IngestDisposition::Applied;
    std::uint32_t applied = 0;
    std::uint32_t duplicates = 0;
    EvidenceGeneration generation{};
};

/// Resource bounds for the evidence store.
struct EvidenceBounds {
    std::size_t max_sensors = 4096;
    std::size_t max_scopes = 512;
    std::size_t max_observations_per_batch = 1024;
};

/// Stored state for one sensor within the current evidence generation.
struct SensorSlot {
    TemperatureObservation latest{};
    bool present = false;
    /// Highest sequence accepted for this sensor in the current generation.
    ObservationSequence accepted_sequence{};
};

/// Per-scope aggregation of stored evidence.
struct ScopeEvidenceView {
    ScopeRef scope{};
    std::size_t sensors = 0;
    std::size_t fresh_measured = 0;
    std::size_t stale_measured = 0;
    std::size_t future_measured = 0;
    std::size_t not_measured = 0;
    std::size_t awaiting_revalidation = 0;

    /// Hottest fresh measured temperature, when one exists.
    bool has_hottest = false;
    Temperature hottest{};
    SensorRef hottest_sensor{};
    ObservationSequence hottest_sequence{};
    Tick hottest_at{};

    /// Highest-precedence reason among the sensors that are not usable.
    EvidenceReason primary_reason = EvidenceReason::None;
    SensorRef primary_reason_sensor{};
    /// True when the scope has at least one sensor and every sensor is fresh
    /// and measured.
    bool all_fresh_measured = false;
    /// True when the scope has no sensors at all in the current generation.
    bool empty = true;

    /// Scope generation the stored observations were taken against. Evidence
    /// taken against one scope generation cannot be compared with limits
    /// published at another, so a mismatch leaves headroom unknown.
    bool has_scope_generation = false;
    ScopeGeneration scope_generation{};
    /// True when the scope's sensors disagree about the scope generation.
    bool scope_generation_mixed = false;
};

/// The authoritative evidence store.
///
/// The store holds a single evidence generation. Within a generation, each
/// sensor has a monotonically increasing sequence. Reordered, conflicting,
/// duplicated and cross-generation submissions are refused with distinct
/// codes, and a batch is applied atomically: a batch that contains any
/// refused observation changes nothing.
class EvidenceStore {
public:
    explicit EvidenceStore(EvidenceBounds bounds = {});

    [[nodiscard]] EvidenceGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] TopologyGeneration topology_generation() const noexcept { return topology_generation_; }
    [[nodiscard]] const EvidenceBounds& bounds() const noexcept { return bounds_; }

    /// Establish the evidence generation at construction or after recovery.
    ///
    /// Every observation previously held is discarded: recovered dynamic
    /// evidence is not fresh evidence for a new generation.
    Status reset(EvidenceGeneration generation, TopologyGeneration topology_generation);

    /// Advance to a strictly newer evidence generation.
    ///
    /// Advancing invalidates every observation taken under the previous
    /// generation. The expected current generation must be supplied and must
    /// match, so a stale advancer cannot roll the stream backwards.
    Status advance_generation(EvidenceGeneration expected_current,
                              EvidenceGeneration next,
                              TopologyGeneration topology_generation);

    /// Submit a batch of observations for the current generation.
    [[nodiscard]] Result<IngestReport> ingest(const ObservationBatch& batch, Tick now);

    /// Look up the slot for a sensor.
    [[nodiscard]] const SensorSlot* find(SensorRef sensor) const;

    /// Aggregate the evidence for one scope as of a monotonic tick.
    [[nodiscard]] ScopeEvidenceView view(ScopeRef scope, Tick now, FreshnessWindow window) const;

    /// Every scope currently known to the store, in canonical order.
    [[nodiscard]] std::vector<ScopeRef> scopes() const;

    [[nodiscard]] std::size_t sensor_count() const noexcept { return slots_.size(); }

    /// Mark observations taken before a tick as requiring revalidation.
    ///
    /// Recovery from durable state uses this: any measurement stamped before
    /// the recovery point is retained for explanation but is not current
    /// evidence, no matter when it is delivered. The mark clears as soon as an
    /// observation stamped at or after the recovery point is applied.
    void require_revalidation(Tick at) noexcept;
    [[nodiscard]] bool revalidation_required() const noexcept { return revalidation_required_; }
    [[nodiscard]] Tick revalidation_tick() const noexcept { return revalidation_tick_; }

private:
    struct ScopeIndex {
        std::map<SensorRef, bool> sensors;
    };

    EvidenceBounds bounds_{};
    EvidenceGeneration generation_{};
    TopologyGeneration topology_generation_{};
    bool revalidation_required_ = false;
    Tick revalidation_tick_{};
    std::map<SensorRef, SensorSlot> slots_;
    std::map<ScopeRef, ScopeIndex> scope_index_;
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_EVIDENCE_HPP

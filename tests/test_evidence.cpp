// Thermal Control Plane — evidence ordering, freshness and atomicity proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

namespace {

using namespace thermal_control_plane;

const ScopeRef kScope = make_scope(1, 10, 100);
const SensorRef kSensorA{1};
const SensorRef kSensorB{2};

}  // namespace

TCPLANE_CASE(evidence, generation_must_be_established_first) {
    EvidenceStore store;
    TCPLANE_PHASE("ingest before a generation exists is refused");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorA, Tick{5}, 60000,
                                                                 ObservationSequence{1})}),
                                    Tick{5}),
                       ErrorCode::EVIDENCE_REQUIRED);

    TCPLANE_PHASE("a zero generation cannot be established");
    TCPLANE_STATUS_ERROR_CODE(store.reset(EvidenceGeneration{0}, TopologyGeneration{1}),
                              ErrorCode::INVALID_ARGUMENT);
    TCPLANE_PHASE("a zero topology generation cannot be established");
    TCPLANE_STATUS_ERROR_CODE(store.reset(EvidenceGeneration{1}, TopologyGeneration{0}),
                              ErrorCode::INVALID_ARGUMENT);
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));
}

TCPLANE_CASE(evidence, cross_generation_batches_are_fenced) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{5}, TopologyGeneration{2}));

    TCPLANE_PHASE("a superseded generation is refused");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{4}, TopologyGeneration{2},
                                               {make_observation(kScope, kSensorA, Tick{5}, 60000,
                                                                 ObservationSequence{1}, EvidenceGeneration{4})}),
                                    Tick{5}),
                       ErrorCode::EVIDENCE_SUPERSEDED);

    TCPLANE_PHASE("an unestablished future generation is refused");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{6}, TopologyGeneration{2},
                                               {make_observation(kScope, kSensorA, Tick{5}, 60000,
                                                                 ObservationSequence{1}, EvidenceGeneration{6})}),
                                    Tick{5}),
                       ErrorCode::FUTURE_EVIDENCE_GENERATION);

    TCPLANE_PHASE("a superseded topology generation is refused");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{5}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorA, Tick{5}, 60000,
                                                                 ObservationSequence{1})}),
                                    Tick{5}),
                       ErrorCode::STALE_TOPOLOGY_GENERATION);

    TCPLANE_PHASE("generation advance requires the expected current generation");
    TCPLANE_STATUS_ERROR_CODE(store.advance_generation(EvidenceGeneration{4}, EvidenceGeneration{6},
                                                       TopologyGeneration{2}),
                              ErrorCode::STALE_EVIDENCE_GENERATION);
    TCPLANE_STATUS_ERROR_CODE(store.advance_generation(EvidenceGeneration{9}, EvidenceGeneration{6},
                                                       TopologyGeneration{2}),
                              ErrorCode::FUTURE_EVIDENCE_GENERATION);
    TCPLANE_STATUS_ERROR_CODE(store.advance_generation(EvidenceGeneration{5}, EvidenceGeneration{5},
                                                       TopologyGeneration{2}),
                              ErrorCode::INVALID_ARGUMENT);
    TCPLANE_STATUS_OK(store.advance_generation(EvidenceGeneration{5}, EvidenceGeneration{6},
                                               TopologyGeneration{2}));
    TCPLANE_CHECK_EQ(store.generation().value, std::uint64_t{6});
}

TCPLANE_CASE(evidence, sequence_ordering_and_duplicates) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));

    TCPLANE_PHASE("the first observation is applied");
    const auto first = store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorA, Tick{10}, 60000,
                                                                 ObservationSequence{5})}),
                                    Tick{10});
    TCPLANE_OK(first);
    TCPLANE_CHECK(first.value().disposition == IngestDisposition::Applied);
    TCPLANE_CHECK_EQ(first.value().applied, std::uint32_t{1});

    TCPLANE_PHASE("re-delivering the same payload is a duplicate, not new evidence");
    const auto duplicate = store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                                   {make_observation(kScope, kSensorA, Tick{10}, 60000,
                                                                     ObservationSequence{5})}),
                                        Tick{11});
    TCPLANE_OK(duplicate);
    TCPLANE_CHECK(duplicate.value().disposition == IngestDisposition::Duplicate);
    TCPLANE_CHECK_EQ(duplicate.value().applied, std::uint32_t{0});
    TCPLANE_CHECK_EQ(duplicate.value().duplicates, std::uint32_t{1});

    TCPLANE_PHASE("reusing an accepted sequence with a different value conflicts");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorA, Tick{10}, 61000,
                                                                 ObservationSequence{5})}),
                                    Tick{11}),
                       ErrorCode::EVIDENCE_CONFLICT);

    TCPLANE_PHASE("a reordered observation is superseded, never applied");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorA, Tick{9}, 59000,
                                                                 ObservationSequence{4})}),
                                    Tick{11}),
                       ErrorCode::EVIDENCE_SUPERSEDED);

    TCPLANE_PHASE("a newer sequence advances the stored evidence");
    const auto newer = store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorA, Tick{12}, 62000,
                                                                 ObservationSequence{6})}),
                                    Tick{12});
    TCPLANE_OK(newer);
    TCPLANE_CHECK_EQ(newer.value().applied, std::uint32_t{1});
    const SensorSlot* slot = store.find(kSensorA);
    TCPLANE_CHECK(slot != nullptr);
    TCPLANE_CHECK_EQ(slot->latest.temperature.milli_celsius(), 62000);
}

TCPLANE_CASE(evidence, batches_are_atomic) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensorA, Tick{10}, 60000,
                                                                ObservationSequence{5})}),
                                   Tick{10}));

    TCPLANE_PHASE("one bad observation refuses the whole batch");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorB, Tick{10}, 50000,
                                                                 ObservationSequence{1}),
                                                make_observation(kScope, kSensorA, Tick{9}, 1,
                                                                 ObservationSequence{4})}),
                                    Tick{10}),
                       ErrorCode::EVIDENCE_SUPERSEDED);
    TCPLANE_CHECK(store.find(kSensorB) == nullptr);

    TCPLANE_PHASE("two conflicting observations for one sensor refuse the batch");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorB, Tick{10}, 50000,
                                                                 ObservationSequence{1}),
                                                make_observation(kScope, kSensorB, Tick{10}, 51000,
                                                                 ObservationSequence{2})}),
                                    Tick{10}),
                       ErrorCode::EVIDENCE_CONFLICT);
    TCPLANE_CHECK(store.find(kSensorB) == nullptr);
}

TCPLANE_CASE(evidence, malformed_observations_are_refused) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));

    TCPLANE_PHASE("an observation without a sensor, scope, id, generation or stamp is refused");
    TemperatureObservation base = make_observation(kScope, kSensorA, Tick{10}, 60000, ObservationSequence{1});

    TemperatureObservation no_sensor = base;
    no_sensor.sensor = SensorRef{0};
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {no_sensor}), Tick{10}),
                       ErrorCode::INVALID_ARGUMENT);

    TemperatureObservation no_scope = base;
    no_scope.scope = ScopeRef{};
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {no_scope}), Tick{10}),
                       ErrorCode::INVALID_ARGUMENT);

    TemperatureObservation no_scope_generation = base;
    no_scope_generation.scope_generation = ScopeGeneration{0};
    TCPLANE_ERROR_CODE(
        store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {no_scope_generation}), Tick{10}),
        ErrorCode::INVALID_ARGUMENT);

    TemperatureObservation no_id = base;
    no_id.id = ObservationId{0};
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {no_id}), Tick{10}),
                       ErrorCode::INVALID_ARGUMENT);

    TemperatureObservation no_stamp = base;
    no_stamp.observed_at = Tick{0};
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {no_stamp}), Tick{10}),
                       ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("a measured observation may not carry a failure reason");
    TemperatureObservation reasoned = base;
    reasoned.reason = EvidenceReason::SensorFaulted;
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {reasoned}), Tick{10}),
                       ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("an unusable observation must carry a reason and no temperature");
    TemperatureObservation silent = base;
    silent.quality = ObservationQuality::Unavailable;
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {silent}), Tick{10}),
                       ErrorCode::INVALID_ARGUMENT);

    TemperatureObservation valued = base;
    valued.quality = ObservationQuality::Unavailable;
    valued.reason = EvidenceReason::SensorFaulted;
    valued.temperature = Temperature::from_milli_celsius_unchecked(1234);
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, {valued}), Tick{10}),
                       ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("an observation stamped ahead of the clock is refused");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, kSensorA, Tick{20}, 60000,
                                                                 ObservationSequence{1})}),
                                    Tick{10}),
                       ErrorCode::EVIDENCE_FUTURE);
    TCPLANE_CHECK(store.find(kSensorA) == nullptr);
}

TCPLANE_CASE(evidence, sensor_binding_and_batch_bounds) {
    EvidenceStore store(EvidenceBounds{2, 2, 2});
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));

    TCPLANE_PHASE("a batch larger than the configured bound is refused");
    std::vector<TemperatureObservation> many;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        many.push_back(make_observation(kScope, SensorRef{i}, Tick{5}, 60000, ObservationSequence{1}));
    }
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1}, many), Tick{5}),
                       ErrorCode::RESOURCE_EXHAUSTED);

    TCPLANE_PHASE("the sensor bound is enforced");
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(kScope, SensorRef{1}, Tick{5}, 60000,
                                                                ObservationSequence{1}),
                                               make_observation(kScope, SensorRef{2}, Tick{5}, 61000,
                                                                ObservationSequence{1})}),
                                   Tick{5}));
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(kScope, SensorRef{3}, Tick{5}, 62000,
                                                                 ObservationSequence{1})}),
                                    Tick{5}),
                       ErrorCode::RESOURCE_EXHAUSTED);

    TCPLANE_PHASE("a sensor cannot be silently rebound to another scope");
    TCPLANE_ERROR_CODE(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                               {make_observation(make_scope(1, 99, 100), SensorRef{1}, Tick{6}, 60000,
                                                                 ObservationSequence{2})}),
                                    Tick{6}),
                       ErrorCode::EVIDENCE_CONFLICT);
}

TCPLANE_CASE(evidence, scope_view_selects_hottest_and_reports_reasons) {
    EvidenceStore store;
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{1}, TopologyGeneration{1}));
    const FreshnessWindow window{Duration::from_millis_unchecked(100)};
    const ScopeRef other = make_scope(1, 11, 100);

    TCPLANE_PHASE("an unknown scope has an empty view and no hottest value");
    const ScopeEvidenceView unknown = store.view(other, Tick{100}, window);
    TCPLANE_CHECK(unknown.empty);
    TCPLANE_CHECK(!unknown.has_hottest);

    TCPLANE_PHASE("a fresh measurement is selected as the hottest");
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensorA, Tick{100}, 60000,
                                                                ObservationSequence{1}),
                                               make_observation(kScope, kSensorB, Tick{100}, 65000,
                                                                ObservationSequence{1})}),
                                   Tick{100}));
    ScopeEvidenceView view = store.view(kScope, Tick{100}, window);
    TCPLANE_CHECK(view.has_hottest);
    TCPLANE_CHECK_EQ(view.hottest.milli_celsius(), 65000);
    TCPLANE_CHECK(view.hottest_sensor == kSensorB);
    TCPLANE_CHECK(view.all_fresh_measured);

    TCPLANE_PHASE("an equal hottest value breaks the tie by sensor identity");
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{1}, TopologyGeneration{1},
                                              {make_observation(kScope, kSensorA, Tick{100}, 65000,
                                                                ObservationSequence{2})}),
                                   Tick{100}));
    view = store.view(kScope, Tick{100}, window);
    TCPLANE_CHECK_EQ(view.hottest.milli_celsius(), 65000);
    TCPLANE_CHECK(view.hottest_sensor == kSensorA);

    TCPLANE_PHASE("aged measurements cease to be usable and never imply comfort");
    view = store.view(kScope, Tick{300}, window);
    TCPLANE_CHECK(!view.has_hottest);
    TCPLANE_CHECK_EQ(view.stale_measured, std::size_t{2});
    TCPLANE_CHECK(view.primary_reason == EvidenceReason::StaleObservation);

    TCPLANE_PHASE("a faulted sensor is reported with its own reason");
    TCPLANE_STATUS_OK(store.reset(EvidenceGeneration{2}, TopologyGeneration{1}));
    TemperatureObservation faulted = make_observation(kScope, kSensorB, Tick{310}, 0, ObservationSequence{1},
                                                      EvidenceGeneration{2});
    faulted.quality = ObservationQuality::Unavailable;
    faulted.reason = EvidenceReason::SensorFaulted;
    faulted.temperature = Temperature::from_milli_celsius_unchecked(0);
    TCPLANE_OK(store.ingest(make_batch(EvidenceGeneration{2}, TopologyGeneration{1}, {faulted}), Tick{310}));
    view = store.view(kScope, Tick{310}, window);
    TCPLANE_CHECK(!view.has_hottest);
    TCPLANE_CHECK_EQ(view.not_measured, std::size_t{1});
    TCPLANE_CHECK(view.primary_reason == EvidenceReason::SensorFaulted);
}

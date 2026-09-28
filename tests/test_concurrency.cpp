// Thermal Control Plane — concurrency and ownership proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace thermal_control_plane;

SyntheticZoneSpec make_zone(std::uint64_t zone, std::uint64_t domain, std::uint64_t sensor) {
    SyntheticZoneSpec spec;
    spec.zone = ZoneRef{zone};
    spec.domain = DomainRef{domain};
    spec.sensor = SensorRef{sensor};
    spec.scope_generation = ScopeGeneration{1};
    return spec;
}

}  // namespace

TCPLANE_CASE(concurrency, parallel_attempts_are_serialised_and_consistent) {
    SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1},
                               {make_zone(10, 100, 1), make_zone(11, 101, 2)});
    auto manual = std::make_unique<ManualClock>(Tick{100});
    ManualClock* clock = manual.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(manual));
    TCPLANE_OK(created);
    ThermalRuntime& runtime = *created.value();
    TCPLANE_STATUS_OK(runtime.install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                             PolicyGeneration{}));
    for (std::size_t index = 0; index < facility.zone_count(); ++index) {
        TCPLANE_STATUS_OK(runtime.publish_limits(facility.limits_for(index, LimitGeneration{1})));
    }
    TCPLANE_STATUS_OK(runtime.declare_interlock_baseline(runtime.topology_generation(), {}));
    TCPLANE_OK(runtime.ingest(facility.observe(clock->peek(), BasisPoints::none(), runtime.incarnation(),
                                               runtime.evidence_generation())));

    const StateRevision base_revision = runtime.revision();
    constexpr int kThreads = 8;
    constexpr int kAttemptsPerThread = 25;
    std::atomic<int> granted{0};
    std::atomic<int> refused{0};
    std::atomic<int> replayed{0};

    TCPLANE_PHASE("eight threads submit attempts concurrently without deadlock or lost decision");
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&runtime, &granted, &refused, &replayed, thread]() {
            for (int index = 0; index < kAttemptsPerThread; ++index) {
                AuthorityAttempt attempt =
                    make_attempt(runtime, RequestId{static_cast<std::uint64_t>(thread * 1000 + index + 1)},
                                 AttemptId{static_cast<std::uint64_t>(thread * 1000 + index + 1)},
                                 "thread-" + std::to_string(thread) + "-" + std::to_string(index),
                                 AttemptKind::RequestDerate);
                attempt.requested_derate = BasisPoints::from_value_unchecked(3000);
                const auto outcome = runtime.authorize(attempt);
                if (!outcome.has_value()) {
                    refused.fetch_add(1);
                    continue;
                }
                switch (outcome.value().verdict) {
                    case AuthorityVerdict::Granted: granted.fetch_add(1); break;
                    case AuthorityVerdict::Replayed: replayed.fetch_add(1); break;
                    case AuthorityVerdict::Refused: refused.fetch_add(1); break;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    TCPLANE_PHASE("every attempt produced exactly one verdict");
    TCPLANE_CHECK_EQ(granted.load() + refused.load() + replayed.load(), kThreads * kAttemptsPerThread);
    TCPLANE_CHECK(granted.load() > 0);
    TCPLANE_CHECK_EQ(replayed.load(), 0);
    TCPLANE_PHASE("the revision advanced by exactly the number of granted attempts");
    TCPLANE_CHECK_EQ(runtime.revision().value,
                     base_revision.value + static_cast<std::uint64_t>(granted.load()));
    TCPLANE_CHECK(runtime.directive_tail(512).size() == static_cast<std::size_t>(granted.load()));
    TCPLANE_STATUS_OK(runtime.close());
}

TCPLANE_CASE(concurrency, parallel_ingest_from_distinct_sensors_is_lossless) {
    SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1}, {make_zone(10, 100, 1)});
    auto manual = std::make_unique<ManualClock>(Tick{100});
    ManualClock* clock = manual.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(manual));
    TCPLANE_OK(created);
    ThermalRuntime& runtime = *created.value();
    TCPLANE_STATUS_OK(runtime.install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                             PolicyGeneration{}));
    TCPLANE_STATUS_OK(runtime.publish_limits(facility.limits_for(0, LimitGeneration{1})));
    TCPLANE_STATUS_OK(runtime.declare_interlock_baseline(runtime.topology_generation(), {}));

    constexpr int kThreads = 6;
    constexpr int kBatches = 20;
    std::atomic<int> applied{0};
    std::atomic<int> failed{0};
    TCPLANE_PHASE("six threads ingest distinct sensors concurrently");
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&runtime, &facility, clock, &applied, &failed, thread]() {
            for (int batch = 0; batch < kBatches; ++batch) {
                ObservationBatch observations;
                observations.generation = runtime.evidence_generation();
                observations.topology_generation = runtime.topology_generation();
                observations.source = runtime.incarnation();
                TemperatureObservation observation;
                observation.id = ObservationId{static_cast<std::uint64_t>(thread * 100000 + batch + 1)};
                observation.sensor = SensorRef{static_cast<std::uint64_t>(thread) + 1};
                observation.scope = facility.scope_of(0);
                observation.scope_generation = ScopeGeneration{1};
                observation.generation = observations.generation;
                observation.sequence = ObservationSequence{static_cast<std::uint64_t>(batch) + 1};
                observation.source = observations.source;
                observation.observed_at = clock->peek();
                observation.temperature = Temperature::from_milli_celsius_unchecked(50000);
                observations.observations.push_back(observation);
                const auto report = runtime.ingest(observations);
                if (!report.has_value()) {
                    failed.fetch_add(1);
                    continue;
                }
                applied.fetch_add(static_cast<int>(report.value().applied));
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    TCPLANE_CHECK_EQ(failed.load(), 0);
    TCPLANE_CHECK_EQ(applied.load(), kThreads * kBatches);
    // Limits were published before any evidence existed, so the facility is
    // degraded and cannot step down until the recovery gate completes. The
    // observations themselves are all present.
    TCPLANE_CHECK(runtime.mode() == ThermalMode::Degraded);
    const auto headroom = runtime.headroom();
    TCPLANE_OK(headroom);
    TCPLANE_CHECK(headroom.value().state == HeadroomState::Known);
    TCPLANE_CHECK_EQ(headroom.value().worst_delta.milli_celsius, std::int64_t{-10000});
    TCPLANE_STATUS_OK(runtime.close());
}

TCPLANE_CASE(concurrency, readers_observe_consistent_state_while_writing) {
    SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1}, {make_zone(10, 100, 1)});
    auto manual = std::make_unique<ManualClock>(Tick{100});
    ManualClock* clock = manual.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(manual));
    TCPLANE_OK(created);
    ThermalRuntime& runtime = *created.value();
    TCPLANE_STATUS_OK(runtime.install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                             PolicyGeneration{}));
    TCPLANE_STATUS_OK(runtime.publish_limits(facility.limits_for(0, LimitGeneration{1})));
    TCPLANE_STATUS_OK(runtime.declare_interlock_baseline(runtime.topology_generation(), {}));

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> bad_reads{0};
    TCPLANE_PHASE("readers never observe a torn state while a writer mutates");
    std::thread reader([&runtime, &stop, &reads, &bad_reads]() {
        while (!stop.load()) {
            const StateRevision revision = runtime.revision();
            const ThermalMode mode = runtime.mode();
            const auto audit = runtime.audit_tail(4);
            if (!is_valid_mode(static_cast<std::uint8_t>(mode)) || revision.is_zero()) {
                bad_reads.fetch_add(1);
            }
            for (const AuditRecord& record : audit) {
                if (!is_valid_audit_kind(static_cast<std::uint8_t>(record.kind))) {
                    bad_reads.fetch_add(1);
                }
            }
            reads.fetch_add(1);
        }
    });

    while (reads.load() == 0) {
        std::this_thread::yield();
    }
    for (int index = 0; index < 2000; ++index) {
        TCPLANE_OK(runtime.ingest(facility.observe(clock->peek(), BasisPoints::none(),
                                                   runtime.incarnation(),
                                                   runtime.evidence_generation())));
        clock->advance(1);
    }
    stop.store(true);
    reader.join();
    TCPLANE_CHECK(reads.load() > 0);
    TCPLANE_CHECK_EQ(bad_reads.load(), 0);
    TCPLANE_STATUS_OK(runtime.close());
}

TCPLANE_CASE(concurrency, shutdown_stops_new_work_and_is_repeatable) {
    SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1}, {make_zone(10, 100, 1)});
    auto manual = std::make_unique<ManualClock>(Tick{100});
    ManualClock* clock = manual.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(manual));
    TCPLANE_OK(created);
    ThermalRuntime& runtime = *created.value();
    TCPLANE_STATUS_OK(runtime.install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                             PolicyGeneration{}));
    TCPLANE_STATUS_OK(runtime.publish_limits(facility.limits_for(0, LimitGeneration{1})));
    TCPLANE_STATUS_OK(runtime.declare_interlock_baseline(runtime.topology_generation(), {}));

    std::atomic<bool> stop{false};
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    TCPLANE_PHASE("a worker keeps working while the runtime is closed underneath it");
    std::thread worker([&runtime, &facility, clock, &stop, &accepted, &rejected]() {
        while (!stop.load()) {
            const auto report = runtime.ingest(facility.observe(clock->peek(), BasisPoints::none(),
                                                                runtime.incarnation(),
                                                                runtime.evidence_generation()));
            if (report.has_value()) {
                accepted.fetch_add(1);
            } else {
                rejected.fetch_add(1);
            }
        }
    });

    TCPLANE_STATUS_OK(runtime.close());
    TCPLANE_CHECK(runtime.closed());
    TCPLANE_PHASE("after close, new work is refused with an explicit code");
    const auto after = runtime.ingest(facility.observe(clock->peek(), BasisPoints::none(),
                                                       runtime.incarnation(),
                                                       runtime.evidence_generation()));
    TCPLANE_ERROR_CODE(after, ErrorCode::RUNTIME_CLOSED);
    stop.store(true);
    worker.join();
    TCPLANE_CHECK(accepted.load() >= 0);
    TCPLANE_PHASE("closing again is safe and idempotent");
    TCPLANE_STATUS_OK(runtime.close());
    TCPLANE_STATUS_OK(runtime.close());
    TCPLANE_CHECK(runtime.closed());
}

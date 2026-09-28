// Thermal Control Plane — completed-operation benchmark.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/synthetic.hpp"

using namespace thermal_control_plane;

namespace {

constexpr std::uint32_t kWarmupIterations = 200;
constexpr std::uint32_t kMeasureIterations = 2000;

SyntheticFacility make_plant(std::size_t zones) {
    std::vector<SyntheticZoneSpec> specs;
    for (std::size_t index = 0; index < zones; ++index) {
        SyntheticZoneSpec spec;
        spec.zone = ZoneRef{static_cast<std::uint64_t>(index) + 1};
        spec.domain = DomainRef{static_cast<std::uint64_t>(index) + 1};
        spec.sensor = SensorRef{static_cast<std::uint64_t>(index) + 1};
        spec.scope_generation = ScopeGeneration{1};
        spec.thresholds = {{40000, 75000, 85000, 95000, 105000}};
        spec.max_derate = BasisPoints::from_value_unchecked(4000);
        specs.push_back(spec);
    }
    return SyntheticFacility(FacilityId{1}, TopologyGeneration{1}, specs);
}

struct Report {
    double nanoseconds_per_operation = 0.0;
    double operations_per_second = 0.0;
};

void print(const char* name, const char* unit, const Report& report, std::uint32_t iterations) {
    std::printf("%-46s %10.2f %-10s %12.1f ops/s  (%u measured iterations)\n", name,
                report.nanoseconds_per_operation, unit, report.operations_per_second, iterations);
}

template <class Operation>
Report measure(Operation&& operation) {
    for (std::uint32_t index = 0; index < kWarmupIterations; ++index) {
        operation();
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < kMeasureIterations; ++index) {
        operation();
    }
    const auto finish = std::chrono::steady_clock::now();
    const double total =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
    Report report;
    report.nanoseconds_per_operation = total / static_cast<double>(kMeasureIterations);
    report.operations_per_second = 1.0e9 / report.nanoseconds_per_operation;
    return report;
}

}  // namespace

int main(int argc, char** argv) {
    std::string store_root = argc > 1 ? argv[1] : std::string(".");
    std::printf("Thermal Control Plane completed-operation benchmark\n");
    std::printf("workload: SYNTHETIC four-zone facility model driven through the public API\n");
    std::printf("timing:   REAL host steady_clock, %u warm-up then %u measured completed operations\n",
                kWarmupIterations, kMeasureIterations);
    std::printf("note:     durable rows include the full commit path (write, flush, atomic publish,\n");
    std::printf("          read-back verification) because that is what completing the operation costs\n\n");

    // 1. Pure policy evaluation over four zones.
    {
        SyntheticFacility facility = make_plant(4);
        EvidenceStore evidence;
        LimitRegistry limits{8};
        InterlockRegistry interlocks{8};
        const TopologyGeneration topology{1};
        const EvidenceGeneration generation{1};
        static_cast<void>(evidence.reset(generation, topology));
        for (const ThermalLimitSet& set : facility.all_limits(LimitGeneration{1})) {
            static_cast<void>(limits.put(set));
        }
        static_cast<void>(evidence.ingest(
            facility.observe(Tick{100}, BasisPoints::none(), ProcessIncarnation{1}, generation), Tick{100}));
        ThermalEnvironment environment;
        environment.evidence = &evidence;
        environment.limits = &limits;
        environment.interlocks = &interlocks;
        environment.topology_generation = topology;
        const ThermalPolicy policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
        PolicyInput input;
        input.now = Tick{100};
        input.evidence_generation = generation;
        input.topology_generation = topology;
        std::uint64_t checksum = 0;
        const Report report = measure([&]() {
            const PolicyOutcome outcome = evaluate_policy(policy, environment, input);
            checksum += outcome.required_derate.value();
        });
        print("policy evaluation (4 zones, no durability)", "ns/op", report, kMeasureIterations);
        std::printf("%-46s %s\n", "  checksum (prevents dead-code elimination)",
                    std::to_string(checksum).c_str());
    }

    // 2. Authority decision without durability.
    {
        SyntheticFacility facility = make_plant(4);
        auto clock = std::make_unique<ManualClock>(Tick{1000});
        ManualClock* ticks = clock.get();
        auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(clock));
        if (!created.has_value()) {
            std::printf("runtime creation failed\n");
            return 1;
        }
        std::unique_ptr<ThermalRuntime>& runtime = created.value();
        static_cast<void>(runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                                  PolicyGeneration{}));
        for (const ThermalLimitSet& set : facility.all_limits(LimitGeneration{1})) {
            static_cast<void>(runtime->publish_limits(set));
        }
        static_cast<void>(runtime->declare_interlock_baseline(runtime->topology_generation(), {}));
        static_cast<void>(runtime->ingest(facility.observe(ticks->peek(), BasisPoints::none(),
                                                          runtime->incarnation(),
                                                          runtime->evidence_generation())));
        std::uint64_t counter = 0;
        std::uint64_t checksum = 0;
        const Report report = measure([&]() {
            ++counter;
            AuthorityAttempt attempt;
            attempt.request = RequestId{counter};
            attempt.attempt = AttemptId{counter};
            attempt.key = IdempotencyKey::parse("bench-" + std::to_string(counter)).value();
            attempt.kind = AttemptKind::RequestDerate;
            attempt.binding = runtime->binding().value();
            attempt.requested_derate = BasisPoints::from_value_unchecked(3000);
            const auto outcome = runtime->authorize(attempt);
            if (outcome.has_value()) {
                checksum += outcome.value().granted_derate.value();
            }
        });
        print("authority decision (volatile runtime)", "ns/op", report, kMeasureIterations);
        std::printf("%-46s %s\n", "  checksum", std::to_string(checksum).c_str());
        static_cast<void>(runtime->close());
    }

    // 3. Authority decision with a durable commit before the answer is returned.
    {
        SyntheticFacility facility = make_plant(4);
        auto clock = std::make_unique<ManualClock>(Tick{1000});
        ManualClock* ticks = clock.get();
        RuntimeOptions options;
        options.store_path = store_root + "/tcplane-bench.state";
        auto created = ThermalRuntime::create(options, std::move(clock));
        if (!created.has_value()) {
            std::printf("durable runtime creation failed: %s\n", created.error().render().c_str());
            return 1;
        }
        std::unique_ptr<ThermalRuntime>& runtime = created.value();
        static_cast<void>(runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                                  PolicyGeneration{}));
        for (const ThermalLimitSet& set : facility.all_limits(LimitGeneration{1})) {
            static_cast<void>(runtime->publish_limits(set));
        }
        static_cast<void>(runtime->declare_interlock_baseline(runtime->topology_generation(), {}));
        static_cast<void>(runtime->ingest(facility.observe(ticks->peek(), BasisPoints::none(),
                                                          runtime->incarnation(),
                                                          runtime->evidence_generation())));
        std::uint64_t counter = 0;
        std::uint64_t checksum = 0;
        const Report report = measure([&]() {
            ++counter;
            AuthorityAttempt attempt;
            attempt.request = RequestId{counter};
            attempt.attempt = AttemptId{counter};
            attempt.key = IdempotencyKey::parse("dbench-" + std::to_string(counter)).value();
            attempt.kind = AttemptKind::RequestDerate;
            attempt.binding = runtime->binding().value();
            attempt.requested_derate = BasisPoints::from_value_unchecked(3000);
            const auto outcome = runtime->authorize(attempt);
            if (outcome.has_value()) {
                checksum += outcome.value().granted_derate.value();
            }
        });
        print("authority decision (durable commit + read-back)", "ns/op", report, kMeasureIterations);
        std::printf("%-46s %s\n", "  checksum", std::to_string(checksum).c_str());
        std::printf("%-46s %llu\n", "  durable commit sequence",
                    static_cast<unsigned long long>(runtime->commit_sequence().value));
        static_cast<void>(runtime->close());
    }

    std::printf("\nSYNTHETIC facility model, REAL process and filesystem behaviour on the benchmark host.\n");
    return 0;
}

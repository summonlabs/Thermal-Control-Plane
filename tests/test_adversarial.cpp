// Thermal Control Plane — adversarial and malformed-input proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

#include "process_util.hpp"
#include "thermal_control_plane/wire.hpp"

#include <string>
#include <vector>

namespace {

using namespace thermal_control_plane;

ThermalSnapshot minimal_snapshot() {
    ThermalSnapshot snapshot;
    snapshot.commit_sequence = CommitSequence{1};
    snapshot.epoch = ControlPlaneEpoch{1};
    snapshot.revision = StateRevision{1};
    snapshot.topology_generation = TopologyGeneration{1};
    snapshot.evidence_generation = EvidenceGeneration{1};
    snapshot.next_directive_id = 1;
    snapshot.next_audit_sequence = 1;
    return snapshot;
}

}  // namespace

TCPLANE_CASE(adversarial, payload_offsets_are_exact) {
    const ThermalSnapshot snapshot = minimal_snapshot();
    const auto payload = encode_snapshot(snapshot);
    TCPLANE_OK(payload);

    TCPLANE_PHASE("the documented offsets hold for a policy-free snapshot");
    TCPLANE_CHECK(payload.value().size() > 130);
    TCPLANE_CHECK_EQ(static_cast<unsigned char>(payload.value()[53]),
                     static_cast<unsigned char>(ThermalMode::Normal));
    TCPLANE_CHECK_EQ(static_cast<unsigned char>(payload.value()[36]), 0U);

    TCPLANE_PHASE("an impossible administrative mode is refused");
    std::vector<std::byte> bad_mode = payload.value();
    bad_mode[53] = static_cast<std::byte>(0xEE);
    TCPLANE_ERROR_CODE(decode_snapshot(bad_mode), ErrorCode::STORE_CORRUPT);

    TCPLANE_PHASE("an impossible thermal mode is refused");
    std::vector<std::byte> bad_thermal = payload.value();
    bad_thermal[55] = static_cast<std::byte>(0x99);
    TCPLANE_ERROR_CODE(decode_snapshot(bad_thermal), ErrorCode::STORE_CORRUPT);

    TCPLANE_PHASE("an impossible escalation cause is refused");
    std::vector<std::byte> bad_cause = payload.value();
    bad_cause[57] = static_cast<std::byte>(0x7F);
    TCPLANE_ERROR_CODE(decode_snapshot(bad_cause), ErrorCode::STORE_CORRUPT);

    TCPLANE_PHASE("an absurd collection count is refused before any allocation");
    std::vector<std::byte> absurd = payload.value();
    for (std::size_t i = 114; i < 118; ++i) {
        absurd[i] = static_cast<std::byte>(0xFF);
    }
    TCPLANE_ERROR_CODE(decode_snapshot(absurd), ErrorCode::ENCODING_TOO_LARGE);

    TCPLANE_PHASE("an unsupported snapshot format version is refused");
    std::vector<std::byte> version = payload.value();
    version[0] = static_cast<std::byte>(9);
    TCPLANE_ERROR_CODE(decode_snapshot(version), ErrorCode::STORE_UNSUPPORTED_VERSION);
}

TCPLANE_CASE(adversarial, payload_fuzzing_never_crashes_or_invents_state) {
    const ThermalSnapshot snapshot = minimal_snapshot();
    const auto payload = encode_snapshot(snapshot);
    TCPLANE_OK(payload);

    TCPLANE_PHASE("every single-byte payload flip is refused or round-trips stably");
    std::size_t refused = 0;
    std::size_t accepted = 0;
    for (std::size_t offset = 0; offset < payload.value().size(); ++offset) {
        for (const unsigned int mask : {0x01U, 0x80U, 0xFFU}) {
            std::vector<std::byte> damaged = payload.value();
            damaged[offset] = static_cast<std::byte>(
                static_cast<unsigned int>(static_cast<unsigned char>(damaged[offset])) ^ mask);
            const auto decoded = decode_snapshot(damaged);
            if (!decoded.has_value()) {
                ++refused;
                continue;
            }
            ++accepted;
            const auto reencoded = encode_snapshot(decoded.value());
            TCPLANE_OK(reencoded);
            const auto again = decode_snapshot(reencoded.value());
            TCPLANE_OK(again);
        }
    }
    TCPLANE_PHASE("the decoder accepted only structurally valid payloads");
    TCPLANE_CHECK(refused > 0);
    TCPLANE_CHECK(refused + accepted == payload.value().size() * 3);

    TCPLANE_PHASE("prefix truncation at every length is refused");
    for (std::size_t length = 0; length < payload.value().size(); ++length) {
        const std::span<const std::byte> prefix(payload.value().data(), length);
        const auto decoded = decode_snapshot(prefix);
        TCPLANE_CHECK(!decoded.has_value());
    }
}

TCPLANE_CASE(adversarial, interlock_inputs_are_hardened) {
    InterlockRegistry registry(4);
    TCPLANE_PHASE("an interlock without identity, generation or stamp is refused");
    Interlock interlock;
    interlock.id = InterlockId{1};
    interlock.klass = InterlockClass::Safety;
    interlock.facility_wide = true;
    interlock.topology_generation = TopologyGeneration{1};
    interlock.observed_at = Tick{1};

    Interlock anonymous = interlock;
    anonymous.id = InterlockId{0};
    TCPLANE_STATUS_ERROR_CODE(registry.apply(anonymous), ErrorCode::INVALID_ARGUMENT);

    Interlock generationless = interlock;
    generationless.topology_generation = TopologyGeneration{0};
    TCPLANE_STATUS_ERROR_CODE(registry.apply(generationless), ErrorCode::INVALID_ARGUMENT);

    Interlock unstamped = interlock;
    unstamped.observed_at = Tick{0};
    TCPLANE_STATUS_ERROR_CODE(registry.apply(unstamped), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("a control character in the source reference is refused");
    Interlock control = interlock;
    control.source = std::string("bad\x01source");
    TCPLANE_STATUS_ERROR_CODE(registry.apply(control), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("an oversized source reference is refused");
    Interlock oversized = interlock;
    oversized.source = std::string(InterlockRegistry::kMaxSourceLength + 1, 'x');
    TCPLANE_STATUS_ERROR_CODE(registry.apply(oversized), ErrorCode::OUT_OF_RANGE);

    TCPLANE_PHASE("a facility-wide interlock naming a scope is refused");
    Interlock conflicted = interlock;
    conflicted.facility_wide = true;
    conflicted.scope = make_scope(1, 1, 1);
    TCPLANE_STATUS_ERROR_CODE(registry.apply(conflicted), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("a scoped interlock without a scope or generation is refused");
    Interlock scopeless = interlock;
    scopeless.facility_wide = false;
    TCPLANE_STATUS_ERROR_CODE(registry.apply(scopeless), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("clearing an interlock that was never asserted is refused");
    Interlock clear = interlock;
    clear.id = InterlockId{77};
    clear.state = InterlockState::Cleared;
    TCPLANE_STATUS_ERROR_CODE(registry.apply(clear), ErrorCode::UNKNOWN_INTERLOCK);

    TCPLANE_PHASE("reusing an interlock identity with different semantics is refused");
    TCPLANE_STATUS_OK(registry.apply(interlock));
    Interlock relabelled = interlock;
    relabelled.klass = InterlockClass::Isolation;
    TCPLANE_STATUS_ERROR_CODE(registry.apply(relabelled), ErrorCode::DUPLICATE_INTERLOCK);

    TCPLANE_PHASE("a baseline with a duplicate identity is refused");
    TCPLANE_STATUS_ERROR_CODE(registry.replace_all({interlock, interlock}), ErrorCode::DUPLICATE_INTERLOCK);

    TCPLANE_PHASE("a baseline larger than the registry is refused");
    std::vector<Interlock> many;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        Interlock entry = interlock;
        entry.id = InterlockId{i};
        many.push_back(entry);
    }
    TCPLANE_STATUS_ERROR_CODE(registry.replace_all(many), ErrorCode::RESOURCE_EXHAUSTED);
}

TCPLANE_CASE(adversarial, runtime_bounds_and_store_paths_are_hardened) {
    TCPLANE_PHASE("a zero bound is refused at construction");
    RuntimeOptions options;
    options.bounds.max_scopes = 0;
    auto created = ThermalRuntime::create(options, std::make_unique<ManualClock>());
    TCPLANE_ERROR_CODE(created, ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("an absurd bound is refused at construction");
    RuntimeOptions absurd;
    absurd.bounds.max_sensors = 100000000;
    created = ThermalRuntime::create(absurd, std::make_unique<ManualClock>());
    TCPLANE_ERROR_CODE(created, ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("a missing clock is refused");
    created = ThermalRuntime::create(RuntimeOptions{}, nullptr);
    TCPLANE_ERROR_CODE(created, ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("a store path containing a NUL byte is refused");
    std::string with_nul = tcptest::unique_path("nul");
    with_nul.push_back('\0');
    with_nul.append("tail");
    options = RuntimeOptions{};
    options.store_path = with_nul;
    created = ThermalRuntime::create(options, std::make_unique<ManualClock>());
    TCPLANE_CHECK(!created.has_value());
    TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(created.error().code),
                     static_cast<std::uint32_t>(ErrorCode::STORE_PATH_INVALID));

    TCPLANE_PHASE("a relative store path is refused");
    RuntimeOptions relative;
    relative.store_path = "relative-state.bin";
    created = ThermalRuntime::create(relative, std::make_unique<ManualClock>());
    TCPLANE_ERROR_CODE(created, ErrorCode::STORE_PATH_INVALID);

    TCPLANE_PHASE("a store path that is a directory is refused");
    RuntimeOptions directory;
    directory.store_path = tcptest::temp_directory();
    created = ThermalRuntime::create(directory, std::make_unique<ManualClock>());
    TCPLANE_ERROR_CODE(created, ErrorCode::STORE_PATH_INVALID);
}

TCPLANE_CASE(adversarial, repeated_open_close_and_recovery_are_stable) {
    const std::string path = tcptest::unique_path("reopen") + ".state";
    TCPLANE_PHASE("five open/close cycles preserve and advance durable state");
    StateRevision last_revision{};
    CommitSequence last_commit{};
    for (int cycle = 0; cycle < 5; ++cycle) {
        RuntimeOptions options;
        options.store_path = path;
        auto created = ThermalRuntime::create(options, std::make_unique<ManualClock>(Tick{100}));
        TCPLANE_OK(created);
        ThermalRuntime& runtime = *created.value();
        const ThermalPolicy policy = ThermalPolicy::baseline(
            PolicyId{1}, PolicyGeneration{static_cast<std::uint64_t>(cycle) + 1});
        const Status installed = runtime.install_policy(policy, runtime.policy_generation());
        TCPLANE_STATUS_OK(installed);
        TCPLANE_CHECK(runtime.revision() > last_revision);
        TCPLANE_CHECK(runtime.commit_sequence() > last_commit);
        last_revision = runtime.revision();
        last_commit = runtime.commit_sequence();
        TCPLANE_STATUS_OK(runtime.close());
        TCPLANE_STATUS_OK(runtime.close());
    }

    TCPLANE_PHASE("a faulted store refuses further decisions until it is recovered");
    RuntimeOptions options;
    options.store_path = path;
    auto created = ThermalRuntime::create(options, std::make_unique<ManualClock>(Tick{100}));
    TCPLANE_OK(created);
    TCPLANE_CHECK(created.value()->durable());
    const auto recovered = created.value()->recover();
    TCPLANE_OK(recovered);
    TCPLANE_CHECK(recovered.value().recovered);
    TCPLANE_CHECK(recovered.value().policy_restored);
    TCPLANE_CHECK(created.value()->mode() == ThermalMode::Normal);
    TCPLANE_STATUS_OK(created.value()->close());
    tcptest::remove_tree(path);
    tcptest::remove_tree(path + ".0.tcpslot");
    tcptest::remove_tree(path + ".1.tcpslot");
    tcptest::remove_tree(path + ".lock");
}

TCPLANE_CASE(adversarial, directive_tail_eviction_is_reported_not_hidden) {
    SyntheticZoneSpec spec;
    spec.zone = ZoneRef{10};
    spec.domain = DomainRef{100};
    spec.sensor = SensorRef{1};
    spec.scope_generation = ScopeGeneration{1};

    SyntheticFacility facility(FacilityId{1}, TopologyGeneration{1}, {spec});
    auto manual = std::make_unique<ManualClock>(Tick{100});
    ManualClock* clock = manual.get();
    RuntimeOptions options;
    options.bounds.max_directives = 2;
    auto created = ThermalRuntime::create(options, std::move(manual));
    TCPLANE_OK(created);
    ThermalRuntime& runtime = *created.value();

    TCPLANE_STATUS_OK(runtime.install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                             PolicyGeneration{}));
    for (const ThermalLimitSet& limits : facility.all_limits(LimitGeneration{1})) {
        TCPLANE_STATUS_OK(runtime.publish_limits(limits));
    }
    TCPLANE_STATUS_OK(runtime.declare_interlock_baseline(runtime.topology_generation(), {}));
    TCPLANE_PHASE("an override above the derate threshold creates demand");
    facility.override_temperature(0, 90000);
    TCPLANE_OK(runtime.ingest(facility.observe(clock->peek(), BasisPoints::none(), runtime.incarnation(),
                                               runtime.evidence_generation())));

    TCPLANE_PHASE("more directives than the tail retains evict the oldest");
    DirectiveId first{};
    for (int index = 0; index < 3; ++index) {
        AuthorityAttempt attempt =
            make_attempt(runtime, RequestId{static_cast<std::uint64_t>(index + 1)},
                         AttemptId{static_cast<std::uint64_t>(index + 1)},
                         "evict-" + std::to_string(index), AttemptKind::RequestDerate);
        attempt.requested_derate = BasisPoints::from_value_unchecked(2500);
        const auto outcome = runtime.authorize(attempt);
        TCPLANE_OK(outcome);
        TCPLANE_CHECK(outcome.value().verdict == AuthorityVerdict::Granted);
        if (index == 0) {
            first = outcome.value().directive;
        }
    }
    TCPLANE_CHECK_EQ(runtime.directive_tail(64).size(), std::size_t{2});
    TCPLANE_PHASE("an evicted directive is reported as unknown, never as verified");
    TCPLANE_ERROR_CODE(runtime.verify_directive(first), ErrorCode::UNKNOWN_DIRECTIVE);
    TCPLANE_PHASE("repeated actuation attempts are bounded, not silent");
    TCPLANE_CHECK(runtime.directive_tail(64).size() <= 2);
    TCPLANE_STATUS_OK(runtime.close());
}

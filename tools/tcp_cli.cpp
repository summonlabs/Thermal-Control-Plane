// Thermal Control Plane — administration and inspection CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/synthetic.hpp"

namespace {

using namespace thermal_control_plane;

void print_usage() {
    std::cout << "tcp_cli — Thermal Control Plane administration\n"
                 "\n"
                 "usage:\n"
                 "  tcp_cli assess    [--zones N] [--temperature mC] [--derate bp]\n"
                 "  tcp_cli authorize [--zones N] [--temperature mC] [--request bp] [--key K]\n"
                 "  tcp_cli inspect   --store PATH\n"
                 "  tcp_cli verify    --store PATH\n"
                 "\n"
                 "Every facility used by this tool is SYNTHETIC: it exercises the\n"
                 "authority semantics of the library and is not a hardware proof.\n";
}

struct Arguments {
    int zones = 2;
    std::int32_t temperature = 90000;
    std::uint32_t derate = 3000;
    std::string store;
    std::string key = "cli-attempt-1";
};

bool parse(int argc, char** argv, Arguments& arguments) {
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&](std::string& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string value;
        if (flag == "--zones") {
            if (!next(value)) {
                return false;
            }
            arguments.zones = std::atoi(value.c_str());
        } else if (flag == "--temperature") {
            if (!next(value)) {
                return false;
            }
            arguments.temperature = static_cast<std::int32_t>(std::atol(value.c_str()));
        } else if (flag == "--derate") {
            if (!next(value)) {
                return false;
            }
            arguments.derate = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (flag == "--request") {
            if (!next(value)) {
                return false;
            }
            arguments.derate = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (flag == "--store") {
            if (!next(arguments.store)) {
                return false;
            }
        } else if (flag == "--key") {
            if (!next(arguments.key)) {
                return false;
            }
        } else {
            std::cout << "unknown option: " << flag << "\n";
            return false;
        }
    }
    return true;
}

struct Session {
    SyntheticFacility facility;
    std::unique_ptr<ThermalRuntime> runtime;
    ManualClock* clock = nullptr;
};

std::unique_ptr<Session> build_session(const Arguments& arguments) {
    std::vector<SyntheticZoneSpec> zones;
    for (int index = 0; index < arguments.zones; ++index) {
        SyntheticZoneSpec spec;
        spec.zone = ZoneRef{static_cast<std::uint64_t>(index) + 1};
        spec.domain = DomainRef{static_cast<std::uint64_t>(index) + 1};
        spec.sensor = SensorRef{static_cast<std::uint64_t>(index) + 1};
        spec.scope_generation = ScopeGeneration{1};
        zones.push_back(spec);
    }
    auto session = std::make_unique<Session>(Session{
        SyntheticFacility(FacilityId{1}, TopologyGeneration{1}, std::move(zones)), nullptr, nullptr});
    auto manual = std::make_unique<ManualClock>(Tick{1000});
    session->clock = manual.get();
    auto created = ThermalRuntime::create(RuntimeOptions{}, std::move(manual));
    if (!created.has_value()) {
        std::cout << "runtime creation failed: " << created.error().render() << "\n";
        return nullptr;
    }
    session->runtime = std::move(created.value());
    const Status installed =
        session->runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                        PolicyGeneration{});
    if (!installed.ok()) {
        std::cout << "policy install failed: " << installed.error().render() << "\n";
        return nullptr;
    }
    for (const ThermalLimitSet& limits : session->facility.all_limits(LimitGeneration{1})) {
        const Status published = session->runtime->publish_limits(limits);
        if (!published.ok()) {
            std::cout << "limit publish failed: " << published.error().render() << "\n";
            return nullptr;
        }
    }
    const Status baseline =
        session->runtime->declare_interlock_baseline(session->runtime->topology_generation(), {});
    if (!baseline.ok()) {
        std::cout << "interlock baseline failed: " << baseline.error().render() << "\n";
        return nullptr;
    }
    return session;
}

int command_assess(const Arguments& arguments) {
    auto session = build_session(arguments);
    if (session == nullptr) {
        return 2;
    }
    for (std::size_t index = 0; index < session->facility.zone_count(); ++index) {
        session->facility.override_temperature(index, arguments.temperature);
    }
    const auto report = session->runtime->ingest(
        session->facility.observe(session->clock->peek(), BasisPoints::from_value_unchecked(arguments.derate),
                                  session->runtime->incarnation(),
                                  session->runtime->evidence_generation()));
    if (!report.has_value()) {
        std::cout << "ingest failed: " << report.error().render() << "\n";
        return 2;
    }
    const auto outcome = session->runtime->assess();
    if (!outcome.has_value()) {
        std::cout << "assessment failed: " << outcome.error().render() << "\n";
        return 2;
    }
    const PolicyOutcome& value = outcome.value();
    std::cout << "mode " << to_string(value.mode) << "\n";
    std::cout << "required_mode " << to_string(value.required_mode) << "\n";
    std::cout << "cause " << to_string(value.reason.cause) << "\n";
    std::cout << "reason_scope " << to_string(value.reason.scope) << "\n";
    std::cout << "facility_headroom " << to_string(value.facility.state) << " "
              << to_string(value.facility.reason) << "\n";
    std::cout << "worst_headroom_mC " << value.facility.worst_delta.milli_celsius << "\n";
    std::cout << "worst_headroom_basis " << to_string(value.facility.worst_basis) << "\n";
    std::cout << "required_derate_bp " << value.required_derate.value() << "\n";
    std::cout << "envelope_ceiling_defined " << (value.envelope.ceiling_defined ? 1 : 0) << "\n";
    std::cout << "envelope_ceiling_mC " << value.envelope.ceiling.milli_celsius << "\n";
    std::cout << "recovery_eligible " << (value.recovery_eligible ? 1 : 0) << "\n";
    return 0;
}

int command_authorize(const Arguments& arguments) {
    auto session = build_session(arguments);
    if (session == nullptr) {
        return 2;
    }
    for (std::size_t index = 0; index < session->facility.zone_count(); ++index) {
        session->facility.override_temperature(index, arguments.temperature);
    }
    const auto report = session->runtime->ingest(
        session->facility.observe(session->clock->peek(), BasisPoints::from_value_unchecked(arguments.derate),
                                  session->runtime->incarnation(),
                                  session->runtime->evidence_generation()));
    if (!report.has_value()) {
        std::cout << "ingest failed: " << report.error().render() << "\n";
        return 2;
    }
    AuthorityAttempt attempt;
    attempt.request = RequestId{1};
    attempt.attempt = AttemptId{1};
    const auto key = IdempotencyKey::parse(arguments.key);
    if (!key.has_value()) {
        std::cout << "invalid key: " << key.error().render() << "\n";
        return 2;
    }
    attempt.key = key.value();
    attempt.kind = AttemptKind::RequestDerate;
    attempt.authority = AuthorityClass::Operator;
    const auto binding = session->runtime->binding();
    if (!binding.has_value()) {
        std::cout << "binding failed: " << binding.error().render() << "\n";
        return 2;
    }
    attempt.binding = binding.value();
    attempt.requested_derate = BasisPoints::from_value_unchecked(arguments.derate);
    const auto outcome = session->runtime->authorize(attempt);
    if (!outcome.has_value()) {
        std::cout << "outcome_error " << to_string(outcome.error().code) << " "
                  << outcome.error().detail << "\n";
        return 1;
    }
    const AuthorityOutcome& value = outcome.value();
    std::cout << "verdict " << to_string(value.verdict) << "\n";
    std::cout << "code " << to_string(value.code) << "\n";
    std::cout << "detail " << value.detail << "\n";
    std::cout << "directive " << value.directive.value << "\n";
    std::cout << "granted_derate_bp " << value.granted_derate.value() << "\n";
    std::cout << "mode " << to_string(value.mode) << "\n";
    std::cout << "revision_after " << value.revision_after.value << "\n";
    std::cout << "commit " << value.commit << "\n";
    std::cout << "replayed " << (value.replayed ? 1 : 0) << "\n";
    return value.verdict == AuthorityVerdict::Refused ? 1 : 0;
}

int command_inspect(const Arguments& arguments) {
    if (arguments.store.empty()) {
        std::cout << "inspect requires --store PATH\n";
        return 2;
    }
    auto backend = std::make_unique<FileStorageBackend>(arguments.store);
    DurableStore store(std::move(backend));
    const Status opened = store.open();
    if (!opened.ok()) {
        std::cout << "store_error " << to_string(opened.code()) << " " << opened.error().detail
                  << "\n";
        return 2;
    }
    const auto loaded = store.load();
    if (!loaded.has_value()) {
        std::cout << "store_error " << to_string(loaded.error().code) << " "
                  << loaded.error().detail << "\n";
        store.close();
        return 2;
    }
    if (!loaded.value().present) {
        std::cout << "present 0\n";
        store.close();
        return 0;
    }
    const ThermalSnapshot& snapshot = loaded.value().snapshot;
    std::cout << "present 1\n";
    std::cout << "slot " << loaded.value().slot << "\n";
    std::cout << "commit_sequence " << snapshot.commit_sequence.value << "\n";
    std::cout << "epoch " << snapshot.epoch.value << "\n";
    std::cout << "revision " << snapshot.revision.value << "\n";
    std::cout << "mode " << to_string(snapshot.mode.mode) << "\n";
    std::cout << "administrative_mode " << to_string(snapshot.administrative_mode) << "\n";
    std::cout << "policy_present " << (snapshot.policy_present ? 1 : 0) << "\n";
    std::cout << "policy_generation " << snapshot.policy.generation.value << "\n";
    std::cout << "topology_generation " << snapshot.topology_generation.value << "\n";
    std::cout << "evidence_generation " << snapshot.evidence_generation.value << "\n";
    std::cout << "interlock_baseline_declared " << (snapshot.interlock_baseline_declared ? 1 : 0) << "\n";
    std::cout << "interlocks " << snapshot.interlocks.size() << "\n";
    std::cout << "directives " << snapshot.directives.size() << "\n";
    std::cout << "audit_records " << snapshot.audit.size() << "\n";
    std::cout << "replay_entries " << snapshot.replay.size() << "\n";
    store.close();
    return 0;
}

int command_verify(const Arguments& arguments) {
    if (arguments.store.empty()) {
        std::cout << "verify requires --store PATH\n";
        return 2;
    }
    auto backend = std::make_unique<FileStorageBackend>(arguments.store);
    DurableStore store(std::move(backend));
    const Status opened = store.open();
    if (!opened.ok()) {
        std::cout << "store_error " << to_string(opened.code()) << " " << opened.error().detail << "\n";
        return 2;
    }
    std::size_t valid = 0;
    std::size_t damaged = 0;
    for (std::size_t slot = 0; slot < store.backend().slot_count(); ++slot) {
        const auto bytes = store.backend().read_slot(slot);
        if (!bytes.has_value()) {
            std::cout << "slot " << slot << " unreadable " << to_string(bytes.error().code) << "\n";
            ++damaged;
            continue;
        }
        if (bytes.value().empty()) {
            std::cout << "slot " << slot << " empty\n";
            continue;
        }
        std::cout << "slot " << slot << " bytes " << bytes.value().size() << "\n";
        ++valid;
    }
    const auto loaded = store.load();
    if (!loaded.has_value()) {
        std::cout << "resolve error " << to_string(loaded.error().code) << " " << loaded.error().detail
                  << "\n";
        store.close();
        return 1;
    }
    std::cout << "resolved " << (loaded.value().present ? 1 : 0) << " slot " << loaded.value().slot
              << "\n";
    std::cout << "slots_with_content " << valid << " unreadable_slots " << damaged << "\n";
    store.close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 64;
    }
    const std::string command = argv[1];
    Arguments arguments;
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
        return 0;
    }
    if (!parse(argc, argv, arguments)) {
        print_usage();
        return 64;
    }
    if (command == "assess") {
        return command_assess(arguments);
    }
    if (command == "authorize") {
        return command_authorize(arguments);
    }
    if (command == "inspect") {
        return command_inspect(arguments);
    }
    if (command == "verify") {
        return command_verify(arguments);
    }
    print_usage();
    return 64;
}

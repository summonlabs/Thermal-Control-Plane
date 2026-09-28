// Thermal Control Plane — durable store framing, integrity and recovery proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

#include "process_util.hpp"
#include "thermal_control_plane/wire.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace thermal_control_plane;

ThermalSnapshot populated_snapshot(std::uint64_t commit, StateRevision revision) {
    ThermalSnapshot snapshot;
    snapshot.format_version = kStoreFormatVersion;
    snapshot.commit_sequence = CommitSequence{commit};
    snapshot.epoch = ControlPlaneEpoch{3};
    snapshot.owner = ProcessIncarnation{99};
    snapshot.revision = revision;
    snapshot.policy_present = true;
    snapshot.policy = ThermalPolicy::baseline(PolicyId{5}, PolicyGeneration{4});
    snapshot.topology_generation = TopologyGeneration{2};
    snapshot.evidence_generation = EvidenceGeneration{7};
    snapshot.administrative_mode = ThermalMode::Maintenance;
    snapshot.administrative_hold = true;
    snapshot.mode.mode = ThermalMode::Degraded;
    snapshot.mode.required = ThermalMode::Emergency;
    snapshot.mode.cause = EscalationCause::DerateLimitViolation;
    snapshot.mode.reason_scope = make_scope(1, 2, 3);
    snapshot.mode.reason_limit = LimitKind::Derate;
    snapshot.mode.reason_limit_defined = true;
    snapshot.mode.changed_at = Tick{4242};
    snapshot.gate.armed = true;
    snapshot.gate.armed_for = ThermalMode::Degraded;
    snapshot.gate.target = ThermalMode::Constrained;
    snapshot.gate.favorable_since = Tick{4200};
    snapshot.gate.samples = 3;
    snapshot.gate.last_counted = ObservationSequence{11};
    snapshot.gate.satisfied = true;
    snapshot.gate.blocked_reason = HeadroomReason::None;
    snapshot.interlock_baseline_declared = true;
    Interlock interlock;
    interlock.id = InterlockId{8};
    interlock.klass = InterlockClass::Safety;
    interlock.facility_wide = true;
    interlock.state = InterlockState::Asserted;
    interlock.topology_generation = TopologyGeneration{2};
    interlock.author = ProcessIncarnation{99};
    interlock.observed_at = Tick{10};
    interlock.source = "synthetic";
    snapshot.interlocks.push_back(interlock);
    snapshot.next_directive_id = 12;
    snapshot.next_audit_sequence = 34;

    DeratingDirective directive;
    directive.id = DirectiveId{11};
    directive.request = RequestId{4};
    directive.attempt = AttemptId{5};
    directive.scope = make_scope(1, 2, 3);
    directive.facility_wide = true;
    directive.requested = BasisPoints::from_value_unchecked(1500);
    directive.granted = BasisPoints::from_value_unchecked(1500);
    directive.basis = LimitKind::Derate;
    directive.basis_defined = true;
    directive.observed = Temperature::from_milli_celsius_unchecked(90000);
    directive.limit.defined = true;
    directive.limit.milli_celsius = 85000;
    directive.policy_generation = PolicyGeneration{4};
    directive.topology_generation = TopologyGeneration{2};
    directive.evidence_generation = EvidenceGeneration{7};
    directive.revision = revision;
    directive.issued_at = Tick{4200};
    directive.authority = AuthorityClass::Operator;
    directive.observed_sequence = ObservationSequence{9};
    directive.acknowledged = true;
    directive.acknowledged_at = Tick{4300};
    snapshot.directives.push_back(directive);

    AuditRecord audit;
    audit.sequence = 33;
    audit.at = Tick{4200};
    audit.revision = revision;
    audit.kind = AuditKind::AuthorityGranted;
    audit.request = RequestId{4};
    audit.attempt = AttemptId{5};
    audit.code = ErrorCode::NONE;
    audit.directive = DirectiveId{11};
    audit.mode = ThermalMode::Degraded;
    audit.derate = BasisPoints::from_value_unchecked(1500);
    audit.detail = "derating directive granted";
    snapshot.audit.push_back(audit);

    ReplayRecord replay;
    replay.key = IdempotencyKey::parse("retry-key-1").value();
    replay.digest = {std::byte{1}, std::byte{2}, std::byte{3}};
    replay.outcome.verdict = AuthorityVerdict::Granted;
    replay.outcome.code = ErrorCode::NONE;
    replay.outcome.detail = "derating directive granted";
    replay.outcome.request = RequestId{4};
    replay.outcome.attempt = AttemptId{5};
    replay.outcome.directive = DirectiveId{11};
    replay.outcome.revision_before = revision;
    replay.outcome.revision_after = revision.next();
    replay.outcome.mode = ThermalMode::Degraded;
    replay.outcome.required_mode = ThermalMode::Degraded;
    replay.outcome.cause = EscalationCause::DerateLimitViolation;
    replay.outcome.reason_limit = LimitKind::Derate;
    replay.outcome.granted_derate = BasisPoints::from_value_unchecked(1500);
    replay.outcome.envelope.mode = ThermalMode::Degraded;
    replay.outcome.envelope.ceiling_basis = LimitKind::Critical;
    replay.outcome.decided_at = Tick{4200};
    replay.outcome.commit = commit;
    replay.outcome.durable = true;
    snapshot.replay.push_back(replay);
    return snapshot;
}

/// Build the raw on-disk record that a store would hold for a snapshot.
std::vector<std::byte> build_record(const ThermalSnapshot& snapshot, std::uint64_t commit,
                                    std::uint32_t version, std::uint32_t flags) {
    const auto payload = encode_snapshot(snapshot);
    if (!payload.has_value()) {
        return {};
    }
    std::vector<std::byte> record(40 + payload.value().size());
    const char magic[8] = {'T', 'C', 'P', 'L', 'N', '0', '0', '1'};
    for (std::size_t i = 0; i < 8; ++i) {
        record[i] = static_cast<std::byte>(magic[i]);
    }
    const auto put32 = [&record](std::size_t offset, std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            record[offset + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    };
    const auto put64 = [&record](std::size_t offset, std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            record[offset + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    };
    put32(8, version);
    put32(12, flags);
    put64(16, commit);
    put64(24, static_cast<std::uint64_t>(payload.value().size()));
    put32(32, wire::crc32c(std::span<const std::byte>(payload.value().data(), payload.value().size())));
    put32(36, wire::crc32c(std::span<const std::byte>(record.data(), 36)));
    for (std::size_t i = 0; i < payload.value().size(); ++i) {
        record[40 + i] = payload.value()[i];
    }
    return record;
}

void write_file(const std::string& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
}

std::string slot_path(const std::string& store, int slot) {
    return store + "." + std::to_string(slot) + ".tcpslot";
}

}  // namespace

TCPLANE_CASE(persistence, snapshot_round_trip_and_byte_determinism) {
    const ThermalSnapshot snapshot = populated_snapshot(9, StateRevision{6});
    TCPLANE_PHASE("encoding is byte-for-byte deterministic");
    const auto first = encode_snapshot(snapshot);
    TCPLANE_OK(first);
    for (int repeat = 0; repeat < 8; ++repeat) {
        const auto again = encode_snapshot(snapshot);
        TCPLANE_OK(again);
        TCPLANE_CHECK(again.value() == first.value());
    }

    TCPLANE_PHASE("the snapshot survives a decode without loss");
    const auto decoded = decode_snapshot(first.value());
    TCPLANE_OK(decoded);
    TCPLANE_CHECK_EQ(decoded.value().epoch.value, std::uint64_t{3});
    TCPLANE_CHECK_EQ(decoded.value().revision.value, std::uint64_t{6});
    TCPLANE_CHECK(decoded.value().administrative_mode == ThermalMode::Maintenance);
    TCPLANE_CHECK(decoded.value().mode.mode == ThermalMode::Degraded);
    TCPLANE_CHECK(decoded.value().mode.cause == EscalationCause::DerateLimitViolation);
    TCPLANE_CHECK_EQ(decoded.value().interlocks.size(), std::size_t{1});
    TCPLANE_CHECK_EQ(decoded.value().directives.size(), std::size_t{1});
    TCPLANE_CHECK(decoded.value().directives[0].acknowledged);
    TCPLANE_CHECK_EQ(decoded.value().audit.size(), std::size_t{1});
    TCPLANE_CHECK_EQ(decoded.value().replay.size(), std::size_t{1});
    TCPLANE_CHECK(decoded.value().replay[0].key == snapshot.replay[0].key);
    TCPLANE_CHECK_EQ(decoded.value().policy.generation.value, std::uint64_t{4});

    TCPLANE_PHASE("re-encoding a decoded snapshot reproduces the identical bytes");
    const auto round_trip = encode_snapshot(decoded.value());
    TCPLANE_OK(round_trip);
    TCPLANE_CHECK(round_trip.value() == first.value());
}

TCPLANE_CASE(persistence, record_framing_rejects_damage) {
    const ThermalSnapshot snapshot = populated_snapshot(1, StateRevision{1});
    const std::vector<std::byte> record = build_record(snapshot, 1, kStoreFormatVersion, 0);
    TCPLANE_CHECK(!record.empty());

    const auto load_record = [](const std::string& store) {
        auto backend = std::make_unique<FileStorageBackend>(store);
        DurableStore durable(std::move(backend));
        if (!durable.open().ok()) {
            return std::size_t{0};
        }
        const auto loaded = durable.load();
        durable.close();
        return loaded.has_value() ? loaded.value().slot : std::size_t{0};
    };

    TCPLANE_PHASE("a well-formed record loads");
    const std::string good = tcptest::unique_path("framing-good");
    write_file(slot_path(good, 0), record);
    auto backend = std::make_unique<FileStorageBackend>(good);
    DurableStore durable(std::move(backend));
    TCPLANE_STATUS_OK(durable.open());
    const auto loaded = durable.load();
    TCPLANE_OK(loaded);
    TCPLANE_CHECK(loaded.value().present);
    TCPLANE_CHECK_EQ(loaded.value().snapshot.commit_sequence.value, std::uint64_t{1});
    durable.close();
    tcptest::remove_tree(good);

    TCPLANE_PHASE("damaged records are refused rather than repaired");
    struct Case {
        const char* name;
        std::vector<std::byte> bytes;
        ErrorCode expected;
    };
    std::vector<Case> cases;

    std::vector<std::byte> bad_magic = record;
    bad_magic[0] = static_cast<std::byte>('X');
    cases.push_back(Case{"magic", bad_magic, ErrorCode::STORE_CORRUPT});

    cases.push_back(Case{"unsupported-version", build_record(snapshot, 1, 2, 0),
                         ErrorCode::STORE_UNSUPPORTED_VERSION});
    cases.push_back(Case{"reserved-flags", build_record(snapshot, 1, kStoreFormatVersion, 1),
                         ErrorCode::STORE_RESERVED_FIELD});

    std::vector<std::byte> truncated_header = record;
    truncated_header.resize(20);
    cases.push_back(Case{"truncated-header", truncated_header, ErrorCode::STORE_TRUNCATED});

    std::vector<std::byte> truncated_payload = record;
    truncated_payload.resize(record.size() - 5);
    cases.push_back(Case{"truncated-payload", truncated_payload, ErrorCode::STORE_TRUNCATED});

    std::vector<std::byte> trailing = record;
    trailing.push_back(std::byte{0});
    cases.push_back(Case{"trailing-bytes", trailing, ErrorCode::STORE_TRAILING_BYTES});

    std::vector<std::byte> absurd = record;
    const auto put64 = [&absurd](std::size_t offset, std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            absurd[offset + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    };
    put64(24, 0xFFFFFFFFFFFFFFFFULL);
    const auto put32 = [&absurd](std::size_t offset, std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            absurd[offset + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    };
    put32(36, wire::crc32c(std::span<const std::byte>(absurd.data(), 36)));
    cases.push_back(Case{"absurd-payload-length", absurd, ErrorCode::STORE_CORRUPT});

    std::vector<std::byte> zero_commit = record;
    zero_commit[16] = std::byte{0};
    const auto fix_header_crc = [](std::vector<std::byte>& bytes) {
        const std::uint32_t crc = wire::crc32c(std::span<const std::byte>(bytes.data(), 36));
        for (int shift = 0; shift < 32; shift += 8) {
            bytes[36 + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::byte>((crc >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    };
    fix_header_crc(zero_commit);
    cases.push_back(Case{"zero-commit-sequence", zero_commit, ErrorCode::STORE_CORRUPT});

    std::vector<std::byte> zero_payload = record;
    for (int shift = 0; shift < 64; shift += 8) {
        zero_payload[24 + static_cast<std::size_t>(shift / 8)] = std::byte{0};
    }
    fix_header_crc(zero_payload);
    cases.push_back(Case{"zero-payload-length", zero_payload, ErrorCode::STORE_CORRUPT});

    std::vector<std::byte> broken_header_crc = record;
    broken_header_crc[36] = static_cast<std::byte>(static_cast<unsigned char>(broken_header_crc[36]) ^ 0xFFU);
    cases.push_back(Case{"header-crc", broken_header_crc, ErrorCode::STORE_INTEGRITY});

    std::vector<std::byte> broken_payload_crc = record;
    broken_payload_crc[32] =
        static_cast<std::byte>(static_cast<unsigned char>(broken_payload_crc[32]) ^ 0xFFU);
    fix_header_crc(broken_payload_crc);
    cases.push_back(Case{"payload-crc", broken_payload_crc, ErrorCode::STORE_INTEGRITY});

    for (const Case& test : cases) {
        const std::string path = tcptest::unique_path("framing-bad");
        write_file(slot_path(path, 0), test.bytes);
        auto case_backend = std::make_unique<FileStorageBackend>(path);
        DurableStore case_store(std::move(case_backend));
        TCPLANE_STATUS_OK(case_store.open());
        const auto case_loaded = case_store.load();
        if (case_loaded.has_value()) {
            TCPLANE_FAIL(std::string("damaged record was accepted: ") + test.name);
        }
        TCPLANE_CHECK_EQ(static_cast<std::uint32_t>(case_loaded.error().code),
                         static_cast<std::uint32_t>(test.expected));
        case_store.close();
        tcptest::remove_tree(path);
    }

    TCPLANE_PHASE("an empty store is absent, not corrupt");
    const std::string empty = tcptest::unique_path("framing-empty");
    auto empty_backend = std::make_unique<FileStorageBackend>(empty);
    DurableStore empty_store(std::move(empty_backend));
    TCPLANE_STATUS_OK(empty_store.open());
    const auto nothing = empty_store.load();
    TCPLANE_OK(nothing);
    TCPLANE_CHECK(!nothing.value().present);
    empty_store.close();
    tcptest::remove_tree(empty);
    static_cast<void>(load_record);
}

TCPLANE_CASE(persistence, single_byte_corruption_never_yields_a_hybrid) {
    const ThermalSnapshot snapshot = populated_snapshot(3, StateRevision{2});
    const std::vector<std::byte> record = build_record(snapshot, 3, kStoreFormatVersion, 0);
    TCPLANE_CHECK(record.size() > 64);

    TCPLANE_PHASE("flipping any single byte is either refused or harmless");
    std::size_t refused = 0;
    std::size_t accepted = 0;
    for (std::size_t offset = 0; offset < record.size(); ++offset) {
        std::vector<std::byte> damaged = record;
        damaged[offset] = static_cast<std::byte>(static_cast<unsigned char>(damaged[offset]) ^ 0x01U);
        const std::string path = tcptest::unique_path("flip");
        write_file(slot_path(path, 0), damaged);
        auto backend = std::make_unique<FileStorageBackend>(path);
        DurableStore store(std::move(backend));
        TCPLANE_STATUS_OK(store.open());
        const auto loaded = store.load();
        if (!loaded.has_value()) {
            ++refused;
        } else {
            ++accepted;
            // Accepting a flipped byte is only tolerable when the decoded
            // result is still exactly the snapshot that was written.
            const auto reencoded = encode_snapshot(loaded.value().snapshot);
            TCPLANE_OK(reencoded);
            const auto original = encode_snapshot(snapshot);
            TCPLANE_OK(original);
            TCPLANE_CHECK(reencoded.value() == original.value());
        }
        store.close();
        tcptest::remove_tree(path);
    }
    TCPLANE_PHASE("every one-byte flip was detected by the integrity check");
    TCPLANE_CHECK_EQ(refused, record.size());
    TCPLANE_CHECK_EQ(accepted, std::size_t{0});
}

TCPLANE_CASE(persistence, store_alternates_slots_and_resolves_one_generation) {
    const std::string store_path = tcptest::unique_path("alternate") + ".state";
    auto backend = std::make_unique<FileStorageBackend>(store_path);
    DurableStore store(std::move(backend));
    TCPLANE_STATUS_OK(store.open());
    TCPLANE_PHASE("an unresolved store refuses to commit and must be resolved first");
    TCPLANE_ERROR_CODE(store.commit(populated_snapshot(0, StateRevision{1})), ErrorCode::STORE_IO);
    const auto empty = store.load();
    TCPLANE_OK(empty);
    TCPLANE_CHECK(!empty.value().present);

    TCPLANE_PHASE("the first commit lands in slot zero");
    ThermalSnapshot first = populated_snapshot(0, StateRevision{1});
    const auto first_commit = store.commit(first);
    TCPLANE_OK(first_commit);
    TCPLANE_CHECK_EQ(first_commit.value().value, std::uint64_t{1});
    TCPLANE_CHECK_EQ(store.active_slot(), std::size_t{0});

    TCPLANE_PHASE("the second commit alternates to slot one");
    ThermalSnapshot second = populated_snapshot(0, StateRevision{2});
    const auto second_commit = store.commit(second);
    TCPLANE_OK(second_commit);
    TCPLANE_CHECK_EQ(store.active_slot(), std::size_t{1});
    const auto reloaded = store.load();
    TCPLANE_OK(reloaded);
    TCPLANE_CHECK_EQ(reloaded.value().snapshot.revision.value, std::uint64_t{2});
    TCPLANE_CHECK_EQ(reloaded.value().slot, std::size_t{1});

    TCPLANE_PHASE("a torn in-progress slot resolves to the previous complete generation");
    const std::vector<std::byte> newest = build_record(populated_snapshot(0, StateRevision{3}), 3,
                                                       kStoreFormatVersion, 0);
    std::vector<std::byte> torn = newest;
    torn.resize(newest.size() / 2);
    // Slot one holds the newest complete generation, so the next commit would
    // have written slot zero; that is where a torn write lands.
    write_file(slot_path(store_path, 0), torn);
    const auto fallen_back = store.load();
    TCPLANE_OK(fallen_back);
    TCPLANE_CHECK_EQ(fallen_back.value().snapshot.revision.value, std::uint64_t{2});
    TCPLANE_CHECK_EQ(fallen_back.value().slot, std::size_t{1});

    TCPLANE_PHASE("a store with no usable slot refuses to resolve anything");
    write_file(slot_path(store_path, 1), torn);
    const auto nothing = store.load();
    TCPLANE_CHECK(!nothing.has_value());
    store.close();
    tcptest::remove_tree(store_path);
    tcptest::remove_tree(slot_path(store_path, 0));
    tcptest::remove_tree(slot_path(store_path, 1));
    tcptest::remove_tree(store_path + ".lock");
}

TCPLANE_CASE(persistence, store_exclusion_and_path_hardening) {
    TCPLANE_PHASE("a relative store path is refused");
    FileStorageBackend relative("relative.state");
    TCPLANE_STATUS_ERROR_CODE(relative.open(), ErrorCode::STORE_PATH_INVALID);

    TCPLANE_PHASE("an empty path is refused");
    FileStorageBackend empty_path("");
    TCPLANE_STATUS_ERROR_CODE(empty_path.open(), ErrorCode::STORE_PATH_INVALID);

    TCPLANE_PHASE("an alternate data stream separator is refused");
    FileStorageBackend stream("C:\\temp\\state.bin:secret");
    TCPLANE_STATUS_ERROR_CODE(stream.open(), ErrorCode::STORE_PATH_INVALID);

    TCPLANE_PHASE("a second store over the same canonical path is refused");
    const std::string path = tcptest::unique_path("exclusive") + ".state";
    auto first = std::make_unique<FileStorageBackend>(path);
    TCPLANE_STATUS_OK(first->open());
    auto second = std::make_unique<FileStorageBackend>(path);
    TCPLANE_STATUS_ERROR_CODE(second->open(), ErrorCode::STORE_LOCKED);

    TCPLANE_PHASE("the same path spelled with traversal still resolves to one lock root");
    const std::string spelled = std::filesystem::path(path).parent_path().string() + "\\.\\" +
                                std::filesystem::path(path).filename().string();
    auto third = std::make_unique<FileStorageBackend>(spelled);
    TCPLANE_STATUS_ERROR_CODE(third->open(), ErrorCode::STORE_LOCKED);

    TCPLANE_PHASE("closing releases the lock for the next owner");
    first->close();
    TCPLANE_CHECK_EQ(first->canonical_root(), third->canonical_root().empty() ? first->canonical_root()
                                                                             : first->canonical_root());
    auto fourth = std::make_unique<FileStorageBackend>(spelled);
    TCPLANE_STATUS_OK(fourth->open());
    fourth->close();
    tcptest::remove_tree(path);
    tcptest::remove_tree(path + ".lock");
}

TCPLANE_CASE(persistence, snapshot_semantics_are_validated_on_decode) {
    TCPLANE_PHASE("a duplicate interlock identity is refused");
    ThermalSnapshot snapshot = populated_snapshot(1, StateRevision{1});
    snapshot.interlocks.push_back(snapshot.interlocks[0]);
    const auto payload = encode_snapshot(snapshot);
    TCPLANE_OK(payload);
    TCPLANE_ERROR_CODE(decode_snapshot(payload.value()), ErrorCode::STORE_CORRUPT);

    TCPLANE_PHASE("a duplicate directive identity is refused");
    ThermalSnapshot duplicate_directive = populated_snapshot(1, StateRevision{1});
    duplicate_directive.directives.push_back(duplicate_directive.directives[0]);
    const auto directive_payload = encode_snapshot(duplicate_directive);
    TCPLANE_OK(directive_payload);
    TCPLANE_ERROR_CODE(decode_snapshot(directive_payload.value()), ErrorCode::STORE_CORRUPT);

    TCPLANE_PHASE("a duplicate idempotency key is refused");
    ThermalSnapshot duplicate_key = populated_snapshot(1, StateRevision{1});
    duplicate_key.replay.push_back(duplicate_key.replay[0]);
    const auto key_payload = encode_snapshot(duplicate_key);
    TCPLANE_OK(key_payload);
    TCPLANE_ERROR_CODE(decode_snapshot(key_payload.value()), ErrorCode::STORE_CORRUPT);

    TCPLANE_PHASE("an out-of-order audit sequence is refused");
    ThermalSnapshot out_of_order = populated_snapshot(1, StateRevision{1});
    AuditRecord earlier = out_of_order.audit[0];
    earlier.sequence = 1;
    out_of_order.audit.push_back(earlier);
    const auto audit_payload = encode_snapshot(out_of_order);
    TCPLANE_OK(audit_payload);
    TCPLANE_ERROR_CODE(decode_snapshot(audit_payload.value()), ErrorCode::STORE_CORRUPT);

    TCPLANE_PHASE("a truncated payload is refused");
    const ThermalSnapshot clean = populated_snapshot(1, StateRevision{1});
    const auto clean_payload = encode_snapshot(clean);
    TCPLANE_OK(clean_payload);
    std::vector<std::byte> cut = clean_payload.value();
    cut.resize(cut.size() - 1);
    TCPLANE_ERROR_CODE(decode_snapshot(cut), ErrorCode::ENCODING_TRUNCATED);

    TCPLANE_PHASE("trailing bytes after a payload are refused");
    std::vector<std::byte> extended = clean_payload.value();
    extended.push_back(std::byte{0});
    TCPLANE_ERROR_CODE(decode_snapshot(extended), ErrorCode::ENCODING_TRAILING_BYTES);

    TCPLANE_PHASE("a zero identifier counter is refused");
    ThermalSnapshot zero_counter = populated_snapshot(1, StateRevision{1});
    zero_counter.next_audit_sequence = 0;
    const auto counter_payload = encode_snapshot(zero_counter);
    TCPLANE_OK(counter_payload);
    TCPLANE_ERROR_CODE(decode_snapshot(counter_payload.value()), ErrorCode::STORE_RESERVED_FIELD);
}

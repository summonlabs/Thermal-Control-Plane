# Thermal Control Plane

Vendor-neutral C++20 runtime for facility-wide thermal operating authority.

## The core question

What facility-wide thermal operating state and thermal authority are valid now,
given current temperature and headroom evidence, thermal limits, degraded
conditions, placement and power dependencies, and the control generation — and
which thermal actions or directives must be refused as stale, unsafe,
unauthorized, or insufficiently evidenced?

Thermal Control Plane 1.0.0 is the answer to that question as a library: it
selects a canonical facility thermal mode, computes exact integer headroom from
externally supplied evidence, selects a bounded derating, gates recovery behind
hysteresis, and refuses every authority attempt that is bound to state which is
no longer current.

## What Thermal Control Plane owns

- Canonical facility thermal operating modes: normal, constrained, degraded,
  emergency, recovery, maintenance and isolated.
- Generation-bound thermal limits and safe operating envelopes.
- Thermal headroom assessment from externally supplied evidence.
- Derating authority and bounded derating directives.
- Escalation precedence and recovery eligibility.
- Deterministic thermal policy evaluation.
- Coordination requests to facility placement and power control.
- Authority attempts, acknowledgements, observations and verified outcomes
  wherever this runtime itself has a state transition.
- Durable audit, recovery, replay and stale-authority fencing.

## What Thermal Control Plane does not own

This runtime deliberately does not own, model or reimplement:

- cooling topology;
- constituent cooling-capacity accounting;
- the facility allocatable Cooling Capacity owned by the cooling-capacity layer;
- airflow actuation;
- liquid-cooling plant or device actuation;
- zone-local thermal modelling, which belongs to the Thermal Zone Manager;
- cooling-source failover orchestration;
- emergency cross-domain orchestration, which belongs to the Thermal Emergency
  Manager;
- facility placement decisions;
- electrical power-control decisions or load shedding;
- ASI workload scheduling or DFI network decisions;
- BMS, PLC or PID control loops.

All of those arrive here as typed, generation-stamped evidence, opaque
references, or advisory requests. This runtime never actuates a physical device.

## Core principles

1. **A temperature observation is not authority.** Evidence is an input to a
   decision; only an explicit authority attempt produces a directive.
2. **A directive is not proof that a physical condition changed.** Verification
   requires evidence observed after the directive, and the default answer for an
   unobserved directive is unproven.
3. **Missing is never safe.** An absent, stale, indeterminate or unsupported
   measurement leaves headroom unknown; it never becomes zero, comfort or
   permission.
4. **Everything that decides authority is an exact integer.** Temperatures are
   millidegrees Celsius, deratings are basis points, and every arithmetic
   operation that can overflow is checked.
5. **Every decision is bound to the state it was planned against.** A stale
   epoch, generation or revision is a refusal with a stable code.

## Thermal modes and escalation precedence

The canonical modes are, in increasing severity:

| Mode | Severity | Meaning |
| --- | --- | --- |
| `Normal` | 0 | every declared threshold is met and evidence is usable |
| `Recovery` | 1 | evidence is favourable and the gate to `Normal` is completing |
| `Maintenance` | 1 | an operator window that restricts authority |
| `Constrained` | 2 | the warning band or the advisory margin is reached |
| `Degraded` | 3 | the derate band is reached, or required evidence is unusable |
| `Emergency` | 4 | a critical or shutdown limit is reached, or a safety interlock is asserted |
| `Isolated` | 5 | an isolation interlock is asserted, or the operator isolated the facility |

The primary reason reported for a decision is chosen by a total precedence order,
so the same evidence always produces the same primary cause:

```
SafetyInterlock, IsolationInterlock, ShutdownLimitViolation, CriticalLimitViolation,
EvidenceIndeterminate, AwaitingRevalidation, EvidenceStale, EvidenceUnknown,
GenerationMismatch, DerateLimitViolation, WarningLimitViolation, RecoveryPending,
MaintenanceWindow, AdvisoryMargin, OperatorDirective, Nominal
```

Explicit interlocks and physical limit violations therefore always outrank
advisory optimisation. Ties inside one cause are broken by canonical scope
order, then limit severity, then sensor identity.

Escalation is mandatory and immediate. De-escalation is gated, moves exactly one
rung at a time, and never skips a rung: leaving `Emergency` reaches `Degraded`,
`Constrained`, then `Recovery`, and only a further completed gate reaches
`Normal`.

## Thermal evidence

An observation carries its sensor, its scope, the generation of the scope object
it was taken against, the generation of the evidence stream, a per-sensor
monotonic sequence, the producing incarnation, and a monotonic stamp. Wall-clock
time is carried for human use and is never used to decide freshness.

Freshness is decided by the monotonic tick window plus the sequence and
generation. A stamp ahead of the clock is `FUTURE`, not fresh. A duplicated
payload under an accepted sequence is a duplicate; the same sequence with a
different payload is a conflict; a lower sequence is superseded. A batch is
applied atomically: one refused observation refuses the whole batch.

Observations are never persisted. A recovered runtime has no current evidence, so
every scope with published limits starts degraded until a fresh source reports.
For an in-place recovery inside one process, evidence stamped before the recovery
point is retained for explanation but marked as awaiting revalidation.

## Thermal limits and headroom

A limit set declares advisory, warning, derate, critical and shutdown thresholds,
each either explicitly defined or explicitly absent. Absent is not zero: an
undefined threshold carries no value. Defined thresholds must be non-decreasing
in severity, and a set that violates the ordering is refused rather than
repaired.

Headroom is `limit - observation` in exact millidegrees. A scope has one headroom
per defined limit kind, and a governing headroom which is the tightest defined
threshold, so it is negative as soon as any declared threshold, including an
advisory one, has been reached.

Facility headroom is `Known` only when every scope with published limits has
known headroom. One missing, stale or indeterminate scope makes the facility
headroom unknown, and an unknown facility headroom has no reportable number at
all. Evidence taken against a different scope generation cannot be compared with
the published limits, so headroom is unknown with a generation-mismatch reason.

## Derating

The policy carries a derating ladder indexed by limit kind, a derating ceiling,
and a floor for unusable evidence. Validation enforces that the ladder never
decreases with severity and that unusable evidence derates at least as hard as
any defined threshold can, which is the precondition that makes derating monotone
in worsening evidence. A policy that deliberately breaks monotonicity must say so
explicitly.

Granted derating is clamped by the scope ceiling and by the policy ceiling, so a
directive can never exceed the authority it was issued under.

## Recovery and hysteresis

Stepping down requires a gate that has been continuously favourable for the
policy dwell and has counted the policy minimum of distinct evidence advances.
Counting is driven by observation sequences, so re-evaluating identical evidence
is idempotent and never accumulates credit. A single favourable observation can
never move the facility: the dwell and the sample requirement both apply.

Recovery is measured against an escalation threshold minus the policy hysteresis,
so a temperature parked exactly on a threshold can never oscillate the facility
between two modes. Worsening evidence resets the whole run.

A gate that was open before a restart is discarded: dwell accumulated in a
previous process incarnation is not evidence that conditions have been sustained
since.

## Authority, generations and fencing

Every authority attempt carries a binding:

```
process incarnation, control-plane epoch, policy generation,
topology generation, evidence generation, state revision
```

The comparison order is fixed, so the same stale attempt always returns the same
primary code: incarnation, epoch, policy, topology, evidence, revision. Each
comparison distinguishes a superseded value (`STALE_*`) from a value that does not
exist yet (`FUTURE_*`).

Before any of that, the runtime refuses authority until an interlock baseline has
been declared: a missing interlock statement is not a statement that nothing is
asserted.

A decision that needs evidence additionally requires known facility headroom. Stale,
unknown, indeterminate, unsupported and awaiting-revalidation evidence each refuse
with their own code and emit nothing.

## Idempotency and replay

Every attempt carries an idempotency key and is reduced to a canonical digest
covering the caller-supplied semantics: request identity, kind, authority class,
binding, scope, requested derating, requested mode, coordination kind and
directive. The attempt identity is excluded, because it distinguishes send
attempts rather than semantics.

A repeated key with an identical digest replays the recorded outcome verbatim,
with `REPLAYED`, without actuating anything and without moving the state
revision. A repeated key with a different digest is refused as
`IDEMPOTENCY_CONFLICT` and never silently replayed. The digest is compared as
bytes, so a hash collision cannot turn a conflict into a replay.

Replay is resolved before the generation fences, because a replay is not a new
act. The replayed outcome carries the revision it originally produced, so a
caller can see that the world has moved on.

## Verification

`verify_directive` answers whether an issued directive was borne out:

- `Proven` requires a fresh observation for the directive scope, newer than the
  observation sequence recorded at issuance, stamped after the directive, and
  back below the directive basis limit;
- `Contradicted` means such an observation exists and the scope is still at or
  above the limit;
- `Unproven` is the answer when there is no such observation, which is also the
  answer immediately after a directive is issued;
- `Superseded` is reported when a newer directive exists for the same scope.

Acknowledgement is recorded as an acknowledgement and never as evidence.

## Coordination

Coordination notices to facility placement and power control are advisory, typed
and generation-stamped. They are emitted only by an explicit granted coordination
request, never automatically, and only while facility headroom is known. The
outbound queue is bounded; a full queue refuses with `RESOURCE_EXHAUSTED` rather
than dropping a notice.

## Persistence and recovery

Durable state is written as alternating generations across two slots. Each record
carries an eight-byte magic, a format version, a reserved flags field that must be
zero, the commit sequence, the payload length and CRC-32C integrity for both the
header and the payload. Decoding is strict: truncation, trailing bytes, unknown
enum values, impossible lengths, reserved fields holding data, duplicate
identities and out-of-order audit sequences are all refused.

```
magic[8] | version u32 | flags u32 | commit u64 | payload_length u64 |
payload_crc u32 | header_crc u32 | payload
```

On open, both slots are read and exactly one generation is resolved: the highest
complete commit sequence wins, a slot that fails integrity is ignored rather than
merged, and two slots claiming the same sequence are only accepted when their
bytes are identical. A store whose slots are all unreadable is refused; it is
never silently started from scratch. A commit writes the slot the previous
generation does not occupy, reads it back byte for byte, and only then becomes the
authoritative generation. A commit is refused until the store has been resolved
with `load()`.

The store takes an exclusive lock on a file next to the store, keyed on the
canonicalised path, so two processes cannot obtain different locks for the same
logical store. A failed durable commit leaves the runtime faulted: further
authority mutations are refused until `recover()` re-reads the store, so in-memory
authority can never drift ahead of durable authority.

What survives a restart is authority: the mode, the installed policy, the
administrative mode and hold, the interlock statement, the audit tail, the
directive tail and the replay ledger. What does not survive is dynamic evidence
and per-scope limits, because a measurement that outlived its process is not
current evidence and limits are republished by their owner.

## Concurrency model

Single writer, with real exclusion:

- one `std::shared_mutex` guards the runtime; scalar identity accessors take a
  shared lock and everything that touches the clock or mutable state takes the
  exclusive lock;
- the lock is never upgraded: no path acquires the exclusive lock while holding a
  shared guard;
- no callback, observer or adapter hook is invoked while the lock is held; the
  injected clock is called only under the exclusive lock, and the storage backend
  never calls back into the runtime;
- there are no internal worker threads, so there is no join-under-lock, no
  shutdown lock inversion and no stale asynchronous completion inside the
  runtime; asynchronous outcomes arrive as ordinary attempts and are fenced by
  their binding like any other;
- durable commits happen under the exclusive lock, which is deliberate: the
  generation published must be the generation the decision was made against.

The persistence record is written and read back while the same exclusive lock is
held, and the storage backend is a leaf: it takes no lock the runtime holds and
invokes no user code.

## Resource bounds

Every caller-controlled resource is bounded and enforced with `RESOURCE_EXHAUSTED`:
scopes, sensors, observations per batch, limit sets, interlocks, audit records,
directives, replay entries, outbound notices and verification records. History is
kept as bounded tails with documented oldest-first eviction; an evicted directive
is reported as unknown rather than silently verified.

## Public API

The policy layer is pure. `evaluate_policy` takes the policy, the environment and
the situation as values and returns the outcome; it reads no clock, takes no lock
and performs no IO, so it is independently testable from every adapter.

```cpp
#include <thermal_control_plane/runtime.hpp>

auto runtime = thermal_control_plane::ThermalRuntime::create(options, std::move(clock));
runtime.value()->install_policy(policy, thermal_control_plane::PolicyGeneration{});
runtime.value()->publish_limits(limits);
runtime.value()->declare_interlock_baseline(topology, asserted_interlocks);
runtime.value()->ingest(observation_batch);
const auto outcome = runtime.value()->authorize(attempt);
```

## Strongly typed identities

Epochs, incarnations, policy, topology, evidence and scope generations, state
revisions, commit sequences, attempt and observation sequences, directives and
interlocks are distinct types, so a generation can never be passed where a
revision is expected. `ScopeRef` orders canonically so that tie-breaking is
deterministic.

## Building

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Requirements: CMake 3.21 or newer and a C++20 compiler. The validated toolchain
is MSVC 19.44 with `/W4 /permissive- /WX`. Both Release and Debug are expected to
build with zero warnings.

Options: `TCPLANE_BUILD_TESTS`, `TCPLANE_BUILD_EXAMPLES`, `TCPLANE_BUILD_BENCHMARKS`,
`TCPLANE_BUILD_TOOLS`, `TCPLANE_ENABLE_ASAN` and `TCPLANE_WARNINGS_AS_ERRORS`.

## Testing

```
ctest --test-dir build/release --output-on-failure
```

Each suite is also a standalone executable that accepts `--list`, `--case <id>`
and `--filter <prefix>`. No test is given a timeout anywhere in the build or the
test definitions; a hanging case is a defect to diagnose rather than terminate.

## Installing and consuming

```
cmake --install build/release --prefix /your/prefix
cmake -S consumer -B build/consumer -DCMAKE_PREFIX_PATH=/your/prefix
cmake --build build/consumer
./build/consumer/tcplane_consumer
```

The `consumer/` directory is a standalone CMake project that is not part of this
build. It resolves the installed package with `find_package(ThermalControlPlane
CONFIG REQUIRED)`, links `ThermalControlPlane::ThermalControlPlane`, and runs a
real library lifecycle: commission a synthetic facility, assess it, obtain a
bounded derating directive, and confirm that the directive is not treated as proof
of a physical change.

## CLI and examples

```
tcp_cli assess    --zones 2 --temperature 91000 --derate 0
tcp_cli authorize --zones 2 --temperature 91000 --request 3000 --key cli-1
tcp_cli inspect   --store state.bin
tcp_cli verify    --store state.bin
```

`assess` and `authorize` drive a synthetic facility; `inspect` opens a durable
store directly and reports the resolved generation; `verify` reports the slot
layout and which generation resolves.

Four examples are built as `tcplane_example_*` targets: `assess_facility`,
`derating_and_recovery`, `stale_evidence_refusal` and `durable_session`.

## Benchmarks

```
tcplane_bench /path/for/durable/state
```

The benchmark measures completed operations: a pure policy evaluation over four
synthetic zones, an authority decision on a volatile runtime, and an authority
decision whose answer is only returned after the durable commit and read-back
verification complete. Warm-up and measured iterations are printed with every row.

## Real versus synthetic proof

**REAL.** The C++20 library, the build, the test executables, the filesystem
behaviour of the durable store, the exclusive inter-process lock, real process
death and restart, the AddressSanitizer run, the CMake install surface and the
out-of-tree `find_package` consumer all ran on the development host.

**SYNTHETIC.** Every facility, zone, sensor, temperature, limit set, interlock and
derating response used by the examples, the CLI, the benchmark and the tests comes
from the synthetic facility model in `synthetic.hpp`. It is exact integer
arithmetic and completely deterministic, and it is not hardware evidence.

**UNSUPPORTED.** No real data centre, chiller plant, CDU, CRAH/CRAC, pump, valve,
facility BMS, DCIM, PLC or vendor thermal SDK was available, so no claim is made
about physical plant behaviour. The cross-process lock and crash recovery are
validated on Windows only: the POSIX branch of the file backend is written but was
not built or exercised here, and no sanitizer other than MSVC AddressSanitizer was
run.

## Validation performed at 1.0.0

- Release build with `/W4 /permissive- /WX`: zero warnings.
- Debug build with the same settings: zero warnings.
- 71 test cases across 13 suites, all passing in Release and in Debug.
- The same 71 cases passing under MSVC AddressSanitizer with no sanitizer report.
- Single-byte corruption of every byte of a store record: refused by the integrity
  check, never accepted as a different generation.
- Torn slot, unreadable slot, missing slot and contradictory slots: each resolves
  to a complete generation or refuses to open.
- Real process death after a slot write and while holding the interlock: the store
  resolves to a complete generation and the lock is released by the operating
  system.
- Independent downstream consumer built against the installed package and executed.

## Limitations

- The runtime makes no attempt to model heat, airflow or coolant: it consumes
  measurements and produces authority.
- Evidence is supplied by adapters. This repository ships a synthetic facility
  model only.
- The durable store keeps bounded history; a directive evicted from the tail is
  reported as unknown rather than reconstructed.
- The POSIX branch of the file backend is unvalidated in this environment.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

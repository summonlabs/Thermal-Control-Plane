# Contributing to Thermal Control Plane

Thank you for your interest in contributing to Thermal Control Plane. This
document describes the contribution terms for this project.

## License

By contributing to this project, you agree that your contributions are licensed
under the **Apache License, Version 2.0**. See the `LICENSE` file for the full
license text and the `NOTICE` file for attribution and license notices. There is
**no separate Contributor License Agreement (CLA)** requirement: you retain
ownership of your contributions and grant the project a license to use them under
the terms of the Apache License 2.0.

## License headers

New source files should carry the following header:

```
// Thermal Control Plane — <file purpose>.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
```

## Coding standards

- C++20, CMake 3.21 or newer, MSVC first.
- Build cleanly with `/W4 /permissive- /WX`; warnings are not suppressed to make a
  build green.
- Physical quantities are exact integers with explicit units. Do not introduce
  floating point into anything that decides authority or accounting.
- Every authority-bearing decision must be bound to the epoch, generations and
  revision it was planned against. A new decision path needs a fence test.
- Keep the policy layer pure: no clock, no locks, no IO. Adapters supply evidence.
- Preserve the systems boundary. Cooling topology, cooling-capacity accounting,
  zone-local modelling, actuation and cross-domain emergency orchestration belong
  to other layers and arrive here as typed evidence or opaque references.

## Testing

Add proof, not decoration. Depending on the change, expect to add:

- a unit or property test against an independent reference model;
- an adversarial or malformed-input test for any decoder, path or bound;
- a fencing test for any new generation or revision comparison;
- a persistence or restart test for any change to the durable format;
- a concurrency or real-process test for any change to locking or durability.

Run the full suite before submitting:

```
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Tests are not given timeouts. A hanging test is a defect to diagnose and fix.

## Pull requests

Please keep changes focused, add tests for new behaviour, and ensure the
repository builds and tests cleanly in both Release and Debug.

Do not add co-author trailers or AI attribution to commits.

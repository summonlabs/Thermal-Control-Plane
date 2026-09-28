// Thermal Control Plane — quantity and limit ordering proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

namespace {

using namespace thermal_control_plane;

}  // namespace

TCPLANE_CASE(quantity, temperature_range_boundaries) {
    TCPLANE_PHASE("absolute zero is the inclusive lower bound");
    const auto below = Temperature::from_milli_celsius(-273151);
    TCPLANE_ERROR_CODE(below, ErrorCode::OUT_OF_RANGE);
    const auto at_zero = Temperature::from_milli_celsius(-273150);
    TCPLANE_OK(at_zero);
    TCPLANE_CHECK_EQ(at_zero.value().milli_celsius(), -273150);

    TCPLANE_PHASE("the upper bound is inclusive and one past it is refused");
    const auto at_max = Temperature::from_milli_celsius(1000000);
    TCPLANE_OK(at_max);
    TCPLANE_CHECK_EQ(at_max.value().milli_celsius(), 1000000);
    const auto above = Temperature::from_milli_celsius(1000001);
    TCPLANE_ERROR_CODE(above, ErrorCode::OUT_OF_RANGE);
}

TCPLANE_CASE(quantity, checked_arithmetic_refuses_overflow) {
    TCPLANE_PHASE("addition overflow");
    TCPLANE_ERROR_CODE(checked_add(9223372036854775807LL, 1), ErrorCode::ARITHMETIC_OVERFLOW);
    TCPLANE_ERROR_CODE(checked_add(-9223372036854775807LL - 1LL, -1), ErrorCode::ARITHMETIC_OVERFLOW);

    TCPLANE_PHASE("subtraction overflow");
    TCPLANE_ERROR_CODE(checked_sub(-9223372036854775807LL - 1LL, 1), ErrorCode::ARITHMETIC_OVERFLOW);

    TCPLANE_PHASE("multiplication overflow");
    TCPLANE_ERROR_CODE(checked_mul(9223372036854775807LL, 2), ErrorCode::ARITHMETIC_OVERFLOW);
    TCPLANE_OK(checked_mul(0, 9223372036854775807LL));
    const auto product = checked_mul(-3, 5);
    TCPLANE_OK(product);
    TCPLANE_CHECK_EQ(product.value(), -15);
}

TCPLANE_CASE(quantity, basis_points_and_duration_bounds) {
    TCPLANE_PHASE("basis points are bounded on both sides");
    TCPLANE_ERROR_CODE(BasisPoints::from_value(-1), ErrorCode::OUT_OF_RANGE);
    TCPLANE_OK(BasisPoints::from_value(0));
    TCPLANE_OK(BasisPoints::from_value(10000));
    TCPLANE_ERROR_CODE(BasisPoints::from_value(10001), ErrorCode::OUT_OF_RANGE);

    TCPLANE_PHASE("durations are never negative");
    TCPLANE_ERROR_CODE(Duration::from_millis(-1), ErrorCode::OUT_OF_RANGE);
    TCPLANE_OK(Duration::from_millis(0));
}

TCPLANE_CASE(quantity, freshness_boundaries) {
    const FreshnessWindow window{Duration::from_millis_unchecked(1000)};
    TCPLANE_PHASE("an unstamped observation is never fresh");
    TCPLANE_CHECK(classify_freshness(Tick{500}, Tick{0}, window) == FreshnessState::Unstamped);
    TCPLANE_PHASE("exactly the window is fresh and one tick past it is stale");
    TCPLANE_CHECK(classify_freshness(Tick{1000}, Tick{0}, window) ==
                  FreshnessState::Unstamped);
    TCPLANE_CHECK(classify_freshness(Tick{1000}, Tick{1}, window) == FreshnessState::Fresh);
    TCPLANE_CHECK(classify_freshness(Tick{1001}, Tick{1}, window) == FreshnessState::Fresh);
    TCPLANE_CHECK(classify_freshness(Tick{1002}, Tick{1}, window) == FreshnessState::Stale);
    TCPLANE_PHASE("a future stamp is refused rather than treated as fresh");
    TCPLANE_CHECK(classify_freshness(Tick{10}, Tick{11}, window) == FreshnessState::Future);
    TCPLANE_PHASE("a monotonic clock that moves backwards is an error");
    TCPLANE_ERROR_CODE(elapsed(Tick{10}, Tick{9}), ErrorCode::OUT_OF_RANGE);
}

TCPLANE_CASE(quantity, limit_ordering_is_enforced) {
    const ScopeRef scope = make_scope(1, 1, 1);
    TCPLANE_PHASE("a well-ordered set validates");
    TCPLANE_STATUS_OK(validate(make_limits(scope, 40000, 75000, 85000, 95000, 105000)));

    TCPLANE_PHASE("equal adjacent thresholds are allowed");
    TCPLANE_STATUS_OK(validate(make_limits(scope, 40000, 40000, 85000, 95000, 105000)));

    TCPLANE_PHASE("an inverted pair is refused as an ordering fault");
    TCPLANE_STATUS_ERROR_CODE(validate(make_limits(scope, 40000, 90000, 85000, 95000, 105000)),
                              ErrorCode::LIMIT_ORDER_INVALID);

    TCPLANE_PHASE("an absent threshold is not zero");
    ThermalLimitSet sparse = make_limits(scope, 40000, 75000, 85000, 95000, 105000);
    sparse[LimitKind::Shutdown].defined = false;
    sparse[LimitKind::Shutdown].milli_celsius = 0;
    TCPLANE_STATUS_OK(validate(sparse));
    TCPLANE_CHECK_EQ(sparse.defined_count(), std::size_t{4});

    TCPLANE_PHASE("an absent threshold carrying a value is refused");
    sparse[LimitKind::Shutdown].milli_celsius = 99000;
    TCPLANE_STATUS_ERROR_CODE(validate(sparse), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("a set with no thresholds at all is refused");
    ThermalLimitSet empty = make_limits(scope, 0, 0, 0, 0, 0);
    for (std::size_t i = 0; i < kLimitKindCount; ++i) {
        empty.thresholds[i].defined = false;
    }
    TCPLANE_STATUS_ERROR_CODE(validate(empty), ErrorCode::LIMIT_UNDEFINED);

    TCPLANE_PHASE("thresholds outside the representable range are refused");
    ThermalLimitSet wild = make_limits(scope, 40000, 75000, 85000, 95000, 105000);
    wild[LimitKind::Shutdown].milli_celsius = 2000000;
    TCPLANE_STATUS_ERROR_CODE(validate(wild), ErrorCode::OUT_OF_RANGE);

    TCPLANE_PHASE("identities and generations are required");
    ThermalLimitSet anonymous = make_limits(scope, 40000, 75000, 85000, 95000, 105000);
    anonymous.id = LimitSetId{0};
    TCPLANE_STATUS_ERROR_CODE(validate(anonymous), ErrorCode::INVALID_ARGUMENT);
    ThermalLimitSet generationless = make_limits(scope, 40000, 75000, 85000, 95000, 105000);
    generationless.generation = LimitGeneration{0};
    TCPLANE_STATUS_ERROR_CODE(validate(generationless), ErrorCode::INVALID_ARGUMENT);
}

TCPLANE_CASE(quantity, limit_registry_generation_rules) {
    const ScopeRef scope = make_scope(1, 1, 1);
    LimitRegistry registry(4);
    TCPLANE_PHASE("publishing then re-publishing identical limits is a no-op");
    TCPLANE_STATUS_OK(registry.put(make_limits(scope, 40000, 75000, 85000, 95000, 105000)));
    TCPLANE_STATUS_OK(registry.put(make_limits(scope, 40000, 75000, 85000, 95000, 105000)));

    TCPLANE_PHASE("different limits at the same generation conflict");
    ThermalLimitSet changed = make_limits(scope, 40000, 75000, 85000, 95000, 106000);
    TCPLANE_STATUS_ERROR_CODE(registry.put(changed), ErrorCode::LIMIT_SET_CONFLICT);

    TCPLANE_PHASE("a zero limit generation is structurally refused");
    ThermalLimitSet generationless = make_limits(scope, 40000, 75000, 85000, 95000, 105000,
                                                 4000, LimitGeneration{0});
    TCPLANE_STATUS_ERROR_CODE(registry.put(generationless), ErrorCode::INVALID_ARGUMENT);

    TCPLANE_PHASE("a newer generation replaces the published limits");
    ThermalLimitSet newer = make_limits(scope, 40000, 75000, 85000, 95000, 104000,
                                        4000, LimitGeneration{2});
    TCPLANE_STATUS_OK(registry.put(newer));
    TCPLANE_CHECK(registry.find(scope) != nullptr);
    TCPLANE_CHECK_EQ(registry.find(scope)->generation.value, std::uint64_t{2});

    TCPLANE_PHASE("the registry bound is enforced");
    LimitRegistry small(1);
    TCPLANE_STATUS_OK(small.put(make_limits(scope, 40000, 75000, 85000, 95000, 105000)));
    TCPLANE_STATUS_ERROR_CODE(
        small.put(make_limits(make_scope(1, 2, 2), 40000, 75000, 85000, 95000, 105000)),
        ErrorCode::RESOURCE_EXHAUSTED);
}

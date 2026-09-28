// Thermal Control Plane — test framework with stable case identifiers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_TESTS_FRAMEWORK_HPP
#define THERMAL_CONTROL_PLANE_TESTS_FRAMEWORK_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tcptest {

/// Per-case execution context.
///
/// Progress is printed and flushed immediately, so a case that blocks is
/// visible rather than buffered away. No case is given a timeout: a hang is a
/// defect to diagnose, never something to terminate.
class Context {
public:
    void phase(const char* name);
    void fail(std::string reason);
    void note(std::string text);

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    void set_id(std::string id) { id_ = std::move(id); }
    [[nodiscard]] const std::string& id() const noexcept { return id_; }

private:
    bool failed_ = false;
    std::string reason_;
    std::string id_;
};

struct TestCase {
    std::string id;
    void (*fn)(Context&) = nullptr;
};

class Registry {
public:
    static Registry& instance();
    void add(std::string id, void (*fn)(Context&));
    [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

private:
    std::vector<TestCase> cases_;
};

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)(Context&));
};

/// Entry point shared by every suite executable.
///
/// Usage:
///   <exe>                     run every case
///   <exe> --list              print every stable case id
///   <exe> --case <suite::id>  run exactly one case
///   <exe> --filter <prefix>   run every case whose id starts with prefix
int run_all(int argc, char** argv);

}  // namespace tcptest

#define TCPLANE_CASE(suite, name)                                                                  static void tcplane_case_##suite##_##name(::tcptest::Context& tc_ctx);                         static const ::tcptest::Registrar tcplane_registrar_##suite##_##name(                              #suite, #name, &tcplane_case_##suite##_##name);                                            static void tcplane_case_##suite##_##name(::tcptest::Context& tc_ctx)

#define TCPLANE_PHASE(name) tc_ctx.phase(name)

#define TCPLANE_NOTE(text) tc_ctx.note(text)

#define TCPLANE_FAIL(message)          \
    do {                               \
        tc_ctx.fail(message);          \
        return;                        \
    } while (false)

#define TCPLANE_CHECK(condition)                                                                   do {                                                                                               if (!(condition)) {                                                                                tc_ctx.fail(std::string("check failed: ") + #condition + " at " + __FILE__ + ":" + \
                        std::to_string(__LINE__));                                                         return;                                                                                    }                                                                                      \
    } while (false)

#define TCPLANE_EXPECT(condition)                                                                  do {                                                                                               if (!(condition)) {                                                                                tc_ctx.fail(std::string("expect failed: ") + #condition + " at " + __FILE__ +     \
                        ":" + std::to_string(__LINE__));                                               }                                                                                      \
    } while (false)

// Operands are copied rather than bound by reference: binding a reference to a
// function result does not extend the lifetime of the temporary behind it.
#define TCPLANE_CHECK_EQ(actual, expected)                                                         do {                                                                                       \
        const auto tcplane_a = (actual);                                                       \
        const auto tcplane_b = (expected);                                                     \
        if (!(tcplane_a == tcplane_b)) {                                                       \
            tc_ctx.fail(std::string("equality failed: ") + #actual + " == " + #expected +      \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));                   \
            return;                                                                            \
        }                                                                                      \
    } while (false)

#define TCPLANE_OK(expression)                                                                 \
    do {                                                                                       \
        auto&& tcplane_result = (expression);                                                  \
        if (!tcplane_result.has_value()) {                                                     \
            tc_ctx.fail(std::string("unexpected error: ") + #expression + " -> " +             \
                        std::string(::thermal_control_plane::to_string(tcplane_result.error().code)) + \
                        " " + tcplane_result.error().detail + " at " + __FILE__ + ":" +        \
                        std::to_string(__LINE__));                                             \
            return;                                                                            \
        }                                                                                      \
    } while (false)

#define TCPLANE_STATUS_OK(expression)                                                              do {                                                                                       \
        auto&& tcplane_status = (expression);                                                  \
        if (!tcplane_status.ok()) {                                                            \
            tc_ctx.fail(std::string("unexpected status: ") + #expression + " -> " +            \
                        std::string(::thermal_control_plane::to_string(tcplane_status.code())) + \
                        " " + tcplane_status.error().detail + " at " + __FILE__ + ":" +        \
                        std::to_string(__LINE__));                                             \
            return;                                                                            \
        }                                                                                      \
    } while (false)

#define TCPLANE_ERROR_CODE(expression, expected_code)                                              do {                                                                                       \
        auto&& tcplane_result = (expression);                                                  \
        if (tcplane_result.has_value()) {                                                      \
            tc_ctx.fail(std::string("expected failure but succeeded: ") + #expression +        \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));                   \
            return;                                                                            \
        }                                                                                      \
        if (tcplane_result.error().code != (expected_code)) {                                  \
            tc_ctx.fail(std::string("wrong error code for ") + #expression + ": got " +        \
                        std::string(::thermal_control_plane::to_string(tcplane_result.error().code)) + \
                        " want " +                                                             \
                        std::string(::thermal_control_plane::to_string(expected_code)) +       \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));                   \
            return;                                                                            \
        }                                                                                      \
    } while (false)

#define TCPLANE_STATUS_ERROR_CODE(expression, expected_code)                                       do {                                                                                       \
        auto&& tcplane_status = (expression);                                                  \
        if (tcplane_status.ok()) {                                                             \
            tc_ctx.fail(std::string("expected failure but succeeded: ") + #expression +        \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));                   \
            return;                                                                            \
        }                                                                                      \
        if (tcplane_status.code() != (expected_code)) {                                        \
            tc_ctx.fail(std::string("wrong error code for ") + #expression + ": got " +        \
                        std::string(::thermal_control_plane::to_string(tcplane_status.code())) + \
                        " want " + std::string(::thermal_control_plane::to_string(expected_code)) + \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));                   \
            return;                                                                            \
        }                                                                                      \
    } while (false)

#endif  // THERMAL_CONTROL_PLANE_TESTS_FRAMEWORK_HPP

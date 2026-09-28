// Thermal Control Plane — real independent process helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_TESTS_PROCESS_UTIL_HPP
#define THERMAL_CONTROL_PLANE_TESTS_PROCESS_UTIL_HPP

#include <memory>
#include <string>
#include <vector>

namespace tcptest {

struct ProcessResult {
    bool started = false;
    bool exited = false;
    int exit_code = 0;
    std::string output;
    std::string error;
};

/// Suppress interactive Windows crash UI for this process and its children.
///
/// Called once by every suite that starts a child process, so an intentional
/// crash terminates without a dialog.
void suppress_crash_ui() noexcept;

/// A running child process whose output is captured to a file.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Start a child with no console window, inheriting this process error
    /// mode. There is no timeout anywhere: the caller decides when to wait.
    [[nodiscard]] static ChildProcess start(const std::string& executable,
                                            const std::vector<std::string>& arguments);

    [[nodiscard]] bool started() const noexcept;

    /// Wait for exit and collect the captured output.
    [[nodiscard]] ProcessResult wait();
    /// Terminate the child and wait for it.
    void kill();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Start a child process, wait for it, and capture its combined output.
[[nodiscard]] ProcessResult run_process(const std::string& executable,
                                        const std::vector<std::string>& arguments);

/// Block until a path exists. Bounded by the caller process lifetime only.
[[nodiscard]] bool wait_for_file(const std::string& path);

/// Create an empty marker file.
void touch_file(const std::string& path);

/// Directory for disposable test state.
[[nodiscard]] std::string temp_directory();

/// A unique path below the temp directory.
[[nodiscard]] std::string unique_path(const std::string& stem);

/// Remove a file or directory tree, ignoring absence.
void remove_tree(const std::string& path);

}  // namespace tcptest

#endif  // THERMAL_CONTROL_PLANE_TESTS_PROCESS_UTIL_HPP

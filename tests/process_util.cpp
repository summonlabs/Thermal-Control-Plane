// Thermal Control Plane — process helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "process_util.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tcptest {
namespace {

std::atomic<std::uint64_t> g_counter{0};

std::string quote_argument(const std::string& argument) {
    std::string out;
    out.push_back('"');
    for (const char raw : argument) {
        if (raw == '"') {
            out.push_back('\\');
        }
        out.push_back(raw);
    }
    out.push_back('"');
    return out;
}

}  // namespace

struct ChildProcess::Impl {
#if defined(_WIN32)
    void* process = nullptr;
    void* thread = nullptr;
#else
    int status = 0;
    bool waited = false;
#endif
    std::string output_path;
    bool waited = false;
};

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(std::move(other.impl_)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

ChildProcess::~ChildProcess() {
    if (impl_ != nullptr && !impl_->waited) {
        kill();
    }
}

ChildProcess ChildProcess::start(const std::string& executable,
                                 const std::vector<std::string>& arguments) {
    ChildProcess child;
    child.impl_ = std::make_unique<Impl>();
    child.impl_->output_path = unique_path("child-output") + ".txt";
    child.impl_->waited = false;

    std::string command = quote_argument(executable);
    for (const std::string& argument : arguments) {
        command.push_back(' ');
        command.append(quote_argument(argument));
    }

#if defined(_WIN32)
    // Child output is captured through an inherited file handle rather than a
    // shell redirection: CreateProcess does not interpret ">", and going
    // through cmd.exe would add an interpreter that is not needed here.
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE output = ::CreateFileA(child.impl_->output_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        child.impl_->process = nullptr;
        return child;
    }
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    ::CloseHandle(output);
    if (created == FALSE) {
        child.impl_->process = nullptr;
        return child;
    }
    child.impl_->process = process.hProcess;
    child.impl_->thread = process.hThread;
#else
    // The asynchronous start is implemented for Windows only, which is the
    // platform this repository validates. On other platforms the child is run
    // to completion here with a shell redirection and wait() only collects the
    // captured output.
    const std::string redirected =
        command + " > " + quote_argument(child.impl_->output_path) + " 2>&1";
    child.impl_->status = std::system(redirected.c_str());
    child.impl_->process = nullptr;
#endif
    return child;
}

bool ChildProcess::started() const noexcept {
#if defined(_WIN32)
    return impl_ != nullptr && impl_->process != nullptr;
#else
    return false;
#endif
}

ProcessResult ChildProcess::wait() {
    ProcessResult result;
    if (impl_ == nullptr) {
        return result;
    }
    result.started = started();
#if defined(_WIN32)
    if (impl_->process != nullptr) {
        ::WaitForSingleObject(static_cast<HANDLE>(impl_->process), INFINITE);
        DWORD code = 0;
        if (::GetExitCodeProcess(static_cast<HANDLE>(impl_->process), &code) != 0) {
            result.exited = true;
            result.exit_code = static_cast<int>(code);
        }
        ::CloseHandle(static_cast<HANDLE>(impl_->thread));
        ::CloseHandle(static_cast<HANDLE>(impl_->process));
        impl_->process = nullptr;
        impl_->thread = nullptr;
    }
#else
    result.started = true;
    result.exited = true;
    result.exit_code = WIFEXITED(impl_->status) ? WEXITSTATUS(impl_->status) : -1;
#endif
    impl_->waited = true;
    std::ifstream stream(impl_->output_path, std::ios::binary);
    if (stream) {
        result.output.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    std::filesystem::remove(impl_->output_path, ec);
    return result;
}

void ChildProcess::kill() {
    if (impl_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    if (impl_->process != nullptr) {
        ::TerminateProcess(static_cast<HANDLE>(impl_->process), 1);
        ::WaitForSingleObject(static_cast<HANDLE>(impl_->process), INFINITE);
        ::CloseHandle(static_cast<HANDLE>(impl_->thread));
        ::CloseHandle(static_cast<HANDLE>(impl_->process));
        impl_->process = nullptr;
        impl_->thread = nullptr;
    }
#endif
    impl_->waited = true;
    std::error_code ec;
    std::filesystem::remove(impl_->output_path, ec);
}

void suppress_crash_ui() noexcept {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments) {
    ChildProcess child = ChildProcess::start(executable, arguments);
    return child.wait();
}

bool wait_for_file(const std::string& path) {
    for (;;) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void touch_file(const std::string& path) {
    std::ofstream stream(path, std::ios::binary);
    stream << "1";
}

std::string temp_directory() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    const std::filesystem::path root = ec ? std::filesystem::path(".") : base / "tcplane-tests";
    std::filesystem::create_directories(root, ec);
    return root.string();
}

std::string unique_path(const std::string& stem) {
    const std::uint64_t counter = g_counter.fetch_add(1);
#if defined(_WIN32)
    const auto pid = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
    const std::filesystem::path path = std::filesystem::path(temp_directory()) /
                                       (stem + "-" + std::to_string(pid) + "-" + std::to_string(counter));
    return path.string();
}

void remove_tree(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

}  // namespace tcptest

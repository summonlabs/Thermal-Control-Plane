// Thermal Control Plane — test framework implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

namespace tcptest {

void Context::phase(const char* name) {
    std::printf("  [%s] %s\n", id_.c_str(), name);
    std::fflush(stdout);
}

void Context::fail(std::string reason) {
    if (!failed_) {
        failed_ = true;
        reason_ = std::move(reason);
    }
}

void Context::note(std::string text) {
    std::printf("  [%s] note: %s\n", id_.c_str(), text.c_str());
    std::fflush(stdout);
}

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::add(std::string id, void (*fn)(Context&)) {
    TestCase test;
    test.id = std::move(id);
    test.fn = fn;
    cases_.push_back(std::move(test));
}

Registrar::Registrar(const char* suite, const char* name, void (*fn)(Context&)) {
    std::string id(suite);
    id.append("::");
    id.append(name);
    Registry::instance().add(std::move(id), fn);
}

int run_all(int argc, char** argv) {
    const char* only_case = nullptr;
    const char* filter = nullptr;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            only_case = argv[++i];
        } else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else {
            std::printf("unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    const auto& cases = Registry::instance().cases();
    if (list) {
        for (const TestCase& test : cases) {
            std::printf("%s\n", test.id.c_str());
        }
        return 0;
    }

    std::size_t executed = 0;
    std::size_t failed = 0;
    for (const TestCase& test : cases) {
        if (only_case != nullptr && test.id != only_case) {
            continue;
        }
        if (filter != nullptr && test.id.rfind(filter, 0) != 0) {
            continue;
        }
        Context context;
        context.set_id(test.id);
        std::printf("RUN %s\n", test.id.c_str());
        std::fflush(stdout);
        const bool threw = [&]() {
            try {
                test.fn(context);
                return false;
            } catch (const std::exception& error) {
                context.fail(std::string("uncaught exception: ") + error.what());
                return true;
            } catch (...) {
                context.fail("uncaught non-standard exception");
                return true;
            }
        }();
        static_cast<void>(threw);
        ++executed;
        if (context.failed()) {
            ++failed;
            std::printf("FAIL %s: %s\n", test.id.c_str(), context.reason().c_str());
        } else {
            std::printf("PASS %s\n", test.id.c_str());
        }
        std::fflush(stdout);
    }

    if (only_case != nullptr && executed == 0) {
        std::printf("no case matched %s\n", only_case);
        return 2;
    }
    std::printf("%zu case(s) run, %zu failed\n", executed, failed);
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}

}  // namespace tcptest

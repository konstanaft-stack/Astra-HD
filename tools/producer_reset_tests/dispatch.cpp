#include "Render/CetLifecycleDispatch.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using cvr::gpu::CetLifecycleDispatch;
using cvr::gpu::ProducerResetGate;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() try {
    CetLifecycleDispatch dispatch;
    using Result = ProducerResetGate::ResetResult;
    std::vector<std::string> trace;
    unsigned bypass = 0;
    auto* marker = reinterpret_cast<void*>(uintptr_t(0x1234));
    auto result = dispatch.Present([&]() -> void* {
        // This is the CET wrapper, including its readiness branch and queue
        // assignments; it is not merely the narrower Update function.
        trace.emplace_back("read-ready");
        Check(dispatch.Resize(1ms, [] { return true; }, []() -> void* { return nullptr; }).status ==
              Result::Reentrant, "frontend readiness read happened outside admission");
        trace.emplace_back("bind-queue");
        trace.emplace_back("record-submit-fence-registration");
        Check(dispatch.BeforeOriginalPresent(), "original dispatch did not release frontend lease");
        trace.emplace_back("game-present");
        // Original Present must be able to trigger resize on this very thread.
        // Holding admission until the wrapper returns would refuse/deadlock it.
        const auto resize = dispatch.Resize(1ms, [&] {
            trace.emplace_back("quiesce"); return true;
        }, [&]() -> void* {
            trace.emplace_back("reset");
            const auto denied = dispatch.Present([]() -> void* {
                throw std::runtime_error("CET frontend admitted during resize");
            }, [&]() -> void* { ++bypass; trace.emplace_back("bypass-present"); return marker; });
            Check(!denied.admitted && denied.value == marker, "denied Present failed to reach game");
            trace.emplace_back("original-resize");
            return marker;
        });
        Check(resize.status == Result::Completed && resize.value == marker, "original Present cannot resize");
        return marker;
    }, []() -> void* { throw std::runtime_error("unexpected bypass"); });
    Check(result.admitted && result.forwarded && result.value == marker, "Present result lost");
    Check(bypass == 1, "denied Present forwarded wrong number of calls");
    const std::vector<std::string> expected{
        "read-ready", "bind-queue", "record-submit-fence-registration", "game-present",
        "quiesce", "reset", "bypass-present", "original-resize"};
    Check(trace == expected, "frontend/reset/original call order changed");

    // Unsupported OS/no producer still reaches the same dispatch shim. It must
    // not depend on Update or Initialize being called to release admission.
    result = dispatch.Present([&]() -> void* {
        Check(dispatch.BeforeOriginalPresent(), "no-producer branch did not release");
        Check(dispatch.Resize(1ms, [] { return true; }, []() -> void* { return nullptr; }).status ==
              Result::Completed, "no-producer branch retained admission across game Present");
        return marker;
    }, []() -> void* { return nullptr; });
    Check(result.admitted && result.forwarded, "no-producer Present not forwarded");

    // A real recursive frontend must not release the still-recording ancestor.
    // Its original Present may request PPD, but admission must defer that request
    // BEFORE any game dimensions are written, with no wait for its own caller.
    unsigned dimensionWrites = 0;
    result = dispatch.Present([&]() -> void* {
        auto nested = dispatch.Present([&]() -> void* {
            Check(dispatch.BeforeOriginalPresent(), "nested frontend dispatch missing");
            auto reserved = dispatch.ReserveBeforeDimensions(1ms, [] { return true; });
            if (reserved.reservation) ++dimensionWrites;
            Check(reserved.status == Result::Reentrant, "nested Present resized beneath active outer frontend");
            return marker;
        }, []() -> void* { return nullptr; });
        Check(nested.ancestorProducerActive && dimensionWrites == 0, "recursive boundary hidden or wrote dimensions");
        Check(dispatch.BeforeOriginalPresent(), "nested TLS did not restore outer invocation");
        auto ready = dispatch.ReserveBeforeDimensions(1ms, [] { return true; });
        Check(ready.status == Result::Reserved && ready.reservation.Cancel(), "deferred PPD could not retry after outer producer");
        return marker;
    }, []() -> void* { return nullptr; });
    Check(!result.ancestorProducerActive, "completed nested invocation polluted outer dispatch");

    unsigned destroyed = 0, originalResize = 0;
    const auto refused = dispatch.Resize(1ms, [] { return false; }, [&]() -> void* {
        ++destroyed; ++originalResize; return marker;
    });
    Check(refused.status == Result::QuiescenceFailed && destroyed == 0 && originalResize == 0,
          "failed GPU wait still forwarded CET reset/original resize");

    // Exceptions must restore TLS and release this scope; do not destroy the
    // enclosing dispatch scope when callbacks recurse into it.
    try {
        dispatch.Present([]() -> void* { throw std::runtime_error("frontend failed"); },
                         []() -> void* { return nullptr; });
        Check(false, "frontend exception swallowed");
    } catch (const std::runtime_error& error) {
        Check(std::string(error.what()) == "frontend failed", "unexpected exception");
    }
    Check(!dispatch.BeforeOriginalPresent(), "exception left stale TLS producer");
    Check(dispatch.Resize(1ms, [] { return true; }, []() -> void* { return nullptr; }).status ==
          Result::Completed, "exception leaked producer admission");

    // A forgotten/missing shim must be visible in the outcome, not a fake PASS.
    result = dispatch.Present([&]() -> void* { return marker; }, []() -> void* { return nullptr; });
    Check(result.admitted && !result.forwarded, "missing original-dispatch shim went unnoticed");
    std::cout << "PASS CET frontend admission, pre-original release, no-producer path, resize refusal, bypass and exception cleanup\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

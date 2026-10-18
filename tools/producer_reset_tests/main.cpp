#include "Render/ProducerResetGate.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using cvr::gpu::ProducerResetGate;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct ReleaseSignals {
    std::promise<void>& submit;
    std::promise<void>& retire;
    ~ReleaseSignals() {
        try { submit.set_value(); } catch (const std::future_error&) {}
        try { retire.set_value(); } catch (const std::future_error&) {}
    }
};

int main() try {
    using Result = ProducerResetGate::ResetResult;
    ProducerResetGate gate;
    std::promise<void> recording, allowSubmit, allowRetire;
    auto submitPermission = allowSubmit.get_future();
    auto retirePermission = allowRetire.get_future();
    std::atomic<bool> resourcesDestroyed{}, originalResize{}, gpuFinished{}, gpuDrainStarted{};
    std::future<void> producer;
    std::future<Result> resize;
    // On a failed assertion, release both worker barriers BEFORE async future
    // destructors join the threads. A regression must fail instead of hanging.
    ReleaseSignals cleanup{allowSubmit, allowRetire};
    producer = std::async(std::launch::async, [&] {
        auto lease = gate.TryEnter();
        Check(bool(lease), "initial producer denied");
        recording.set_value();
        submitPermission.wait();
        Check(!resourcesDestroyed.load(), "recorded resources destroyed before submit");
        // The scope covers registration of the physical-queue completion stamp,
        // not merely entry/return of the nominal ExecuteCommandLists wrapper.
    });
    Check(recording.get_future().wait_for(1s) == std::future_status::ready, "producer did not start");
    resize = std::async(std::launch::async, [&] {
        return gate.ResetFor(2s, [&] {
            gpuDrainStarted = true;
            Check(!resourcesDestroyed.load(), "resources destroyed before GPU wait");
            Check(!gate.TryEnter(), "producer admitted during GPU wait");
            Check(gate.ResetFor(1ms, [] { throw std::runtime_error("busy quiesce called"); return true; },
                                [] { throw std::runtime_error("busy apply called"); }) == Result::Busy,
                  "competing reset admitted during quiesce");
            retirePermission.wait();
            return gpuFinished.load();
        }, [&] {
            Check(gpuFinished.load(), "reset before actual queue completion");
            Check(!gate.TryEnter(), "producer admitted inside original resize");
            Check(gate.ResetFor(1ms, [] { return true; }, [] {}) == Result::Busy,
                  "competing reset admitted during apply");
            resourcesDestroyed = true;
            originalResize = true;
        });
    });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!gate.ResetPending() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Check(gate.ResetPending(), "resize did not close producer admission");
    Check(!gate.TryEnter(), "new producer admitted while resize waits for recording");
    Check(resize.wait_for(20ms) == std::future_status::timeout, "resize ended with active recording");
    Check(!originalResize.load(), "original resize ran before pending recording submission");
    Check(!gpuDrainStarted.load(), "GPU drain ran before pending CPU recording was submitted");
    allowSubmit.set_value();
    Check(producer.wait_for(1s) == std::future_status::ready, "producer did not finish submit");
    producer.get();
    Check(!originalResize.load(), "CPU submission mistaken for GPU completion");
    gpuFinished = true; allowRetire.set_value();
    Check(resize.get() == Result::Completed, "resize did not complete");
    Check(gate.Epoch() == 1 && bool(gate.TryEnter()), "completed resize did not reopen next epoch");

    // Failure to prove GPU completion must not forward the original resize.
    unsigned calls = 0;
    Check(gate.ResetFor(10ms, [] { return false; }, [&] { ++calls; }) == Result::QuiescenceFailed,
          "failed fence was accepted");
    Check(calls == 0 && gate.Epoch() == 1, "failed fence still forwarded reset");
    Check(bool(gate.TryEnter()), "unchanged resources inaccessible after refused reset");
    Check(gate.ResetFor(10ms, []() -> bool { throw std::runtime_error("fence creation"); },
                        [&] { ++calls; }) == Result::QuiescenceFailed,
          "quiesce exception was accepted");
    Check(calls == 0 && gate.Epoch() == 1, "quiesce exception forwarded reset");

    {
        auto outer = gate.TryEnter();
        auto nested = gate.TryEnter();
        Check(bool(outer) && bool(nested), "nested producer blocked");
        Check(gate.ResetFor(10ms, [] { return true; }, [&] { ++calls; }) == Result::Reentrant,
              "callback reset waited for its own producer lease");
        Check(calls == 0, "reentrant callback forwarded unsafe reset");
        auto timedOut = std::async(std::launch::async, [&] {
            return gate.ResetFor(5ms, [] { return true; }, [&] { ++calls; });
        });
        Check(timedOut.get() == Result::ProducerTimeout && calls == 0, "timeout forwarded reset");
    }

    // An existing producer may call a nested reader after another thread closes
    // admission; it must finish instead of waiting for the reset waiting for it.
    {
        auto outer = gate.TryEnter();
        auto resetting = std::async(std::launch::async, [&] {
            return gate.ResetFor(2s, [] { return true; }, [] {});
        });
        const auto until = std::chrono::steady_clock::now() + 1s;
        while (!gate.ResetPending() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        Check(gate.ResetPending(), "nested scenario missed reset admission");
        auto nested = gate.TryEnter();
        Check(bool(nested), "existing producer cannot finish nested callback");
        outer.Release();
        Check(resetting.wait_for(20ms) == std::future_status::timeout,
              "reset ignored nested lease after outer scope ended");
        auto moved = std::move(nested);
        nested.Release();
        Check(resetting.wait_for(20ms) == std::future_status::timeout, "moved lease retired early");
        moved.Release(); moved.Release();
        Check(resetting.get() == Result::Completed, "last nested lease did not unblock reset");
    }
    Check(gate.Epoch() == 2, "successful recovery after timeout did not advance epoch");

    // A throwing application callback may have partially reset resources. Do not
    // silently reopen producers against that unknown state.
    Check(gate.ResetFor(10ms, [] { return true; }, [] { throw std::runtime_error("partial reset"); }) ==
          Result::ApplyFailed, "apply exception lost");
    Check(!gate.TryEnter(), "partial reset reopened producer admission");
    Check(gate.ResetFor(10ms, [&] { ++calls; return true; }, [&] { ++calls; }) == Result::Poisoned,
          "partial reset retried blindly");
    Check(calls == 0, "poisoned reset invoked a callback");
    std::cout << "PASS pending recording, physical completion, reset interval, failed fence, nested callbacks, timeout, partial reset\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

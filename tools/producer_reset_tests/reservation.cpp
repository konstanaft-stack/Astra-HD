#include "Render/ProducerResetGate.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
using cvr::gpu::ProducerResetGate;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() try {
    ProducerResetGate gate;
    using Result = ProducerResetGate::ResetResult;
    unsigned pendingWidth = 976;
    {
        auto denied = gate.ReserveFor(10ms, [] { return false; });
        Check(denied.status == Result::QuiescenceFailed && !denied.reservation,
              "failed fence admitted a pending dimension write");
        Check(pendingWidth == 976 && bool(gate.TryEnter()), "pre-write refusal changed live state");
    }
    {
        auto prepared = gate.ReserveFor(10ms, [] { return true; });
        Check(prepared.status == Result::Reserved && bool(prepared.reservation), "pre-admission failed");
        Check(!gate.TryEnter(), "producer admitted between prepare and settings write");
        Check(!prepared.reservation.Complete(), "unarmed reservation accepted as actual resize");
        Check(prepared.reservation.Arm(), "failed to arm game mutation boundary");
        pendingWidth = 1600;
        Check(!prepared.reservation.Cancel(), "cancel reopened after dimensions changed");
        auto resize = std::async(std::launch::async, [&, reservation = std::move(prepared.reservation)]() mutable {
            Check(!gate.TryEnter(), "new render thread admitted before actual resize");
            Check(gate.Epoch() == 0, "pending settings write mistaken for completed resize");
            // Actual CET ResetState and original game resize would execute here.
            Check(reservation.Complete(), "matching actual resize did not release reservation");
        });
        resize.get();
        Check(gate.Epoch() == 1 && bool(gate.TryEnter()), "completed native resize did not reopen");
    }
    {
        auto unused = gate.ReserveFor(10ms, [] { return true; });
        Check(unused.status == Result::Reserved, "reservation after completed resize failed");
        // No native dimension write. Scope exit safely cancels this reservation.
    }
    Check(bool(gate.TryEnter()) && gate.Epoch() == 1, "unused reservation changed epoch or blocked render");
    {
        auto interrupted = gate.ReserveFor(10ms, [] { return true; });
        Check(interrupted.reservation.Arm(), "second mutation failed to arm");
        pendingWidth = 1968;
        // The game never proves original resize completion. Scope exit must not
        // pretend rollback of game state that has already changed happened.
    }
    Check(!gate.TryEnter(), "abandoned mutation silently reopened CET");
    Check(gate.ReserveFor(10ms, [] { return true; }).status == Result::Poisoned,
          "unknown game state was accepted for another PPD transaction");
    std::cout << "PASS pre-write refusal, reservation across threads, actual-resize completion, safe cancellation and poisoned incomplete mutation\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

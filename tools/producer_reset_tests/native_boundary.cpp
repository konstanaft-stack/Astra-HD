#include <Windows.h>
#include "Overlay/PpdNativeApply.hpp"
#include "Render/ProducerResetGate.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
using namespace cvr::ppd;
using cvr::gpu::ProducerResetGate;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Admission {
    ProducerResetGate::Reservation* reservation{};
    uint64_t revision{};
    unsigned calls{};
    bool allow{};
    NativeSize observed{};
};
bool Admit(const NativeApplyStatus& status, void* context) noexcept {
    auto& admission = *static_cast<Admission*>(context);
    ++admission.calls;
    admission.observed = status.observedActive;
    return admission.allow && status.revision == admission.revision &&
           admission.reservation && admission.reservation->Arm();
}

int main() try {
    std::array<uint32_t, 64> settings{};
    settings.fill(0xD15C0DE);
    settings[0x18 / 4] = settings[0x84 / 4] = 976;
    settings[0x1c / 4] = settings[0x88 / 4] = 1040;
    const auto before = settings;
    const auto pointer = reinterpret_cast<uintptr_t>(settings.data());
    ProducerResetGate gate;
    Admission admission;
    admission.revision = RequestNativeSize(1600, 1728);
    Check(ProcessNativeSizeAtSettingsHook(pointer, true, Admit, &admission), "owner released on refusal");
    Check(admission.calls == 1 && settings == before, "admission refusal did not precede native dimension stores");
    Check(ReadNativeApplyStatus().stage == NativeApplyStage::Requested, "refused write marked dispatched");
    Check(admission.observed == NativeSize{976,1040}, "admission missed coherent active dimensions");

    auto ready = gate.ReserveFor(10ms, [] { return true; });
    admission.reservation = &ready.reservation;
    admission.allow = true;
    Check(ProcessNativeSizeAtSettingsHook(pointer, false, Admit, &admission) && admission.calls == 1,
          "suspended settings consumed a reservation");
    Check(ProcessNativeSizeAtSettingsHook(pointer, true, Admit, &admission), "permitted write lost owner");
    Check(admission.calls == 2 && !gate.TryEnter(), "write did not retain closed producer admission");
    Check(settings[0x84/4] == 1600 && settings[0x88/4] == 1728 && settings[0x18/4] == 976,
          "native pending/active contract changed");
    Check(!ready.reservation.Cancel(), "native dimension stores were not armed");
    Check(ReadNativeApplyStatus().stage == NativeApplyStage::Dispatched && gate.Epoch() == 0,
          "settings write was mistaken for completed resize");
    settings[0x18/4] = 1600; settings[0x1c/4] = 1728;
    Check(ProcessNativeSizeAtSettingsHook(pointer, true, Admit, &admission) && admission.calls == 2,
          "observation re-entered destructive admission");
    Check(ready.reservation.Complete(), "actual resize completion could not release reservation");
    Check(CancelNativeSize(admission.revision), "cleanup failed");

    const auto revision = RequestNativeSize(1968,2112);
    auto stale = gate.ReserveFor(10ms, [] { return true; });
    admission.reservation = &stale.reservation; admission.revision = revision;
    RequestNativeSize(2048,2200);
    const auto unchanged = settings;
    ProcessNativeSizeAtSettingsHook(pointer, true, Admit, &admission);
    Check(settings == unchanged && stale.reservation.Cancel(), "stale reservation armed a replacement request");
    Check(bool(gate.TryEnter()), "stale pre-write request blocked producers");
    std::cout << "PASS production native stores: admission refusal, coherent size/revision, pre-write arm, deferred observation and stale reservation\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

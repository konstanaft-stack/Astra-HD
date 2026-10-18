#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace cvr::gpu {

// CPU admission only. Integration must hold a Lease from BEFORE the producer's
// readiness check through recording, actual submission and fence registration.
// The quiesce callback separately proves GPU completion on all receiving queues.
// No callbacks, original D3D functions or fence waits run under this class's mutex.
// The gate must outlive its leases, reservations and all ResetFor/ReserveFor calls.
class ProducerResetGate final {
public:
    enum class ResetResult { Completed, Busy, Reentrant, ProducerTimeout, QuiescenceFailed, ApplyFailed, Poisoned, Reserved, Stopping };
    enum class StopResult {Stopped,Busy,Reentrant,ProducerTimeout,QuiescenceFailed};
    class Lease final {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)), owner_(other.owner_), epoch_(other.epoch_) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                Release(); gate_ = std::exchange(other.gate_, nullptr);
                owner_ = other.owner_; epoch_ = other.epoch_;
            }
            return *this;
        }
        ~Lease() { Release(); }
        explicit operator bool() const noexcept { return gate_ != nullptr; }
        uint64_t Epoch() const noexcept { return epoch_; }
        void Release() noexcept {
            if (auto* gate = std::exchange(gate_, nullptr)) gate->Leave(owner_);
        }
    private:
        friend class ProducerResetGate;
        Lease(ProducerResetGate* gate, std::thread::id owner, uint64_t epoch)
            : gate_(gate), owner_(owner), epoch_(epoch) {}
        ProducerResetGate* gate_{};
        std::thread::id owner_{};
        uint64_t epoch_{};
    };

    // May move from the settings thread to the actual resize thread. Arm MUST
    // precede the first game-state mutation; Complete requires proof that the
    // actual original resize returned. A settings write alone is not completion.
    class Reservation final {
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)), armed_(other.armed_) {}
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                Abandon(); gate_ = std::exchange(other.gate_, nullptr); armed_ = other.armed_;
            }
            return *this;
        }
        ~Reservation() { Abandon(); }
        explicit operator bool() const noexcept { return gate_ != nullptr; }
        bool Arm() {
            if (!gate_ || armed_) return false;
            std::lock_guard lock(gate_->mutex_);
            if (gate_->phase_ != Phase::Reserved || gate_->stopping_) return false;
            gate_->phase_ = Phase::Applying; armed_ = true;
            return true;
        }
        bool Complete() {
            if (!gate_ || !armed_) return false;
            std::lock_guard lock(gate_->mutex_);
            if (gate_->phase_ != Phase::Applying) return false;
            ++gate_->epoch_; gate_->phase_ = Phase::Open; gate_ = nullptr; armed_ = false;
            return true;
        }
        bool Cancel() {
            if (!gate_ || armed_) return false;
            Abandon();
            return true;
        }
        template<class Apply>
        [[nodiscard]] ResetResult ApplyNow(Apply&& apply) {
            static_assert(std::is_same_v<std::invoke_result_t<Apply>, void>,
                          "Apply must return void and report incomplete reset by throwing");
            if (!Arm()) return ResetResult::ApplyFailed;
            try { std::forward<Apply>(apply)(); }
            catch (...) { Abandon(); return ResetResult::ApplyFailed; }
            return Complete() ? ResetResult::Completed : ResetResult::ApplyFailed;
        }
    private:
        friend class ProducerResetGate;
        explicit Reservation(ProducerResetGate* gate) : gate_(gate) {}
        void Abandon() noexcept {
            if (auto* gate = std::exchange(gate_, nullptr)) {
                std::lock_guard lock(gate->mutex_);
                gate->phase_ = armed_ ? Phase::Poisoned : Phase::Open;
            }
        }
        ProducerResetGate* gate_{};
        bool armed_{};
    };
    struct ReservationOutcome { ResetResult status; Reservation reservation; };

    [[nodiscard]] Lease TryEnter() {
        std::lock_guard lock(mutex_);
        const auto caller = std::this_thread::get_id();
        auto existing = owners_.find(caller);
        if(stopping_ && existing==owners_.end())return {};
        // Existing callbacks borrow their outer producer's admission. Blocking
        // them behind a reset waiting for that outer scope would deadlock.
        if (phase_ != Phase::Open && !(phase_ == Phase::Closing && existing != owners_.end())) return {};
        if (existing == owners_.end()) owners_.emplace(caller, 1);
        else ++existing->second;
        ++active_;
        return Lease(this, caller, epoch_);
    }

    template<class Rep, class Period, class Quiesce>
    [[nodiscard]] ReservationOutcome ReserveFor(std::chrono::duration<Rep, Period> timeout,
                                                Quiesce&& quiesce) {
        // timeout bounds CPU admission only. Quiesce must use a bounded wait for
        // its actual queue fences; this gate cannot interrupt an external call.
        {
            std::unique_lock lock(mutex_);
            if(stopping_)return {ResetResult::Stopping,{}};
            if (phase_ == Phase::Poisoned) return {ResetResult::Poisoned, {}};
            if (owners_.contains(std::this_thread::get_id())) return {ResetResult::Reentrant, {}};
            if (phase_ != Phase::Open) return {ResetResult::Busy, {}};
            phase_ = Phase::Closing;resetOperation_=true;
            if (!changed_.wait_for(lock, timeout, [&] { return active_ == 0; })) {
                phase_ = Phase::Open;resetOperation_=false;
                return {ResetResult::ProducerTimeout, {}};
            }
            if(stopping_){phase_=Phase::Open;resetOperation_=false;return {ResetResult::Stopping,{}};}
        }
        // Admission stays closed while the GPU drains AND while the outer resize
        // executes. ResetState's own return is not the end of the transaction.
        bool ready = false;
        try { ready = std::forward<Quiesce>(quiesce)(); }
        catch (...) { ready = false; }
        {
            std::lock_guard lock(mutex_);
            resetOperation_=false;
            if(stopping_){phase_=Phase::Open;return {ResetResult::Stopping,{}};}
            if (!ready) {
            phase_ = Phase::Open; // no destructive callback has run
            return {ResetResult::QuiescenceFailed, {}};
            }
            phase_ = Phase::Reserved;
        }
        return {ResetResult::Reserved, Reservation(this)};
    }

    template<class Rep, class Period, class Quiesce, class Apply>
    [[nodiscard]] ResetResult ResetFor(std::chrono::duration<Rep, Period> timeout,
                                      Quiesce&& quiesce, Apply&& apply) {
        auto prepared = ReserveFor(timeout, std::forward<Quiesce>(quiesce));
        if (prepared.status != ResetResult::Reserved) return prepared.status;
        return prepared.reservation.ApplyNow(std::forward<Apply>(apply));
    }

    // Terminal stop is deliberately different from a refused resize: fresh
    // readers stay denied after a timeout/fence failure. Existing producers may
    // finish nested callbacks, and an already-armed resize may retire, but neither
    // cancellation nor completion of that reservation reopens terminal admission.
    // Call outside loader lock when waiting is possible; this API alone does not
    // make waiting in DllMain safe. Caller must not destroy state on failure.
    template<class Rep,class Period,class Quiesce>
    [[nodiscard]] StopResult StopFor(std::chrono::duration<Rep,Period> timeout,Quiesce&& quiesce){
        {
            std::unique_lock lock(mutex_);
            stopping_=true;
            if(phase_==Phase::Stopped)return StopResult::Stopped;
            if(phase_==Phase::Poisoned)return StopResult::QuiescenceFailed;
            if(owners_.contains(std::this_thread::get_id()))return StopResult::Reentrant;
            if(stopOperation_||resetOperation_||phase_==Phase::Reserved||phase_==Phase::Applying)return StopResult::Busy;
            stopOperation_=true;phase_=Phase::Closing;
            if(!changed_.wait_for(lock,timeout,[&]{return active_==0;})){
                stopOperation_=false;return StopResult::ProducerTimeout;
            }
        }
        bool ready=false;
        try{ready=std::forward<Quiesce>(quiesce)();}catch(...){ready=false;}
        std::lock_guard lock(mutex_);
        stopOperation_=false;
        if(!ready)return StopResult::QuiescenceFailed;
        phase_=Phase::Stopped;
        return StopResult::Stopped;
    }

    bool ResetPending() const { std::lock_guard lock(mutex_); return phase_ != Phase::Open; }
    uint64_t Epoch() const { std::lock_guard lock(mutex_); return epoch_; }

private:
    enum class Phase { Open, Closing, Reserved, Applying, Poisoned, Stopped };
    void Leave(std::thread::id owner) noexcept {
        std::lock_guard lock(mutex_);
        auto found = owners_.find(owner);
        if (--found->second == 0) owners_.erase(found);
        --active_;
        if (active_ == 0) changed_.notify_all();
    }
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::thread::id, unsigned> owners_;
    unsigned active_{};
    uint64_t epoch_{};
    Phase phase_{Phase::Open};
    bool stopping_{},stopOperation_{},resetOperation_{};
};
}

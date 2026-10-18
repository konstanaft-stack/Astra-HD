#pragma once
#include "Render/ProducerResetGate.hpp"
#include <utility>

namespace cvr::gpu {
// Separates CET's producer frontend from its stored original-game-Present
// dispatch. The binary adapter must redirect that stored dispatch through
// BeforeOriginalPresent; calling only Update/Initialize is not this boundary.
class CetLifecycleDispatch final {
public:
    struct PresentOutcome { void* value{}; bool admitted{}, forwarded{}, ancestorProducerActive{},terminalDenied{}; };
    struct ResizeOutcome { ProducerResetGate::ResetResult status; void* value{}; };

    class FrontendLease {
    public:
        FrontendLease()=default;
        FrontendLease(ProducerResetGate::Lease invocation,ProducerResetGate::Lease producer)
            : invocation_(std::move(invocation)),producer_(std::move(producer)){}
        bool HasInvocation()const noexcept{return bool(invocation_);}
        explicit operator bool()const noexcept{return bool(producer_);}
        void ReleaseProducer()noexcept{producer_.Release();}
    private:
        // Reverse destruction retires producer first, full invocation last.
        ProducerResetGate::Lease invocation_,producer_;
    };

    [[nodiscard]] FrontendLease TryEnterFrontend(){
        auto invocation=invocations_.TryEnter();
        if(!invocation)return {};
        auto producer=gate_.TryEnter();
        return {std::move(invocation),std::move(producer)};
    }

    template<class Frontend, class Bypass>
    PresentOutcome Present(Frontend&& frontend, Bypass&& originalBypass) {
        auto lease = TryEnterFrontend();
        if(!lease.HasInvocation())return {nullptr,false,false,false,true};
        if (!lease) return {std::forward<Bypass>(originalBypass)(), false, true};
        Frame frame{this, std::move(lease), current_, false, false};
        struct Restore {
            Frame* previous;
            ~Restore() { current_ = previous; }
        } restore{current_};
        current_ = &frame;
        auto* value = std::forward<Frontend>(frontend)();
        return {value, true, frame.forwarded, frame.ancestorProducerActive};
    }

    // Called by the replacement for CET's m_realInternalPresent immediately
    // before invoking the saved original pointer. No gate mutex crosses it.
    // Recursive frontend Present cannot release an outer producer that will
    // resume recording. Its pre-write reservation is refused as Reentrant;
    // defer that PPD request, and never wait for the outer lease on this thread.
    // Unsolicited native/window resize is NOT covered by that policy.
    bool BeforeOriginalPresent() noexcept {
        if (!current_ || current_->owner != this || current_->forwarded) return false;
        current_->lease.ReleaseProducer();
        current_->forwarded = true;
        for (auto* ancestor = current_->previous; ancestor; ancestor = ancestor->previous)
            if (ancestor->owner == this && ancestor->lease) current_->ancestorProducerActive = true;
        return true;
    }

    // A logical transaction API, NOT an error return for the game's resize ABI.
    // The observed game caller ignores the return value and has already changed
    // dimensions. A binary adapter must obtain admission BEFORE that mutation;
    // it must never map a refused outcome to `return nullptr` in the game hook.
    template<class Rep, class Period, class Quiesce, class Resize>
    ResizeOutcome Resize(std::chrono::duration<Rep, Period> timeout,
                         Quiesce&& quiesce, Resize&& resize) {
        void* value = nullptr;
        const auto status = gate_.ResetFor(timeout, std::forward<Quiesce>(quiesce), [&] {
            value = std::forward<Resize>(resize)();
        });
        return {status, value};
    }

    [[nodiscard]] FrontendLease TryEnterReader() { return TryEnterFrontend(); }

    template<class Rep,class Period,class Quiesce>
    [[nodiscard]] ProducerResetGate::StopResult StopFor(std::chrono::duration<Rep,Period> timeout,Quiesce&& quiesce){
        // CET owns the original MinHook trampolines. Their lifetime extends
        // through the FULL forwarded call, beyond the shorter recording lease.
        const auto retired=invocations_.StopFor(timeout,[]{return true;});
        if(retired!=ProducerResetGate::StopResult::Stopped)return retired;
        return gate_.StopFor(timeout,std::forward<Quiesce>(quiesce));
    }

    template<class Rep, class Period, class Quiesce>
    [[nodiscard]] ProducerResetGate::ReservationOutcome ReserveBeforeDimensions(
        std::chrono::duration<Rep, Period> timeout, Quiesce&& quiesce) {
        return gate_.ReserveFor(timeout, std::forward<Quiesce>(quiesce));
    }

private:
    struct Frame {
        CetLifecycleDispatch* owner;
        FrontendLease lease;
        Frame* previous;
        bool forwarded;
        bool ancestorProducerActive;
    };
    inline static thread_local Frame* current_{};
    ProducerResetGate gate_;
    ProducerResetGate invocations_;
};
}

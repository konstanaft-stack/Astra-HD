#pragma once
#include "Render/ProducerResetGate.hpp"
#include <optional>

namespace cvr::gpu {
// A reservation can outlive the settings callback: its predicate may have
// cached the old width before our hook writes pending dimensions. Only the
// original native resize helper's completion may reopen CET recording.
class NativeSettingsTransaction final {
public:
    struct Plan {uintptr_t settings{};uint64_t revision{};uint32_t width{},height{};};
    struct Pass {
        Plan plan;
        ProducerResetGate::Reservation reservation;
        bool armed{},completed{};
        bool Reserved()const noexcept{return bool(reservation);}
        bool Arm(){if(armed)return true;return armed=reservation.Arm();}
        void NativeResizeCompleted()noexcept{if(armed)completed=true;}
    };
    struct Outcome {bool ran{},pending{};ProducerResetGate::ResetResult status=ProducerResetGate::ResetResult::Completed;};

    template<class Reserve,class Apply>
    Outcome Run(Plan plan,bool needsReservation,bool nativeWillResize,Reserve&& reserve,Apply&& apply){
        std::optional<Pass> pass;
        {
            std::lock_guard lock(mutex_);
            if(running_||(pending_&&pending_->plan.settings!=plan.settings))
                return {false,bool(pending_),ProducerResetGate::ResetResult::Busy};
            running_=true;
            if(pending_){pass.emplace(std::move(*pending_));pending_.reset();}
        }
        struct Release{NativeSettingsTransaction& owner;~Release(){std::lock_guard lock(owner.mutex_);owner.running_=false;}}release{*this};
        if(!pass){
            if(needsReservation){
                auto reserved=std::forward<Reserve>(reserve)();
                if(reserved.status!=ProducerResetGate::ResetResult::Reserved)return {false,false,reserved.status};
                pass.emplace(Pass{plan,std::move(reserved.reservation)});
            }else pass.emplace(Pass{plan,{}});
        }
        if(nativeWillResize&&pass->Reserved()&&!pass->Arm())return {false,false,ProducerResetGate::ResetResult::ApplyFailed};
        std::forward<Apply>(apply)(*pass);
        if(!pass->Reserved())return {true,false,ProducerResetGate::ResetResult::Completed};
        if(pass->completed){
            const bool ok=pass->reservation.Complete();
            return {true,false,ok?ProducerResetGate::ResetResult::Completed:ProducerResetGate::ResetResult::ApplyFailed};
        }
        if(pass->armed){
            std::lock_guard lock(mutex_);pending_.emplace(std::move(*pass));
            return {true,true,ProducerResetGate::ResetResult::Reserved};
        }
        pass->reservation.Cancel();return {true,false,ProducerResetGate::ResetResult::Completed};
    }
private:
    std::mutex mutex_;
    bool running_{};
    std::optional<Pass> pending_;
};
}

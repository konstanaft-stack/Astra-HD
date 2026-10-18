#pragma once
#include <functional>
#include <future>
#include <chrono>
namespace cvr::framegen {
enum class RetirementState {Idle,Pending,Complete,Failed};
class AsyncRetirement {
    RetirementState state_=RetirementState::Idle;
    std::future<bool> task_;
public:
    // Begin/Poll belong to one owner thread. The caller keeps all objects used
    // by stop alive until Complete/Failed; Finish is for shutdown after joining
    // the render thread, never for a live frame callback.
    bool Begin(std::function<bool()> stop) noexcept {
        if(state_!=RetirementState::Idle)return false;
        try{task_=std::async(std::launch::async,std::move(stop));state_=RetirementState::Pending;return true;}
        catch(...){return false;} // Owner still holds the worker; retry is safe.
    }
    RetirementState Poll() noexcept {
        if(state_==RetirementState::Pending){
            try{if(task_.wait_for(std::chrono::seconds(0))==std::future_status::ready)
                state_=task_.get()?RetirementState::Complete:RetirementState::Failed;}
            catch(...){state_=RetirementState::Failed;}
        }
        return state_;
    }
    RetirementState Finish() noexcept {
        if(state_==RetirementState::Pending){try{task_.wait();}catch(...){state_=RetirementState::Failed;}}
        return Poll();
    }
    bool Reset() noexcept {if(state_!=RetirementState::Complete)return false;state_=RetirementState::Idle;return true;}
};
}

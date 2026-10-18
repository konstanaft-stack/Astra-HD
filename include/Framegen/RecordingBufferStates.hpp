#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace cvr::framegen {
enum class BarrierPhase { Full, Begin, End };
// Caller serializes access. Owners pin identity only until this recording is
// reset/evicted; no state is inferred from a previous command-list execution.
template<class Owner, size_t Capacity=16> class RecordingBufferStates {
public:
    struct State {uint32_t state{};bool known{},split{};};
    void Reset(uint64_t recording){if(recording<recording_)return;entries_={};cursor_=0;recording_=recording;}
    void Invalidate(uint64_t recording){if(!Bind(recording))return;for(auto& e:entries_){e.value={};e.before=e.after=0;}}
    template<class Predicate> void InvalidateIf(uint64_t recording,Predicate affected){
        if(!Bind(recording))return;
        for(auto& e:entries_)if(e.key&&affected(e.key,e.owner)){e.value={};e.before=e.after=0;}
    }
    std::optional<State> Lookup(uint64_t recording,uintptr_t key)const {
        if(!recording||recording!=recording_||!key)return {};
        for(const auto& e:entries_)if(e.key==key)return e.value;return {};
    }
    State Seed(uint64_t recording,uintptr_t key,State previous,bool covered)const {
        // A bounded miss cannot revive TLS evidence that a later scope may
        // have invalidated. Only resources outside this ledger keep that path.
        return covered?Lookup(recording,key).value_or(State{}):previous;
    }
    void Transition(uint64_t recording,uintptr_t key,Owner owner,uint32_t before,uint32_t after,BarrierPhase phase,bool full){
        auto* e=EntryFor(recording,key,std::move(owner));if(!e)return;
        if(!full){e->value={};return;}
        if(phase==BarrierPhase::Begin){e->before=before;e->after=after;e->value={after,false,true};}
        else if(phase==BarrierPhase::End){const bool match=e->value.split&&e->before==before&&e->after==after;e->value={after,match,false};}
        else e->value={after,true,false};
    }
    void Access(uint64_t recording,uintptr_t key,Owner owner,uint32_t state){
        auto* e=EntryFor(recording,key,std::move(owner));if(e&&!e->value.split)e->value={state,true,false};
    }
private:
    static_assert(Capacity>0);
    struct Entry {uintptr_t key{};Owner owner{};State value{};uint32_t before{},after{};};
    std::array<Entry,Capacity> entries_{};uint64_t recording_{};size_t cursor_{};
    bool Bind(uint64_t recording){if(!recording||recording<recording_)return false;if(recording!=recording_)Reset(recording);return true;}
    Entry* EntryFor(uint64_t recording,uintptr_t key,Owner owner){
        if(!key||!Bind(recording))return nullptr;
        for(auto& e:entries_)if(e.key==key)return &e;
        for(auto& e:entries_)if(!e.key){e.key=key;e.owner=std::move(owner);return &e;}
        auto& e=entries_[cursor_++%Capacity];e={};e.key=key;e.owner=std::move(owner);return &e;
    }
};
}

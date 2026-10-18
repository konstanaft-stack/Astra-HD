#pragma once
#include <cstdint>
namespace cvr::framegen {
// A successful evaluation may need the caller's next explicit state transition
// before its output can be copied. Never carry a ticket across a reset/write.
struct DeferredNativeCapture {
    uint64_t recording{};
    uint64_t mutation{};
    void Arm(uint64_t value,uint64_t writes=0){recording=value;mutation=writes;}
    void Cancel(){recording=0;}
    bool Take(uint64_t current,bool statesKnown,uint64_t writes=0){
        if(!recording||recording!=current||mutation!=writes){Cancel();return false;}
        if(!statesKnown)return false;
        Cancel();return true;
    }
};
}

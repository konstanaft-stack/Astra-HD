#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
namespace cvr::capture {
// Rotation is not submission history: failed recording may advance index without a fence.
struct CaptureContextFences {
 std::array<uint64_t,3> values{};
 uint64_t Required(uint32_t slot)const{return slot<3?values[slot]:UINT64_MAX;}
 bool CanReset(uint32_t slot,uint64_t completed)const{const auto required=Required(slot);return completed!=UINT64_MAX&&required!=UINT64_MAX&&completed>=required;}
 void Executed(uint32_t slot,uint64_t value,bool signaled){if(slot<3)values[slot]=signaled&&value&&value!=UINT64_MAX?value:UINT64_MAX;}
 uint64_t AllRequired()const{return *std::max_element(values.begin(),values.end());}
 void Cleared(){values={};}
};
}

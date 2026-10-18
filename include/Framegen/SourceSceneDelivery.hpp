#pragma once
#include "Framegen/SourceSceneEpoch.hpp"
namespace cvr::framegen {
struct SourceSceneDeliveryStamp {uint64_t sinkRevision{};SourceSceneFrame scene;};
inline bool SourceSceneDeliveryCurrent(const SourceSceneDeliveryStamp& delivered,uint64_t revision,const SourceSceneFrame& current){
    return revision && delivered.sinkRevision==revision && delivered.scene==current;
}
template<class Callback,class Fault>void InvokeSourceSceneCallback(Callback&& callback,Fault&& fault){try{callback();}catch(...){fault();}}
}

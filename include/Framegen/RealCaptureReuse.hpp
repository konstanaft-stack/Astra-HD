#pragma once
namespace cvr::framegen {
struct RealCaptureLifetime {
    bool submitted{},retired{},invalid{};
    bool CanShare()const{return submitted&&retired&&!invalid;}
    bool CanConsume(bool completed,bool sameQueue)const{return CanShare()&&(completed||sameQueue);}
};
// The changing interpolation index intentionally is NOT part of a real image's
// identity. All other source/generation boundaries must match.
template<class Receipt> bool SamePairedRealSource(const Receipt& a,const Receipt& b){
    return a.paired&&b.paired&&a.pairedEye==b.pairedEye&&a.currentPairSerial==b.currentPairSerial&&
        a.gameWorldEpoch==b.gameWorldEpoch&&a.moduleGeneration==b.moduleGeneration&&
        a.lifecycleGeneration==b.lifecycleGeneration&&a.createOrdinal==b.createOrdinal&&
        a.historyEpoch==b.historyEpoch&&a.resourceEpoch==b.resourceEpoch&&
        a.policyRevision==b.policyRevision&&a.count==b.count&&a.current==b.current;
}
}

#pragma once
#include "Framegen/MainEyeSourceLatch.hpp"
#include "Framegen/MainEyePoseDiagnostics.hpp"
namespace cvr::framegen {
// Presence of an AER receipt chooses an exact-source path, including its
// failures. It must not quietly fall through to a differently timed legacy pose.
template<class Source,class Legacy>
bool SelectMainEyePosePath(bool sourcePresent,Source&& source,Legacy&& legacy){
    return sourcePresent ? source() : legacy();
}
// Owner-serialized bounded storage of the actual locate result. A descriptor
// read may use its saved older result, but may never re-locate it after eviction.
template<class Sample,std::size_t Capacity=64> class MainEyePoseSample {
    static_assert(Capacity>0);
public:
    template<class Validate,class Locate>
    std::optional<Sample> Acquire(const MainEyeSourceReceipt& receipt,Validate&& validate,Locate&& locate,MainEyePoseCode* code=nullptr){
        const auto report=[&](MainEyePoseCode value){if(code)*code=value;};
        if(!receipt){report(MainEyePoseCode::InvalidReceipt);return {};}
        if(!validate(receipt)){report(MainEyePoseCode::OwnerRejected);return {};}
        if(const auto saved=Read(receipt,validate)){report(MainEyePoseCode::CacheHit);return saved;}
        if(last && SameDomain(*last,receipt) && receipt.sceneOrdinal<=last->sceneOrdinal){report(MainEyePoseCode::StaleScene);return {};}
        Sample value{};
        if(!locate(receipt.requestedPoseTimeNs,value)){report(MainEyePoseCode::LocateFailed);return {};}
        if(!Valid(value,receipt)){report(MainEyePoseCode::InvalidSample);return {};}
        if(!validate(receipt)){report(MainEyePoseCode::OwnerChanged);return {};}
        entries[next]={receipt,value,true};next=(next+1)%Capacity;last=receipt;
        report(MainEyePoseCode::Stored);
        return value;
    }
    template<class Validate>
    std::optional<Sample> Read(const MainEyeSourceReceipt& receipt,Validate&& validate,MainEyePoseCode* code=nullptr) const {
        const auto report=[&](MainEyePoseCode value){if(code)*code=value;};
        if(!receipt){report(MainEyePoseCode::InvalidReceipt);return {};}
        if(!validate(receipt)){report(MainEyePoseCode::OwnerRejected);return {};}
        for(const auto& entry:entries)if(entry.valid && entry.receipt==receipt && Valid(entry.sample,receipt)){
            report(MainEyePoseCode::ReadHit);return entry.sample;
        }
        report(MainEyePoseCode::ReadMiss);
        return {};
    }
private:
    struct Entry {MainEyeSourceReceipt receipt;Sample sample{};bool valid{};};
    static bool SameDomain(const MainEyeSourceReceipt& a,const MainEyeSourceReceipt& b){
        return a.requestRevision==b.requestRevision && a.latchRevision==b.latchRevision &&
            a.epochs==b.epochs && a.clockGeneration==b.clockGeneration;
    }
    static bool Valid(const Sample& value,const MainEyeSourceReceipt& receipt){
        const auto& h=value.head;
        if(!value.poseId || !h.valid || !receipt.MatchesWrittenPose(h.originSerial,h.locatedTime))return false;
        const float fields[]{h.posX,h.posY,h.posZ,h.oriX,h.oriY,h.oriZ,h.oriW};
        for(float field:fields)if(!std::isfinite(field))return false;
        const double norm=double(h.oriX)*h.oriX+double(h.oriY)*h.oriY+double(h.oriZ)*h.oriZ+double(h.oriW)*h.oriW;
        return std::abs(norm-1.0)<=0.02;
    }
    std::array<Entry,Capacity> entries{};
    std::size_t next{};
    std::optional<MainEyeSourceReceipt> last;
};
}

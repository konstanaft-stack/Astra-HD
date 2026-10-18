#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace cvr::menu_preview {
struct EditorNativeFrameSample {
    PreviewFrameEye frame{};
    XrPosef headPose{{0,0,0,1},{0,0,0}};
    uint64_t origin{},bindingGeneration{};
    bool firstInFrame{};
    explicit operator bool()const noexcept{return bool(frame);}
};

// Caller serializes access. Both actual native views use one frozen tracking
// sample, while each eye owns a distinct source sequence. The caller must also
// freeze common camera/control/settings inputs when firstInFrame is true; this
// helper does not schedule a view or establish GPU/history/rendering success.
class EditorNativeFrameLatch {
public:
    explicit EditorNativeFrameLatch(uint64_t lastSourceSequence=0)noexcept:sequence_(lastSourceSequence){}

    EditorNativeFrameSample Resolve(const Metadata& canonical,uint64_t bindingGeneration,
        uint32_t nativeFrame,uint32_t actualFixedEye,uint64_t origin,const XrPosef& headPose)noexcept {
        if(canonical.screen!=2||!canonical.screenEpoch||!canonical.componentIdentity||
           !canonical.virtualCameraNameHash||!bindingGeneration||actualFixedEye>1||!origin||!ValidPose(headPose))return {};
        const PreviewFrameKey key{canonical.screenEpoch,canonical.setupID,
            canonical.componentIdentity,canonical.virtualCameraNameHash};
        const bool sameFrame=active_&&nativeFrame==nativeFrame_;
        if(active_){
            // Match PreviewFrameEyeOwner's native uint32 clock policy. Preserve
            // the watermark across identity changes so stale work cannot reopen
            // a closed frame merely by supplying another key/generation.
            if(!sameFrame&&int32_t(nativeFrame-nativeFrame_)<=0)return {};
            if(sameFrame&&(key!=key_||bindingGeneration!=bindingGeneration_||origin!=origin_))return {};
        }
        if(sameFrame&&eyeSequences_[actualFixedEye]){
            return {{key_,nativeFrame_,actualFixedEye,eyeSequences_[actualFixedEye],false},
                headPose_,origin_,bindingGeneration_,false};
        }
        // Reject without altering a live frame/snapshot when the shared
        // sequence counter can no longer allocate a nonzero identity.
        if(sequence_==std::numeric_limits<uint64_t>::max())return {};
        if(!sameFrame){
            key_=key;bindingGeneration_=bindingGeneration;nativeFrame_=nativeFrame;
            origin_=origin;headPose_=headPose;eyeSequences_={};active_=true;
        }
        const auto sourceSequence=++sequence_;
        eyeSequences_[actualFixedEye]=sourceSequence;
        return {{key_,nativeFrame_,actualFixedEye,sourceSequence,true},
            headPose_,origin_,bindingGeneration_,!sameFrame};
    }

    void Clear()noexcept {
        active_=false;key_={};bindingGeneration_=0;nativeFrame_=0;origin_=0;
        headPose_={{0,0,0,1},{0,0,0}};eyeSequences_={};
        // Source identities remain monotonic across setup/binding/cache resets.
    }
private:
    static bool ValidPose(const XrPosef& p)noexcept {
        if(!std::isfinite(p.position.x)||!std::isfinite(p.position.y)||!std::isfinite(p.position.z)||
           !std::isfinite(p.orientation.x)||!std::isfinite(p.orientation.y)||
           !std::isfinite(p.orientation.z)||!std::isfinite(p.orientation.w))return false;
        const double norm=double(p.orientation.x)*p.orientation.x+double(p.orientation.y)*p.orientation.y+
            double(p.orientation.z)*p.orientation.z+double(p.orientation.w)*p.orientation.w;
        return std::isfinite(norm)&&norm>=.999&&norm<=1.001;
    }
    PreviewFrameKey key_{};
    uint64_t bindingGeneration_{},origin_{},sequence_{};
    uint32_t nativeFrame_{};
    XrPosef headPose_{{0,0,0,1},{0,0,0}};
    std::array<uint64_t,2> eyeSequences_{};
    bool active_{};
};
}

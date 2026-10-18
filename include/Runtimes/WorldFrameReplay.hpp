#pragma once
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
#include <cmath>

namespace cvr::xr {
struct WorldFrameKey {
    XrSession session{};
    XrSpace space{};
    uint64_t world{},origin{},resources{};
    std::array<XrSwapchain,2> eyes{};
    uint64_t sessionRun{};
    bool menu{};
    XrSwapchain editorUi{}; // Explicit independent UI quad; owner lives in OpenXRManager.
    bool operator==(const WorldFrameKey&)const=default;
};
// Only snapshots already accepted by xrEndFrame may be retained here.
class WorldFrameReplay {
public:
    static constexpr uint32_t Capacity=32;
    WorldFrameReplay()=default;
    WorldFrameReplay(const WorldFrameReplay&)=delete;
    WorldFrameReplay& operator=(const WorldFrameReplay&)=delete;
    bool Capture(const WorldFrameKey& key,uint64_t serial,const XrFrameEndInfo& frame){
        Invalidate();
        if(!key.session||!key.space||(!key.menu&&!key.world)||!key.origin||!key.resources||
           !key.eyes[0]||!key.eyes[1]||key.eyes[0]==key.eyes[1]||!serial||
           frame.type!=XR_TYPE_FRAME_END_INFO||frame.next||!frame.layers||
           !frame.layerCount||frame.layerCount>Capacity||frame.displayTime<=0)return false;
        if(key.editorUi&&(!key.menu||key.editorUi==key.eyes[0]||key.editorUi==key.eyes[1]))return false;
        unsigned projections=0,menuCoverage=0,editorQuads=0;
        for(uint32_t i=0;i<frame.layerCount;++i){
            const auto* layer=frame.layers[i];
            if(!layer||layer->next)return false;
            if(layer->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION){
                if(++projections!=1)return false;
                const auto& source=*reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                if(source.space!=key.space||source.viewCount!=2||!source.views)return false;
                for(unsigned eye=0;eye<2;++eye){const auto& view=source.views[eye];
                    if(view.type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW||view.next||view.subImage.swapchain!=key.eyes[eye])return false;
                    views_[eye]=view;
                }
                projection_=source;projection_.views=views_.data();
                layers_[i]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection_);
            }else if(layer->type==XR_TYPE_COMPOSITION_LAYER_QUAD){
                quads_[i]=*reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                layers_[i]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads_[i]);
                const auto& quad=quads_[i];
                if(key.editorUi&&quad.subImage.swapchain==key.editorUi){
                    const auto& q=quad.pose.orientation;const auto& p=quad.pose.position;
                    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
                    if(++editorQuads!=1||quad.space!=key.space||quad.eyeVisibility!=XR_EYE_VISIBILITY_BOTH||
                       quad.subImage.imageArrayIndex||quad.subImage.imageRect.offset.x<0||quad.subImage.imageRect.offset.y<0||
                       quad.subImage.imageRect.extent.width<=0||quad.subImage.imageRect.extent.height<=0||
                       !std::isfinite(quad.size.width)||!std::isfinite(quad.size.height)||quad.size.width<=0||quad.size.height<=0||
                       !std::isfinite(norm)||std::fabs(norm-1.f)>.001f||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))return false;
                }
                if(key.menu&&quad.space==key.space){
                    if(quad.subImage.swapchain==key.eyes[0]){
                        if(quad.eyeVisibility==XR_EYE_VISIBILITY_BOTH)menuCoverage|=3;
                        else if(quad.eyeVisibility==XR_EYE_VISIBILITY_LEFT)menuCoverage|=1;
                    }else if(quad.subImage.swapchain==key.eyes[1]){
                        if(quad.eyeVisibility==XR_EYE_VISIBILITY_BOTH)menuCoverage|=3;
                        else if(quad.eyeVisibility==XR_EYE_VISIBILITY_RIGHT)menuCoverage|=2;
                    }
                }
            }else return false;
        }
        if(key.editorUi?(projections!=1||editorQuads!=1||menuCoverage!=0):
           key.menu?(projections!=0||menuCoverage!=3):(projections!=1))return false;
        key_=key;serial_=serial;frame_=frame;frame_.layers=layers_.data();valid_=true;
        return true;
    }
    const XrFrameEndInfo* Prepare(const WorldFrameKey& key,XrTime displayTime){
        if(!valid_)return nullptr;
        if(key!=key_||displayTime<frame_.displayTime){Invalidate();return nullptr;}
        frame_.displayTime=displayTime;
        return &frame_;
    }
    // xrEndFrame uses the last RELEASED image of each swapchain. Once a new
    // acquire/release attempt starts, the old layer descriptions cannot safely
    // identify those images anymore, even if that attempt never renders.
    void Invalidate(){valid_=false;serial_=0;}
    uint64_t Serial()const{return serial_;}
private:
    bool valid_{};
    uint64_t serial_{};
    WorldFrameKey key_{};
    XrFrameEndInfo frame_{XR_TYPE_FRAME_END_INFO};
    XrCompositionLayerProjection projection_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView,2> views_{};
    std::array<XrCompositionLayerQuad,Capacity> quads_{};
    std::array<const XrCompositionLayerBaseHeader*,Capacity> layers_{};
};
}

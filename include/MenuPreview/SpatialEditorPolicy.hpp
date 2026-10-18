#pragma once
#include "Camera/ExternalViewMath.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include <array>
#include <cstddef>
#include <span>
#include <bit>
#include <cstring>

namespace cvr::menu_preview {
struct EditorVertex {float x{},y{},z{};uint16_t color[4]{};uint16_t u{},v{};};
static_assert(sizeof(EditorVertex)==24);
using EditorQuad=std::array<EditorVertex,6>;
struct EditorSourceExtent {uint32_t width{},height{};explicit operator bool()const{return width&&height;}};
inline EditorSourceExtent PlanEditorSourceExtent(uint32_t canvasWidth){
    if(canvasWidth<128||canvasWidth>8192)return {};
    const uint32_t height=(canvasWidth*3+2)/4;if(height<128||height>8192)return {};
    return {canvasWidth,height};
}
inline bool MapEditorCamera(const cvr::camera::ExternalViewPose& source,cvr::camera::ExternalViewPose& output){
    // Luke27 2AF6..., scene-camera mode4 (RVA1AEFE0): the four staging
    // rooms use distinct affine position maps before tracked head/IPD.
    // Constants are pinned in audit/luke-chain-20261016/LUKE-EDITOR-MAP-CONSTANTS.json.
    const float x=float(source.position[0])/131072.f,y=-float(source.position[1])/131072.f,z=float(source.position[2])/131072.f;
    float ax{},ay{},az{},sx{},sy{},sz{},bx{},by{},bz{};
    if(y<=-825.f){ax=-207.17959594726562f;ay=-848.8823852539062f;az=-194.2100067138672f;
        sx=.025789352133870125f;sy=.025761360302567482f;sz=.46806254982948303f;
        bx=207.43460083007812f;by=849.6323852539062f;bz=193.77000427246094f;}
    else if(y<=-788.f){ax=-207.17959594726562f;ay=-807.4423828125f;az=-194.16000366210938f;
        sx=.04328018054366112f;sy=.02111426182091236f;sz=.3764040172100067f;
        bx=207.42759704589844f;by=808.161376953125f;bz=193.6840057373047f;}
    else {ax=-249.6696014404297f;sx=.039617910981178284f;sy=.025898819789290428f;sz=.4444432556629181f;
        bx=249.90859985351562f;bz=113.6500015258789f;
        if(y<=-750.f){ay=-774.8922729492188f;az=-114.0999984741211f;by=775.6932983398438f;}
        else {ay=-733.4522705078125f;az=-114.05000305175781f;by=734.2532958984375f;}}
    // Reject the actually observed unsettled near-zero component transform;
    // supported typed staging-room cameras stay near their own room anchor.
    if(std::abs(x-ax)>16||std::abs(y-ay)>16||std::abs(z-az)>8)return false;
    const float mapped[]{(x-ax)*sx-bx,-((y-ay)*sy-by),(z-az)*sz-bz};
    auto result=source;
    for(unsigned i=0;i<3;++i){const double fp=std::round(double(mapped[i])*131072.);
        if(!std::isfinite(fp)||fp<std::numeric_limits<int32_t>::min()||fp>std::numeric_limits<int32_t>::max())return false;result.position[i]=int32_t(fp);}
    output=result;return true;
}
enum class EditorImageRoute {LegacyCroppedQuad,FullRawImage};
struct EditorEye {
    cvr::camera::ExternalViewPose camera;
    XrPosef localEye{{0,0,0,1},{0,0,0}};
    XrFovf projection{};
    float cameraVerticalFov{},cameraAspect=4.f/3.f,nearPlane=.01f;
};
class EditorCaptureOwner {
public:
    PreviewFrameEye Resolve(const Metadata& scope,uint32_t frame,bool matchingAck,uint32_t ackEye){
        const PreviewFrameKey key{scope.screenEpoch,scope.setupID,scope.componentIdentity,scope.virtualCameraNameHash};
        if(selected_&&selected_.key==key&&selected_.nativeFrame==frame){auto repeat=selected_;repeat.changed=false;return repeat;}
        auto next=owner_.Resolve(scope,scope.virtualCameraNameHash,true,frame);if(!next)return {};
        next.eye=matchingAck&&ackEye<2?(ackEye^1u):0;selected_=next;return next;
    }
private:PreviewFrameEyeOwner owner_;PreviewFrameEye selected_{};
};
inline bool MatchesEditorBaseline(std::span<const uint8_t> descriptor,const std::array<uint8_t,32>& baseline){
    if(descriptor.size()<28||std::memcmp(descriptor.data(),baseline.data()+16,16))return false;
    for(unsigned i=0;i<3;++i){int32_t native{},typed{};std::memcpy(&native,descriptor.data()+16+i*4,4);std::memcpy(&typed,baseline.data()+i*4,4);
        if(std::abs(int64_t(native)-int64_t(typed))>32)return false;}
    return true;
}
inline bool OffsetEditorCamera(const cvr::camera::ExternalViewPose& baseline,float height,float distance,float scale,cvr::camera::ExternalViewPose& output){
    if(!std::isfinite(height)||!std::isfinite(distance)||std::abs(height)>5||std::abs(distance)>5||!std::isfinite(scale)||scale<=0||scale>100)return false;
    auto orientation=baseline.rotation;if(!cvr::camera::NormalizeExternal(orientation))return false;
    const auto forward=RotateVector(orientation,{0,1,0});auto result=baseline;
    if(!cvr::camera::ShiftExternal(result,{forward.x*distance*scale,forward.y*distance*scale,(forward.z*distance+height)*scale}))return false;
    output=result;return true;
}
// Camera registration occurs before the script's next-tick world-pose receipt.
// Identify that copied eye from the owned baseline, then require the final
// typed publication to match the same bytes before admitting an image.
inline bool SelectCopiedEditorEye(const std::array<uint8_t,32>& baseline,
                                  const std::array<uint8_t,32>& copied,
                                  float halfIpd,uint32_t& eye){
    if(!std::isfinite(halfIpd)||halfIpd<=0||halfIpd>.15f||std::memcmp(baseline.data()+16,copied.data()+16,16))return false;
    cvr::camera::ExternalViewPose base;std::memcpy(base.position.data(),baseline.data(),12);std::memcpy(&base.rotation,baseline.data()+16,16);
    const XrPosef identity{{0,0,0,1},{0,0,0}};uint32_t selected=UINT32_MAX;
    for(uint32_t candidate=0;candidate<2;++candidate){
        cvr::camera::ExternalViewPose expected;
        if(!cvr::camera::ComposeExternal(base,identity,1.f,candidate?halfIpd:-halfIpd,expected))return false;
        bool matches=true;for(unsigned i=0;i<3;++i){int32_t actual{};std::memcpy(&actual,copied.data()+i*4,4);
            if(std::abs(int64_t(actual)-int64_t(expected.position[i]))>32)matches=false;}
        if(matches){if(selected!=UINT32_MAX)return false;selected=candidate;}
    }
    if(selected==UINT32_MAX)return false;eye=selected;return true;
}
inline uint16_t EditorHalf(float value){
    const auto bits=std::bit_cast<uint32_t>(value);const int exponent=int((bits>>23)&255)-112;
    uint32_t mantissa=bits&0x7fffff;
    if(exponent<=0){if(exponent < -10)return 0;mantissa|=0x800000;const unsigned shift=unsigned(14-exponent);
        const auto truncated=mantissa>>shift,rest=mantissa&((1u<<shift)-1),mid=1u<<(shift-1);
        return uint16_t(truncated+unsigned(rest>mid||(rest==mid&&(truncated&1))));}
    const auto truncated=mantissa>>13,rest=mantissa&8191;
    return uint16_t((uint32_t(exponent)<<10)+truncated+unsigned(rest>4096||(rest==4096&&(truncated&1))));
}
inline bool ExpandEditorPreview(std::span<const std::byte> bytes,uint32_t width,uint32_t height,EditorQuad& output,float projectionAspect=0){
    if(bytes.size()!=sizeof(EditorQuad)||width<128||height<128||width>8192||height>8192)return false;
    EditorQuad input{};std::memcpy(input.data(),bytes.data(),bytes.size());
    float left=input[0].x,right=left,top=input[0].y,bottom=top;
    for(const auto& v:input){if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||std::abs(v.z)>.0001f)return false;
        left=std::min(left,v.x);right=std::max(right,v.x);top=std::min(top,v.y);bottom=std::max(bottom,v.y);}
    const float w=right-left,h=bottom-top;
    if(w<float(width)*.35f||h<float(height)*.25f||std::abs(w-h*(4.f/3.f))>h*.08f)return false;
    constexpr bool xs[]{false,true,true,false,true,false},ys[]{false,false,true,false,true,true};
    for(unsigned i=0;i<6;++i)if(std::abs(input[i].x-(xs[i]?right:left))>.01f||
        std::abs(input[i].y-(ys[i]?bottom:top))>.01f||input[i].u!=(xs[i]?0x3c00:0)||input[i].v!=(ys[i]?0x3c00:0))return false;
    // The preview RTT is authored at 4:3. Fill the eye extent with a centred
    // UV window, retaining the source aspect instead of stretching its pixels.
    const float aspect=projectionAspect>0?projectionAspect:float(width)/float(height);
    const float halfU=.5f*std::min(1.f,aspect/(4.f/3.f)),halfV=.5f*std::min(1.f,(4.f/3.f)/aspect);
    auto result=input;
    for(unsigned i=0;i<6;++i){result[i].x=xs[i]?float(width):0.f;result[i].y=ys[i]?float(height):0.f;
        result[i].u=EditorHalf(.5f+(xs[i]?halfU:-halfU));result[i].v=EditorHalf(.5f+(ys[i]?halfV:-halfV));}
    output=result;return true;
}
inline bool BuildEditorEye(const cvr::camera::ExternalViewPose& baseline,const XrPosef& reference,
                           const XrPosef& current,const XrPosef& recenter,float scale,float halfIpd,
                           uint32_t eye,float horizontal,uint32_t width,uint32_t height,EditorEye& output,
                           EditorImageRoute route=EditorImageRoute::LegacyCroppedQuad){
    if(eye>1||!std::isfinite(halfIpd)||halfIpd<=0||halfIpd>.15f||!std::isfinite(horizontal)||horizontal<=1||horizontal>=170||
       width<128||height<128||width>8192||height>8192)return false;
    auto start=reference,head=current,origin=recenter;
    if(!cvr::camera::NormalizeExternal(start.orientation)||!cvr::camera::NormalizeExternal(head.orientation)||!cvr::camera::NormalizeExternal(origin.orientation))return false;
    EditorEye result;const float sign=eye?1.f:-1.f;
    if(!cvr::camera::ComposeExternal(baseline,RelativePose(start,head),scale,sign*halfIpd*scale,result.camera))return false;
    result.localEye.orientation=MultiplyQuat(origin.orientation,head.orientation);
    const auto position=RotateVector(origin.orientation,head.position);
    const auto offset=RotateVector(result.localEye.orientation,{sign*halfIpd,0,0});
    result.localEye.position={origin.position.x+position.x+offset.x,origin.position.y+position.y+offset.y,origin.position.z+position.z+offset.z};
    constexpr float radians=3.14159265358979323846f/180.f;
    const float halfH=horizontal*radians*.5f,aspect=float(width)/float(height);
    const float halfV=std::atan(std::tan(halfH)/aspect);
    result.projection={-halfH,halfH,halfV,-halfV};
    // Full raw transport samples the entire source image. Its camera must
    // render the same angular rectangle as the submitted projection. Only
    // the legacy quad route compensates for its centred 4:3 UV crop.
    if(route==EditorImageRoute::FullRawImage){
        result.cameraAspect=aspect;
        result.cameraVerticalFov=2.f*halfV/radians;
    }else{
        const float visibleV=std::min(1.f,(4.f/3.f)/aspect);
        result.cameraVerticalFov=2.f*std::atan(std::tan(halfV)/visibleV)/radians;
    }
    if(!std::isfinite(result.cameraVerticalFov)||result.cameraVerticalFov>=179)return false;
    output=result;return true;
}
}

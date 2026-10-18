#include "MenuPreview/SpatialEditorPolicy.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace cvr::menu_preview;
void require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
EditorQuad RecordedQuad(){
    EditorQuad q{};const float x[]{0,1517,1517,0,1517,0},y[]{542.5f,542.5f,1649.5f,542.5f,1649.5f,1649.5f};
    for(unsigned i=0;i<6;++i){q[i].x=x[i];q[i].y=y[i];for(auto& c:q[i].color)c=0x3c00;q[i].u=x[i]?0x3c00:0;q[i].v=y[i]>600?0x3c00:0;}
    return q;
}
int main(){
    auto reference=PlanEditorSourceExtent(1968);require(reference&&reference.width==1968&&reference.height==1476,"measured Luke RTT extent follows the full render width at authored 4:3");
    auto target=PlanEditorSourceExtent(1920);require(target&&target.width==1920&&target.height==1440,"current 1920-wide canvas must not retain the old 960x720 source");
    require(!PlanEditorSourceExtent(0)&&!PlanEditorSourceExtent(16384),"unavailable or invalid canvas cannot allocate an editor texture");
    Metadata scope{};scope.screen=2;scope.screenEpoch=1;scope.setupID=6;scope.componentIdentity=0x11;scope.virtualCameraNameHash=0x22;
    EditorCaptureOwner captureOwner;
    auto first=captureOwner.Resolve(scope,100,false,0);require(first&&first.eye==0,"native editor starts with the left eye without moving the game component");
    auto same=captureOwner.Resolve(scope,100,true,0);require(same&&same.eye==0&&same.sourceSequence==first.sourceSequence,"a GPU acknowledgement arriving during one native frame cannot change that frame's eye");
    auto next=captureOwner.Resolve(scope,101,true,0);require(next&&next.eye==1,"the next native frame renders the opposite of the completed captured eye");
    require(!captureOwner.Resolve(scope,99,true,1),"older native work cannot replace a newer editor frame");
    const auto input=RecordedQuad();EditorQuad output{};
    require(ExpandEditorPreview(std::as_bytes(std::span(input)),1968,2192,output),"recorded editor preview must fill render extent");
    require(near(output[0].x,0)&&near(output[0].y,0)&&near(output[2].x,1968)&&near(output[2].y,2192),"full scene uses the entire render rectangle");
    require(input[0].y==542.5f&&input[2].x==1517,"source vertices are never rewritten");
    require(output[0].u>0&&output[1].u<0x3c00&&output[0].v==0&&output[2].v==0x3c00,"portrait aspect crops UV horizontally without squashing");
    require(ExpandEditorPreview(std::as_bytes(std::span(input)),1920,1920,output,1280.f/1400.f)&&near(output[2].x,1920)&&near(output[2].y,1920),"UI canvas extent and physical eye projection aspect are independent");
    require(output[0].u>0&&output[1].u<0x3c00&&output[2].v==0x3c00,"a square UI backbuffer retains the portrait eye projection crop");
    auto button=input;for(auto& v:button){v.x*=.05f;v.y=(v.y-542.5f)*.05f;}
    require(!ExpandEditorPreview(std::as_bytes(std::span(button)),1968,2192,output),"ordinary six-vertex UI buttons must not expand");
    auto unknown=input;unknown[4].u=0x3800;
    require(!ExpandEditorPreview(std::as_bytes(std::span(unknown)),1968,2192,output),"unrecognized UV topology must pass through");
    cvr::camera::ExternalViewPose baseline;baseline.position={100*131072,200*131072,3*131072};
    cvr::camera::ExternalViewPose recorded;recorded.position={int32_t(std::llround(-249.55123901367188*131072.)),int32_t(std::llround(774.5672607421875*131072.)),int32_t(std::llround(-113.3800048828125*131072.))};recorded.rotation={0,0,.17364818f,.98480773f};
    cvr::camera::ExternalViewPose mapped;
    require(MapEditorCamera(recorded,mapped),"editor source room must map to Luke's VR camera space before head and eye offsets");
    require(std::abs(double(mapped.position[0])/131072.+249.90391)<.0001&&std::abs(double(mapped.position[1])/131072.-775.68488)<.0001&&std::abs(double(mapped.position[2])/131072.+113.33000)<.0001,"measured editor render origin follows the recovered affine room transform");
    const XrPosef zero{{0,0,0,1},{0,0,0}};EditorEye calibrated;
    require(BuildEditorEye(mapped,zero,zero,zero,1,.0315f,0,94,1968,2192,calibrated),"mapped editor camera composes one physical eye");
    require(std::abs(double(calibrated.camera.position[0])/131072.+249.9340057)<.002&&std::abs(double(calibrated.camera.position[1])/131072.-775.6749878)<.002&&std::abs(double(calibrated.camera.position[2])/131072.+113.3300018)<.0001,"mapped physical eye agrees with steady Luke camera within render-origin sampling tolerance");
    cvr::camera::ExternalViewPose unsettled;require(!MapEditorCamera(unsettled,mapped),"unsettled zero scene transforms cannot establish the editor's head anchor");
    const float sourceRoom[][3]{{-207.179595947f,848.882385254f,-194.210006714f},{-207.179595947f,807.442382813f,-194.160003662f},{-249.66960144f,774.892272949f,-114.099998474f},{-249.66960144f,733.452270508f,-114.050003052f}};
    const float targetRoom[][3]{{-207.43460083f,849.632385254f,-193.770004272f},{-207.427597046f,808.161376953f,-193.684005737f},{-249.908599854f,775.69329834f,-113.650001526f},{-249.908599854f,734.253295898f,-113.650001526f}};
    for(unsigned room=0;room<4;++room){auto sample=recorded;for(unsigned axis=0;axis<3;++axis)sample.position[axis]=int32_t(std::llround(double(sourceRoom[room][axis])*131072.));
        require(MapEditorCamera(sample,mapped),"each recovered editor staging room is supported");for(unsigned axis=0;axis<3;++axis)require(std::abs(double(mapped.position[axis])/131072.-targetRoom[room][axis])<.0001,"each staging-room anchor maps to its recovered VR position");}
    cvr::camera::ExternalViewPose controlled;
    require(OffsetEditorCamera(baseline,0,0,1,controlled)&&controlled.position==baseline.position,"zero editor controls preserve the exact native baseline");
    require(OffsetEditorCamera(baseline,-1,.5f,1,controlled)&&controlled.position[2]==2*131072&&controlled.position[1]==200*131072+65536,"height and zoom move the camera independently in scene units");
    require(baseline.position[2]==3*131072,"editor controls never write the game's camera component");
    std::array<uint8_t,32> staged{},copied{};
    std::memcpy(staged.data(),baseline.position.data(),12);std::memcpy(staged.data()+16,&baseline.rotation,16);copied=staged;
    Descriptor native{};std::memcpy(native.data(),staged.data()+16,16);std::memcpy(native.data()+16,staged.data(),12);
    require(MatchesEditorBaseline(native,staged),"the renderer's unmodified camera must match the owned typed component baseline");
    int32_t alien=baseline.position[0]+64;std::memcpy(native.data()+16,&alien,4);
    require(!MatchesEditorBaseline(native,staged),"an unrelated render-camera position cannot acquire editor ownership");
    uint32_t copiedEye=UINT32_MAX;int32_t shifted=baseline.position[0]-int32_t(std::llround(.0315*131072.));
    std::memcpy(copied.data(),&shifted,4);
    require(SelectCopiedEditorEye(staged,copied,.0315f,copiedEye)&&copiedEye==0,"registered left copy precedes script publication and must retain its physical eye");
    shifted=baseline.position[0]+int32_t(std::llround(.0315*131072.));std::memcpy(copied.data(),&shifted,4);
    require(SelectCopiedEditorEye(staged,copied,.0315f,copiedEye)&&copiedEye==1,"registered right copy is identified from the same staged baseline");
    require(!SelectCopiedEditorEye(staged,staged,.0315f,copiedEye),"unshifted unrelated registered camera cannot acquire editor eye ownership");
    const XrPosef identity{{0,0,0,1},{0,0,0}},recenter{{0,0,0,1},{0,1.6f,0}};
    EditorEye left,right;
    require(BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,0,100,1920,1920,left),"left editor eye builds from tracked pose");
    require(BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,1,100,1920,1920,right),"right editor eye builds from same baseline");
    require(std::abs(double(right.camera.position[0]-left.camera.position[0])/131072.-.063)<.00002,"eye separation applied once in engine space");
    require(near(left.localEye.position.x,-.0315f)&&near(right.localEye.position.x,.0315f)&&near(left.localEye.position.y,1.6f),"submitted poses describe actual physical eyes");
    require(near(left.cameraVerticalFov,100)&&near(left.nearPlane,.01f),"camera optics match full square projection");
    EditorEye measured;
    require(BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,0,94,1968,2192,measured)&&std::abs(measured.cameraVerticalFov-100.126419f)<.0001f,"steady Luke FOV and near agree with the same projection formula");
    const float h=.3f;XrPosef lookingDown{{std::sin(h),0,0,std::cos(h)},{0,0,0}};EditorEye down;
    require(BuildEditorEye(baseline,identity,lookingDown,recenter,1,.0315f,0,100,1920,1920,down),"look-down editor view builds");
    require(std::abs(down.camera.rotation.x)>.1f&&std::abs(down.localEye.orientation.x)>.1f,"head pitch changes scene camera and matching projection pose");
    require(!BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,2,100,1920,1920,down),"unknown physical eye rejected");
    require(!BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,0,0,1920,1920,down),"unresolved FOV rejected");
    // Full raw transport uses the complete source image: its native camera rays
    // must equal the submitted eye rays without the legacy portrait UV crop.
    for(const auto extent:std::array<std::array<uint32_t,2>,4>{{{1280,1400},{1920,1920},{1782,1920},{1920,1080}}}){
        for(uint32_t eye=0;eye<2;++eye){
            EditorEye raw;
            require(BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,eye,94,extent[0],extent[1],raw,EditorImageRoute::FullRawImage),"raw eye optics build");
            constexpr float radians=3.14159265358979323846f/180.f;
            const float nativeV=std::tan(raw.cameraVerticalFov*radians*.5f);
            require(near(nativeV*raw.cameraAspect,std::tan(raw.projection.angleRight)),"full raw horizontal native/submitted angular rays match");
            require(near(nativeV,std::tan(raw.projection.angleUp)),"full raw vertical native/submitted angular rays match");
            EditorEye legacy,explicitLegacy;
            require(BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,eye,94,extent[0],extent[1],legacy)&&
                BuildEditorEye(baseline,identity,identity,recenter,1,.0315f,eye,94,extent[0],extent[1],explicitLegacy,EditorImageRoute::LegacyCroppedQuad),"legacy default and explicit routes build");
            require(legacy.cameraAspect==4.f/3.f&&legacy.cameraVerticalFov==explicitLegacy.cameraVerticalFov,"default route retains legacy 4:3 optics");
            const float aspect=float(extent[0])/float(extent[1]);
            const float uSpan=std::min(1.f,aspect/(4.f/3.f)),vSpan=std::min(1.f,(4.f/3.f)/aspect);
            const float legacyV=std::tan(legacy.cameraVerticalFov*radians*.5f);
            require(near(legacyV*legacy.cameraAspect*uSpan,std::tan(legacy.projection.angleRight)),"legacy cropped horizontal angular rays remain matched");
            require(near(legacyV*vSpan,std::tan(legacy.projection.angleUp)),"legacy cropped vertical angular rays remain matched");
            require(raw.camera.position==legacy.camera.position&&raw.localEye.position.x==legacy.localEye.position.x&&
                raw.localEye.position.y==legacy.localEye.position.y&&raw.localEye.position.z==legacy.localEye.position.z&&
                raw.projection.angleRight==legacy.projection.angleRight&&raw.projection.angleUp==legacy.projection.angleUp,"image route leaves physical eye pose and submitted projection unchanged");
        }
    }
    std::puts("PASS spatial editor full-scene geometry, paired camera and projection contract");
}

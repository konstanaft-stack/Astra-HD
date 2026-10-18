#ifndef CPVR_TEST_LEGACY_OWNED_BINDING
#include "MenuPreview/OwnedEditorBinding.hpp"
#else
#include "MenuPreview/Scope.hpp"
#include <span>
namespace cvr::menu_preview {
constexpr uint64_t EditorRightKey=0x9e62210faa8c1037ull,EditorRightTexture=0x2058242661b85b3dull;
struct EditorCameraCandidate {uintptr_t component{},owner{};uint64_t name{},key{},texture{};bool enabled{},rtt{};};
struct EditorCameraSelection {uintptr_t primary{},secondary{};uint32_t enabledPrimary{},exactSecondary{};};
// Source-baseline behavior: the original script-thread loop counted every
// enabled RTT; it had no owned-secondary exclusion. No SDK/render mock.
inline EditorCameraSelection SelectOwnedEditorCameras(std::span<const EditorCameraCandidate> values,uintptr_t owner){
 EditorCameraSelection result;for(const auto& v:values)if(v.rtt&&v.owner==owner&&v.enabled){++result.enabledPrimary;result.primary=v.component;}return result;}
}
#endif
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace cvr::menu_preview;
static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
 constexpr uintptr_t owner=0x5000,left=0x6000,right=0x7000;
 EditorCameraCandidate primary{left,owner,11,12,13,true,true};
 EditorCameraCandidate owned{right,owner,EditorRightKey,EditorRightKey,EditorRightTexture,true,true};
 std::array cameras{primary,owned};auto selected=SelectOwnedEditorCameras(cameras,owner);
 Require(selected.enabledPrimary==1&&selected.primary==left&&selected.secondary==right&&selected.exactSecondary==1,
     "an exact owned right RTT must not become a second primary/ambiguous camera");
#ifndef CPVR_TEST_LEGACY_OWNED_BINDING
 for(unsigned field=0;field<4;++field){auto bad=cameras;if(field==0)bad[1].name^=1;if(field==1)bad[1].key^=1;if(field==2)bad[1].texture^=1;if(field==3)bad[1].owner^=1;
  const auto result=SelectOwnedEditorCameras(bad,owner);Require(!result.secondary&&result.exactSecondary==0,"wrong right identity/owner cannot be excluded or enabled");
  Require(result.enabledPrimary==(field==3?1u:2u),"an unknown enabled RTT on the owner preserves ambiguity rejection");}
 auto disabled=cameras;disabled[1].enabled=false;selected=SelectOwnedEditorCameras(disabled,owner);
 Require(selected.primary==left&&selected.secondary==right&&selected.exactSecondary==1&&selected.enabledPrimary==1,"an authored disabled right camera remains selectable for exact script-thread enabling");
 std::array duplicate{primary,owned,owned};duplicate[2].component=0x8000;
 Require(SelectOwnedEditorCameras(duplicate,owner).exactSecondary==2,"duplicate exact owned candidates remain ambiguous");
 Require(!ReadyEditorBackings(0,2)&&!ReadyEditorBackings(1,0)&&!ReadyEditorBackings(2,2)&&ReadyEditorBackings(1,2),"two nonzero independent backing objects required");
 Metadata m{};m.screen=2;m.screenEpoch=1;m.setupID=6;m.controllerIdentity=1;m.componentIdentity=left;m.entityIdentity=owner;m.entityID=55;m.sceneNameHash=7;m.cameraRefHash=8;m.slotNameHash=9;m.virtualCameraNameHash=12;m.publicationSerial=10;m.publishedAtMs=11;
 EditorBindingIdentity id{m,NativePreviewMode::NativeEditorDual,right,EditorRightKey,0xA000,0xB000};EditorBindingClock clock;
 const auto generation=clock.Resolve(id);Require(generation!=0,"first full binding generation is nonzero");
 id.common.publicationSerial++;id.common.publishedAtMs++;Require(clock.Resolve(id)==generation,"timestamp/serial refresh must not manufacture a binding generation");
 for(unsigned field=0;field<15;++field){auto changed=id;switch(field){case 0:++changed.common.screenEpoch;break;case 1:++changed.common.setupID;break;case 2:++changed.common.screen;break;case 3:++changed.common.controllerIdentity;break;case 4:++changed.common.componentIdentity;break;case 5:++changed.common.entityIdentity;break;case 6:++changed.common.entityID;break;case 7:++changed.common.sceneNameHash;break;case 8:++changed.common.cameraRefHash;break;case 9:++changed.common.slotNameHash;break;case 10:++changed.common.virtualCameraNameHash;break;case 11:changed.mode=NativePreviewMode::LegacyOneView;break;case 12:++changed.secondaryComponent;break;case 13:++changed.primaryResource;break;case 14:++changed.secondaryResource;break;}
  const auto before=clock.Resolve(id),after=clock.Resolve(changed);Require(after>before,"canonical identity/mode/component/resource change must advance generation");}
 auto changedKey=id;++changedKey.secondaryKey;const auto beforeKey=clock.Resolve(id);Require(clock.Resolve(changedKey)>beforeKey,"secondary key change must advance generation");
 const auto beforeReset=clock.Resolve(id);clock.Invalidate();Require(clock.Resolve(id)>beforeReset,"real close never reuses the old process-lifetime generation");
#endif
 std::puts("PASS exact owned secondary selection, ambiguity, backing independence and stable full binding generations");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}

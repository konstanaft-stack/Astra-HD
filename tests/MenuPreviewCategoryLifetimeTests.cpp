#include "MenuPreview/Scope.hpp"
#include "MenuPreview/UiPairPolicy.hpp"
#include "Runtimes/EditorUiLayerPolicy.hpp"
#include <cstdio>
#include <cstdlib>
using cvr::menu_preview::Metadata;
#include "CategoryLifetimeProduction.hpp"
static void Check(bool ok,const char* reason){if(!ok){std::fprintf(stderr,"FAIL %s\n",reason);std::exit(1);}}
int main(){
 Metadata initial{};initial.screen=2;initial.screenEpoch=1;initial.setupID=6;initial.slotNameHash=6;
 initial.controllerIdentity=11;initial.entityIdentity=22;initial.entityID=33;initial.componentIdentity=44;initial.virtualCameraNameHash=55;
 auto next=initial;next.setupID=9;next.slotNameHash=9;
#ifdef CPVR_OLD_CATEGORY_GUARD
 Check(NativeCategoryAllows(initial,next),"same validated owner category must not require a new epoch");
#else
 AppearanceLifetimeIdentity identity;
 struct S{Metadata metadata;};cvr::menu_preview::ScopeStore<S> scopes;
 const auto epoch=scopes.Begin();initial.screenEpoch=epoch;
 const auto publish=[&](const Metadata& value){
  if(!identity.Allows(value,scopes.IsEpochOpen(value.screenEpoch))){scopes.Clear(value.screenEpoch);identity.Clear();return false;}
  auto scope=std::make_shared<S>();scope->metadata=value;if(!scopes.Publish(scope))return false;identity.Remember(value);return true;
 };
 Check(publish(initial),"first validated appearance binds lifetime");
 const Metadata accepted=initial;const uint64_t acceptedBodyLeft=100,acceptedBodyRight=200,acceptedUi=300;
 for(const auto setup:{9u,2u,6u}){
  Check(scopes.Invalidate(epoch)&&!scopes.Read()&&scopes.IsEpochOpen(epoch),"category source gap invalidates only current producer");
  Check(cvr::xr::EditorUiRetainedAllowed(accepted,7,7,scopes.IsEpochOpen(epoch),nullptr),"complete accepted body/UI packet retained across gap");
  Check(acceptedBodyLeft==100&&acceptedBodyRight==200&&acceptedUi==300,"whole accepted display immutable");
  auto changed=initial;changed.setupID=setup;changed.slotNameHash=setup;changed.componentIdentity++;
  Check(publish(changed),"same owner different setup/component admitted after validation");
  Check(cvr::xr::EditorUiRetainedAllowed(accepted,7,7,true,&changed),"prior complete packet remains valid until replacement complete");
  cvr::menu_preview::UiPairPolicy pair;
  const cvr::menu_preview::UiPairScope oldScope{epoch,6,11,44,55,66},newScope{epoch,setup,11,45,55,66};
  pair.Publish({oldScope,100,0,1,1,1,0,1});pair.Publish({oldScope,200,0,2,2,2,1,2});
  pair.Publish({newScope,400,0,3,3,3,0,3});Check(!pair.Read(newScope),"new left cannot mix with prior setup right");
  pair.Publish({newScope,500,0,4,4,4,1,4});Check(bool(pair.Read(newScope)),"complete new strict setup pair admitted");
  struct Attachment{Metadata scope;uint32_t eye{},nativeFrame{};uint64_t sourceSequence{},cameraRevision{};};
  Attachment receipt{changed,1,12,14,16},ui=receipt;
  Check(cvr::xr::EditorUiAttachmentMatches(ui,receipt),"matching fresh setup UI/application admitted");
  ui.scope.setupID++;Check(!cvr::xr::EditorUiAttachmentMatches(ui,receipt),"old/new setup attachment not interchangeable");ui=receipt;
  ui.sourceSequence++;Check(!cvr::xr::EditorUiAttachmentMatches(ui,receipt),"source association remains strict");ui=receipt;
  ui.cameraRevision++;Check(!cvr::xr::EditorUiAttachmentMatches(ui,receipt),"camera association remains strict");
 }
 Check(scopes.Invalidate(epoch),"foreign owner follows a real invalidation gap");
 auto foreign=initial;foreign.entityIdentity++;Check(!publish(foreign),"foreign owner rejected after source invalidation");
 Check(!scopes.IsEpochOpen(epoch)&&!cvr::xr::EditorUiRetainedAllowed(accepted,7,7,false,nullptr),"foreign owner closes old retention");
 Check(!identity.Allows(initial,false),"expired epoch cannot rebind even a matching fingerprint");
 for(unsigned field=0;field<4;++field){auto zero=initial;if(field==0)zero.screenEpoch=0;if(field==1)zero.controllerIdentity=0;if(field==2)zero.entityIdentity=0;if(field==3)zero.entityID=0;Check(!identity.Allows(zero,true),"zero lifecycle identity rejected");}
 const auto nextEpoch=scopes.Begin();identity.Clear();auto fresh=initial;fresh.screenEpoch=nextEpoch;fresh.controllerIdentity++;
 Check(publish(fresh),"clear/begin new controller binds its new lifetime");
 Check(!cvr::xr::EditorUiRetainedAllowed(accepted,7,7,false,&fresh),"new controller cannot retain old packet");
 Check(!cvr::xr::EditorUiRetainedAllowed(fresh,7,8,true,nullptr),"tracking origin reset rejects retained packet");
 Check(scopes.Clear(nextEpoch),"authoritative exit closes lifetime");identity.Clear();Check(!identity.Allows(fresh,false),"clear rejects stale fingerprint");
#endif
 std::puts("PASS category lifetime: validated owner gaps, complete retained packet, strict setup pairing and authoritative close");
}

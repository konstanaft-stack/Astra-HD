#include "MenuPreview/EditorSceneImage.hpp"
#include "MenuPreview/EditorUiSurface.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/EditorScenePolicy.hpp"
#include "MenuPreview/EditorNativeScenePairPolicy.hpp"
#include "Render/CommandResources.hpp"
#include <array>
#include <mutex>
extern void Log(const char*,...);
namespace cvr::menu_preview {
using Microsoft::WRL::ComPtr;
// No native-list COM ref: the list owns the replayable staging token through
// CommandResources; PixelReadback retains only this opaque recording.
struct EditorSceneRecording {
 cvr::render::NativeColorSnapshot stage;std::shared_ptr<const CameraApplication> app;
 ComPtr<ID3D12Fence> orderFence;ComPtr<ID3D12CommandQueue> lastQueue;
 uint64_t generation{},orderValue{},cacheEpoch{};bool unknown{};
};
namespace {
struct Context {ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12CommandQueue> queue;uint64_t value{};bool unknown{};};
struct MissKey {uint32_t epoch{},setup{},eye{};uint64_t source{};bool operator==(const MissKey&)const=default;};
struct State {std::mutex mutex;cvr::render::NativeColorSnapshots stages,finals;std::array<Context,3> contexts;std::array<std::shared_ptr<const EditorSceneImage>,5> cache;std::array<std::shared_ptr<EditorSceneRecording>,8> quarantine;uint64_t serial{},rawExecutionSerial{},cacheEpoch{};uint32_t stageRows{},finalRows{},missRows{};std::array<MissKey,8> misses{};};
State& S(){static auto* s=new State;return *s;}
void Quarantine(const std::shared_ptr<EditorSceneRecording>& r){r->unknown=true;for(auto& p:S().quarantine){if(p==r)return;if(!p){p=r;return;}}}
bool Known(const std::shared_ptr<EditorSceneRecording>& r){if(!r||r->unknown||!r->stage||!r->stage.lifetime||!r->app||!r->orderFence)return false;if(r->orderFence->GetCompletedValue()==UINT64_MAX){Quarantine(r);return false;}return true;}
bool SignalOrder(ID3D12CommandQueue* q,const std::shared_ptr<EditorSceneRecording>& r){if(!Known(r)||r->orderValue>=UINT64_MAX-1){if(r)Quarantine(r);return false;}r->lastQueue=q;const auto v=++r->orderValue;if(FAILED(q->Signal(r->orderFence.Get(),v))||r->orderFence->GetCompletedValue()==UINT64_MAX){Quarantine(r);return false;}return true;}
Context* RecycleContext(ID3D12Device* device){for(auto& c:S().contexts){
 if(c.unknown)continue;if(c.fence){const auto done=c.fence->GetCompletedValue();if(done==UINT64_MAX){c.unknown=true;continue;}if(done<c.value)continue;}
 // Release this completed list's operand tokens BEFORE reserving a final cell.
 EditorUiPrivateCommandScope privateCommands;
 if(c.commands)cvr::gpu::ResetCommandResources(c.commands.Get());cvr::gpu::CollectCommandResources();
 if(c.device.Get()!=device){c.commands.Reset();c.allocator.Reset();c.fence.Reset();c.queue.Reset();c.value=0;c.device=device;}
 if(!c.fence&&FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&c.fence))))continue;
 bool ready{};if(!c.allocator||!c.commands){c.commands.Reset();c.allocator.Reset();ready=SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c.allocator)))&&SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c.allocator.Get(),nullptr,IID_PPV_ARGS(&c.commands)));if(!ready){c.commands.Reset();c.allocator.Reset();}}
 else ready=SUCCEEDED(c.allocator->Reset())&&SUCCEEDED(c.commands->Reset(c.allocator.Get(),nullptr));if(ready)return &c;
 }return nullptr;}
}
bool EditorSceneCaptureEnabled()noexcept{return EditorUiSeparationEnabled();}
std::shared_ptr<EditorSceneRecording> RecordEditorSceneImage(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES state,std::shared_ptr<const CameraApplication> app,uint64_t generation)noexcept{
 if(!EditorSceneCaptureEnabled()||!list||!resource||!generation||!app||app->metadata.screen!=2||app->frame.eye>1||!app->frame.sourceSequence||!app->projection.valid||!app->projection.origin)return {};
 if(app->source.mode==NativePreviewMode::NativeEditorDual&&!EditorNativeSourceRegistered(app->metadata,app->source))return {};
 try{std::lock_guard lock(S().mutex);auto r=std::make_shared<EditorSceneRecording>();r->app=std::move(app);r->generation=generation;r->cacheEpoch=S().cacheEpoch;ComPtr<ID3D12Device> device;
 if(FAILED(list->GetDevice(IID_PPV_ARGS(&device)))||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&r->orderFence))))return {};
 r->stage=S().stages.Record(list,resource,state);const bool ready=r->stage&&cvr::gpu::KeepCommandResources(list,{r->stage.lifetime.Get(),r->orderFence.Get()});
 if(S().stageRows++<8){const auto d=resource->GetDesc();Log("[EditorSceneImage] staging ready=%u list=%p generation=%llu raw=%p desc=%llux%u format=%u mip=%u eye=%u frame=%u source=%llu revision=%llu state=%X; exact native writer, replayable staging NOT final\n",unsigned(ready),list,generation,resource,d.Width,d.Height,unsigned(d.Format),d.MipLevels,r->app->frame.eye,r->app->frame.nativeFrame,r->app->frame.sourceSequence,r->app->localPoseRevision,unsigned(state));}
 if(!ready)return {};return r;
 }catch(...){return {};}}
void BeforeEditorSceneExecute(ID3D12CommandQueue* q,std::span<const std::shared_ptr<EditorSceneRecording>> records)noexcept{
 if(!q||!EditorSceneCaptureEnabled())return;try{std::lock_guard lock(S().mutex);for(const auto& r:records)if(Known(r)&&r->lastQueue&&r->lastQueue.Get()!=q&&r->orderValue){
 // Covers both the previous staging write and the subsequent final-copy read.
 if(FAILED(q->Wait(r->orderFence.Get(),r->orderValue)))Quarantine(r);
 }}catch(...){}}
void SubmittedEditorSceneImages(ID3D12CommandQueue* q,std::span<const std::shared_ptr<EditorSceneRecording>> records,uint64_t uiSubmission,EditorSceneExecute execute,bool nativeExecutionValid)noexcept{
 if(!q||!EditorSceneCaptureEnabled()||q->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
 // Preserve evicted SDK owners until after publication mutex is released.
 std::array<std::shared_ptr<const CameraApplication>,5> retiredApps;size_t retiredCount{};
 try{std::lock_guard lock(S().mutex);for(const auto& r:records){if(!Known(r))continue;std::shared_ptr<EditorSceneImage> image;Context* c{};
 const bool native=r->app->source.mode==NativePreviewMode::NativeEditorDual;
 const bool execution=EditorSceneExecutionAllowsFinalize(r->app->source.mode,nativeExecutionValid,uiSubmission)&&
  (!native||(S().rawExecutionSerial<UINT64_MAX&&EditorNativeSourceRegistered(r->app->metadata,r->app->source)));
 if(execution&&execute&&r->cacheEpoch==S().cacheEpoch){ComPtr<ID3D12Device> device;if(SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&device))))c=RecycleContext(device.Get());}
 if(c){bool closed{};size_t slot{};for(size_t n=0;n<S().cache.size();++n){if(!S().cache[n]){slot=n;break;}if(S().cache[n]->serial<S().cache[slot]->serial)slot=n;}
 // Evict before Record; a held consumer still owns its exact pixel token.
 auto old=std::move(S().cache[slot]);if(old&&retiredCount<retiredApps.size())retiredApps[retiredCount++]=old->application;old.reset();
 EditorUiPrivateCommandScope privateCommands;
 if(cvr::gpu::KeepCommandResources(c->commands.Get(),{r->stage.lifetime.Get(),r->orderFence.Get()})){
 auto final=S().finals.Record(c->commands.Get(),r->stage.texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(final&&(closed=SUCCEEDED(c->commands->Close()))&&c->value<UINT64_MAX-1){
 image=std::make_shared<EditorSceneImage>();image->application=r->app;image->pixels=std::move(final);image->producerFence=c->fence;image->producerValue=++c->value;image->producerUiSubmission=native?0:uiSubmission;image->rawExecutionSerial=native?++S().rawExecutionSerial:0;image->serial=++S().serial;image->queue=reinterpret_cast<uintptr_t>(q);
 ID3D12CommandList* lists[]{c->commands.Get()};auto operands=cvr::gpu::PrepareCommandResources(1,lists);c->queue=q;execute(q,1,lists);
 const auto signal=q->Signal(c->fence.Get(),c->value);cvr::gpu::SubmitCommandResources(q,std::move(operands));
 if(FAILED(signal)||c->fence->GetCompletedValue()==UINT64_MAX){c->unknown=true;Quarantine(r);image.reset();}
 }}
 // A no-free-cell attempt still leaves a recording-open private list. Close
 // it before its next Reset; otherwise saturation poisons all three contexts.
 if(!closed&&FAILED(c->commands->Close())){cvr::gpu::ResetCommandResources(c->commands.Get());c->commands.Reset();c->allocator.Reset();}
 // Even no publication must order the original staging write against replay.
 if(SignalOrder(q,r)&&image){image->publishedAtMs=PublicationNowMs();if(S().finalRows++<8)Log("[EditorSceneImage] final serial=%llu uiSubmission=%llu rawExecution=%llu eye=%u frame=%u source=%llu queue=%p fence=%p value=%llu context=%llu pixels=%p; independent per-execution version\n",image->serial,image->producerUiSubmission,image->rawExecutionSerial,r->app->frame.eye,r->app->frame.nativeFrame,r->app->frame.sourceSequence,q,image->producerFence.Get(),image->producerValue,uint64_t(c-S().contexts.data()),image->pixels.texture.Get());S().cache[slot]=std::move(image);}
 }else SignalOrder(q,r);
 }}catch(...){try{std::lock_guard lock(S().mutex);for(auto& c:S().contexts)c.unknown=true;for(const auto& r:records)if(r)Quarantine(r);}catch(...){}}
}
std::shared_ptr<const EditorSceneImage> AcquireEditorSceneImage(const UiFrameReceipt& receipt)noexcept{
 if(!EditorSceneCaptureEnabled())return {};try{std::lock_guard lock(S().mutex);std::shared_ptr<const EditorSceneImage> result;for(const auto& f:S().cache)if(f&&EditorSceneMatches(*f,receipt)&&f->producerFence&&f->producerValue&&f->producerFence->GetCompletedValue()!=UINT64_MAX&&(!result||f->producerUiSubmission>result->producerUiSubmission||(f->producerUiSubmission==result->producerUiSubmission&&f->serial>result->serial)))result=f;
 if(!result&&S().missRows<S().misses.size()){
  const MissKey key{receipt.scope.screenEpoch,receipt.scope.setupID,receipt.eye,receipt.sourceSequence};bool seen{};for(uint32_t n=0;n<S().missRows;++n)seen|=S().misses[n]==key;
  if(!seen){S().misses[S().missRows++]=key;const EditorSceneImage* nearest{};uint32_t count{};uint64_t distance=UINT64_MAX;
   for(const auto& f:S().cache)if(f&&f->application){++count;const auto seq=f->application->frame.sourceSequence;const auto delta=seq>receipt.sourceSequence?seq-receipt.sourceSequence:receipt.sourceSequence-seq;if(delta<distance){nearest=f.get();distance=delta;}}
   Log("[EditorSceneImage] acquire-miss epoch=%u setup=%u eye=%u frame=%u source=%llu revision=%llu uiSubmission=%llu origin=%llu cache=%u nearestEye=%u nearestFrame=%u nearestSource=%llu nearestUi=%llu; strict scope/application association required\n",key.epoch,key.setup,receipt.eye,receipt.nativeFrame,receipt.sourceSequence,receipt.cameraRevision,receipt.uiSubmission,receipt.projection.origin,count,nearest?nearest->application->frame.eye:~0u,nearest?nearest->application->frame.nativeFrame:0,nearest?nearest->application->frame.sourceSequence:0,nearest?nearest->producerUiSubmission:0);
  }
 }return result;}catch(...){return {};}}
std::optional<EditorNativeScenePair> AcquireEditorNativeScenePair(const Metadata& common)noexcept{
 if(!EditorSceneCaptureEnabled())return {};
 try{std::lock_guard lock(S().mutex);std::array<EditorNativeSceneProof,5> proofs{};
  for(size_t n=0;n<S().cache.size();++n){const auto& image=S().cache[n];if(!image||!image->pixels.texture||!image->producerFence||
      image->restingState!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)continue;
   const auto d=image->pixels.texture->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!d.Width||!d.Height||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1)continue;
   proofs[n]={image->application,reinterpret_cast<uintptr_t>(image->pixels.texture.Get()),reinterpret_cast<uintptr_t>(image->pixels.lifetime.Get()),
    reinterpret_cast<uintptr_t>(image->producerFence.Get()),image->queue,image->producerValue,image->producerFence->GetCompletedValue(),image->serial,image->rawExecutionSerial,image->publishedAtMs};
  }
  const auto selected=SelectEditorNativeScenePair(proofs,common,PublicationNowMs());if(!selected)return {};
  EditorNativeScenePair pair;pair.eyes={S().cache[(*selected)[0]],S().cache[(*selected)[1]]};const auto& app=*pair.eyes[0]->application;
  pair.nativeFrame=app.frame.nativeFrame;pair.controlRevision=app.editorControlRevision;pair.origin=app.projection.origin;
  pair.bindingGeneration=app.source.bindingGeneration;pair.serial=std::max(pair.eyes[0]->serial,pair.eyes[1]->serial);return pair;
 }catch(...){return {};}
}
void ClearEditorSceneImages()noexcept{std::array<std::shared_ptr<const EditorSceneImage>,5> old;try{std::lock_guard lock(S().mutex);old.swap(S().cache);++S().cacheEpoch;}catch(...){} // Replayable stages/submitted operands/unknown contexts remain owned.
}
}

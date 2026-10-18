#pragma once
#include <cstdint>
#include <unordered_map>
#include <optional>
#include <vector>
namespace cvr::menu_preview {
inline bool MatchPreviewExpectedDescriptor(bool known,uintptr_t expected,uintptr_t actual){return known&&expected&&expected==actual;}
struct PreviewRtvViewEvidence {uint32_t format{},dimension{},mip{};bool known{};};
// SDK-independent values are checked against D3D12/DXGI enums by the adapter.
inline PreviewRtvViewEvidence MakePreviewRtvViewEvidence(uint32_t resourceFormat,uint32_t arraySize,
 uint32_t mipLevels,uint32_t samples,bool texture2D,const PreviewRtvViewEvidence* explicitView=nullptr)noexcept{
 // Narrow typed FP16 proof. Unsupported resources retain resource provenance,
 // but cannot acquire view semantics by inheriting a guessed resource format.
 if(resourceFormat!=10||!texture2D||arraySize!=1||!mipLevels||samples!=1)return {};
 if(!explicitView)return {10,4,0,true};
 if(!explicitView->known||explicitView->format!=10||explicitView->dimension!=4||explicitView->mip>=mipLevels)return {};
 return *explicitView;
}
struct PreviewDescriptorProof {uintptr_t handle{},resource{},device{},heap{};uint64_t heapGeneration{},resourceGeneration{},writeSequence{};uint32_t rtvFormat{},rtvDimension{},rtvMip{};bool rtvViewKnown{};bool operator==(const PreviewDescriptorProof&)const=default;};
// Metadata only, bounded and fail closed. Copies invalidate instead of propagating.
class PreviewDescriptorLedger {
 struct Heap {uintptr_t device{},start{};uint32_t count{},increment{};uint64_t generation{};};
 std::unordered_map<uintptr_t,Heap> heaps;
 std::unordered_map<uintptr_t,uint64_t> resources;
 std::unordered_map<uintptr_t,PreviewDescriptorProof> slots;
 struct Range {uintptr_t start{};uint32_t count{},increment{};};
 struct Mutation {uintptr_t device{},heap{};uint64_t heapGeneration{};bool poisoned{};std::vector<Range> ranges;};
 std::unordered_map<uint64_t,Mutation> mutations;
 uint64_t sequence{};bool valid{true};
 bool InRange(const Range& r,uintptr_t p)const{return p>=r.start&&r.increment&&(p-r.start)%r.increment==0&&(p-r.start)/r.increment<r.count;}
 bool Pending(uintptr_t device,uintptr_t p)const{for(const auto& [_,m]:mutations)if(m.device==device)for(const auto& r:m.ranges)if(InRange(r,p))return true;return false;}
 bool Contains(const Heap& h,uintptr_t p)const{return p>=h.start&&h.increment&&(p-h.start)%h.increment==0&&(p-h.start)/h.increment<h.count;}
public:
 void Fail(){valid=false;slots.clear();}
 uint64_t RegisterHeap(uintptr_t heap,uintptr_t device,uintptr_t start,uint32_t count,uint32_t increment){
  if(!valid||!heap||!device||!start||!count||!increment||uint64_t(count)*increment>UINTPTR_MAX-start)return 0;
  ForgetHeap(heap,0);for(auto i=heaps.begin();i!=heaps.end();){const auto& old=i->second;const auto end=start+uintptr_t(count)*increment,oldEnd=old.start+uintptr_t(old.count)*old.increment;
   if(start<oldEnd&&old.start<end){const auto id=i->first;++i;ForgetHeap(id,0);}else ++i;}
  if(heaps.size()>=128){Fail();return 0;}const auto generation=++sequence;heaps[heap]={device,start,count,increment,generation};return generation;
 }
 uint64_t RegisterResource(uintptr_t resource){if(!valid||!resource)return 0;const auto i=resources.find(resource);if(i!=resources.end())return i->second;if(resources.size()>=8192){Fail();return 0;}return resources[resource]=++sequence;}
 bool HasHeap(uintptr_t device,uintptr_t handle)const{if(!valid)return false;for(const auto& [_,h]:heaps)if(h.device==device&&Contains(h,handle))return true;return false;}
 bool WriteDirect(uintptr_t device,uintptr_t handle,uintptr_t resource,uint64_t resourceGeneration,PreviewRtvViewEvidence view={}){
  slots.erase(handle);const auto r=resources.find(resource);if(!valid||!resource||!resourceGeneration||r==resources.end()||r->second!=resourceGeneration)return false;
  for(const auto& [id,h]:heaps)if(h.device==device&&Contains(h,handle)){if(slots.size()>=8192){Fail();return false;}if(!view.known)view={};slots[handle]={handle,resource,device,id,h.generation,resourceGeneration,++sequence,view.format,view.dimension,view.mip,view.known};return true;}return false;
 }
 std::optional<PreviewDescriptorProof> Read(uintptr_t handle)const{if(!valid)return {};const auto s=slots.find(handle);if(s==slots.end())return {};const auto h=heaps.find(s->second.heap);const auto r=resources.find(s->second.resource);
  if(Pending(s->second.device,handle)||h==heaps.end()||h->second.generation!=s->second.heapGeneration||!Contains(h->second,handle)||r==resources.end()||r->second!=s->second.resourceGeneration)return {};return s->second;}
 bool Validate(const PreviewDescriptorProof& proof)const{const auto actual=Read(proof.handle);return actual&&*actual==proof;}
 uint64_t BeginMutation(uintptr_t device){if(!valid||!device||mutations.size()>=128){Fail();return 0;}const auto token=++sequence;mutations[token].device=device;return token;}
 void AddDestination(uint64_t token,uintptr_t start,uint32_t count,uint32_t increment){auto m=mutations.find(token);if(m==mutations.end()||!count)return;if(!increment||uint64_t(count)*increment>UINTPTR_MAX-start||m->second.ranges.size()>=128){Fail();return;}
  if(m->second.ranges.empty()&&count==1)for(const auto& [id,h]:heaps)if(h.device==m->second.device&&Contains(h,start)){m->second.heap=id;m->second.heapGeneration=h.generation;break;}
  const Range next{start,count,increment};const auto end=start+uintptr_t(count)*increment;
  for(auto& [other,x]:mutations)if(other!=token&&x.device==m->second.device)for(const auto& r:x.ranges)if(start<r.start+uintptr_t(r.count)*r.increment&&r.start<end){m->second.poisoned=true;x.poisoned=true;}
  m->second.ranges.push_back(next);InvalidateDestination(m->second.device,start,count,increment);
 }
 bool FinishDirect(uint64_t token,uintptr_t handle,uintptr_t resource,uint64_t generation,PreviewRtvViewEvidence view={}){auto m=mutations.find(token);if(m==mutations.end())return false;const auto device=m->second.device,heap=m->second.heap;const auto heapGeneration=m->second.heapGeneration;const bool poisoned=m->second.poisoned;mutations.erase(m);const auto h=heaps.find(heap);
  if(poisoned||!heap||h==heaps.end()||h->second.generation!=heapGeneration){slots.erase(handle);return false;}return WriteDirect(device,handle,resource,generation,view);
 }
 void FinishCopies(uint64_t token){mutations.erase(token);}
 void InvalidateDestination(uintptr_t device,uintptr_t start,uint32_t count,uint32_t increment){
  if(!count)return;if(!increment||uint64_t(count)*increment>UINTPTR_MAX-start){Fail();return;}
  // Native RTV writes/copies normally affect one or a few handles. Walking
  // every retained RTV under the shared lock made this sparse update costly.
  if(count<=slots.size()){
   for(uint32_t n=0;n<count;++n){const auto i=slots.find(start+uintptr_t(n)*increment);if(i!=slots.end()&&i->second.device==device)slots.erase(i);}
   return;
  }
  const Range range{start,count,increment};for(auto i=slots.begin();i!=slots.end();)if(i->second.device==device&&InRange(range,i->first))i=slots.erase(i);else ++i;
 }
 void ForgetHeap(uintptr_t heap,uint64_t generation){auto h=heaps.find(heap);if(h==heaps.end()||(generation&&h->second.generation!=generation))return;for(auto i=slots.begin();i!=slots.end();)if(i->second.heap==heap)i=slots.erase(i);else ++i;heaps.erase(h);}
 void ForgetResource(uintptr_t resource,uint64_t generation){auto r=resources.find(resource);if(r==resources.end()||(generation&&r->second!=generation))return;for(auto i=slots.begin();i!=slots.end();)if(i->second.resource==resource)i=slots.erase(i);else ++i;resources.erase(r);}
};
}

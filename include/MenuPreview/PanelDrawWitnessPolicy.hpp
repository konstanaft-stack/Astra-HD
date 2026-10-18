#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
namespace cvr::menu_preview {
struct PanelWitnessScope {
 uint32_t epoch{},setup{};uint64_t owner{},component{};
 bool operator==(const PanelWitnessScope&)const=default;
 explicit operator bool()const{return epoch&&owner&&component;}
};
struct PanelWitnessListKey {
 uintptr_t list{};uint64_t life{},generation{};
 bool operator==(const PanelWitnessListKey&)const=default;
 explicit operator bool()const{return list&&life&&generation;}
};
// Metadata only. The integration supplies immutable draw-time binding values,
// not live descriptors or GPU leases. Neither dropped diagnostics nor a full
// pool can change the pixel-consumer admission policy.
template<class Record,class Binding,size_t Records=64,size_t Bindings=16> class PanelDrawWitnessPool {
public:
 struct Batch {
  PanelWitnessScope scope{};PanelWitnessListKey key{};
  std::array<Record,Records> records{};std::array<Binding,Bindings> bindings{};
  size_t count{},bindingCount{};uint64_t seen{},recordDrops{},bindingDrops{},evictedRecords{},poolDrops{};
  uintptr_t bridgeTarget{};uint64_t bridgeTargetGeneration{};
  bool qualified{},reserved{},prefixIncomplete{};
 };
private:
 std::array<Batch,2> cells_{};PanelWitnessScope scope_{};
 uint32_t reserved_{},submitted_{};uint64_t evictedRecords_{},poolDrops_{};
 Batch* Find(PanelWitnessListKey key)noexcept{for(auto& c:cells_)if(c.key==key)return &c;return nullptr;}
 void Scope(PanelWitnessScope scope)noexcept {
  if(scope_==scope)return;
  cells_={};scope_=scope;reserved_=submitted_=0;evictedRecords_=poolDrops_=0;
 }
public:
 bool Accepting(PanelWitnessScope scope)const noexcept{return bool(scope)&&(!scope_||scope.epoch>=scope_.epoch)&&(scope!=scope_||reserved_+submitted_<2);}
 bool Observe(PanelWitnessScope scope,PanelWitnessListKey key,Record record,const Binding& binding)noexcept{
  if(!Accepting(scope)||!key)return false;Scope(scope);
  if(reserved_+submitted_>=2)return false;
  auto* cell=Find(key);
  if(!cell){
   for(auto& candidate:cells_)if(!candidate.key){cell=&candidate;break;}
   if(!cell)for(auto& candidate:cells_)if(!candidate.qualified&&!candidate.reserved){cell=&candidate;break;}
   if(!cell){++poolDrops_;return false;}
   if(cell->key)evictedRecords_+=cell->seen;
   *cell={};cell->scope=scope;cell->key=key;cell->evictedRecords=evictedRecords_;
   cell->prefixIncomplete=evictedRecords_!=0;
  }
  if(cell->reserved)return false;
  record.ordinal=++cell->seen;
  if(cell->count==Records){++cell->recordDrops;cell->prefixIncomplete=true;return false;}
  size_t index=0;for(;index<cell->bindingCount;++index)if(cell->bindings[index]==binding)break;
  if(index==cell->bindingCount){
   if(index==Bindings){++cell->bindingDrops;cell->prefixIncomplete=true;return false;}
   cell->bindings[index]=binding;++cell->bindingCount;
  }
  record.bindingIndex=static_cast<uint32_t>(index);cell->records[cell->count++]=record;return true;
 }
 void Qualify(PanelWitnessScope scope,PanelWitnessListKey key,uintptr_t target,uint64_t generation)noexcept{
  if(scope!=scope_||!target||!generation)return;
  if(auto* cell=Find(key);cell&&!cell->reserved){cell->qualified=true;cell->bridgeTarget=target;cell->bridgeTargetGeneration=generation;}
 }
 bool TakeReady(PanelWitnessScope scope,PanelWitnessListKey key,Batch& output)noexcept{
  if(scope!=scope_||reserved_+submitted_>=2)return false;
  auto* cell=Find(key);if(!cell||!cell->qualified||cell->reserved)return false;
  cell->reserved=true;++reserved_;output=*cell;output.poolDrops=poolDrops_;return true;
 }
 bool CanTakeReady(PanelWitnessScope scope,PanelWitnessListKey key)const noexcept{
  if(scope!=scope_||reserved_+submitted_>=2)return false;
  for(const auto& cell:cells_)if(cell.key==key)return cell.qualified&&!cell.reserved;
  return false;
 }
 bool Commit(PanelWitnessScope scope,PanelWitnessListKey key)noexcept{
  if(scope!=scope_)return false;
  auto* cell=Find(key);if(!cell||!cell->reserved||!reserved_||submitted_>=2)return false;
  --reserved_;++submitted_;*cell={};return true;
 }
 void Retire(PanelWitnessListKey key)noexcept{
  if(auto* cell=Find(key)){if(cell->reserved&&reserved_)--reserved_;*cell={};}
 }
};
}

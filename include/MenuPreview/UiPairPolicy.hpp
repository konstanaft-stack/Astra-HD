#pragma once
#include <array>
#include <cstdint>
#include <optional>
namespace cvr::menu_preview {
struct UiPairScope {uint32_t epoch{},setup{};uint64_t owner{},component{},camera{},pool{};bool operator==(const UiPairScope&)const=default;explicit operator bool()const{return epoch&&owner&&component&&camera&&pool;}};
struct UiPairCell {UiPairScope scope;uintptr_t resource{};uint64_t version{},sourceSequence{},uiSubmission{},serial{};uint32_t eye{},nativeFrame{};uint64_t controlRevision{};explicit operator bool()const{return bool(scope)&&resource&&version&&sourceSequence&&uiSubmission&&serial&&eye<2;}};
// Metadata only. Caller pins publications/readers with the existing capture lease cache.
class UiPairPolicy {
 std::array<UiPairCell,2> cells{};uint64_t version{};
public:
 std::array<uintptr_t,2> Clear(){std::array<uintptr_t,2> released{cells[0].resource,cells[1].resource};cells={};return released;}
 std::array<uintptr_t,2> Publish(UiPairCell cell){std::array<uintptr_t,2> retired{};if(!cell.scope||cell.eye>1||!cell.resource||!cell.sourceSequence||!cell.uiSubmission||!cell.serial)return retired;
  if((cells[0]&&cells[0].scope!=cell.scope)||(cells[1]&&cells[1].scope!=cell.scope))retired=Clear();
  auto& old=cells[cell.eye];if(old&&cell.sourceSequence<=old.sourceSequence)return retired;
  if(version==UINT64_MAX)return retired;cell.version=++version;if(!retired[cell.eye])retired[cell.eye]=old.resource;old=cell;return retired;
 }
 std::optional<std::array<UiPairCell,2>> Read(UiPairScope scope)const{if(!scope||!cells[0]||!cells[1]||cells[0].scope!=scope||cells[1].scope!=scope||cells[0].resource==cells[1].resource||cells[0].controlRevision!=cells[1].controlRevision)return {};return cells;}
 bool HasSource(UiPairScope scope,uint32_t eye,uint64_t source)const{return eye<2&&cells[eye]&&cells[eye].scope==scope&&cells[eye].sourceSequence>=source;}
 const std::array<UiPairCell,2>& Cells()const{return cells;}
};
}

#pragma once
#include "Framegen/Inputs.hpp"

namespace cvr::framegen {
struct InputTag {ID3D12Resource* resource{};uint32_t state{},type{},x{},y{},width{},height{};};
struct InputExtent {uint32_t width{},height{};DXGI_FORMAT format{};};
bool InspectInputDescription(const D3D12_RESOURCE_DESC&,const InputTag&,InputExtent&);
bool InspectInputTag(const InputTag&,InputExtent&);
bool CopyInput(ID3D12GraphicsCommandList* list,const InputTag& tag,Inputs& out);
}

#pragma once
#include "MenuPreview/PipelineEvidence.hpp"
#include <limits>
namespace cvr::menu_preview {
enum class UiEditorShaderLayout:uint8_t {Unknown,TextureOnly,BinkColorConvert};
inline UiEditorShaderLayout EditorUiShaderLayout(const PipelineShaderEvidence& shader)noexcept{
 if(!shader.srvBindingsKnown||shader.srvBindingCount>shader.srvBindings.size())return UiEditorShaderLayout::Unknown;
 if(shader.vsHash==0x8D749B2D19263CDFull&&shader.vsBytes==3042){
  for(uint32_t i=0;i<shader.srvBindingCount;++i){const auto& b=shader.srvBindings[i];if(b.resourceKind!=2||b.firstRegister!=b.lastRegister)return UiEditorShaderLayout::Unknown;}
  return UiEditorShaderLayout::TextureOnly;
 }
 // Separately decoded CB-only VS and fixed Bink color-conversion PS. This does
 // not claim matrix equivalence with the older VS or inspect parameter bytes.
 if(shader.psHash==0x3D25937C1079FEE4ull&&shader.psBytes==3536&&
    shader.vsHash==0x1A5BF587E8197141ull&&shader.vsBytes==3318&&
    shader.srvBindingCount==5&&shader.colorOutputsKnown&&shader.colorOutputMask==1){
  uint32_t seen{};
  for(uint32_t i=0;i<shader.srvBindingCount;++i){const auto& b=shader.srvBindings[i];
   if(b.registerSpace||b.firstRegister!=b.lastRegister||b.firstRegister>4||
      b.resourceKind!=(b.firstRegister==4?12u:2u)||(seen&(1u<<b.firstRegister)))return UiEditorShaderLayout::Unknown;
   seen|=1u<<b.firstRegister;
  }
  if(seen==31)return UiEditorShaderLayout::BinkColorConvert;
 }
 return UiEditorShaderLayout::Unknown;
}
struct UiSrvView {
 uint64_t firstElement{},resourceWidth{},resourceGeneration{};
 uint32_t dimension{},format{},elements{},stride{},flags{},resourceDimension{};
 bool known{};
 bool operator==(const UiSrvView&)const=default;
};
struct UiSrvIdentity {
 uintptr_t resource{};uint64_t generation{},version{};
 bool present{},pending{},overlap{},unorderedAccess{},cbvOnly{};
};
inline bool UiTypedSrvCurrent(const UiSrvView& v,const UiSrvIdentity& i)noexcept{
 return v.known&&i.present&&i.resource&&i.generation&&i.version&&v.resourceGeneration==i.generation&&
  !i.pending&&!i.overlap&&!i.unorderedAccess&&!i.cbvOnly;
}
inline bool UiBinkBufferKnown(const UiSrvView& v,const UiSrvIdentity& i)noexcept{
 if(!UiTypedSrvCurrent(v,i)||v.dimension!=D3D12_SRV_DIMENSION_BUFFER||v.format!=DXGI_FORMAT_UNKNOWN||
    v.flags!=D3D12_BUFFER_SRV_FLAG_NONE||v.stride!=96||!v.elements||v.resourceDimension!=D3D12_RESOURCE_DIMENSION_BUFFER)return false;
 constexpr auto maximum=(std::numeric_limits<uint64_t>::max)();
 if(v.firstElement>maximum/v.stride||v.elements>maximum/v.stride)return false;
 const auto first=v.firstElement*v.stride,bytes=uint64_t(v.elements)*v.stride;
 return first<=v.resourceWidth&&bytes<=v.resourceWidth-first;
}
inline bool UiBinkTextureKnown(const UiSrvView& v,const UiSrvIdentity& i)noexcept{
 return UiTypedSrvCurrent(v,i)&&v.dimension==D3D12_SRV_DIMENSION_TEXTURE2D;
}
inline UiSrvView UiCopiedSrvView(const UiSrvView& source,bool accepted,bool sourceCurrent,bool pending)noexcept{
 return accepted&&sourceCurrent&&!pending?source:UiSrvView{};
}
}

#pragma once
#include <cstdint>
#include <array>
#include <d3d12.h>

namespace cvr::menu_preview {
struct ShaderSrvBinding { uint32_t registerSpace{},firstRegister{},lastRegister{},resourceKind{}; };
struct PipelineShaderEvidence {
    uint64_t psHash{},vsHash{};
    uint32_t psBytes{},vsBytes{};
    uint32_t srvBindingCount{};
    std::array<ShaderSrvBinding,16> srvBindings{};
    bool srvBindingsKnown{};
    uint32_t colorOutputMask{}; // Potentially written SV_Target registers (Mask & ~NeverWrites).
    bool colorOutputsKnown{};
};
struct ComputeShaderEvidence {
    uint64_t csHash{};
    uint32_t csBytes{},srvBindingCount{},uavBindingCount{};
    std::array<ShaderSrvBinding,16> srvBindings{};
    std::array<ShaderSrvBinding,8> uavBindings{};
    bool bindingsKnown{};
};
struct RootDescriptorEvidence {
    D3D12_SHADER_VISIBILITY visibility{};
    D3D12_DESCRIPTOR_RANGE_TYPE type{};
    uint32_t rangeIndex{},baseShaderRegister{},shaderRegister{},registerSpace{},descriptorCount{},declaredOffset{};
    uint64_t effectiveOffset{};
};
struct ShaderSrvLocation {uint32_t rootIndex{},descriptorOffset{};};
struct ComputeLocation {uint32_t rootIndex{},descriptorOffset{};bool unorderedAccess{};};
// Resource bindings are compiler PSV0 declarations; the caller must resolve the actual
// bound descriptor and resource, not infer sampling from a root range alone.
// Call outside the UiConsumer mutex. Each implementation owns its cache lock.
bool QueryPipelineShaders(uintptr_t pipeline,PipelineShaderEvidence&)noexcept;
// Creation hook supplies the actual CS, including any existing replacement.
void RecordComputeShader(uintptr_t pipeline,const D3D12_SHADER_BYTECODE&)noexcept;
bool QueryComputeShader(uintptr_t pipeline,ComputeShaderEvidence&)noexcept;
bool ResolveComputeLocations(uintptr_t root,const ComputeShaderEvidence&,std::array<ComputeLocation,24>&,uint32_t& count)noexcept;
bool ResolveRootDescriptor(uintptr_t root,uint32_t rootIndex,uint64_t descriptorOffset,RootDescriptorEvidence&)noexcept;
// Unique direct vertex-visible CBV only; descriptor-table CBVs are unsupported.
// Resolve outside the UiConsumer mutex; shader register is not a root index.
bool ResolveVertexConstantRoot(uintptr_t root,uint32_t shaderRegister,uint32_t registerSpace,uint32_t& rootIndex)noexcept;
struct VertexConstantLocation {uint32_t rootIndex=UINT32_MAX;uint64_t descriptorOffset{};bool table{};};
// Unique vertex-visible direct or table CBV; never root constants or a guessed index.
bool ResolveVertexConstantLocation(uintptr_t root,uint32_t shaderRegister,uint32_t registerSpace,VertexConstantLocation&)noexcept;
bool ResolvePixelSrvLocations(uintptr_t root,const PipelineShaderEvidence&,std::array<ShaderSrvLocation,16>&,uint32_t& count)noexcept;
}

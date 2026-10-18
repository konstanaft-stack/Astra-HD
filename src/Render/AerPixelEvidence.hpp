#pragma once
#include "Render/AerReconstructionPass.hpp"
#include <wrl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>

namespace cvr::render::pixel_evidence {
using Microsoft::WRL::ComPtr;
struct Surface {
    ComPtr<ID3D12Resource> readback;
    D3D12_RESOURCE_DESC description{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t bytes{},rowBytes{};
    uint32_t rows{};
};
struct Capture {
    Surface input,output;
    std::filesystem::path directory;
    AerWarpParams warp;
    AerVertexConstants vertex;
    std::array<float,4> filter{};
    AerColorRoute color;
    int64_t sourceId{};
    uint64_t pairSerial{},fenceValue{};
    uint32_t eye{};
    std::array<uint64_t,9> identity{};
};
struct Configuration {
    std::filesystem::path directory;
    int64_t firstSource=800;
    uint32_t attemptedEyes{};
    Configuration() {
        wchar_t path[32768]{};
        const auto n=GetEnvironmentVariableW(L"CPVR_AER_PIXEL_CAPTURE_DIR",path,DWORD(std::size(path)));
        if(n && n<std::size(path))directory=path;
    }
};
inline bool Allocate(ID3D12Device* device,ID3D12Resource* texture,Surface& out) {
    out.description=texture->GetDesc();
    device->GetCopyableFootprints(&out.description,0,1,0,&out.footprint,&out.rows,&out.rowBytes,&out.bytes);
    if(!out.bytes || out.bytes>64ull*1024*1024 || !out.rows || out.rowBytes>out.footprint.Footprint.RowPitch)return false;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=out.bytes;
    desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out.readback)));
}
inline std::unique_ptr<Capture> Prepare(Configuration& config,ID3D12Device* device,
    const AerRecordRequest& request,const AerVertexConstants& vertex,const std::array<float,4>& filter) {
    if(config.directory.empty() || request.evidenceSourceFloor<config.firstSource || request.eye>1 ||
       (config.attemptedEyes&(1u<<request.eye)))return {};
    config.attemptedEyes|=1u<<request.eye; // bounded one attempt per eye, even on allocation failure
    auto c=std::make_unique<Capture>();c->directory=config.directory;c->warp=request.warp;c->vertex=vertex;
    c->filter=filter;c->color=request.color;c->sourceId=request.evidenceSourceId;c->pairSerial=request.evidencePairSerial;
    c->fenceValue=request.completion.value;c->eye=request.eye;
    c->identity=request.evidenceIdentity;
    if(!Allocate(device,request.source,c->input)||!Allocate(device,request.destination,c->output))return {};
    return c;
}
inline void Copy(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,
    D3D12_RESOURCE_STATES before,const Surface& surface) {
    const bool transition=(before&D3D12_RESOURCE_STATE_COPY_SOURCE)==0;
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,D3D12_RESOURCE_STATE_COPY_SOURCE};
    if(transition)list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=surface.readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=surface.footprint;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    if(transition){barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=before;list->ResourceBarrier(1,&barrier);}
}
inline bool SaveSurface(const Surface& s,const std::filesystem::path& path) {
    void* raw{};D3D12_RANGE read{0,SIZE_T(s.bytes)};
    if(FAILED(s.readback->Map(0,&read,&raw))||!raw)return false;
    bool ok=false;
    try {
        std::ofstream file(path,std::ios::binary);
        const auto* pixels=static_cast<const char*>(raw)+s.footprint.Offset;
        for(uint32_t y=0;y<s.rows;++y)file.write(pixels+size_t(y)*s.footprint.Footprint.RowPitch,std::streamsize(s.rowBytes));
        file.close();ok=bool(file);
    }catch(...){}
    D3D12_RANGE none{};s.readback->Unmap(0,&none);return ok;
}
template<class T,size_t N> inline void Array(std::ostream& out,const std::array<T,N>& values) {
    out<<'[';for(size_t i=0;i<N;++i){if(i)out<<',';out<<values[i];}out<<']';
}
inline bool Save(const Capture& c) noexcept {
    try {
        std::filesystem::create_directories(c.directory);
        const auto stem="eye"+std::to_string(c.eye)+"-source"+std::to_string(c.sourceId);
        if(!SaveSurface(c.input,c.directory/(stem+"-input.raw")) || !SaveSurface(c.output,c.directory/(stem+"-output.raw")))return false;
        std::ofstream out(c.directory/(stem+".json"));out<<std::setprecision(9);
        out<<"{\"source_id\":"<<c.sourceId<<",\"eye\":"<<c.eye<<",\"pair_serial\":"<<c.pairSerial<<",\"completed_fence\":"<<c.fenceValue;
        out<<",\"source_identity\":";Array(out,c.identity);
        const auto surface=[&](const char* name,const Surface& s){out<<",\""<<name<<"\":{\"width\":"<<s.description.Width
            <<",\"height\":"<<s.description.Height<<",\"format\":"<<uint32_t(s.description.Format)<<",\"row_bytes\":"<<s.rowBytes<<",\"rows\":"<<s.rows<<'}';};
        surface("input",c.input);surface("output",c.output);
        out<<",\"input_encoding\":"<<uint32_t(c.color.input)<<",\"output_encoding\":"<<uint32_t(c.color.output);
        out<<",\"filter_constants\":";Array(out,c.filter);out<<",\"tangents\":";Array(out,c.warp.tangents);
        out<<",\"crop\":";Array(out,c.warp.crop);out<<",\"reference_quaternion\":";Array(out,c.warp.reference);
        out<<",\"captured_quaternion\":";Array(out,c.warp.captured);out<<",\"source_viewport\":";Array(out,c.vertex.sourceViewport);
        out<<",\"gpu_matrix\":";Array(out,c.vertex.matrix0);out<<"}\n";out.close();return bool(out);
    }catch(...){return false;}
}
}

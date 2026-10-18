#pragma once
// Opt-in, bounded diagnostic of the inputs actually admitted to the SR fix.
// Reuses the established readback/copy helpers; never changes the selected shader.
#include "../Render/AerPixelEvidence.hpp"
#include "Framegen/SrHistoryOwner.hpp"
#include <bit>
#include <cwchar>

namespace cvr::framegen::sr_evidence {
namespace pixels=cvr::render::pixel_evidence;
using Microsoft::WRL::ComPtr;
inline constexpr std::array<const char*,9> Names{
    "cur_mv_snapshot","prev_mv_snapshot","cur_depth_snapshot","prev_depth_snapshot",
    "old_depth_snapshot","corrected_mv","current_color","native_mv","native_depth"};
struct Configuration {
    std::filesystem::path directory;
    int64_t firstSource=800;
    uint32_t attemptedEyes{};
    Configuration(){
        wchar_t path[32768]{};
        const auto count=GetEnvironmentVariableW(L"CPVR_SR_PIXEL_CAPTURE_DIR",path,DWORD(std::size(path)));
        if(!count||count>=std::size(path))return;
        directory=path;if(!directory.is_absolute()){directory.clear();return;}
        wchar_t value[32]{};
        const auto n=GetEnvironmentVariableW(L"CPVR_SR_PIXEL_CAPTURE_FIRST_SOURCE",value,DWORD(std::size(value)));
        if(n&&n<std::size(value)){wchar_t*end{};const auto v=std::wcstoll(value,&end,10);if(end&&!*end&&v>=2)firstSource=v;}
    }
};
struct Capture {
    std::filesystem::path directory;
    std::array<pixels::Surface,9> surfaces;
    // In particular, native NGX inputs must survive until this pass's fence,
    // independently of the earlier tag-copy fence and the host's tag lifetime.
    std::array<ComPtr<ID3D12Resource>,9> pins;
    std::array<SrConstantsObservation,3> observations; // old, previous, current
    std::array<sr::NgxCameraInputs,3> nativeInputs;
    sr::Constants constants{};
    uint64_t historyEpoch{},resourceEpoch{},parametersKey{},fenceValue{};
    uint32_t nativeResult{};bool nativeResultKnown{};
    HRESULT submissionResult=S_OK;
};
inline std::unique_ptr<Capture> Record(Configuration&config,ID3D12GraphicsCommandList*list,
    const std::array<ID3D12Resource*,9>&images,const std::array<SrConstantsObservation,3>&observations,
    const std::array<sr::NgxCameraInputs,3>&nativeInputs,const sr::Constants&constants,
    uint64_t historyEpoch,uint64_t resourceEpoch,uint64_t parametersKey,uint64_t fenceValue) noexcept {
    if(config.directory.empty())return {};
    const auto&receipt=observations[2].source.source;const auto eye=uint32_t(receipt.physicalEye);
    if(!list||!receipt||receipt.sourceId<config.firstSource||eye>1||(config.attemptedEyes&(1u<<eye)))return {};
    config.attemptedEyes|=1u<<eye; // At most one allocation/copy attempt per eye.
    try {
        auto capture=std::make_unique<Capture>();ComPtr<ID3D12Device> device;
        if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return {};
        capture->directory=config.directory;capture->observations=observations;capture->nativeInputs=nativeInputs;
        capture->constants=constants;capture->historyEpoch=historyEpoch;capture->resourceEpoch=resourceEpoch;
        capture->parametersKey=parametersKey;capture->fenceValue=fenceValue;
        uint64_t bytes=0;
        for(size_t i=0;i<images.size();++i){
            if(!images[i]||!pixels::Allocate(device.Get(),images[i],capture->surfaces[i]))return {};
            bytes+=capture->surfaces[i].bytes;if(bytes>128ull*1024*1024)return {};
            capture->pins[i]=images[i];
        }
        // The private pass ends all outputs in NPS and preserves its NPS inputs.
        // The two native images are the validated NGX input resources at the
        // StreamlineOriginalEvaluate boundary (the reference's 0x40 contract).
        for(size_t i=0;i<images.size();++i)pixels::Copy(list,images[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,capture->surfaces[i]);
        return capture;
    }catch(...){return {};}
}
inline bool Save(const Capture&c) noexcept {
    try {
        std::filesystem::create_directories(c.directory);
        const auto&current=c.observations[2].source.source;
        const auto stem="source"+std::to_string(current.sourceId)+"-eye"+std::to_string(uint32_t(current.physicalEye));
        for(size_t i=0;i<c.surfaces.size();++i)
            if(!pixels::SaveSurface(c.surfaces[i],c.directory/(stem+"-"+Names[i]+".raw")))return false;
        std::ofstream cb(c.directory/(stem+".cb480"),std::ios::binary);
        cb.write(reinterpret_cast<const char*>(&c.constants),sizeof(c.constants));cb.close();if(!cb)return false;
        std::ofstream out(c.directory/(stem+".json"));out<<std::setprecision(9);
        out<<"{\"schema\":1,\"scope\":\"SR correction inputs and output; native DLSS color output is not captured\",\"source_id\":"<<current.sourceId
           <<",\"eye\":"<<uint32_t(current.physicalEye)<<",\"completed_fence\":"<<c.fenceValue
           <<",\"history_epoch\":"<<c.historyEpoch<<",\"resource_epoch\":"<<c.resourceEpoch<<",\"parameters_key\":"<<c.parametersKey
           <<",\"submission_result\":"<<int64_t(c.submissionResult)<<",\"native_result\":";
        if(c.nativeResultKnown)out<<c.nativeResult;else out<<"null";
        out<<",\"observations_old_to_current\":[";
        for(size_t i=0;i<c.observations.size();++i){if(i)out<<',';const auto&o=c.observations[i];const auto&s=o.source;const auto&r=s.source;const auto&n=c.nativeInputs[i];
            out<<"{\"source_id\":"<<r.sourceId<<",\"eye\":"<<uint32_t(r.physicalEye)<<",\"scene_ordinal\":"<<r.sceneOrdinal
               <<",\"native_frame\":"<<s.nativeFrame<<",\"sl_token\":"<<o.slToken<<",\"pose_id\":"<<s.poseId<<",\"write_id\":"<<s.writeId
               <<",\"origin\":"<<s.origin<<",\"capture_clock_generation\":"<<s.captureClockGeneration<<",\"requested_pose_ns\":"<<r.requestedPoseTimeNs
               <<",\"render_subrect\":";pixels::Array(out,n.renderSubrect);out<<",\"output_size\":";pixels::Array(out,n.outputSize);
            out<<",\"mv_scale\":";pixels::Array(out,n.mvScale);out<<",\"jitter\":";pixels::Array(out,n.jitterPixels);
            out<<",\"lowres_mv\":"<<(n.lowResMv?"true":"false")<<",\"sl_flags\":";pixels::Array(out,o.constants.flags);
            out<<",\"sl_values_bits\":[";for(size_t j=0;j<o.constants.values.size();++j){if(j)out<<',';out<<std::bit_cast<uint32_t>(o.constants.values[j]);}
            out<<"],\"sl_values_present\":\""<<o.constants.valuePresent.to_string()<<"\",\"sl_flags_present\":\""<<o.constants.flagPresent.to_string()<<"\""
               <<",\"eye_fixed\":";pixels::Array(out,s.appliedEye.writtenEye.position);out<<",\"centre_fixed\":";pixels::Array(out,s.appliedEye.centreFixed);
            out<<",\"eye_quaternion_bits\":";pixels::Array(out,s.appliedEye.writtenEye.rotation);out<<'}';
        }
        out<<"],\"surfaces\":[";
        for(size_t i=0;i<c.surfaces.size();++i){if(i)out<<',';const auto&s=c.surfaces[i];
            out<<"{\"name\":\""<<Names[i]<<"\",\"width\":"<<s.description.Width<<",\"height\":"<<s.description.Height
               <<",\"format\":"<<uint32_t(s.description.Format)<<",\"row_bytes\":"<<s.rowBytes<<",\"rows\":"<<s.rows<<'}';}
        out<<"]}\n";out.close();return bool(out);
    }catch(...){return false;}
}
}

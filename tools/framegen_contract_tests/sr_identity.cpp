#include "Framegen/SrIdentityDiagnostics.hpp"
#include "CpuD3d.hpp"
#include <sl_core_types.h>
#include <sl_consts.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
using namespace cvr::framegen;
static unsigned checks{},failures{};
#define CHECK(x) do {++checks;if(!(x)){++failures;std::printf("FAIL line%d: %s\n",__LINE__,#x);}}while(false)
struct Token final:sl::FrameToken {uint32_t value{};operator uint32_t() const override{return value;}};
static SrTraceRecord Evaluate(const sl::BaseStructure* const* inputs,uint32_t n,Token& token){
    SrTraceRecord out;out.nativeFrame=77;out.flags|=SrNativeFrameKnown|SrSourceKnown|SrPhysicalEyeKnown;
    out.sourceId=1001;out.physicalEye=1;SnapshotSrEvaluate(out,1001,&token,inputs,n,reinterpret_cast<void*>(0x1234));return out;
}
static SrConstantsWire Constants(const SrTraceRecord& record){SrConstantsWire out;std::memcpy(&out,record.payload.data(),sizeof(out));return out;}
static SrTagWire Tag(const SrTraceRecord& record,unsigned index){SrTagWire out;std::memcpy(&out,reinterpret_cast<const char*>(record.payload.data())+index*sizeof(out),sizeof(out));return out;}
int main(int argc,char** argv){
    Token token;sl::ViewportHandle viewport(0);const sl::BaseStructure* input=&viewport;
    auto value=Evaluate(&input,1,token);
#ifdef SR_IDENTITY_NEGATIVE_CONTROL
    // Historical conflation: zero SL token overwritten with native frame ID.
    // The production snapshot is otherwise unchanged; this must be rejected.
    value.slFrameToken=value.nativeFrame;
#endif
    CHECK(value.slFrameToken==0 && (value.flags&SrTokenKnown));CHECK(value.nativeFrame==77 && value.sourceId==1001);
    CHECK(value.physicalEye==1 && value.slViewport==0 && value.viewportStatus==uint32_t(SrViewportStatus::Known));
    CHECK(value.commandList==0x1234 && value.feature==1001 && !(value.flags&SrResultKnown));CHECK(value.ngxFeatureHandle==0);
    auto absent=Evaluate(nullptr,0,token);CHECK(absent.viewportStatus==uint32_t(SrViewportStatus::Missing));
    auto malformed=Evaluate(nullptr,1,token);CHECK(malformed.viewportStatus==uint32_t(SrViewportStatus::Malformed));
    input=reinterpret_cast<const sl::BaseStructure*>(1);malformed=Evaluate(&input,1,token);
    CHECK(malformed.viewportStatus==uint32_t(SrViewportStatus::Malformed) && !(malformed.flags&SrViewportKnown));
    sl::ViewportHandle other(7);const sl::BaseStructure* duplicates[]{&viewport,&other};
    auto duplicate=Evaluate(duplicates,2,token);CHECK(duplicate.viewportStatus==uint32_t(SrViewportStatus::Ambiguous));
    duplicates[1]=&viewport;duplicate=Evaluate(duplicates,2,token);CHECK(duplicate.viewportStatus==uint32_t(SrViewportStatus::Ambiguous));
    CHECK(duplicate.viewportsFound==2 && !(duplicate.flags&SrViewportKnown));
    viewport.next=&other;input=&viewport;duplicate=Evaluate(&input,1,token);CHECK(duplicate.viewportsFound==2);
    viewport.next=&viewport;auto cycle=Evaluate(&input,1,token);CHECK(cycle.viewportStatus==uint32_t(SrViewportStatus::Cycle));viewport.next=nullptr;
    viewport.structVersion=2;auto version=Evaluate(&input,1,token);CHECK(version.viewportStatus==uint32_t(SrViewportStatus::UnsupportedVersion));viewport.structVersion=1;
    sl::ViewportHandle invalid(uint32_t(UINT32_MAX));input=&invalid;
    CHECK(Evaluate(&input,1,token).viewportStatus==uint32_t(SrViewportStatus::InvalidValue));
    CHECK(Evaluate(nullptr,17,token).viewportStatus==uint32_t(SrViewportStatus::Limit));
    sl::Constants unknown;input=&unknown;CHECK(Evaluate(&input,1,token).viewportStatus==uint32_t(SrViewportStatus::Missing));
    std::array<sl::Constants,9> chain;for(size_t i=0;i+1<chain.size();++i)chain[i].next=&chain[i+1];input=&chain[0];
    CHECK(Evaluate(&input,1,token).viewportStatus==uint32_t(SrViewportStatus::Limit));
    chain[7].next=nullptr;std::array<const sl::BaseStructure*,9> repeatedChains;repeatedChains.fill(&chain[0]);
    auto total=Evaluate(repeatedChains.data(),uint32_t(repeatedChains.size()),token);
    CHECK(total.viewportStatus==uint32_t(SrViewportStatus::Limit) && total.capturedCount==64);
    input=&viewport;token.structVersion=2;auto badToken=Evaluate(&input,1,token);CHECK(!(badToken.flags&SrTokenKnown));token.structVersion=1;
    SnapshotSrEvaluate(badToken,0,reinterpret_cast<const sl::FrameToken*>(1),nullptr,0,nullptr);
    CHECK(badToken.flags&SrArgumentsMalformed);

    // Independent SDK field placement: capture exact bits, including -0 and NaN.
    sl::Constants constants;std::array<uint32_t,104> expected{};
    for(unsigned i=0;i<104;++i)expected[i]=0x3F800000+i*131;
    expected[1]=0x80000000;expected[62]=0x7FC01234;expected[80]=0xBE800000;expected[81]=0x3F400000;
    auto fill=[&](void* field,unsigned first,unsigned count){std::memcpy(field,expected.data()+first,count*4);};
    fill(&constants.cameraViewToClip,0,16);fill(&constants.clipToCameraView,16,16);fill(&constants.clipToLensClip,32,16);
    fill(&constants.clipToPrevClip,48,16);fill(&constants.prevClipToClip,64,16);fill(&constants.jitterOffset,80,2);
    fill(&constants.mvecScale,82,2);fill(&constants.cameraPinholeOffset,84,2);fill(&constants.cameraPos,86,3);
    fill(&constants.cameraUp,89,3);fill(&constants.cameraRight,92,3);fill(&constants.cameraFwd,95,3);
    fill(&constants.cameraNear,98,1);fill(&constants.cameraFar,99,1);fill(&constants.cameraFOV,100,1);
    fill(&constants.cameraAspectRatio,101,1);fill(&constants.motionVectorsInvalidValue,102,1);fill(&constants.minRelativeLinearDepthObjectSeparation,103,1);
    constants.reset=sl::eInvalid;constants.depthInverted=sl::eTrue;constants.motionVectors3D=sl::eFalse;
    SrTraceRecord c;SnapshotSrConstants(c,&constants,&token,&viewport);const auto copied=Constants(c);
    CHECK(c.flags&SrConstantsKnown);CHECK(c.constantsVersion==2 && c.payloadKind==1);
    for(unsigned i=0;i<104;++i){CHECK(copied.floatBits[i]==expected[i]);CHECK(copied.valuePresent[i/32]&(1u<<(i%32)));}
    CHECK(copied.flags[0]==1 && copied.flags[2]==0 && copied.flags[3]==2 && copied.flagPresent==127);
    constants.clipToPrevClip[3].x=0;token.value=99;viewport=sl::ViewportHandle(77);
    CHECK(Constants(c).floatBits==expected && c.slFrameToken==0 && c.slViewport==0);
    constants.structVersion=1;SrTraceRecord v1;SnapshotSrConstants(v1,&constants,&token,&viewport);
    CHECK(!(Constants(v1).valuePresent[3]&(1u<<7)) && Constants(v1).floatBits[103]==0);
    SrTraceRecord bad;SnapshotSrConstants(bad,reinterpret_cast<const sl::Constants*>(1),&token,&viewport);
    CHECK(!(bad.flags&SrConstantsKnown) && (bad.flags&SrArgumentsMalformed));

    Microsoft::WRL::ComPtr<CpuDevice> device;device.Attach(new CpuDevice);
    Microsoft::WRL::ComPtr<CpuResource> resource;resource.Attach(new CpuResource(device.Get()));
    resource->descriptor.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;resource->descriptor.Width=2032;resource->descriptor.Height=1984;
    resource->descriptor.Format=DXGI_FORMAT_R16G16_FLOAT;resource->descriptor.DepthOrArraySize=2;resource->descriptor.SampleDesc.Count=1;
    sl::Resource srResource(sl::ResourceType::eTex2d,resource.Get(),64);sl::Extent extent{};
    extent.left=2;extent.top=3;extent.width=1000;extent.height=900;
    sl::ResourceTag tags(&srResource,sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&extent);
    const auto refs=resource->refs.load();SrTraceRecord t;SnapshotSrTags(t,&viewport,&tags,1,reinterpret_cast<void*>(0x2345));const auto tag=Tag(t,0);
    CHECK(tag.flags==(SrTagKnown|SrResourceKnown|SrDescriptorKnown));CHECK(tag.width==1000 && tag.height==900 && tag.left==2 && tag.top==3);
    CHECK(tag.actualWidth==2032 && tag.actualHeight==1984 && tag.depthOrArraySize==2 && tag.format==uint32_t(DXGI_FORMAT_R16G16_FLOAT));
    CHECK(tag.native==reinterpret_cast<uintptr_t>(resource.Get()) && tag.state==64 && tag.type==sl::kBufferTypeMotionVectors);
    CHECK(resource->refs.load()==refs && resource->unexpected==0);CHECK(!(t.flags&SrResultKnown) && !(t.flags&SrTokenKnown));
    tags.resource=nullptr;SrTraceRecord unbind;SnapshotSrTags(unbind,&viewport,&tags,1,nullptr);CHECK(Tag(unbind,0).flags==SrTagKnown);
    SrTraceRecord badTags;SnapshotSrTags(badTags,&viewport,reinterpret_cast<void*>(1),16,nullptr);CHECK(badTags.tagMalformedCount==16);
    SrTraceRecord truncated;SnapshotSrTags(truncated,&viewport,nullptr,1000,nullptr);
    CHECK(truncated.capturedCount==16 && truncated.tagMalformedCount==16 && (truncated.flags&SrTagsTruncated));
    SrTraceRecord badView;SnapshotSrTags(badView,reinterpret_cast<void*>(1),nullptr,0,nullptr);
    CHECK(badView.viewportStatus==uint32_t(SrViewportStatus::Malformed));

    auto ring=std::make_unique<SrIdentityTraceStorage>();CHECK(ring->bytes==395368 && ring->slotsOffset==104 && ring->qpcFrequency>0);
    c.nativeFrame=77;c.sourceId=1001;c.physicalEye=1;c.flags|=SrNativeFrameKnown|SrSourceKnown|SrPhysicalEyeKnown;
    CHECK(ring->Record(c));CHECK(ring->Record(t));CHECK(ring->Record(value));
    if(argc>1){std::ofstream output(argv[1],std::ios::binary);output.write(reinterpret_cast<const char*>(ring.get()),sizeof(*ring));CHECK(bool(output));}
    for(unsigned i=4;i<=600;++i){value.sourceId=i;CHECK(ring->Record(value));}
    CHECK(ring->published==600 && ring->writeEpoch==1200);
    for(uint64_t seq=345;seq<=600;++seq){const auto& slot=ring->slots[(seq-1)%256];CHECK(slot.sequence==seq && slot.record.sourceId==int64_t(seq));}
    ring->writer=1;CHECK(!ring->Record(value));CHECK(ring->dropped==1 && ring->published==600);ring->writer=0;
    SrTraceRecord invalidKind;CHECK(!ring->Record(invalidKind));CHECK(ring->dropped==2);
    std::vector<std::thread> threads;for(unsigned i=0;i<4;++i)threads.emplace_back([&]{for(unsigned n=0;n<1000;++n)ring->Record(value);});
    for(auto& thread:threads)thread.join();CHECK(ring->attempted==4602 && ring->published+ring->dropped==4602);
    CHECK(!(ring->writeEpoch.load()&1) && ring->writer==0);const auto end=ring->published.load();
    for(uint64_t seq=end-255;seq<=end;++seq)CHECK(ring->slots[(seq-1)%256].sequence==seq);
    ring->published=UINT64_MAX;CHECK(!ring->Record(value));CHECK(ring->writer==0);
    std::printf("SR identity CPU: %u checks, %u failures; GPU/game not run\n",checks,failures);return failures?1:0;
}

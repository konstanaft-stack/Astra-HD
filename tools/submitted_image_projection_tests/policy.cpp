#include <array>
#include <optional>
#include <cstdint>
#include <mutex>
#include <cassert>
#include <cstdio>
#include <memory>
#include <algorithm>
struct ID3D12CommandQueue{};
namespace cvr::camera {
struct PoseIdentity {uint64_t poseId{},writeId{},clockGeneration{};struct{uint64_t originSerial{};bool valid=true;}head;uint32_t view{},renderFrameId{};bool hasRenderFrameId=true;int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};explicit operator bool()const{return poseId&&writeId&&head.valid;}};
struct ImagePoseIdentity {uintptr_t resource{},queue{};uint64_t generation{};PoseIdentity pose;explicit operator bool()const{return generation!=0;}};
inline PoseIdentity current;inline PoseIdentity CurrentNodePoseIdentity(){return current;}
}
namespace cvr::framegen {
struct CameraConstants {uint64_t version=17;std::array<float,16> values{};friend bool operator==(const CameraConstants&,const CameraConstants&)=default;};
struct CameraData {CameraConstants constants;bool reset=true;};
struct FrameProjectionMetadata {CameraConstants constants;ID3D12CommandQueue* queue{};uint64_t poseId{},poseWriteId{},origin{},clockGeneration{};int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};uint32_t frameId{},view{},srInputWidth{},srInputHeight{};bool srInputKnown{},reset{};};
enum class SubmittedProjectionReason:uint32_t {Ready,ImageUnknown,IdentityUnknown,CameraMissing,IdentityMismatch};
struct Constants {CameraData camera{};uint32_t frame{};bool valid{};uint64_t pose{},origin{};cvr::camera::PoseIdentity identity{};};
ID3D12CommandQueue* m_d3dQueue{};std::mutex mutex;std::array<Constants,8> cameras[2];
struct GuideData {bool submitted=true;uint32_t frameId{};uint64_t poseId{},origin{};ID3D12CommandQueue* queue{};};struct Record{bool invalid{};GuideData data;};std::array<std::shared_ptr<Record>,4> pools[2];uint64_t projectionMisses[2]{},unrelatedTagRecords{};template<class... T>void Log(const char*,T...){} FrameProjectionMetadata SnapshotFrameProjection(const GuideData&){return {};}
bool CaptureProjectionEnabled(){return true;}bool NativeCamera(void* p,CameraData* out){*out=*static_cast<CameraData*>(p);return true;}
#include "SubmittedProjectionProduction.hpp"
}
using namespace cvr::framegen;
using cvr::camera::ImagePoseIdentity;
int main(){
 ID3D12CommandQueue queue;m_d3dQueue=&queue;ImagePoseIdentity image;image.resource=0x123;image.queue=reinterpret_cast<uintptr_t>(&queue);image.generation=11;
 auto& p=image.pose;p.poseId=41;p.writeId=42;p.clockGeneration=3;p.head.originSerial=7;p.view=2;p.renderFrameId=17649;p.cameraApplicationQpc=101;p.contentTimeNs=102;p.posePredictionNs=103;
 CameraData data;for(unsigned i=0;i<16;++i)data.constants.values[i]=float(i)+.125f;
 cvr::camera::current=p;RecordNativeCamera(&data,2);
 for(unsigned i=0;i<4;++i){pools[1][i]=std::make_shared<Record>();pools[1][i]->data.frameId=17645+i;}
 assert(!AcquireFrameProjection(2,17649,p.poseId,p.head.originSerial,&queue));
 SubmittedProjectionReason reason;auto out=TestCapture(image,&reason);
 assert(out&&reason==SubmittedProjectionReason::Ready);assert(out->constants==data.constants&&out->queue==&queue&&out->reset&&!out->srInputKnown&&!out->srInputWidth&&!out->srInputHeight);
 assert(out->poseId==p.poseId&&out->poseWriteId==p.writeId&&out->origin==p.head.originSerial&&out->clockGeneration==p.clockGeneration&&out->frameId==p.renderFrameId&&out->view==p.view&&out->cameraApplicationQpc==p.cameraApplicationQpc&&out->contentTimeNs==p.contentTimeNs&&out->posePredictionNs==p.posePredictionNs);
 unsigned checks=0;auto reject=[&](auto mutate){auto bad=image;mutate(bad);assert(!TestCapture(bad,&reason));++checks;};
 reject([](auto& x){x.generation=0;});reject([](auto& x){x.resource=0;});reject([](auto& x){x.queue=0;});reject([](auto& x){x.queue+=8;});
 reject([](auto& x){x.pose.poseId++;});reject([](auto& x){x.pose.writeId++;});reject([](auto& x){x.pose.clockGeneration++;});reject([](auto& x){x.pose.head.originSerial++;});reject([](auto& x){x.pose.view=1;});reject([](auto& x){x.pose.renderFrameId++;});reject([](auto& x){x.pose.cameraApplicationQpc++;});reject([](auto& x){x.pose.contentTimeNs++;});reject([](auto& x){x.pose.posePredictionNs++;});
 reject([](auto& x){x.pose.hasRenderFrameId=false;});reject([](auto& x){x.pose.head.valid=false;});reject([](auto& x){x.pose.writeId=0;});reject([](auto& x){x.pose.clockGeneration=0;});reject([](auto& x){x.pose.cameraApplicationQpc=0;});reject([](auto& x){x.pose.contentTimeNs=0;});reject([](auto& x){x.pose.posePredictionNs=0;});
 auto next=p;next.renderFrameId+=8;cvr::camera::current=next;RecordNativeCamera(&data,2);assert(!TestCapture(image,&reason));++checks;
 std::printf("PASS actual RecordNativeCamera/acquisition/capture statement: exact constants + full identity, %u mutation/unknown/overwrite rejects; CPU only\n",checks);
}

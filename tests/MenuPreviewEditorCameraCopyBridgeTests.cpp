#include "MenuPreview/EditorCameraTransport.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
using namespace cvr::menu_preview;
using Fingerprint=cvr::camera::PoseFingerprint;
void require(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
std::shared_ptr<const CameraApplication> app(uint32_t eye,uint64_t sequence){
    auto result=std::make_shared<CameraApplication>();result->metadata.screen=2;result->metadata.screenEpoch=1;result->metadata.setupID=6;result->metadata.componentIdentity=0x10000;result->metadata.virtualCameraNameHash=100;
    result->source={uintptr_t(0x10000+eye*0x1000),100+eye,eye,NativePreviewMode::NativeEditorDual,9};result->frame={{1,6,0x10000,100},42,eye,sequence,true};
    float q[4]{0,0,0,1};int32_t p[3]{eye?40:-40,30,50};std::memcpy(result->eyeDescriptor.data(),q,16);std::memcpy(result->eyeDescriptor.data()+16,p,12);return result;
}
struct NativeMemory {
    std::unordered_map<uintptr_t,Fingerprint> values;
    bool Read(uintptr_t address,Fingerprint& output)const{const auto found=values.find(address);if(found==values.end())return false;output=found->second;return true;}
    bool Copy(uintptr_t source,uintptr_t destination){Fingerprint value;if(!Read(source,value))return false;values[destination]=value;return true;}
};
template<size_t Capacity>
bool copy(EditorCameraTransport& transport,EditorCameraCopyTicketStore<Capacity>& tickets,NativeMemory& memory,uintptr_t source,uintptr_t destination){
    Fingerprint actual;const auto before=memory.Read(source,actual)?transport.Capture(source,actual):EditorCameraTransport::Receipt{};
    const auto ticket=tickets.Begin(before);if(!memory.Copy(source,destination))return false;
    return tickets.Complete(ticket,transport,destination,[&](uintptr_t address,Fingerprint& out){return memory.Read(address,out);});
}
void realStageSequence(){
    EditorCameraTransport transport;EditorCameraCopyTicketStore<8> tickets;NativeMemory memory;const auto left=app(0,101),right=app(1,102);
    constexpr uintptr_t earlyStack=0x100,packetL=0x200,componentL=0x5d0,contextL=0x1e20,packetR=0x300,componentR=0x15d0,contextR=0x2e20;
    const auto lf=EditorSetupFingerprint(left->eyeDescriptor),rf=EditorSetupFingerprint(right->eyeDescriptor);
    memory.values[earlyStack]=lf;require(transport.Publish(earlyStack,lf,left),"early left source published");
    require(copy(transport,tickets,memory,earlyStack,packetL),"AC3E71 built camera to packet+330 carries left lineage");
    require(copy(transport,tickets,memory,packetL,componentL),"36F9FF packet to component+5D0 carries left lineage");
    memory.values[earlyStack]=rf;require(transport.Publish(earlyStack,rf,right),"right callback reuses the exact early stack address with a new generation");
    require(copy(transport,tickets,memory,earlyStack,packetR)&&copy(transport,tickets,memory,packetR,componentR),"right uses independent packet/component destinations");
    require(copy(transport,tickets,memory,componentL,contextL)&&copy(transport,tickets,memory,componentR,contextR),"36FD7C registration reaches both context+1E20 sources");
    require(transport.Capture(contextL,lf).payload==left&&transport.Capture(contextR,rf).payload==right,"same-frame L/R immutable applications survive early stack reuse through all three real stages");
    require(tickets.Active()==0,"successful native copies leave no pending ticket");std::puts("PASS three-stage native copy chain and same-stack L/R generation reuse");
}
void rejectionAndLegacy(){
    EditorCameraTransport transport;EditorCameraCopyTicketStore<2> tickets;NativeMemory memory;const auto left=app(0,1),newLeft=app(0,3);const auto fp=EditorSetupFingerprint(left->eyeDescriptor);memory.values[100]=fp;memory.values[200]=fp;
    require(transport.Publish(100,fp,left),"left publication");const auto before=transport.Capture(100,fp);const auto stale=tickets.Begin(before);require(stale!=0,"stale test begins");
    require(transport.Publish(100,fp,newLeft),"identical source bytes can have a newer publication generation");
    const auto read=[&](uintptr_t address,Fingerprint& out){return memory.Read(address,out);};
    require(!tickets.Complete(stale,transport,200,read)&&!transport.Capture(200,fp)&&tickets.Active()==0,"generation replacement rejects a stale ticket and invalidates destination");
    require(tickets.Begin({})==0,"unknown source does not allocate a ticket");
    const auto valid=tickets.Begin(transport.Capture(100,fp));require(valid>stale,"new tickets never reuse stale scalar identities");
    require(!tickets.Complete(valid+99,transport,200,read)&&tickets.Active()==1,"wrong ticket rejects without consuming a live unrelated ticket");
    require(tickets.Complete(valid,transport,200,read)&&!tickets.Complete(valid,transport,200,read)&&!transport.Capture(200,fp),"a ticket completes once; repeated/wrong completion cannot keep old destination ancestry");
    const auto missingDestination=tickets.Begin(transport.Capture(100,fp));require(!tickets.Complete(missingDestination,transport,999,read)&&tickets.Active()==0,"unreadable native source/destination fails closed and consumes its ticket");
    cvr::camera::PoseAddressLedger<uint32_t> legacy;legacy.Publish(100,fp,77);const auto legacyBefore=legacy.Capture(100,fp);
    const auto editor=tickets.Begin(transport.Capture(100,fp));require(memory.Copy(100,200)&&tickets.Complete(editor,transport,200,read),"editor bracket succeeds around the native copy");
    require(legacy.Transfer(legacyBefore,memory.values[100],200,memory.values[200])&&legacy.Capture(200,fp).payload==77,"existing legacy copy/receipt transfer remains independent and succeeds after the editor completion");
    std::puts("PASS stale/unknown/wrong/consumed tickets, unreadable data and independent legacy transfer");
}
void boundedAndOverflow(){
    EditorCameraTransport transport;NativeMemory memory;const auto left=app(0,1);const auto fp=EditorSetupFingerprint(left->eyeDescriptor);memory.values[100]=memory.values[200]=fp;transport.Publish(100,fp,left);const auto before=transport.Capture(100,fp);
    EditorCameraCopyTicketStore<2> bounded;const auto a=bounded.Begin(before),b=bounded.Begin(before);require(a&&b&&a!=b&&bounded.Begin(before)==0&&bounded.Active()==2,"bounded nested copies reject overflow without replacing live tickets");
    const auto read=[&](uintptr_t address,Fingerprint& out){return memory.Read(address,out);};
    require(bounded.Complete(b,transport,200,read)&&bounded.Complete(a,transport,200,read)&&bounded.Active()==0,"nested copies can complete in reverse order");
    const auto c=bounded.Begin(before);require(c>b&&bounded.Complete(c,transport,200,read),"released capacity admits a new distinct ticket");
    EditorCameraCopyTicketStore<1> exhausted(std::numeric_limits<uint64_t>::max()-1);const auto last=exhausted.Begin(before);
    require(last==std::numeric_limits<uint64_t>::max()&&exhausted.Complete(last,transport,200,read)&&exhausted.Begin(before)==0,"ticket counter exhaustion never wraps to zero or recycles a scalar");
    std::puts("PASS bounded capacity, nested order and nonwrapping ticket exhaustion");
}
int main(){realStageSequence();rejectionAndLegacy();boundedAndOverflow();std::puts("PASS editor camera copy bridge CPU contract; no native/game execution");}

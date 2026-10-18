#include "Runtimes/EditorNativeUiPacketPolicy.hpp"
#include "Runtimes/CapturePoolPolicy.hpp"
#include <cstdio>
#include <optional>
#include <stdexcept>
using namespace cvr::menu_preview;
namespace cvr::menu_preview {bool EditorFreshnessEnabled()noexcept{return false;}void NoteEditorPublication(const CameraApplication*,const CameraApplication*,bool,bool)noexcept{}}
struct PinOwner{cvr::capture::BufferLeases* leases;std::array<uintptr_t,2> resources;bool* lockHeld{};bool* releasedUnderLock{};~PinOwner(){if(lockHeld&&*lockHeld)*releasedUnderLock=true;for(auto r:resources)leases->Unpin(r);}};
static std::shared_ptr<PinOwner> Hold(cvr::capture::BufferLeases& leases,std::array<uintptr_t,2> resources,bool* held=nullptr,bool* under=nullptr){for(auto r:resources)leases.Pin(r);return std::shared_ptr<PinOwner>(new PinOwner{&leases,resources,held,under});}
struct Packet{uint64_t rawSceneSerial[2]{};std::shared_ptr<PinOwner> lease;};
struct State{std::shared_ptr<const CameraApplication> application;std::optional<Packet> packet;uint64_t presentSerial{};};
static void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static void Run(bool keepActualBReader){
 cvr::capture::BufferLeases leases;const std::array<uintptr_t,5> pool{100,200,300,400,500};auto acceptedA=Hold(leases,{100,200});
 bool lockHeld=false,releasedUnderLock=false;auto app=std::make_shared<CameraApplication>();
 auto manager=std::make_shared<State>();manager->application=app;manager->presentSerial=42;manager->packet=Packet{{10,11},Hold(leases,{300,400},&lockHeld,&releasedUnderLock)};
 auto localSnapshot=manager;std::shared_ptr<PinOwner> actualBReader=keepActualBReader?manager->packet->lease:nullptr;
 Check(!cvr::capture::ReserveMainWrite(pool,500,0,leases),"five-cell fixture must reproduce A2+B2+excluded-main starvation");
 auto exact=cvr::xr::EditorNativeUiStateWithoutSupersededPacket(manager,10,11);
 Check(exact==manager&&manager->packet.has_value(),"exact raw-pair hit must retain cached packet");exact.reset();
 std::shared_ptr<State> retired;
 {lockHeld=true;auto replacement=cvr::xr::EditorNativeUiStateWithoutSupersededPacket(manager,12,13);
  retired=std::move(manager);manager=std::move(replacement);lockHeld=false;}
 Check(manager&&manager->application==app&&manager->presentSerial==42&&!manager->packet,"detach lost actual UI provenance or retained stale B packet");
 // A manager-only detach is insufficient: this exact acquisition snapshot also
 // owns B and must retire outside the same lock before C reservations.
 retired.reset();Check(!cvr::capture::ReserveMainWrite(pool,500,0,leases),"fixture no longer detects an old local state retaining B");
 localSnapshot.reset();Check(!releasedUnderLock,"publisher lease retired under present lock");
 Check(leases.Readers(100)==1&&leases.Readers(200)==1,"superseded cache detach released accepted display A");
 if(keepActualBReader){Check(!cvr::capture::ReserveMainWrite(pool,500,0,leases),"actual B consumer lost its independent read lease");actualBReader.reset();}
 const auto left=cvr::capture::ReserveMainWrite(pool,500,0,leases);Check(left&&pool[*left]==300,"C left did not recover the retired cache cell");
 const auto right=cvr::capture::ReserveMainWrite(pool,500,*left,leases);Check(right&&pool[*right]==400&&*left!=*right,"C right did not recover a second distinct cell");
 Check(leases.Readers(100)==1&&leases.Readers(200)==1,"new C writes replaced accepted A readers");leases.EndWrite(300);leases.EndWrite(400);
}
int main(){try{Run(false);Run(true);std::puts("PASS five-cell Aaccepted+Bcached+newC recovery; exact-cache reuse, local+publisher retirement outside lock, accepted and actual-reader preservation");return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
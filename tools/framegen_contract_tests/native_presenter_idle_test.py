"""Exercise production retirement and the actual XR idle maintenance call site."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
presenter = (root / 'src/Framegen/NativeFgPresenter.cpp').read_text(encoding='utf-8-sig')
loop = (root / 'src/Runtimes/OpenXRFrameLoop.cpp').read_text(encoding='utf-8-sig')

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

retired = function(presenter, 'bool Retired(')
collect = function(presenter, 'void Collect()')
start = loop.index('if(!aerActive&&!pairedWorld)m_gameFgPresenter.Reset();')
end = loop.index('cvr::framegen::AerRealSelection aerSelection;', start)
maintenance = loop[start:end]
# Include a new public method's real body when present; no synthetic fallback
# is invoked by the old loop, making the old paired/FG-off path fail below.
public_collect = ''
if 'void NativeFgPresenter::Collect()' in presenter:
    public_collect = function(presenter, 'void NativeFgPresenter::Collect()').replace('NativeFgPresenter::', '')

fixture = r'''
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
struct Fence { uint64_t completed{}; uint64_t GetCompletedValue()const{return completed;} };
struct NativeSelectionLease {
    bool submissionReported{},copySubmitted{},signalSucceeded{};
    std::shared_ptr<Fence> consumerFence;
    uint64_t consumerValue{};
};
@@RETIRED@@
struct Impl {
    std::array<std::shared_ptr<NativeSelectionLease>,8> consumers;
    @@COLLECT@@
};
struct Presenter {
    std::unique_ptr<Impl> impl=std::make_unique<Impl>();
    unsigned resets{};
    void Reset(){++resets;impl->Collect();}
    @@PUBLIC@@
};
void Tick(Presenter& m_gameFgPresenter,bool aerActive,bool pairedWorld){@@MAINTENANCE@@}
void require(bool pass,const char* why){if(!pass){std::cerr<<why<<'\n';std::exit(1);}}
int main(){
    Presenter p;
    auto fence=std::make_shared<Fence>();
    auto& slots=p.impl->consumers;
    slots[0]=std::make_shared<NativeSelectionLease>(NativeSelectionLease{true,true,true,fence,9});
    std::weak_ptr<NativeSelectionLease> pending=slots[0];
    Tick(p,false,true); // paired world continues but FG is no longer selected
    require(!pending.expired(),"must retain in-flight GPU consumer");
    fence->completed=9;
    Tick(p,false,true);
    require(pending.expired(),"paired FG-off must retire completed GPU consumer without another generated selection");
    require(p.resets==0,"collection must not reset active paired history");
    slots[1]=std::make_shared<NativeSelectionLease>(); // never reported
    slots[2]=std::make_shared<NativeSelectionLease>(NativeSelectionLease{true,true,false,fence,9});
    slots[3]=std::make_shared<NativeSelectionLease>(NativeSelectionLease{true,true,true,{},9});
    slots[4]=std::make_shared<NativeSelectionLease>(NativeSelectionLease{true,true,true,fence,0});
    slots[5]=std::make_shared<NativeSelectionLease>(NativeSelectionLease{true,false,false,{},0});
    fence->completed=UINT64_MAX;
    slots[6]=std::make_shared<NativeSelectionLease>(NativeSelectionLease{true,true,true,fence,9});
    Tick(p,true,false);
    require(slots[1]&&slots[2]&&slots[3]&&slots[4]&&slots[6],"unknown GPU completion must retain every lease");
    require(!slots[5],"reported selection with no copy can retire");
    require(p.resets==0,"AER collection must preserve history");
    fence->completed=10;
    Tick(p,false,false);
    require(!slots[6]&&p.resets==1,"menu reset and completion collection must remain compatible");
    std::cout<<"PASS: idle paired/AER consumer retirement; pending, failed, missing and removed-device fences retained\n";
}
'''
for key, value in {'RETIRED':retired, 'COLLECT':collect, 'PUBLIC':public_collect, 'MAINTENANCE':maintenance}.items():
    fixture = fixture.replace('@@'+key+'@@', value)
out = Path(sys.argv[1]); out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(fixture, encoding='utf-8')

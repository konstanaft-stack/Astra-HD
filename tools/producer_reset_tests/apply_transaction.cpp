#include "Render/NativeSettingsTransaction.hpp"
#include "Overlay/PpdNativeApply.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
using namespace cvr::gpu;
using namespace cvr::ppd;
void Check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
struct Writer {NativeSettingsTransaction::Pass* pass{};};
bool Admit(const NativeApplyStatus& status,void* p) noexcept {
    auto& pass=*static_cast<Writer*>(p)->pass;
    return pass.plan.revision==status.revision && pass.plan.width==status.requested.width &&
        pass.plan.height==status.requested.height && pass.Arm();
}
int main() try {
    ProducerResetGate gate;NativeSettingsTransaction tx;
    std::array<uint32_t,128> s{};
    s[0x18/4]=s[0x84/4]=976;s[0x1c/4]=s[0x88/4]=1040;
    const auto key=reinterpret_cast<uintptr_t>(s.data());
    const auto rev=RequestNativeSize(1104,1040); // width-only: EAX was loaded before hook stores.
    NativeSettingsTransaction::Plan plan{key,rev,1104,1040};
    auto reserve=[&]{return gate.ReserveFor(5ms,[]{return true;});};
    bool helper=false;
    auto first=tx.Run(plan,true,false,reserve,[&](auto& pass){
        const auto cachedWidth=s[0x84/4];Writer writer{&pass};
        Check(ProcessNativeSizeAtSettingsHook(key,true,Admit,&writer),"lost native owner");
        if(s[0x18/4]!=cachedWidth||s[0x1c/4]!=s[0x88/4])helper=true;
    });
    Check(first.ran&&!helper&&first.pending&&!gate.TryEnter(),"pending width exposed stale CET resources between native apply calls");
    bool foreignRan=false;
    auto foreign=tx.Run({key+8,rev,1104,1040},true,true,reserve,[&](auto&){foreignRan=true;});
    Check(!foreign.ran&&!foreignRan&&!gate.TryEnter(),"foreign settings stole pending reset");
    auto second=tx.Run(plan,true,true,reserve,[&](auto& pass){
        Check(!gate.TryEnter(),"producer admitted during real resize");
        s[0x18/4]=s[0x84/4];s[0x1c/4]=s[0x88/4];
        pass.NativeResizeCompleted();helper=true;
    });
    Check(second.ran&&!second.pending&&helper&&bool(gate.TryEnter())&&gate.Epoch()==1,"real helper completion failed to reopen CET");
    Check(CancelNativeSize(rev),"cleanup failed");
    const auto before=s;
    bool ran=false;
    auto denied=tx.Run(plan,true,false,[&]{return gate.ReserveFor(5ms,[]{return false;});},[&](auto&){ran=true;});
    Check(!denied.ran&&!ran&&s==before,"fence refusal changed native state");
    const auto old=RequestNativeSize(1200,1300);
    auto replaced=tx.Run({key,old,1200,1300},true,false,reserve,[&](auto& pass){
        RequestNativeSize(1300,1400);Writer writer{&pass};
        ProcessNativeSizeAtSettingsHook(key,true,Admit,&writer);
    });
    Check(replaced.ran&&!replaced.pending&&s==before&&bool(gate.TryEnter()),"stale request consumed reservation or left gate closed");
    bool nestedRan=false;
    tx.Run(plan,true,true,reserve,[&](auto& pass){
        auto nested=tx.Run(plan,true,true,reserve,[&](auto&){nestedRan=true;});
        Check(!nested.ran,"recursive native apply admitted");pass.NativeResizeCompleted();
    });
    Check(!nestedRan,"recursive native apply mutated state");
    Check(CancelNativeSize(ReadNativeApplyStatus().revision),"release live PPD ownership before author case");
    helper=false;
    auto author=tx.Run({key,0,1104,1152},true,false,reserve,[&](auto& pass){
        const auto cachedWidth=s[0x84/4];
        Check(pass.Arm(),"author write unreserved");
        Check(WriteAuthorNativeSize(key,1104,1152,true),"author writer refused");
        // Actual native width was cached before the hook; height is read after it.
        if(s[0x18/4]!=cachedWidth||s[0x1c/4]!=s[0x88/4]){
            s[0x18/4]=s[0x84/4];s[0x1c/4]=s[0x88/4];
            helper=true;pass.NativeResizeCompleted();
        }
    });
    Check(author.ran&&helper&&!author.pending&&bool(gate.TryEnter()),"author height-only write erased resize predicate and stranded CET reservation");
    std::cout<<"PASS native transaction: delayed width-only helper, foreign owner, failed fence, changed request, nested apply, actual completion\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}

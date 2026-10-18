#include "Render/NativeColorPolicy.hpp"
#include "Camera/ImagePoseLedger.hpp"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace cvr::render;
static unsigned checks;
static void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
int main(){try {
    NativeColorDescription native{1456,1584,27,4,0,true};
    Check(NativeColorCandidate(native,1456,1584),"native RGBA typeless mip0 was not selected at its actual game size");
    Check(!NativeColorCandidate(native,947,1024),"desktop client size was used as native render size");
    Check(!NativeColorCandidate(native,1776,1920),"previous PPD source admitted after native size changed");
    native.width=1776;native.height=1920;
    Check(NativeColorCandidate(native,1776,1920),"new native PPD size was rejected");
    for(auto format:{28u,29u,10u}){auto other=native;other.format=format;Check(!NativeColorCandidate(other,1776,1920),"other original descriptor class entered the typeless source path");}
    auto other=native;other.mip=1;Check(!NativeColorCandidate(other,1776,1920),"nonzero RTV mip accepted");
    other=native;other.viewDimension=5;Check(!NativeColorCandidate(other,1776,1920),"array RTV accepted as native 2D");
    other=native;other.explicitView=false;Check(!NativeColorCandidate(other,1776,1920),"implicit RTV substituted for original explicit descriptor contract");
    NativeColorSequence<int> sequence;
    Check(!sequence.Bind(false,true,42),"native bind published before window transition");
    Check(sequence.Bind(true,false,99)==42,"window transition selected window instead of preceding native color");
    Check(!sequence.Bind(true,false,99),"window transition reused an already consumed selection");
    sequence.Bind(false,true,42);sequence.Bind(false,false,53);
    Check(!sequence.Bind(true,false,99),"unrelated target did not retire the previous native candidate");
    sequence.Bind(false,true,42);sequence.Reset();
    Check(!sequence.Bind(true,false,99),"command-list reset retained a native candidate");
    // Exercise the same immutable submission ledger used by production with
    // an owning color payload: submitted selection survives post-Execute Reset.
    cvr::camera::ImagePoseLedger<std::shared_ptr<int>> ledger;
    auto first=std::make_shared<int>(42);std::weak_ptr<int> lifetime=first;
    ledger.Record(10,99,first,false);first.reset();
    const std::array<uintptr_t,1> lists{10};auto batch=ledger.Prepare(7,lists);
    ledger.Reset(10);ledger.Record(10,99,std::make_shared<int>(53),false);
    ledger.Commit(7,std::move(batch),1);
    Check(*ledger.Read(99).pose==42,"post-Execute Reset relabelled old native pixels");
    Check(!lifetime.expired(),"selected native resource owner released before capture");
    ledger.ForgetResource(99);
    Check(!ledger.Read(99),"retired swapchain retained a native selection");
    batch={};Check(lifetime.expired(),"retired selection retained its resource after batch retirement");
    std::cout<<"PASS "<<checks<<" native source contract checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<"\n";return 1;}}

#include "Render/ProducerResetGate.hpp"
#include "Render/CetLifecycleDispatch.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
using cvr::gpu::ProducerResetGate;
void Check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int main()try{
    using Stop=ProducerResetGate::StopResult;
    using Reset=ProducerResetGate::ResetResult;
    ProducerResetGate gate;
    auto producer=gate.TryEnter();
    auto stopped=std::async(std::launch::async,[&]{return gate.StopFor(5ms,[]{return true;});});
    Check(stopped.get()==Stop::ProducerTimeout,"shutdown ignored active producer");
    auto fresh=std::async(std::launch::async,[&]{return bool(gate.TryEnter());});
    Check(!fresh.get(),"shutdown timeout reopened new producer admission");
    Check(bool(gate.TryEnter()),"existing producer cannot finish nested callback during shutdown");
    Check(gate.ReserveFor(1ms,[]{return true;}).status==Reset::Stopping,"resize admitted during terminal stop");
    producer.Release();
    Check(gate.StopFor(1ms,[]{return false;})==Stop::QuiescenceFailed,"unknown GPU completion accepted for deletion");
    Check(!gate.TryEnter(),"GPU failure reopened terminal admission");
    unsigned calls=0;
    Check(gate.StopFor(1ms,[&]{++calls;return true;})==Stop::Stopped,"terminal retry failed");
    Check(gate.StopFor(1ms,[&]{++calls;return true;})==Stop::Stopped&&calls==1,"completed shutdown invoked quiesce again");
    Check(!gate.TryEnter(),"terminal state admitted a producer");

    ProducerResetGate reserved;
    auto pending=reserved.ReserveFor(1ms,[]{return true;});
    Check(reserved.StopFor(1ms,[]{return true;})==Stop::Busy,"terminal stop ignored pre-write reservation");
    Check(!pending.reservation.Arm(),"pending reservation mutated game state after stop intent");
    Check(pending.reservation.Cancel(),"unused reservation could not retire on shutdown");
    Check(!reserved.TryEnter(),"reservation cancellation reopened terminal admission");
    Check(reserved.StopFor(1ms,[]{return true;})==Stop::Stopped,"cancelled reservation prevented terminal stop");

    ProducerResetGate applying;
    auto mutation=applying.ReserveFor(1ms,[]{return true;});Check(mutation.reservation.Arm(),"arm mutation");
    Check(applying.StopFor(1ms,[]{return true;})==Stop::Busy,"shutdown overtook running original resize");
    Check(mutation.reservation.Complete(),"completed resize could not retire on shutdown");
    Check(!applying.TryEnter(),"resize completion reopened after stop intent");
    Check(applying.StopFor(1ms,[]{return true;})==Stop::Stopped,"completed resize prevented terminal stop");

    ProducerResetGate reentrant;
    auto own=reentrant.TryEnter();
    Check(reentrant.StopFor(1ms,[]{return true;})==Stop::Reentrant,"shutdown waited for its own scope");
    own.Release();
    Check(reentrant.StopFor(1ms,[]{return true;})==Stop::Stopped,"reentrant shutdown could not retry after scope ended");

    ProducerResetGate preparing;
    std::promise<void> entered,release;
    auto permission=release.get_future();
    auto preparation=std::async(std::launch::async,[&]{
        return preparing.ReserveFor(1s,[&]{entered.set_value();permission.wait();return true;});
    });
    const bool enteredOnTime=entered.get_future().wait_for(1s)==std::future_status::ready;
    const auto stopDuringPreparation=preparing.StopFor(1ms,[]{return true;});
    release.set_value(); // unblock before any assertion can unwind the future
    auto prepared=preparation.get();
    Check(enteredOnTime&&stopDuringPreparation==Stop::Busy,"terminal teardown overtook in-flight resize quiescence");
    Check(prepared.status==Reset::Stopping&&!prepared.reservation,"preparation issued reservation after stop intent");
    Check(preparing.StopFor(1ms,[]{return true;})==Stop::Stopped,"retired preparation prevented terminal stop");

    ProducerResetGate poisoned;
    Check(poisoned.ResetFor(1ms,[]{return true;},[]{throw std::runtime_error("partial mutation");})==Reset::ApplyFailed,"poison fixture");
    unsigned poisonedCalls=0;
    Check(poisoned.StopFor(1ms,[&]{++poisonedCalls;return true;})==Stop::QuiescenceFailed&&poisonedCalls==0,
          "GPU idle was mistaken for safe destruction of partially mutated state");

    cvr::gpu::CetLifecycleDispatch dispatch;
    unsigned forwarded=0;
    auto present=dispatch.Present([&]()->void*{
        Check(dispatch.BeforeOriginalPresent(),"missing original dispatch");
        Check(dispatch.Resize(1ms,[]{return true;},[]()->void*{return nullptr;}).status==Reset::Completed,
              "ordinary resize blocked by full-call lifetime lease");
        // Producer recording has ended, but this original call still executes
        // via a CET-owned MinHook trampoline. Terminal teardown must not free it.
        Check(dispatch.StopFor(1ms,[]{return true;})==Stop::Reentrant,
              "shutdown freed trampoline during forwarded original Present");
        return nullptr;
    },[&]()->void*{++forwarded;return nullptr;});
    Check(present.forwarded,"original dispatch not recorded");
    Check(dispatch.StopFor(1ms,[]{return true;})==Stop::Stopped,"full-call lease did not retire");
    auto denied=dispatch.Present([&]()->void*{++forwarded;return nullptr;},[&]()->void*{++forwarded;return nullptr;});
    Check(denied.terminalDenied&&!denied.forwarded&&forwarded==0,"terminal denial called retired CET trampoline");
    std::cout<<"PASS terminal CPU/GPU refusal, nested completion, reservation retirement, no reopening and reentrant shutdown\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}

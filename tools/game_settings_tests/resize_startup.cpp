#include "Render/ResizeQuiescence.hpp"
#include "Render/NativeSettingsTransaction.hpp"
#include "Render/CetLifecycleDispatch.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace cvr::gpu;
using namespace std::chrono_literals;
static void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static bool Apply(QueueDrainResult result,bool known,uint8_t initialized,bool noReferences){
    CetLifecycleDispatch life;NativeSettingsTransaction transaction;unsigned applies=0;
    const NativeSettingsTransaction::Plan plan{0x1234,1,1920,1920};
    const auto outcome=transaction.Run(plan,true,true,[&]{
        return life.ReserveBeforeDimensions(1ms,[&]{return ResizeQuiescent(result,known,initialized,noReferences);});
    },[&](auto& pass){
        Check(!life.TryEnterReader(),"producer entered during native resize");
        ++applies;pass.NativeResizeCompleted();
    });
    Check(bool(life.TryEnterReader()),"producer admission was not restored");
    Check(outcome.ran==(applies==1),"apply outcome and callback differ");
    return outcome.ran;
}
int main(){try{
    Check(Apply(QueueDrainResult::NoQueuedWork,true,0,true),"pristine startup apply was dropped before first queue");
    Check(!Apply(QueueDrainResult::NoQueuedWork,false,0,true),"unknown renderer initialization admitted");
    Check(!Apply(QueueDrainResult::NoQueuedWork,true,1,true),"initialized renderer without queue proof admitted");
    Check(!Apply(QueueDrainResult::NoQueuedWork,true,0,false),"untracked graphics references admitted");
    for(const auto failure:{QueueDrainResult::RegistryFailed,QueueDrainResult::SignalFailed,QueueDrainResult::DeviceRemoved,QueueDrainResult::TimedOut,QueueDrainResult::Exhausted,QueueDrainResult::Failed})
        Check(!Apply(failure,true,0,true),"a failed drain was reinterpreted as pristine startup");
    Check(Apply(QueueDrainResult::Complete,true,1,false),"completed drain rejected for an initialized renderer");
    std::cout<<"PASS: pristine startup, real queues, failed drains and closed producer admission\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}

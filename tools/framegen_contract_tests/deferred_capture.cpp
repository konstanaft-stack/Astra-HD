#include "Framegen/DeferredNativeCapture.hpp"
#include <cstdio>
#include <cstdlib>
using cvr::framegen::DeferredNativeCapture;
static void Check(bool ok,const char* reason){if(!ok){std::fprintf(stderr,"FAIL: %s\n",reason);std::exit(1);}}
int main(){
    DeferredNativeCapture ticket;
    Check(!ticket.Take(17,true),"no successful evaluation cannot capture");
    ticket.Arm(17);
    Check(!ticket.Take(17,false),"unknown output state cannot capture at evaluation return");
    Check(ticket.Take(17,true),"first phase must survive until caller publishes resource state");
    Check(!ticket.Take(17,true),"same phase cannot be recorded twice");
    ticket.Arm(18);ticket.Cancel();
    Check(!ticket.Take(18,true),"overwrite or alias cancels old pixels");
    ticket.Arm(19);
    Check(!ticket.Take(20,true),"new recording cannot use old evaluation");
    Check(!ticket.Take(19,true),"recording mismatch permanently cancels ticket");
    ticket.Arm(0);Check(!ticket.Take(0,true),"unknown recording rejected");
    ticket.Arm(21);Check(ticket.Take(21,true),"immediate known output still works");
    ticket.Arm(22,7);Check(!ticket.Take(22,true,8),"another recording thread's write cancels pending capture");
    std::puts("PASS: deferred state, one capture, overwrite, recording lifetime, immediate output");
}

#include "Framegen/PairedImageContract.hpp"
#include <iostream>
#include <stdexcept>
using namespace cvr::framegen;
void Check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
int main(){try{
    PairedImageIdentity pair;pair.serial=42;pair.origin=3;pair.resourceEpoch=7;pair.producerSignalKnown=true;
    pair.eyes[0]={101,42,501,601,3,1,900,true};pair.eyes[1]={102,42,502,602,3,2,900,true};
    Check(ValidatePairedImage(pair)==PairedImageAdmission::Ready,"same-step MAIN/VRCAM pair rejected");
    auto bad=pair;bad.eyes[1].color=101;Check(ValidatePairedImage(bad)==PairedImageAdmission::SameImage,"mono source accepted as two cameras");
    bad=pair;bad.eyes[1].nativeFrame=899;Check(ValidatePairedImage(bad)==PairedImageAdmission::WrongFrame,"old second-camera frame accepted");
    bad=pair;bad.eyes[1].view=1;Check(ValidatePairedImage(bad)==PairedImageAdmission::WrongView,"two MAIN sources accepted");
    bad=pair;bad.eyes[1].origin=2;Check(ValidatePairedImage(bad)==PairedImageAdmission::WrongOrigin,"mixed tracking origins accepted");
    bad=pair;bad.eyes[1].serial=41;Check(ValidatePairedImage(bad)==PairedImageAdmission::WrongSerial,"reused color with current metadata accepted");
    bad=pair;bad.eyes[1].pose=0;Check(ValidatePairedImage(bad)==PairedImageAdmission::MissingIdentity,"missing source pose accepted");
    bad=pair;bad.producerSignalKnown=false;Check(ValidatePairedImage(bad)==PairedImageAdmission::MissingCompletion,"unscheduled producer accepted");
    std::swap(pair.eyes[0],pair.eyes[1]);Check(ValidatePairedImage(pair)==PairedImageAdmission::Ready,"valid reversed physical-eye mapping rejected");
    std::cout<<"PASS paired-camera source identity and mono/stale/origin/pose/fence negative cases\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}

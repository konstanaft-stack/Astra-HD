#include "Framegen/NativeCameraSnapshot.hpp"
#include <array>
#include <iostream>
#include <new>
#include <stdexcept>
using namespace cvr::framegen;
static void Check(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
int main() {
    try {
        alignas(sl::Constants) std::array<unsigned char,0x500> state{};
        auto* slBlock=new(state.data()+0x200) sl::Constants;
        // A prior emitted frame has different dynamic values. The producer
        // must preserve the current staging matrices, not these stale copies.
        for(auto* matrix:{&slBlock->cameraViewToClip,&slBlock->clipToCameraView,&slBlock->clipToPrevClip,&slBlock->prevClipToClip})
            for(unsigned row=0;row<4;++row)(*matrix)[row]={999,999,999,999};
        const unsigned offsets[]={0xA0,0xE0,0x20,0x60};
        for(unsigned matrix=0;matrix<4;++matrix)for(unsigned i=0;i<16;++i) {
            const float value=float(100*matrix+i+1);std::memcpy(state.data()+offsets[matrix]+i*4,&value,4);
        }
        const float nativeFov=113.7f;std::memcpy(state.data()+0x1D8,&nativeFov,4);
        CameraData scalar{};scalar.nearPlane=.02f;scalar.farPlane=16000;scalar.aspect=1.1f;
        scalar.verticalFov=1.98f;scalar.position[0]=12;scalar.cameraMotion=true;scalar.inverted=true;
        auto raw=SnapshotNativeCameraState(state.data(),scalar);
        Check(raw.version==2 && raw.valuePresent.all() && raw.flagPresent.all(),"proven native schema not retained");
        Check(raw.values[0]==1 && raw.values[15]==16 && raw.values[16]==101 && raw.values[31]==116 &&
            raw.values[48]==201 && raw.values[63]==216 && raw.values[64]==301 && raw.values[79]==316,
            "native camera used stale emitted matrices or wrong staging offsets");
        Check(raw.values[100]==nativeFov && raw.values[86]==12,"raw FOV/scalars changed");
        Check(raw.values[32]==sl::INVALID_FLOAT && raw.values[47]==sl::INVALID_FLOAT &&
            raw.values[84]==sl::INVALID_FLOAT && raw.values[102]==sl::INVALID_FLOAT && raw.values[103]==40,
            "constructor sentinels were fabricated into zero or identity");
        Check(!HasCompleteCameraConstants(raw),"physical presence conflated with numeric validity");
        Check(raw.flags==std::array<uint32_t,7>{1,1,0,0,0,0,0},"native literal flags lost");
        slBlock->structType.data1^=1;raw=SnapshotNativeCameraState(state.data(),scalar);
        Check(!raw.valuePresent[0] && raw.valuePresent.count()==19,"foreign native header admitted");
        std::cout<<"PASS native_camera\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}

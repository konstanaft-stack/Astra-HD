#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

#if __has_include("MenuPreview/RegisteredEditorCamera.hpp")
#include "MenuPreview/RegisteredEditorCamera.hpp"
using namespace cvr::menu_preview;
namespace {
void require(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
void putFloat(uint8_t* destination,float value){std::memcpy(destination,&value,4);}
float getFloat(const uint8_t* source){float value;std::memcpy(&value,source,4);return value;}
Descriptor desired(){
    Descriptor value;value.fill(0xA7);
    const float rotation[4]{.5f,.5f,.5f,.5f};const int32_t position[3]{12345,-67125,0x12345678};
    std::memcpy(value.data(),rotation,16);std::memcpy(value.data()+0x10,position,12);
    putFloat(value.data()+0x38,110.f);putFloat(value.data()+0x3C,1.3f);
    putFloat(value.data()+0x40,.03125f);putFloat(value.data()+0x44,9999.f);return value;
}
std::array<uint8_t,0x400> native(){
    std::array<uint8_t,0x400> value{};for(size_t i=0;i<value.size();++i)value[i]=uint8_t(i*37+11);
    putFloat(value.data()+0x44,1000.f);return value;
}
void selectiveOverlay(){
    auto bytes=native();const auto before=bytes;const auto descriptor=desired();
    require(OverlayRegisteredEditorCamera(bytes,descriptor),"valid registered camera overlay accepted");
    auto expected=before;
    std::memcpy(expected.data(),descriptor.data()+0x10,12);
    std::memcpy(expected.data()+0x10,descriptor.data(),16);
    std::memcpy(expected.data()+0x20,descriptor.data()+0x38,4);
    std::memcpy(expected.data()+0x28,descriptor.data()+0x3C,4);
    std::memcpy(expected.data()+0x40,descriptor.data()+0x40,4);
    require(bytes==expected,"only XYZ0, quaternion10, FOV20, aspect28 and near40 may change");
    require(getFloat(bytes.data()+0x44)==1000.f,"native far remains original, not desired descriptor far");
    for(size_t i=0;i<bytes.size();++i){
        const bool mapped=i<0x0C||(i>=0x10&&i<0x24)||(i>=0x28&&i<0x2C)||(i>=0x40&&i<0x44);
        if(!mapped)require(bytes[i]==before[i],"padding, derived/unknown fields, origin280 and refs388 remain bit-identical");
    }
    std::array<uint8_t,0x48> minimum{};putFloat(minimum.data()+0x44,1000.f);
    require(OverlayRegisteredEditorCamera(minimum,descriptor),"exact minimum native0x48 accepted without constructor");
    Descriptor alias=descriptor;auto aliasExpected=descriptor;
    std::memcpy(aliasExpected.data(),descriptor.data()+0x10,12);std::memcpy(aliasExpected.data()+0x10,descriptor.data(),16);
    std::memcpy(aliasExpected.data()+0x20,descriptor.data()+0x38,4);std::memcpy(aliasExpected.data()+0x28,descriptor.data()+0x3C,4);
    std::memcpy(aliasExpected.data()+0x40,descriptor.data()+0x40,4);
    require(OverlayRegisteredEditorCamera(alias,alias)&&alias==aliasExpected,"desired/native alias snapshots source before any selective write");
    std::puts("PASS exact selective overlay, protected bytes, original far and minimum layout");
}
void reconstruction(){
    auto bytes=native();const auto descriptor=desired();require(OverlayRegisteredEditorCamera(bytes,descriptor),"reconstruction source overlay");
    Descriptor baseline;for(size_t i=0;i<baseline.size();++i)baseline[i]=uint8_t(0xF1-i);
    Descriptor output;output.fill(0x33);const auto before=bytes;
    require(ReadRegisteredEditorDescriptor(bytes,baseline,output),"valid native local bytes reconstruct a descriptor");
    auto expected=baseline;
    std::memcpy(expected.data()+0x10,bytes.data(),12);std::memcpy(expected.data(),bytes.data()+0x10,16);
    std::memcpy(expected.data()+0x38,bytes.data()+0x20,4);std::memcpy(expected.data()+0x3C,bytes.data()+0x28,4);
    std::memcpy(expected.data()+0x40,bytes.data()+0x40,4);std::memcpy(expected.data()+0x44,bytes.data()+0x44,4);
    require(output==expected,"reconstruction maps known pose/optics including read-only far and preserves every other baseline byte");
    require(bytes==before,"reconstruction never writes native bytes");
    require(ReadRegisteredEditorDescriptor(bytes,output,output)&&output==expected,"baseline/output alias is safe");
    Descriptor aliasNative{};std::memcpy(aliasNative.data(),bytes.data(),aliasNative.size());
    require(ReadRegisteredEditorDescriptor(aliasNative,baseline,aliasNative)&&aliasNative==expected,"native/output alias is safe");
    require(ReadRegisteredEditorDescriptor(std::span<const uint8_t>(bytes).first(0x48),baseline,output)&&output==expected,"exact minimum reconstruction source accepted");
    std::puts("PASS full descriptor reconstruction with unknown baseline bytes preserved");
}
void rejectedOverlayIsAtomic(){
    const auto valid=desired();
    for(size_t offset:std::array<size_t,7>{0,4,8,12,0x38,0x3C,0x40}){
        auto invalid=valid;putFloat(invalid.data()+offset,std::numeric_limits<float>::quiet_NaN());
        auto bytes=native();const auto before=bytes;
        require(!OverlayRegisteredEditorCamera(bytes,invalid)&&bytes==before,"NaN in any written float rejects before every write");
    }
    for(const auto values:std::array<std::array<float,2>,8>{{{0x38,0},{0x38,179},{0x38,-1},{0x3C,0},{0x3C,-1},{0x40,0},{0x40,-1},{0x40,1000}}}){
        auto invalid=valid;putFloat(invalid.data()+size_t(values[0]),values[1]);auto bytes=native();const auto before=bytes;
        require(!OverlayRegisteredEditorCamera(bytes,invalid)&&bytes==before,"invalid FOV/aspect/near or near>=nativefar rejects atomically");
    }
    auto invalid=valid;const float zero[4]{};std::memcpy(invalid.data(),zero,16);auto bytes=native();const auto before=bytes;
    require(!OverlayRegisteredEditorCamera(bytes,invalid)&&bytes==before,"unnormalized quaternion rejects atomically");
    invalid=valid;putFloat(invalid.data(),2.f);
    require(!OverlayRegisteredEditorCamera(bytes,invalid)&&bytes==before,"excessive quaternion norm rejects atomically");
    invalid=valid;putFloat(invalid.data()+0x3C,std::numeric_limits<float>::infinity());
    require(!OverlayRegisteredEditorCamera(bytes,invalid)&&bytes==before,"positive infinite aspect rejects atomically");
    for(float far:std::array<float,5>{0.f,-1.f,.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        bytes=native();putFloat(bytes.data()+0x44,far);const auto original=bytes;
        require(!OverlayRegisteredEditorCamera(bytes,valid)&&bytes==original,"nonfinite/invalid native far is read-only and rejects atomically");
    }
    bytes=native();require(!OverlayRegisteredEditorCamera(std::span<uint8_t>(bytes).first(0x47),valid)&&bytes==before,"short native buffer rejects without writes");
    require(!OverlayRegisteredEditorCamera({},valid),"empty native buffer rejected");
    std::puts("PASS invalid quaternion/optics/far/size and all-or-none overlay rejection");
}
void rejectedReadPreservesOutput(){
    const auto baseline=desired();auto bytes=native();require(OverlayRegisteredEditorCamera(bytes,baseline),"invalid-read seed");
    for(size_t offset:std::array<size_t,8>{0x10,0x14,0x18,0x1C,0x20,0x28,0x40,0x44}){
        auto invalid=bytes;putFloat(invalid.data()+offset,std::numeric_limits<float>::quiet_NaN());
        Descriptor output;output.fill(0x6D);const auto before=output;
        require(!ReadRegisteredEditorDescriptor(invalid,baseline,output)&&output==before,"invalid reconstructed float leaves all output bytes unchanged");
    }
    Descriptor output;output.fill(0x6D);const auto before=output;
    require(!ReadRegisteredEditorDescriptor(std::span<const uint8_t>(bytes).first(0x47),baseline,output)&&output==before,"short reconstruction source leaves output unchanged");
    require(!ReadRegisteredEditorDescriptor({},baseline,output)&&output==before,"empty reconstruction source rejected");
    std::puts("PASS reconstruction validation and all-or-none output rejection");
}
}
int main(){selectiveOverlay();reconstruction();rejectedOverlayIsAtomic();rejectedReadPreservesOutput();std::puts("PASS registered editor camera pure byte-layout contract; no native constructor/game/GPU");}
#else
int main(){std::fputs("FAIL: missing selective registered editor camera helper\n",stderr);return 1;}
#endif

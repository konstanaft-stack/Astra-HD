#include "MenuPreview/Producer.hpp"
#include <iostream>
#include <stdexcept>
using namespace cvr::menu_preview;
void Check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
template<class T> void Write(Descriptor& d,size_t o,T v){std::memcpy(d.data()+o,&v,sizeof(v));}
int32_t Pos(const Descriptor& d,unsigned i){int32_t v;std::memcpy(&v,d.data()+0x10+i*4,4);return v;}
int main(){
    Descriptor d{};for(size_t i=0;i<d.size();++i)d[i]=uint8_t(i+1);
    for(unsigned i=0;i<4;++i)Write(d,i*4,i==3?1.f:0.f);
    for(unsigned i=0;i<3;++i)Write(d,0x10+i*4,int32_t(100000+i*200000));
    Descriptor l{},r{};Check(MakeEyeDescriptor(d,-.032f,l)&&MakeEyeDescriptor(d,.032f,r),"identity eyes");
    Check(Pos(l,0)==Pos(d,0)-4194&&Pos(r,0)==Pos(d,0)+4194,"native truncation");
    for(size_t i=0;i<d.size();++i)if(i<0x10||i>=0x1C)Check(l[i]==d[i]&&r[i]==d[i],"all optics/off-axis/scene bytes preserved");
    Check(Pos(l,1)==Pos(d,1)&&Pos(l,2)==Pos(d,2),"other positions preserved");
    const float h=std::sqrt(.5f);Write(d,8,h);Write(d,12,h);
    Check(MakeEyeDescriptor(d,.032f,r)&&std::abs(Pos(r,1)-Pos(d,1)-4194)<=1&&std::abs(Pos(r,0)-Pos(d,0))<=1,"90 degree yaw rotates right to Y");
    Write(d,8,0.f);Write(d,4,h);
    Check(MakeEyeDescriptor(d,.032f,r)&&std::abs(Pos(r,2)-Pos(d,2)+4194)<=1,"90 degree pitch rotates right to minus Z");
    Write(d,0,std::numeric_limits<float>::quiet_NaN());const auto previous=r;
    Check(!MakeEyeDescriptor(d,.032f,r)&&r==previous,"NaN rejected without output mutation");
    Write(d,0,0.f);Check(!MakeEyeDescriptor(d,.2f,r),"excess IPD rejected");
    for(unsigned i=0;i<4;++i)Write(d,i*4,i==3?1.f:0.f);
    Write(d,0x10,std::numeric_limits<int32_t>::max());Check(!MakeEyeDescriptor(d,.032f,r),"overflow rejected");
    Metadata m{};m.screen=1;m.screenEpoch=3;m.componentIdentity=100;m.virtualCameraNameHash=9;m.publishedAtMs=1000;
    Check(MatchesProducerIdentity(m,100,1500)&&!MatchesProducerIdentity(m,101,1000)&&!MatchesProducerIdentity(m,100,1501),"identity/staleness");
    int slot=1;{ScopedProducer outer(slot,2);Check(slot==2,"outer installed");{ScopedProducer nested(slot,0);Check(slot==0,"invalid nested cleared");}Check(slot==2,"nested restored");try{ScopedProducer nested(slot,3);throw 1;}catch(int){}Check(slot==2,"exception unwinds");}Check(slot==1,"outer restored");
    std::cout<<"PASS preview descriptor optics/axes/fixed-point/rejection and nested producer scope\n";
}

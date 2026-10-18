#include "Framegen/SrTemporalConstants.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <random>
#include <cstring>
using namespace cvr::framegen::sr;
static unsigned checks{};
static void Check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
static Parameters ActualFixtureParameters(){
    Parameters p;p.known=p.effectiveFix=true;p.key=31;p.ngxFeature=1;p.mvMultiplier={1,1};p.projectionGain={1,1};p.depthScale=1;
    p.depthTolerance=.01f;p.mvTolerancePixels=2;p.staticThreshold=.05f;p.staticZExp=1;p.mcDepthTolerance=.2f;return p;
}
static Sample Frame(int64_t id){
    Sample s;s.sourceId=s.matrixSourceId=id;s.featureKey=19;s.basisId=23;s.parametersKey=31;s.epochs={1,2,3,4};s.eyeOffsetsKnown=true;
    s.eyeFromReference[0].position={-.03f,0,0};s.eyeFromReference[1].position={.03f,0,0};
    s.camera.position=s.eyeFromReference[unsigned(id&1)].position;s.projection={1,1,1,1};s.nearZ=.1f;s.farZ=100;
    s.depthSize=s.motionSize=s.renderSubrect={8,8};s.outputSize={16,16};s.mvScale={8,8};s.lowResMv=true;return s;
}
static void StaticAlternatingEyes(){
    const History history{Frame(2),Frame(1),Frame(0)};const auto result=Build(history,ActualFixtureParameters());
    Check(result.status==Status::Ready,"confirmed source triple did not build constants");
    const auto& c=result.constants;
    Check(std::abs(c.transf0[3]+.06f)<1e-7f&&std::abs(c.transf_2[3]-.06f)<1e-7f,"physical alternating cameras have wrong relative translation/order");
    for(unsigned i=0;i<16;++i)if(i!=3){const float identity=i%5==0?1.f:0.f;
        Check(std::abs(c.transf0[i]-identity)<1e-7f&&std::abs(c.transf_2[i]-identity)<1e-7f,"static alternating pair changed rotation");}
    Check(c.camMoComp_2==V4{0,0,0,0},"constant reference head acquired false acceleration");
}
using D3=std::array<long double,3>;using D4=std::array<long double,4>;
static D4 HamiltonOracle(const D4&a,const D4&b){
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
static D4 QuaternionOracle(const Pose&p,bool normalize){
    D4 q{};long double n=0;for(unsigned i=0;i<4;++i){q[i]=p.orientation[i];n+=q[i]*q[i];}
    if(normalize)for(auto&v:q)v/=std::sqrt(n);return q;
}
static D3 RotateOracle(const D4&q,const D3&v){
    const D4 point{v[0],v[1],v[2],0},conjugate{-q[0],-q[1],-q[2],q[3]};
    const auto result=HamiltonOracle(HamiltonOracle(q,point),conjugate);return {result[0],result[1],result[2]};
}
static D3 RelativePointOracle(const Pose&old,const Pose&now,const D3&point,bool normalize){
    const auto newQ=QuaternionOracle(now,normalize),oldQ=QuaternionOracle(old,normalize);
    auto world=RotateOracle(newQ,point);for(unsigned i=0;i<3;++i)world[i]+=static_cast<long double>(now.position[i])-old.position[i];
    return RotateOracle(D4{-oldQ[0],-oldQ[1],-oldQ[2],oldQ[3]},world);
}
static D3 ApplyGpuRows(const M4&m,const D3&v){
    D3 result{};for(unsigned r=0;r<3;++r){result[r]=m[r*4+3];for(unsigned c=0;c<3;++c)result[r]+=m[r*4+c]*v[c];}
    const long double w=m[12]*v[0]+m[13]*v[1]+m[14]*v[2]+m[15];Check(std::abs(w-1)<1e-7L,"extra transpose corrupted homogeneous coordinate");return result;
}
static void IndependentWorldPointOracle(){
    std::mt19937 rng(20261004);std::uniform_real_distribution<float> dist(-1.f,1.f);long double maxima[2]{};
    for(unsigned precision=0;precision<2;++precision)for(unsigned test=0;test<240;++test){
        History h{Frame(2),Frame(1),Frame(0)};auto parameters=ActualFixtureParameters();parameters.debugFlags=precision?0x10000:0;
        for(auto* s:{&h.current,&*h.previous,&*h.previousPrevious}){
            V3 axis{dist(rng),dist(rng),dist(rng)};const float norm=std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
            const float halfAngle=dist(rng)*1.4f;for(unsigned i=0;i<3;++i)s->camera.orientation[i]=axis[i]/norm*std::sin(halfAngle);s->camera.orientation[3]=std::cos(halfAngle);
            for(auto&x:s->camera.position)x=dist(rng)*12.f;
            s->explicitReference=Pose{};for(auto&x:s->explicitReference->position)x=dist(rng)*3.f;
        }
        const auto result=Build(h,parameters);Check(result.status==Status::Ready,"valid world sample rejected");
        for(unsigned point=0;point<7;++point){const D3 p{dist(rng)*4,dist(rng)*4,-.2L-std::abs(dist(rng))*20};
            const auto expected0=RelativePointOracle(h.previous->camera,h.current.camera,p,!precision);
            const auto expected2=RelativePointOracle(h.previousPrevious->camera,h.previous->camera,p,!precision);
            const auto actual0=ApplyGpuRows(result.constants.transf0,p),actual2=ApplyGpuRows(result.constants.transf_2,p);
            for(unsigned i=0;i<3;++i){const auto error=std::max(std::abs(expected0[i]-actual0[i]),std::abs(expected2[i]-actual2[i]));maxima[precision]=std::max(maxima[precision],error);
                Check(error<.00004L,"matrix order/basis differs from independent quaternion world-point oracle");}
        }
        D3 acceleration{};for(unsigned i=0;i<3;++i)acceleration[i]=static_cast<long double>(h.current.explicitReference->position[i])+h.previousPrevious->explicitReference->position[i]-2.L*h.previous->explicitReference->position[i];
        const auto q=QuaternionOracle(h.previousPrevious->camera,!precision);
        const auto expected=RotateOracle(D4{-q[0],-q[1],-q[2],q[3]},acceleration);
        for(unsigned i=0;i<3;++i)Check(std::abs(expected[i]-result.constants.camMoComp_2[i])<.00001L,"camera compensation is not second difference in oldest camera basis");
    }
    std::cout<<"world_point_oracle double_max="<<double(maxima[0])<<" float_max="<<double(maxima[1])<<'\n';
}
static void TranslationAndDebugConvention(){
    History h{Frame(2),Frame(1),Frame(0)};h.current.camera.position={4,0,0};h.previous->camera.position={1,0,0};h.previousPrevious->camera.position={0,0,0};
    h.current.explicitReference=h.current.camera;h.previous->explicitReference=h.previous->camera;h.previousPrevious->explicitReference=h.previousPrevious->camera;
    for(uint32_t flags:{0u,0x10000u,0x20000u,0x30000u}){
        auto p=ActualFixtureParameters();p.debugFlags=flags;const auto result=Build(h,p);Check(result.status==Status::Ready,"confirmed precision/convention branch rejected");
        const float sign=(flags&0x20000)?-1.f:1.f;
        Check(result.constants.transf0[3]==3*sign&&result.constants.transf_2[3]==sign&&result.constants.camMoComp_2[0]==2*sign,"translation/inversion convention wrong");
    }
    for(auto* s:{&h.current,&*h.previous,&*h.previousPrevious}){s->camera.orientation={0,0,0,2};s->camera.position={0,0,0};s->explicitReference=Pose{};}
    auto p=ActualFixtureParameters();const auto normalized=Build(h,p);p.debugFlags=0x10000;const auto raw=Build(h,p);
    Check(normalized.constants.transf0[0]==1.f&&raw.constants.transf0[0]==16.f,"SR double normalization was omitted or invented in raw float debug branch");
}
static void ScalarHistoryAndProjection(){
    History h{Frame(2),Frame(1),Frame(0)};auto p=ActualFixtureParameters();
    for(auto* s:{&h.current,&*h.previous,&*h.previousPrevious}){s->depthSize={16,8};s->motionSize={16,8};s->renderSubrect={12,6};s->outputSize={24,12};s->jitteredMv=true;}
    h.current.jitterPixels={.25f,-.5f};h.previous->jitterPixels={-.25f,.125f};h.previousPrevious->jitterPixels={.5f,.25f};
    h.current.mvScale={16,8};h.previous->mvScale={8,4};h.previousPrevious->mvScale={4,2};
    h.current.projection={1,2,3,4};h.previous->projection={2,3,4,5};h.previousPrevious->projection={3,4,5,6};
    h.current.nearZ=.2f;h.previous->nearZ=.1f;h.previousPrevious->nearZ=.05f;
    h.current.farZ=200;h.previous->farZ=100;h.previousPrevious->farZ=50;
    const auto result=Build(h,p);Check(result.status==Status::Ready,"scalar history fixture rejected");const auto&c=result.constants;
    Check(c.uvJitter0==V2{.25f/16,-.5f/8}&&c.uvJitter_1==V2{-.25f/16,.125f/8}&&c.uvJitter_2==V2{.5f/16,.25f/8},"jitter order/normalization wrong");
    Check(c.mvScale0==V2{1,1}&&c.mvScale_1==V2{.5f,.5f},"previous MV was decoded with current scale");
    for(unsigned i=0;i<2;++i){Check(c.mvOffset0[i]==c.uvJitter0[i]-c.uvJitter_1[i],"current jitter offset wrong");
        Check(c.mvOffset_1[i]==c.uvJitter_1[i]-c.uvJitter_2[i],"previous jitter offset wrong");
        Check(c.mvInvOffset0[i]==c.uvJitter_2[i]-c.uvJitter0[i],"inverse combined MV targeted -1 instead of -2");}
    Check(c.motionMapSize==U2{12,6}&&c.uvMax==V2{.75f,.75f}&&c.mv2depthScale==V2{1,1},"low-res descriptor routing wrong");
    Check(c.src2OutRatio==V2{2,2}&&c.src2OutMVRatio==V2{0,0},"source/output coefficient wrong");
    Check(c.dso0==V2{-.5f/16,-.5f/8}&&c.dso3==V2{.5f/16,.5f/8},"depth sample offsets use the wrong resource size");
    Check(c.zNear0==.2f&&c.zNear_1==.1f&&c.zNear_2==.05f&&c.zFar_2==50,"near/far -2 history shifted too early");
    const float scale2x=(5.f+6.f)/.75f,scale2y=-(3.f+4.f)/.75f;
    Check(std::abs(c.xyz2uvwScale_2[0]-1.f/scale2x)<1e-7f&&std::abs(c.xyz2uvwScale_2[1]-1.f/scale2y)<1e-7f,"inverse -2 projection uses current input");
    Check(std::abs(c.xyz2uvwOffset_2[0]-5.f/scale2x)<1e-7f&&std::abs(c.xyz2uvwOffset_2[1]+3.f/scale2y)<1e-7f,"inverse -2 offset sign wrong");
    h.current.lowResMv=false;const auto high=Build(h,p).constants;
    Check(high.motionMapSize==U2{16,8}&&high.uvMax==V2{1,1}&&high.mv2depthScale==V2{.75f,.75f},"full-res MV descriptor routing wrong");
    Check(high.uvJitter0==V2{.25f/12,-.5f/6},"full-res jitter used MV dimensions instead of render subrect");
}
static void GuardsAndNoFabricatedHistory(){
    const History valid{Frame(2),Frame(1),Frame(0)};const auto parameters=ActualFixtureParameters();
    for(unsigned mutation=0;mutation<15;++mutation){auto h=valid;auto p=parameters;Status expected=Status::HistoryMismatch;
        switch(mutation){case 0:p.effectiveFix=false;expected=Status::Disabled;break;case 1:p.known=false;expected=Status::UnknownParameters;break;
        case 2:p.ngxFeature=11;expected=Status::UnsupportedFeature;break;case 3:h.previousPrevious.reset();expected=Status::NeedHistory;break;
        case 4:h.previous->sourceId=0;break;case 5:++h.previous->epochs.gameWorld;break;case 6:++h.previous->basisId;break;case 7:++h.previous->featureKey;break;
        case 8:++h.previous->parametersKey;break;case 9:h.current.beginsNewHistory=true;break;case 10:h.previous->beginsNewHistory=true;break;
        case 11:h.current.camera.orientation={0,0,0,0};expected=Status::InvalidSample;break;case 12:h.current.motionSize[0]=0;expected=Status::InvalidSample;break;
        case 13:p.depthScale=0;expected=Status::InvalidParameters;break;case 14:h.current.camera.position[0]=std::numeric_limits<float>::quiet_NaN();expected=Status::InvalidSample;break;}
        Check(Build(h,p).status==expected,"unsupported/missing input silently acquired valid constants");
    }
    auto h=valid;h.previousPrevious->beginsNewHistory=true;Check(Build(h,parameters).status==Status::Ready,"first valid endpoint after reset cannot seed a later complete triple");
}
static Pose ComposePoseOracle(const Pose&a,const Pose&b){
    const auto qa=QuaternionOracle(a,true),qb=QuaternionOracle(b,true),q=HamiltonOracle(qa,qb);
    const auto p=RotateOracle(qa,D3{b.position[0],b.position[1],b.position[2]});Pose out;
    for(unsigned i=0;i<4;++i)out.orientation[i]=float(q[i]);for(unsigned i=0;i<3;++i)out.position[i]=float(p[i]+a.position[i]);return out;
}
static Pose InversePoseOracle(const Pose&p){
    const auto q=QuaternionOracle(p,true);const D4 inverse{-q[0],-q[1],-q[2],q[3]};
    const auto xyz=RotateOracle(inverse,D3{-p.position[0],-p.position[1],-p.position[2]});Pose out;
    for(unsigned i=0;i<4;++i)out.orientation[i]=float(inverse[i]);for(unsigned i=0;i<3;++i)out.position[i]=float(xyz[i]);return out;
}
static void ComparePoseConstants(const Constants&a,const Constants&b,float tolerance,const char* reason){
    for(unsigned i=0;i<16;++i){Check(std::abs(a.transf0[i]-b.transf0[i])<tolerance,reason);Check(std::abs(a.transf_2[i]-b.transf_2[i])<tolerance,reason);}
    for(unsigned i=0;i<4;++i)Check(std::abs(a.camMoComp_2[i]-b.camMoComp_2[i])<tolerance,reason);
}
static void ActualEyeReferenceAndParity(){
    History explicitHistory{Frame(2),Frame(1),Frame(0)},implicit=explicitHistory;
    const V4 q{.2f,-.3f,.1f,.92736185f};unsigned index=0;
    for(auto* s:{&explicitHistory.current,&*explicitHistory.previous,&*explicitHistory.previousPrevious}){
        Pose head;head.orientation=q;head.position={float(4-index*index),float(index),-2};++index;
        s->eyeFromReference[0].orientation={0,.02f,0,.99979998f};s->eyeFromReference[1].orientation={0,-.02f,0,.99979998f};
        s->camera=ComposePoseOracle(head,s->eyeFromReference[unsigned(s->sourceId&1)]);s->explicitReference=head;
    }
    implicit=explicitHistory;implicit.current.explicitReference.reset();implicit.previous->explicitReference.reset();implicit.previousPrevious->explicitReference.reset();
    const auto parameters=ActualFixtureParameters();const auto a=Build(explicitHistory,parameters),b=Build(implicit,parameters);
    Check(a.status==Status::Ready&&b.status==Status::Ready,"actual reference derivation rejected");
    ComparePoseConstants(a.constants,b.constants,.000004f,"native eye pose did not recover supplied reference head");
    for(auto* s:{&implicit.current,&*implicit.previous,&*implicit.previousPrevious})++s->matrixSourceId;
    auto p=parameters;p.useActualParity=true;const auto actual=Build(implicit,p);Check(actual.status==Status::Ready,"actual parity override rejected");
    ComparePoseConstants(a.constants,actual.constants,.000004f,"actual source parity was replaced by matrix-record parity");
    p.useActualParity=false;p.swapEyeOffsets=true;const auto swapped=Build(implicit,p);
    ComparePoseConstants(a.constants,swapped.constants,.000004f,"measured eye-offset swap gate was ignored");
}
static void GaugeInvariance(){
    History source{Frame(2),Frame(1),Frame(0)};unsigned n=0;
    for(auto* s:{&source.current,&*source.previous,&*source.previousPrevious}){
        s->camera.orientation={.1f*float(n+1),.2f,-.05f,.95f};s->camera.position={float(3+n),float(2*n*n),-float(1+n)};
        s->explicitReference=s->camera;s->explicitReference->position[0]+=.1f;++n;
    }
    Pose world;world.orientation={.2f,.3f,-.1f,.92736185f};world.position={101,-89,17};
    // This is our SE(3) derivation, not an assertion about the game's world
    // origin. Transform all stamped camera/reference inputs coherently by W.
    auto shifted=source;
    for(auto* s:{&shifted.current,&*shifted.previous,&*shifted.previousPrevious}){
        s->camera=ComposePoseOracle(world,s->camera);s->explicitReference=ComposePoseOracle(world,*s->explicitReference);s->basisId=24;
    }
    const auto p=ActualFixtureParameters();const auto original=Build(source,p),changed=Build(shifted,p);
    Check(original.status==Status::Ready&&changed.status==Status::Ready,"coherent gauge samples rejected");
    ComparePoseConstants(original.constants,changed.constants,.000035f,"common rigid world gauge changed view-space constants");
    // Canonical gauge W=inverse(V[-2]): V[-2]=I, V[-1]=T_2,
    // V[0]=T_2*T0. Eye-local offsets are unchanged by this world transform.
    auto canonical=source;const auto inverseOldest=InversePoseOracle(source.previousPrevious->camera);
    for(auto* s:{&canonical.current,&*canonical.previous,&*canonical.previousPrevious}){
        s->camera=ComposePoseOracle(inverseOldest,s->camera);s->explicitReference=ComposePoseOracle(inverseOldest,*s->explicitReference);s->basisId=25;
    }
    const auto result=Build(canonical,p);Check(result.status==Status::Ready,"canonical gauge rejected");
    for(float value:canonical.previousPrevious->camera.position)Check(std::abs(value)<.000002f,"canonical oldest camera was not identity");
    ComparePoseConstants(original.constants,result.constants,.000008f,"canonical relative-transform gauge changed output");
    // Other fields are unaffected by a pose-only gauge change.
    const auto* rawA=reinterpret_cast<const unsigned char*>(&original.constants);
    const auto* rawB=reinterpret_cast<const unsigned char*>(&result.constants);
    Check(std::memcmp(rawA+144,rawB+144,480-144)==0,"pose gauge changed unrelated scalar payload");
}
int main(){try{StaticAlternatingEyes();IndependentWorldPointOracle();TranslationAndDebugConvention();ScalarHistoryAndProjection();GuardsAndNoFabricatedHistory();ActualEyeReferenceAndParity();GaugeInvariance();
std::cout<<"PASS CPU reference checks="<<checks<<" GPU=not-used\n";return 0;}
catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}}

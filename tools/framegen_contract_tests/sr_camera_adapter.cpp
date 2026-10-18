#include "Framegen/SrCameraAdapter.hpp"
#include "fixtures/sr_actual_120_122.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <limits>
#include <bit>
#include <cstring>
using namespace cvr::framegen;
using namespace cvr::framegen::sr;
using D3=std::array<double,3>;using D4=std::array<double,4>;using DM=std::array<double,16>;
static unsigned checks{};
static void Check(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
static D4 Hamilton(D4 a,D4 b){return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
static D3 Rotate(D4 q,D3 p){auto v=Hamilton(Hamilton(q,{p[0],p[1],p[2],0}),{-q[0],-q[1],-q[2],q[3]});return {v[0],v[1],v[2]};}
static DM Identity(){DM m{};for(unsigned i=0;i<4;++i)m[i*5]=1;return m;}
static DM Mul(const DM&a,const DM&b){DM m{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)m[i*4+j]+=a[i*4+k]*b[k*4+j];return m;}
static DM Inverse(DM a){DM b=Identity();for(unsigned c=0;c<4;++c){unsigned pivot=c;for(unsigned r=c+1;r<4;++r)if(std::abs(a[r*4+c])>std::abs(a[pivot*4+c]))pivot=r;
    for(unsigned k=0;k<4;++k){std::swap(a[c*4+k],a[pivot*4+k]);std::swap(b[c*4+k],b[pivot*4+k]);}
    const double scale=a[c*4+c];Check(std::abs(scale)>1e-12,"oracle singular matrix");for(unsigned k=0;k<4;++k){a[c*4+k]/=scale;b[c*4+k]/=scale;}
    for(unsigned r=0;r<4;++r)if(r!=c){const double f=a[r*4+c];for(unsigned k=0;k<4;++k){a[r*4+k]-=f*a[c*4+k];b[r*4+k]-=f*b[c*4+k];}}}return b;}
static DM Read(const CameraConstants& c,unsigned offset){DM m{};for(unsigned i=0;i<16;++i)m[i]=c.values[offset+i];return m;}
static void Store(CameraConstants&c,unsigned offset,const DM&m){for(unsigned i=0;i<16;++i)c.values[offset+i]=float(m[i]);}
static D4 Row(D4 p,const DM&m){D4 result{};for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)result[j]+=p[i]*m[i*4+j];return result;}
static D3 GpuPoint(const M4&m,D3 p){D3 out{};for(unsigned r=0;r<3;++r){out[r]=m[r*4+3];for(unsigned c=0;c<3;++c)out[r]+=m[r*4+c]*p[c];}return out;}
static Parameters SyntheticParameters(){Parameters p;p.known=p.effectiveFix=true;p.key=31;p.ngxFeature=1;
    p.depthMode=1;p.depthScale=1;p.mvMultiplier={1,1};p.projectionGain={1,1};p.depthTolerance=.01f;p.mvTolerancePixels=2;
    p.staticThreshold=.05f;p.staticZExp=1;p.mcDepthTolerance=.2f;return p;}
static void SyntheticOwnerFields(CameraSample& s){s.key.resourceEpoch=4;s.key.historyEpoch=8;s.key.featurePairKey=19;s.key.parametersKey=31;
    s.ngx.known=true;s.ngx.depthSize=s.ngx.motionSize=s.ngx.renderSubrect={960,960};s.ngx.outputSize={1920,1920};
    s.ngx.lowResMv=true;s.ngx.mvScale={960,960};s.ngx.jitterPixels={.125f,-.25f};}
static D4 WrittenQ(const CameraSample&s){D4 q{};for(unsigned i=0;i<4;++i)q[i]=std::bit_cast<float>(s.appliedEye.reference.writtenEye.rotation[i]);return q;}
static void StoreBasis(CameraConstants& c,D4 q){const auto right=Rotate(q,{1,0,0}),up=Rotate(q,{0,0,1}),fwd=Rotate(q,{0,1,0});
    for(unsigned j=0;j<3;++j){c.values[89+j]=float(up[j]);c.values[92+j]=float(right[j]);c.values[95+j]=float(fwd[j]);}}
static DM CameraToWorld(const CameraSample&s){const auto&q=s.appliedEye.reference;DM v=Identity();
    for(unsigned j=0;j<3;++j){v[j]=s.original.values[92+j];v[4+j]=s.original.values[89+j];v[8+j]=s.original.values[95+j];v[12+j]=double(q.writtenEye.position[j])*0x1p-17;}return v;}
static std::array<CameraSample,3> SyntheticTriple(){std::array<CameraSample,3> a{};
    for(unsigned i=0;i<3;++i){auto&s=a[i];const double angle=.13+double(i)*.17;D4 q{std::sin(angle)*.6,std::sin(angle)*.8,0,std::cos(angle)};
        // Actual captured quaternion is fp32. Oracle and source use those exact bits.
        for(auto&v:q)v=float(v);
        const D3 centre{.1*double(i*i),.04*double(i),.02*double(i*i)};
        const D3 shift=Rotate(q,{i==1?.032:-.032,0,0});int32_t centreFP[3]{},eyeFP[3]{};float quat[4]{};
        for(unsigned j=0;j<3;++j){centreFP[j]=int32_t(std::llround(centre[j]*0x1p17));eyeFP[j]=int32_t(std::llround((centre[j]+shift[j])*0x1p17));}
        for(unsigned j=0;j<4;++j)quat[j]=float(q[j]);
        s.key.source={2,i+1,3,{1,2,3},int64_t(i),i==1?AerV2Eye::Right:AerV2Eye::Left,9,1000+int64_t(i),false};
        s.key.captureClockGeneration=7;s.key.origin=10;s.key.poseId=20+i;s.key.writeId=40+i;s.key.locatedPoseTimeNs=1000+int64_t(i);
        s.key.nativeFrame=500+i;s.key.slToken=499+i;s.key.viewport=0;s.key.engineView=1;s.key.nativeFrameKnown=s.key.slTokenKnown=s.key.viewportKnown=true;
        SyntheticOwnerFields(s);s.appliedEye.key=s.key;s.appliedEye.reference=cvr::camera::MakeAppliedEyeReference(centreFP,eyeFP,quat,cvr::camera::AppliedEyeRoute::Component,true,true);
        s.appliedEye.selectedFingerprint=s.appliedEye.reference.writtenEye;
        auto&c=s.original;c.source=CameraConstantsSource::Streamline;c.version=2;c.valuePresent.set();c.flagPresent.set();c.flags={1,1,0,0,0,0,0};
        DM p{};p[0]=.7+double(i)*.03;p[5]=.8+double(i)*.04;p[8]=.1;p[9]=-.07;p[10]=-.02/(16000-.02);p[11]=1;p[14]=.02*16000/(16000-.02);
        Store(c,0,p);Store(c,16,Inverse(Read(c,0)));StoreBasis(c,q);c.values[98]=.02f;c.values[99]=16000;c.values[100]=108.54f;c.values[101]=1;c.values[103]=40;
        for(unsigned j=32;j<48;++j)c.values[j]=(std::numeric_limits<float>::max)();c.values[84]=c.values[85]=c.values[102]=(std::numeric_limits<float>::max)();
        c.values[80]=.1f*float(i);c.values[81]=-.2f;c.values[82]=c.values[83]=1;
        for(unsigned j=0;j<3;++j)c.values[86+j]=float(CameraToWorld(s)[12+j]);
    }
    for(unsigned i=0;i<3;++i){const DM relative=i?Mul(CameraToWorld(a[i]),Inverse(CameraToWorld(a[i-1]))):Identity();
        const DM clip=Mul(Mul(Inverse(Read(a[i].original,0)),relative),Read(a[i?i-1:0].original,0));
        Store(a[i].original,48,clip);Store(a[i].original,64,Inverse(Read(a[i].original,48)));}
    return a;
}
static void ProjectedPointAndReferenceOracle(){const auto input=SyntheticTriple();const auto r=AdaptCameraHistory(input,SyntheticParameters());
    Check(r.status==AdapterStatus::Ready,"coherent synthetic source/proof triple cannot build production constants");
    for(unsigned newer=1;newer<3;++newer){const auto& current=input[newer];const auto& previous=input[newer-1];
        const auto m=newer==2?r.constants.transf0:r.constants.transf_2;
        for(D3 nativePoint:{D3{.4,-.6,2},D3{-.7,.3,10},D3{1,2,30}}){
            // Independent physical construction: camera-local X/Y/Z are engine X/Z/Y.
            const auto worldDirection=Rotate(WrittenQ(current),{nativePoint[0],nativePoint[2],nativePoint[1]});
            D3 fromPrevious{};for(unsigned j=0;j<3;++j)fromPrevious[j]=worldDirection[j]+double(current.appliedEye.reference.writtenEye.position[j]-previous.appliedEye.reference.writtenEye.position[j])*0x1p-17;
            const auto q=WrittenQ(previous);const auto local=Rotate({-q[0],-q[1],-q[2],q[3]},fromPrevious);
            const D3 expectedShader{local[0],local[2],-local[1]};const auto actual=GpuPoint(m,{nativePoint[0],nativePoint[1],-nativePoint[2]});
            for(unsigned j=0;j<3;++j)Check(std::abs(actual[j]-expectedShader[j])<.000025,"S*Trow^T*S differs from independent physical point transform");
            const auto throughOriginal=Row(Row({nativePoint[0],nativePoint[1],nativePoint[2],1},Read(current.original,0)),Read(current.original,48));
            const auto throughShader=Row({actual[0],actual[1],-actual[2],1},Read(previous.original,0));
            for(unsigned j=0;j<3;++j)Check(std::abs(throughOriginal[j]/throughOriginal[3]-throughShader[j]/throughShader[3])<.00002,"reflected transform changes projected NDC point");
        }
    }
    D3 second{};for(unsigned j=0;j<3;++j)second[j]=double(int64_t(input[2].appliedEye.reference.centreFixed[j])+input[0].appliedEye.reference.centreFixed[j]-2LL*input[1].appliedEye.reference.centreFixed[j])*0x1p-17;
    const auto q=WrittenQ(input[0]);const auto local=Rotate({-q[0],-q[1],-q[2],q[3]},second);const D3 expected{local[0],local[2],-local[1]};
    for(unsigned j=0;j<3;++j)Check(std::abs(r.constants.camMoComp_2[j]-expected[j])<.00002,"reference centre uses guessed eye offsets or wrong gauge");
    Check(r.constants.zFar0==16000 && r.constants.zNear0==.02f,"builder silently replaced raw near/far with projection endpoints");
    Check(r.constants.mvScale0==V2{1,1},"adapter confused SL normalized scale with actual NGX input scale");
    Check(std::abs(r.history.current.projection[0]-(1-input[2].original.values[9])/input[2].original.values[5])<1e-6f,"projection top tangent sign/order wrong");
    Check(std::abs(r.history.current.projection[2]-(1+input[2].original.values[8])/input[2].original.values[0])<1e-6f,"projection left tangent sign/order wrong");
    auto shifted=input;for(auto&s:shifted){s.original.values[86]=123456;s.original.values[87]=-98765;s.original.values[88]=50000;s.original.values[100]=std::numeric_limits<float>::quiet_NaN();}
    const auto ignored=AdaptCameraHistory(shifted,SyntheticParameters());Check(ignored.status==AdapterStatus::Ready && std::memcmp(&r.constants,&ignored.constants,sizeof(Constants))==0,"adapter assumed SL.cameraPos equals FP world origin or used raw FOV");
}
static void SourceAndProofGuards(){const auto original=SyntheticTriple();const auto p=SyntheticParameters();
    for(unsigned mutation=0;mutation<28;++mutation){auto a=original;auto parameters=p;AdapterStatus expected=AdapterStatus::HistoryMismatch;
        auto&x=a[1];
        switch(mutation){
        case 0:parameters.known=false;expected=AdapterStatus::UnknownConfiguration;break;
        case 1:parameters.effectiveFix=false;expected=AdapterStatus::Disabled;break;
        case 2:x.ngx.known=false;expected=AdapterStatus::UnknownNgxInputs;break;
        case 3:x.appliedEye.reference={};expected=AdapterStatus::MissingEyeProof;break;
        case 4:++x.appliedEye.selectedFingerprint.position[0];expected=AdapterStatus::EyeProofMismatch;break;
        case 5:++x.appliedEye.key.writeId;expected=AdapterStatus::EyeProofMismatch;break;
        case 6:++x.key.captureClockGeneration;x.appliedEye.key=x.key;break;
        case 7:++x.key.source.epochs.gameWorld;x.appliedEye.key=x.key;break;
        case 8:++x.key.historyEpoch;x.appliedEye.key=x.key;break;
        case 9:x.key.source.sourceId+=2;x.appliedEye.key=x.key;break;
        case 10:++x.key.slToken;x.appliedEye.key=x.key;break;
        case 11:x.original.flags[3]=1;expected=AdapterStatus::ResetBoundary;break;
        case 12:x.original.flagPresent[3]=false;expected=AdapterStatus::MissingConstants;break;
        case 13:x.original.values[89]*=-1;x.original.values[90]*=-1;x.original.values[91]*=-1;expected=AdapterStatus::CameraBasisMismatch;break;
        case 14:x.original.values[11]=-1;expected=AdapterStatus::UnsupportedProjection;break;
        case 15:x.key.nativeFrameKnown=false;x.appliedEye.key=x.key;expected=AdapterStatus::UnknownSource;break;
        case 16:parameters.depthMode=0;expected=AdapterStatus::UnsupportedFlags;break;
        case 17:x.original.flags[2]=1;expected=AdapterStatus::UnsupportedFlags;break;
        case 18:++x.key.source.requestRevision;x.appliedEye.key=x.key;break;
        case 19:++x.key.source.latchRevision;x.appliedEye.key=x.key;break;
        case 20:++x.key.source.clockGeneration;x.appliedEye.key=x.key;break;
        case 21:++x.key.source.epochs.renderer;x.appliedEye.key=x.key;break;
        case 22:++x.key.resourceEpoch;x.appliedEye.key=x.key;break;
        case 23:++x.key.origin;x.appliedEye.key=x.key;break;
        case 24:++x.key.source.epochs.hmdWorld;x.appliedEye.key=x.key;break;
        case 25:++x.key.locatedPoseTimeNs;x.appliedEye.key=x.key;expected=AdapterStatus::UnknownSource;break;
        case 26:parameters.debugFlags=0x20000;expected=AdapterStatus::UnsupportedFlags;break;
        case 27:x.ngx.depthSize[0]=0;expected=AdapterStatus::HistoryMismatch;break;
        }
        Check(AdaptCameraHistory(a,parameters).status==expected,"unknown/mismatched source proof, flag or temporal predecessor accepted");
    }
    auto a=original;a[0].original.flags[3]=1;Check(AdaptCameraHistory(a,p).status==AdapterStatus::Ready,"oldest reset endpoint cannot bootstrap two later consecutive sources");
    a=original;a[2].key.source.sourceId=SourceDisplayClock::MaxExactSourceId+1;a[2].appliedEye.key=a[2].key;
    Check(AdaptCameraHistory(a,p).status==AdapterStatus::UnknownSource,"unsafe source domain accepted");
    a=original;auto c=Read(a[2].original,48);c[0]*=1.1;Store(a[2].original,48,c);Store(a[2].original,64,Inverse(c));
    Check(AdaptCameraHistory(a,p).status==AdapterStatus::NonRigidTransform,"nonrigid camera transform sanitized into a pose");
    a=original;for(unsigned i=0;i<3;++i){a[i].key.nativeFrame=uint32_t(UINT32_MAX-1u+i);a[i].key.slToken=uint32_t(UINT32_MAX-1u+i);a[i].appliedEye.key=a[i].key;}
    Check(AdaptCameraHistory(a,p).status==AdapterStatus::Ready,"known native/token zero at unsigned wrap was treated as missing or aliased to sourceId");
}
static void ActualDatasetNeedsProof(){auto a=MeasuredSr120_122();const auto p=SyntheticParameters();
    Check(AdaptCameraHistory(a,p).status!=AdapterStatus::Ready,"actual677 dataset invented absent owner inputs/eye proof");
    for(auto&s:a)SyntheticOwnerFields(s);
    Check(AdaptCameraHistory(a,p).status==AdapterStatus::MissingEyeProof,"synthetic owner context invented absent applied-eye source proof");
}
static void ActualConstantsWithExplicitSyntheticProof(){auto a=MeasuredSr120_122();
    for(unsigned i=0;i<3;++i){auto&s=a[i];SyntheticOwnerFields(s);float q[4]{};for(unsigned j=0;j<4;++j)q[j]=std::bit_cast<float>(SyntheticWriterQuaternionBits[i][j]);
        const D3 eye=Rotate({q[0],q[1],q[2],q[3]},{i==1?.032:-.032,0,0});int32_t centre[3]{},eyeFP[3]{};
        for(unsigned j=0;j<3;++j)eyeFP[j]=int32_t(std::llround(eye[j]*0x1p17));
        s.appliedEye.key=s.key;s.appliedEye.reference=cvr::camera::MakeAppliedEyeReference(centre,eyeFP,q,cvr::camera::AppliedEyeRoute::Scene,true,true);
        s.appliedEye.selectedFingerprint=s.appliedEye.reference.writtenEye;
        // Known only within this synthetic owner fixture; original NGX values
        // were not recorded in677A. Never populate them in the measured fixture.
        s.ngx.jitterPixels={s.original.values[80],s.original.values[81]};
    }
    const auto result=AdaptCameraHistory(a,SyntheticParameters());Check(result.status==AdapterStatus::Ready,"actual SL projection/matrices rejected with explicitly synthetic compatible writer proof");
    for(unsigned i=0;i<16;++i){Check(std::abs(result.constants.transf0[i]-ActualT0Shader[i])<.000003,"actual120-122 current matrix changed order or reflection");
        Check(std::abs(result.constants.transf_2[i]-ActualT2Shader[i])<.000003,"actual120-122 previous matrix changed order or reflection");}
    Check(result.constants.zFar0==16000&&result.constants.zNear0==a[2].original.values[98],"actual raw near/far fidelity replaced by projective endpoints");
    Check(std::abs(result.diagnostics.projectionFar[2]-16777.2359375)<.001,"actual Pfar/rawFar distinction disappeared");
    Check(std::abs(result.diagnostics.fovYRadians[2]-1.8943802201331255)<1e-7,"actual P was replaced by raw108.54 as radians");
}
static void ProvenCpRawInverseDepth(){const auto inputs=SyntheticTriple();auto p=SyntheticParameters();p.depthMode=2;
    const auto result=AdaptCameraHistory(inputs,p);Check(result.status==AdapterStatus::Ready,"proven CP mode2 inverse-depth denominator refused");
    Check(result.constants.uvw2xyzScale0[2]==1&&result.constants.uvw2xyzOffset0[2]==0&&result.constants.zFar0==16000,"CP mode2 was silently changed to finite reversed projection");
    auto wrong=inputs;wrong[1].original.flags[0]=0;
    Check(AdaptCameraHistory(wrong,p).status==AdapterStatus::UnsupportedFlags,"raw inverse-depth mode2 accepted an ordinary forward-depth source");
}
static void GaugeInvariance(){auto a=SyntheticTriple();const auto expected=AdaptCameraHistory(a,SyntheticParameters());
    // A common exact half-turn about engine Z and an integer FP translation.
    // Camera clip transforms do not change. All writer/basis evidence does.
    for(auto&s:a){auto& ref=s.appliedEye.reference;auto q=Hamilton({0,0,1,0},WrittenQ(s));
        for(unsigned j=0;j<4;++j)ref.writtenEye.rotation[j]=std::bit_cast<uint32_t>(float(q[j]));
        for(unsigned j=0;j<3;++j){const int sign=j<2?-1:1;ref.centreFixed[j]=sign*ref.centreFixed[j]+100000;ref.writtenEye.position[j]=sign*ref.writtenEye.position[j]+100000;}
        s.appliedEye.selectedFingerprint=ref.writtenEye;StoreBasis(s.original,q);
    }
    const auto actual=AdaptCameraHistory(a,SyntheticParameters());Check(actual.status==AdapterStatus::Ready,"common rigid gauge invalidated proven basis");
    for(unsigned j=0;j<3;++j)Check(std::abs(expected.constants.camMoComp_2[j]-actual.constants.camMoComp_2[j])<.000002,"camera compensation depends on unobserved absolute world origin");
    Check(expected.constants.transf0==actual.constants.transf0&&expected.constants.transf_2==actual.constants.transf_2,"relative matrices changed under common world gauge");
}
int main(){try{ProvenCpRawInverseDepth();ProjectedPointAndReferenceOracle();SourceAndProofGuards();ActualDatasetNeedsProof();ActualConstantsWithExplicitSyntheticProof();GaugeInvariance();
    std::cout<<"PASS sr camera adapter checks="<<checks<<"; synthetic proofs labelled; actual677 remains NOTREADY\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}

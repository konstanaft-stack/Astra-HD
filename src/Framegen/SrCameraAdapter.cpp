#include "Framegen/SrCameraAdapter.hpp"
#include <bit>
namespace cvr::framegen::sr {
namespace adapter_detail {
using D3=std::array<double,3>;using D4=std::array<double,4>;using Matrix=std::array<double,16>;
// Numerical admission tolerances, not render/configuration defaults. Retained
// actual SL relative rotations were <=1.23e-6 from orthonormal; no fitting or
// replacement of an out-of-domain matrix is allowed by this tolerance.
constexpr double Tolerance=2e-4;
constexpr uint64_t ShaderBasisId=1; // This adapter's validated +Z -> -Z basis.
Matrix Identity(){Matrix m{};m[0]=m[5]=m[10]=m[15]=1;return m;}
Matrix Read(const CameraConstants& c,unsigned offset){Matrix m{};for(unsigned i=0;i<16;++i)m[i]=c.values[offset+i];return m;}
Matrix Multiply(const Matrix&a,const Matrix&b){Matrix out{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
    for(unsigned k=0;k<4;++k)out[r*4+c]+=a[r*4+k]*b[k*4+c];return out;}
bool Inverse(Matrix a,Matrix& b){b=Identity();for(unsigned c=0;c<4;++c){unsigned pivot=c;
    for(unsigned r=c+1;r<4;++r)if(std::abs(a[r*4+c])>std::abs(a[pivot*4+c]))pivot=r;
    const double divisor=a[pivot*4+c];if(!std::isfinite(divisor)||std::abs(divisor)<1e-14)return false;
    for(unsigned k=0;k<4;++k){std::swap(a[c*4+k],a[pivot*4+k]);std::swap(b[c*4+k],b[pivot*4+k]);a[c*4+k]/=divisor;b[c*4+k]/=divisor;}
    for(unsigned r=0;r<4;++r)if(r!=c){const double f=a[r*4+c];for(unsigned k=0;k<4;++k){a[r*4+k]-=f*a[c*4+k];b[r*4+k]-=f*b[c*4+k];}}
    }return std::all_of(b.begin(),b.end(),[](double v){return std::isfinite(v);});}
double MaxIdentityResidual(const Matrix&a){double maximum=0;for(unsigned i=0;i<16;++i)maximum=(std::max)(maximum,std::abs(a[i]-(i%5==0?1.:0.)));return maximum;}
bool FieldsPresent(const CameraConstants& c,unsigned start,unsigned end){for(unsigned i=start;i<end;++i)if(!c.valuePresent[i])return false;return true;}
bool FieldsValid(const CameraConstants& c,unsigned start,unsigned end){for(unsigned i=start;i<end;++i)
    if(!std::isfinite(c.values[i])||std::abs(c.values[i])>=3.4e38f)return false;return true;}
bool KeyValid(const CameraSourceKey&k){return bool(k.source)&&k.resourceEpoch&&k.captureClockGeneration&&k.historyEpoch&&k.origin&&k.poseId&&k.writeId&&
    k.featurePairKey&&k.parametersKey&&k.locatedPoseTimeNs==k.source.requestedPoseTimeNs&&k.nativeFrameKnown&&k.slTokenKnown&&k.viewportKnown&&
    k.viewport==0&&k.engineView==1;}
bool SameScope(const CameraSourceKey&a,const CameraSourceKey&b){return a.source.epochs==b.source.epochs&&a.source.requestRevision==b.source.requestRevision&&
    a.source.latchRevision==b.source.latchRevision&&a.source.clockGeneration==b.source.clockGeneration&&a.resourceEpoch==b.resourceEpoch&&
    a.captureClockGeneration==b.captureClockGeneration&&a.historyEpoch==b.historyEpoch&&a.origin==b.origin&&a.featurePairKey==b.featurePairKey&&
    a.parametersKey==b.parametersKey&&a.viewport==b.viewport&&a.engineView==b.engineView;}
bool Consecutive(const CameraSourceKey& newer,const CameraSourceKey& older){return SameScope(newer,older)&&
    newer.source.sourceId-older.source.sourceId==1&&older.source.sceneOrdinal!=(std::numeric_limits<uint64_t>::max)()&&
    newer.source.sceneOrdinal==older.source.sceneOrdinal+1&&uint32_t(newer.nativeFrame-older.nativeFrame)==1u&&uint32_t(newer.slToken-older.slToken)==1u;}
bool SameSizes(const NgxCameraInputs&a,const NgxCameraInputs&b){return a.depthSize==b.depthSize&&a.motionSize==b.motionSize&&a.renderSubrect==b.renderSubrect&&
    a.outputSize==b.outputSize&&a.lowResMv==b.lowResMv&&a.linearDepth==b.linearDepth;}
double Determinant3(const Matrix&m){return m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);}
Matrix Rotation(D4 q){const auto[x,y,z,w]=q;Matrix m=Identity();
    m[0]=1-2*(y*y+z*z);m[1]=2*(x*y-z*w);m[2]=2*(x*z+y*w);
    m[4]=2*(x*y+z*w);m[5]=1-2*(x*x+z*z);m[6]=2*(y*z-x*w);
    m[8]=2*(x*z-y*w);m[9]=2*(y*z+x*w);m[10]=1-2*(x*x+y*y);return m;}
bool WriterBasis(const CameraSample&s,AdapterDiagnostics&d,unsigned index,D3&localEye){
    D4 q{};double norm=0;for(unsigned i=0;i<4;++i){q[i]=std::bit_cast<float>(s.appliedEye.reference.writtenEye.rotation[i]);norm+=q[i]*q[i];}
    if(!std::isfinite(norm)||std::abs(norm-1)>Tolerance)return false;
    const auto rotation=Rotation(q);const auto&c=s.original;Matrix nativeToWorld=Identity();double maximum=0;
    // CameraMath.cpp BuildGameViewRowsFromQuaternion: native camera basis is
    // [R(q)X, R(q)Z, R(q)Y]. Check actual SL axes; never recompute its up axis.
    const unsigned offsets[3]{92,89,95},engineColumns[3]{0,2,1};
    for(unsigned axis=0;axis<3;++axis)for(unsigned row=0;row<3;++row){const double v=c.values[offsets[axis]+row];
        nativeToWorld[row*4+axis]=v;maximum=(std::max)(maximum,std::abs(v-rotation[row*4+engineColumns[axis]]));}
    d.writerBasisResidual[index]=maximum;d.nativeBasisDeterminant[index]=Determinant3(nativeToWorld);
    if(maximum>Tolerance||std::abs(d.nativeBasisDeterminant[index]+1)>Tolerance)return false;
    Matrix inverse{};if(!Inverse(nativeToWorld,inverse))return false;const auto measured=s.appliedEye.reference.DeltaMetres();
    for(unsigned row=0;row<3;++row){localEye[row]=0;for(unsigned col=0;col<3;++col)localEye[row]+=inverse[row*4+col]*measured[col];}
    localEye[2]=-localEye[2];d.shaderLocalEyeDelta[index]=localEye;
    return std::all_of(localEye.begin(),localEye.end(),[](double v){return std::isfinite(v);});
}
AdapterStatus Projection(const CameraSample&s,const Parameters&p,unsigned index,Sample&out,Matrix&inv,AdapterDiagnostics&d){
    const auto&c=s.original;const auto m=Read(c,0);
    // Current contract: finite non-oblique perspective, native +viewZ. Off-axis
    // X/Y centre is supported; clip.w=-viewZ or other layouts are explicit rejects.
    if(!(m[0]>0&&m[5]>0)||std::abs(m[11]-1)>1e-6)return AdapterStatus::UnsupportedProjection;
    for(unsigned i:{1u,2u,3u,4u,6u,7u,12u,13u,15u})if(std::abs(m[i])>1e-6)return AdapterStatus::UnsupportedProjection;
    if(!Inverse(m,inv)||MaxIdentityResidual(Multiply(m,Read(c,16)))>Tolerance||MaxIdentityResidual(Multiply(Read(c,48),Read(c,64)))>Tolerance)return AdapterStatus::InvalidProjection;
    if(!(c.values[98]>0&&c.values[99]>c.values[98]))return AdapterStatus::InvalidConstants;
    const bool inverted=c.flags[0]==1;
    if(!p.linearDepth&&((inverted&&!(m[14]>0&&m[10]<=0))||(!inverted&&!(m[14]<0&&m[10]>0))))return AdapterStatus::UnsupportedProjection;
    const D4 tangent{(1-m[9])/m[5],(1+m[9])/m[5],(1+m[8])/m[0],(1-m[8])/m[0]};
    for(unsigned i=0;i<4;++i){if(!(tangent[i]>0)||!std::isfinite(tangent[i]))return AdapterStatus::UnsupportedProjection;out.projection[i]=float(tangent[i]);}
    // Raw game near/far are fidelity inputs. Do not silently substitute these
    // diagnostic projective endpoints (actual trace rawFar16000 != Pfar16777).
    out.nearZ=c.values[98];out.farZ=c.values[99];d.rawNear[index]=out.nearZ;d.rawFar[index]=out.farZ;
    const double atZero=m[10]!=0?-m[14]/m[10]:(std::numeric_limits<double>::infinity)();
    const double atOne=m[10]!=1?m[14]/(1-m[10]):(std::numeric_limits<double>::infinity)();
    d.projectionNear[index]=inverted?atOne:atZero;d.projectionFar[index]=inverted?atZero:atOne;
    d.fovYRadians[index]=std::atan(tangent[0])+std::atan(tangent[1]);return AdapterStatus::Ready;
}
bool Rigid(const Matrix&m,AdapterDiagnostics&d,unsigned index){double affine=0,orthogonal=0;
    for(unsigned c=0;c<4;++c)affine=(std::max)(affine,std::abs(m[12+c]-(c==3?1.:0.)));
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c){double dot=0;for(unsigned k=0;k<3;++k)dot+=m[r*4+k]*m[c*4+k];orthogonal=(std::max)(orthogonal,std::abs(dot-(r==c?1.:0.)));}
    const double determinant=std::abs(Determinant3(m)-1);d.affineResidual[index]=affine;d.orthogonalResidual[index]=orthogonal;d.determinantResidual[index]=determinant;
    return std::all_of(m.begin(),m.end(),[](double v){return std::isfinite(v);})&&affine<=Tolerance&&orthogonal<=Tolerance&&determinant<=Tolerance;
}
bool MatrixPose(const Matrix&m,Pose&pose){D4 q{};const double trace=m[0]+m[5]+m[10];
    if(trace>0){const double t=2*std::sqrt(trace+1);q={(m[9]-m[6])/t,(m[2]-m[8])/t,(m[4]-m[1])/t,t/4};}
    else{unsigned i=0;if(m[5]>m[0])i=1;if(m[10]>m[i*4+i])i=2;const unsigned j=(i+1)%3,k=(j+1)%3;
        const double t=2*std::sqrt((std::max)(0.,1+m[i*4+i]-m[j*4+j]-m[k*4+k]));if(t<1e-12)return false;
        q[i]=t/4;q[j]=(m[i*4+j]+m[j*4+i])/t;q[k]=(m[i*4+k]+m[k*4+i])/t;q[3]=(m[k*4+j]-m[j*4+k])/t;}
    double norm=0;for(double v:q)norm+=v*v;norm=std::sqrt(norm);if(!(norm>0)||!std::isfinite(norm))return false;
    for(unsigned i=0;i<4;++i)pose.orientation[i]=float(q[i]/norm);
    for(unsigned i=0;i<3;++i)pose.position[i]=float(m[i*4+3]);return detail::ValidPose(pose);
}
}
AdapterResult AdaptCameraHistory(const std::array<CameraSample,3>& input,const Parameters&p) noexcept {
    using namespace adapter_detail;AdapterResult result;
    const auto fail=[&](AdapterStatus status){result.status=status;return result;};
    if(!p.known||!p.key)return fail(AdapterStatus::UnknownConfiguration);
    if(!p.effectiveFix)return fail(AdapterStatus::Disabled);
    // Inverting absolute world poses is not invariant under the canonical
    // gauge. The standalone builder retains that known debug branch, but this
    // adapter cannot recreate it without observing the original world poses.
    if((p.debugFlags&0x20000u)||p.depthMode<0||p.depthMode>2)return fail(AdapterStatus::UnsupportedFlags);
    std::array<Sample,3> converted{};std::array<Matrix,3> invProjection{};std::array<D3,3> eyeDelta{};
    for(unsigned i=0;i<3;++i){const auto&s=input[i];const auto&k=s.key;const auto&c=s.original;auto&out=converted[i];
        if(!KeyValid(k)||k.parametersKey!=p.key)return fail(AdapterStatus::UnknownSource);
        if(!s.ngx.known)return fail(AdapterStatus::UnknownNgxInputs);
        if(!s.appliedEye.reference)return fail(AdapterStatus::MissingEyeProof);
        if(s.appliedEye.key!=k||!s.appliedEye.reference.MatchesWrittenFingerprint(s.appliedEye.selectedFingerprint))return fail(AdapterStatus::EyeProofMismatch);
        if(c.source!=CameraConstantsSource::Streamline||c.version<1||c.version>2||!c.flagPresent.all()||
           !FieldsPresent(c,0,32)||!FieldsPresent(c,48,80)||!FieldsPresent(c,89,100))return fail(AdapterStatus::MissingConstants);
        if(!FieldsValid(c,0,32)||!FieldsValid(c,48,80)||!FieldsValid(c,89,100)||
           std::any_of(c.flags.begin(),c.flags.end(),[](uint32_t f){return f>1;}))return fail(AdapterStatus::InvalidConstants);
        // Proven CP mode2 keeps the raw inverse-depth denominator (scale1,
        // offset0), unlike mode1's finite reversed near/far formula. Both need
        // an explicitly inverted source. Do not "correct" mode2 from actual P.
        const bool depthMatches=p.linearDepth||(p.depthMode==0?c.flags[0]==0:c.flags[0]==1);
        if(c.flags[1]!=1||c.flags[2]!=0||c.flags[4]!=0||c.flags[5]!=0||p.linearDepth!=s.ngx.linearDepth||!depthMatches)
            return fail(AdapterStatus::UnsupportedFlags);
        const auto projection=Projection(s,p,i,out,invProjection[i],result.diagnostics);if(projection!=AdapterStatus::Ready)return fail(projection);
        if(!WriterBasis(s,result.diagnostics,i,eyeDelta[i]))return fail(AdapterStatus::CameraBasisMismatch);
        out.sourceId=out.matrixSourceId=k.source.sourceId;out.featureKey=k.featurePairKey;out.basisId=ShaderBasisId;out.parametersKey=k.parametersKey;
        out.epochs={k.source.epochs.renderer,k.source.epochs.gameWorld,k.source.epochs.hmdWorld,k.resourceEpoch};
        out.jitterPixels=s.ngx.jitterPixels;out.mvScale=s.ngx.mvScale;out.depthSize=s.ngx.depthSize;out.motionSize=s.ngx.motionSize;
        out.renderSubrect=s.ngx.renderSubrect;out.outputSize=s.ngx.outputSize;out.lowResMv=s.ngx.lowResMv;out.specularMvPresent=s.ngx.specularMvPresent;
        out.jitteredMv=c.flags[6]==1;out.beginsNewHistory=c.flags[3]==1;
    }
    for(unsigned i=1;i<3;++i){if(input[i].original.flags[3])return fail(AdapterStatus::ResetBoundary);
        if(!Consecutive(input[i].key,input[i-1].key)||!SameSizes(input[i].ngx,input[i-1].ngx))return fail(AdapterStatus::HistoryMismatch);}
    std::array<Matrix,2> shader{};constexpr double reflection[4]{1,1,-1,1};
    for(unsigned newer=1;newer<3;++newer){
        const auto row=Multiply(Multiply(Read(input[newer].original,0),Read(input[newer].original,48)),invProjection[newer-1]);
        const unsigned index=2-newer;
        // Original SL row convention: Pcur * Ccur * inv(Pprev). Shader P0 is
        // (x,y,-z,1), hence S * transpose(Trow) * S; both endpoints reflect.
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)shader[index][r*4+c]=reflection[r]*row[c*4+r]*reflection[c];
        if(!Rigid(shader[index],result.diagnostics,index))return fail(AdapterStatus::NonRigidTransform);
        for(unsigned j=0;j<16;++j)result.diagnostics.shaderRelative[index][j]=float(shader[index][j]);
    }
    const std::array<Matrix,3> canonical{Identity(),shader[1],Multiply(shader[1],shader[0])};
    for(unsigned i=0;i<3;++i){auto&s=converted[i];if(!MatrixPose(canonical[i],s.camera))return fail(AdapterStatus::NonRigidTransform);
        // R_i and p_i use the exact normalized pose the builder will consume.
        const auto pose=detail::ConvertPose<double>(s.camera);const auto worldDelta=detail::Rotate(pose.q,eyeDelta[i]);
        Pose reference=s.camera;for(unsigned j=0;j<3;++j)reference.position[j]=float(double(s.camera.position[j])-worldDelta[j]);
        s.explicitReference=reference; // No unobserved opposite-eye offset exists.
    }
    result.history={converted[2],converted[1],converted[0]};const auto built=Build(result.history,p);result.builderStatus=built.status;
    if(built.status!=Status::Ready)return fail(AdapterStatus::BuilderRejected);
    result.constants=built.constants;result.status=AdapterStatus::Ready;return result;
}
}

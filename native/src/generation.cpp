#include "cpvr/generation.hpp"
#include "../nvidia_backend/bridge_abi.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <d3dcompiler.h>
#include <limits>
#include <thread>
#include <vector>

namespace cpvr {
namespace {
std::int64_t cpu_time_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
bool pose_valid(const Pose& p) {
    double n=0;for(float f:p.orientation){if(!std::isfinite(f))return false;n+=double(f)*f;}
    for(float f:p.position)if(!std::isfinite(f)||std::abs(f)>1000)return false;
    return std::abs(n-1)<0.002;
}
bool fov_valid(const Fov& f) {
    for(float x:{f.left,f.right,f.up,f.down})if(!std::isfinite(x)||std::abs(x)>=1.5707963267948966f)return false;
    return f.left<f.right&&f.down<f.up;
}
bool frame_valid(const EyeFrame& f) {
    return std::uint32_t(f.eye)<2&&f.epoch&&f.space_epoch&&f.source_id&&f.content_time_ns>0&&
        pose_valid(f.render_pose)&&fov_valid(f.fov);
}
std::vector<std::array<bool,2>> pattern_of(const GenerationPolicy& p) {
    if(!p.generated_per_interval)return {{{false,false}}};
    if(p.presentation_pattern==PresentationPattern::Chessboard)return {{{true,false}},{{false,true}}};
    if(p.presentation_pattern==PresentationPattern::RealThenGeneratedStereo){
        std::vector<std::array<bool,2>> series{{false,false}};
        for(unsigned i=0;i<p.generated_per_interval;++i)series.push_back({true,true});
        return series;
    }
    std::vector<std::array<bool,2>> result;
    const auto& s=p.custom_sequence;std::size_t at=0;
    while(at<s.size()) {
        if(result.size()>=16||at+2>s.size()||(s[at]!='R'&&s[at]!='G')||(s[at+1]!='R'&&s[at+1]!='G'))return {};
        result.push_back({s[at]=='G',s[at+1]=='G'});at+=2;
        if(at==s.size())break;
        if(s[at++]!=','||at==s.size())return {};
    }
    return result;
}
std::int64_t phase_time(std::int64_t a,std::int64_t b,unsigned index,unsigned count) {
    const auto delta=b-a;const std::int64_t divisor=count+1;
    return a+(delta/divisor)*index+((delta%divisor)*index+divisor/2)/divisor;
}
bool product_valid(const GenerationProduct& p,Eye eye) {
    if(!p.frame||p.frame->eye!=eye||!frame_valid(*p.frame)||p.origin.generated!=p.frame->generated||
       p.origin.current_source_id!=p.frame->source_id)return false;
    const auto& o=p.origin;
    if(!o.generated)return !o.index&&!o.count&&o.previous_source_id==o.current_source_id&&
        o.previous_time_ns==o.current_time_ns&&o.current_time_ns==p.frame->content_time_ns;
    return o.previous_source_id&&o.current_source_id>o.previous_source_id&&o.previous_time_ns>0&&
        o.current_time_ns>o.previous_time_ns&&o.count>=1&&o.count<=5&&o.index>=1&&o.index<=o.count&&
        phase_time(o.previous_time_ns,o.current_time_ns,o.index,o.count)==p.frame->content_time_ns;
}
void log_safe(const Log& log,const std::string& s) noexcept {try{if(log)log(s);}catch(...) {}}
}

GenerationPolicy generation_policy_for_source(const Settings& s, Cadence actual_source) {
    GenerationPolicy p;
    p.generated_per_interval=s.frame_generation?s.multiplier-1:0;
    // A presentation choice never changes when the engine rendered each eye.
    // Pair using the producer's actual contract and retain both original times.
    p.cadence=actual_source;
    p.presentation_pattern=s.display_pattern==DisplayPattern::Chessboard?PresentationPattern::Chessboard:
        s.display_pattern==DisplayPattern::StereoPairs?PresentationPattern::RealThenGeneratedStereo:PresentationPattern::Custom;
    p.custom_sequence=s.eye_pattern;
    // Only a requested real/real step needs the existing staggered-real bound.
    // Adding GG to a custom generated cycle must not widen every eye match.
    const bool needs_staggered_real=!s.frame_generation||s.display_pattern==DisplayPattern::StereoPairs||
        (s.display_pattern==DisplayPattern::Custom&&s.eye_pattern.find("RR")!=std::string::npos);
    p.maximum_eye_skew_ns=needs_staggered_real?50'000'000:8'000'000;
    return p;
}

Validation validate_generation_policy(const GenerationPolicy& p) {
    if(p.generated_per_interval>5)return {false,"Vendor batch supports zero(disabled) or1..5 generated outputs"};
    if(p.maximum_eye_skew_ns<0||p.maximum_eye_skew_ns>50'000'000)return {false,"Eye skew must be0..50ms"};
    if(p.maximum_age_ns<=0||p.maximum_age_ns>1'000'000'000||p.maximum_interval_ns<=0||p.maximum_interval_ns>1'000'000'000)
        return {false,"History/age bound must be positive and at most one second"};
    if(p.cadence!=Cadence::StereoPairs&&p.cadence!=Cadence::AlternateEyes&&p.cadence!=Cadence::MixedPairs)
        return {false,"Unknown temporal cadence"};
    if(p.presentation_pattern!=PresentationPattern::Chessboard&&p.presentation_pattern!=PresentationPattern::RealThenGeneratedStereo&&
       p.presentation_pattern!=PresentationPattern::Custom)return {false,"Unknown presentation pattern"};
    auto requested=p;if(!requested.generated_per_interval)requested.generated_per_interval=1;
    if(pattern_of(requested).empty())return {false,"Custom sequence requires1..16 comma-separated LEFT/RIGHT R/G tokens"};
    return {true,{}};
}

std::optional<InterpolationStamp> interpolation_stamp(const EyeFrame& a,const EyeFrame& b,unsigned index,unsigned count) {
    if(!frame_valid(a)||!frame_valid(b)||a.generated||b.generated||a.eye!=b.eye||a.epoch!=b.epoch||
       a.space_epoch!=b.space_epoch||b.source_id<=a.source_id||b.content_time_ns<=a.content_time_ns||
       count<1||count>5||index<1||index>count)return {};
    const double t=double(index)/double(count+1);
    InterpolationStamp result;result.content_time_ns=phase_time(a.content_time_ns,b.content_time_ns,index,count);
    if(result.content_time_ns<=a.content_time_ns||result.content_time_ns>=b.content_time_ns)return {};
    for(unsigned i=0;i<3;i++)result.render_pose.position[i]=float(double(a.render_pose.position[i])*(1-t)+double(b.render_pose.position[i])*t);
    std::array<double,4> qa{},qb{};double dot=0;
    for(unsigned i=0;i<4;i++){qa[i]=a.render_pose.orientation[i];qb[i]=b.render_pose.orientation[i];dot+=qa[i]*qb[i];}
    if(dot<0){dot=-dot;for(auto&v:qb)v=-v;}
    dot=std::clamp(dot,-1.0,1.0);double wa=1-t,wb=t;
    if(dot<0.9995){const double angle=std::acos(dot),den=std::sin(angle);wa=std::sin((1-t)*angle)/den;wb=std::sin(t*angle)/den;}
    double norm=0;std::array<double,4> q{};
    for(unsigned i=0;i<4;i++){q[i]=wa*qa[i]+wb*qb[i];norm+=q[i]*q[i];}
    if(norm<1e-12)return {};
    for(unsigned i=0;i<4;i++)result.render_pose.orientation[i]=float(q[i]/std::sqrt(norm));
    auto mix=[&](float x,float y){return float(double(x)*(1-t)+double(y)*t);};
    result.fov={mix(a.fov.left,b.fov.left),mix(a.fov.right,b.fov.right),mix(a.fov.up,b.fov.up),mix(a.fov.down,b.fov.down)};
    result.origin={true,a.source_id,b.source_id,a.stereo_pair_id,b.stereo_pair_id,a.content_time_ns,b.content_time_ns,index,count};
    return result;
}

GenerationSelection choose_generation_pair(std::span<const GenerationProduct> left,std::span<const GenerationProduct> right,
    std::int64_t target,std::int64_t period,const GenerationPolicy& policy,GenerationCursor& cursor) {
    GenerationSelection result;
    if(!validate_generation_policy(policy).valid||target<=0||period<=0||left.size()>32||right.size()>32)return result;
    const auto pattern=pattern_of(policy);const auto expected=pattern[cursor.pattern_step%pattern.size()];
    result.requested_step={expected[0]?'G':'R',expected[1]?'G':'R'};
    auto& diagnostic=result.diagnostics;
    for(unsigned eye=0;eye<2;++eye)for(const auto& p:eye==0?left:right)if(product_valid(p,static_cast<Eye>(eye))){
        auto& latest=p.frame->generated?diagnostic.newest_generated_ns[eye]:diagnostic.newest_real_ns[eye];
        latest=std::max(latest,p.frame->content_time_ns);
        if(p.frame->generated==expected[eye])++diagnostic.expected_products[eye];
    }
    diagnostic.display_period_ns=period;
    const bool have_clock=cursor.valid&&cursor.display_time_ns>0&&cursor.content_clock_ns>0&&
        cursor.content_clock_ns<=cursor.display_time_ns;
    if(have_clock&&target<cursor.display_time_ns)return result;
    const bool paced=have_clock&&target-std::min(cursor.eye_times[0],cursor.eye_times[1])<=policy.maximum_age_ns;
    // Advance the intended content timeline with actual display time. A phase
    // chosen slightly before/after it does not accumulate rounding error.
    const auto due=paced?target-(cursor.display_time_ns-cursor.content_clock_ns):0;
    diagnostic.desired_content_ns=due;
    const std::int64_t bound=policy.cadence==Cadence::AlternateEyes?policy.maximum_eye_skew_ns:0;
    const GenerationProduct *best_l=nullptr,*best_r=nullptr;std::int64_t newest=0,best_skew=std::numeric_limits<std::int64_t>::max();
    const bool generated_step=expected[0]||expected[1];
    const bool stereo_batch=policy.cadence==Cadence::StereoPairs&&policy.generated_per_interval>0&&
        pattern.size()==policy.generated_per_interval+1&&!pattern[0][0]&&!pattern[0][1]&&
        std::all_of(pattern.begin()+1,pattern.end(),[](const auto& step){return step[0]&&step[1];});
    const auto following=pattern[(cursor.pattern_step+1)%pattern.size()];
    std::array<std::vector<std::int64_t>,2> following_ends;
    // A real pair preceding G must enter at the start of a usable fresh
    // interval, not its endpoint: endpoint selection would strand every G.
    if(!generated_step&&(following[0]||following[1])) {
        for(unsigned eye=0;eye<2;eye++) {
            const auto row=eye==0?left:right;following_ends[eye].resize(row.size());
            for(std::size_t i=0;i<row.size();i++) {
                const auto& real=row[i];
                if(!product_valid(real,static_cast<Eye>(eye))||real.frame->generated)continue;
                if(!following[eye]){following_ends[eye][i]=std::numeric_limits<std::int64_t>::max();continue;}
                for(const auto& g:row) {
                    if(!product_valid(g,static_cast<Eye>(eye))||!g.frame->generated||
                        g.frame->epoch!=real.frame->epoch||g.frame->space_epoch!=real.frame->space_epoch||
                        g.origin.previous_source_id!=real.frame->source_id||g.origin.previous_time_ns!=real.frame->content_time_ns||
                        g.frame->content_time_ns>target||target-g.frame->content_time_ns>policy.maximum_age_ns||
                        g.origin.current_time_ns-g.origin.previous_time_ns>policy.maximum_interval_ns)continue;
                    following_ends[eye][i]=std::max(following_ends[eye][i],g.origin.current_time_ns);
                }
            }
        }
    }
    int best_priority=-1;std::int64_t best_interval_end=0,best_distance=std::numeric_limits<std::int64_t>::max();
    bool best_recovery=false;
    for(std::size_t li=0;li<left.size();li++) for(std::size_t ri=0;ri<right.size();ri++) {
        const auto& l=left[li];const auto& r=right[ri];
        if(!product_valid(l,Eye::Left)||!product_valid(r,Eye::Right)||l.frame->generated!=expected[0]||r.frame->generated!=expected[1])continue;
        const auto& a=*l.frame;const auto& b=*r.frame;
        if(a.epoch!=b.epoch||a.space_epoch!=b.space_epoch||
           (a.stereo_pair_id&&b.stereo_pair_id&&a.stereo_pair_id!=b.stereo_pair_id))continue;
        const auto presentation=std::max(a.content_time_ns,b.content_time_ns);
        const auto skew=std::abs(a.content_time_ns-b.content_time_ns);
        const auto age=target-std::min(a.content_time_ns,b.content_time_ns);
        if(!diagnostic.compatible_pairs++){diagnostic.minimum_candidate_skew_ns=skew;diagnostic.minimum_candidate_age_ns=age;}
        else{diagnostic.minimum_candidate_skew_ns=std::min(diagnostic.minimum_candidate_skew_ns,skew);
            diagnostic.minimum_candidate_age_ns=std::min(diagnostic.minimum_candidate_age_ns,age);}
        if(presentation>target){++diagnostic.future_pairs;continue;}
        if(age>policy.maximum_age_ns){++diagnostic.expired_pairs;continue;}
        if(skew>bound){++diagnostic.skew_pairs;continue;}
        if(have_clock&&target==cursor.display_time_ns){++diagnostic.regressed_pairs;continue;}
        if(cursor.valid&&(a.epoch!=cursor.epoch||a.space_epoch!=cursor.space_epoch||
            a.content_time_ns<cursor.eye_times[0]||b.content_time_ns<cursor.eye_times[1]||
            presentation<=std::max(cursor.eye_times[0],cursor.eye_times[1]))){++diagnostic.regressed_pairs;continue;}
        // A serial generated eye advances in content time. Advancing the other
        // eye does not turn the already consumed G texture into a new G phase.
        if(cursor.valid&&((expected[0]&&cursor.last_origins[0].generated&&a.content_time_ns<=cursor.eye_times[0])||
            (expected[1]&&cursor.last_origins[1].generated&&b.content_time_ns<=cursor.eye_times[1]))){++diagnostic.regressed_pairs;continue;}
        if(generated_step&&paced&&presentation>due&&presentation-due>period/2){++diagnostic.early_for_slot;continue;}
        int priority=0;std::int64_t interval_end=0;
        if(generated_step) {
            interval_end=std::numeric_limits<std::int64_t>::max();
            const std::array<const GenerationProduct*,2> pair{&l,&r};
            for(unsigned eye=0;eye<2;eye++)if(expected[eye]) {
                const auto& origin=pair[eye]->origin;
                interval_end=std::min(interval_end,origin.current_time_ns);
            }
            // At startup/after expiry enter the freshest usable interval.
            // Once paced, older unconsumed G phases get no FIFO priority.
            priority=1;
        } else if(!following_ends[0].empty()) {
            interval_end=std::min(following_ends[0][li],following_ends[1][ri]);
            if(interval_end>0)priority=1;
        }
        // A multiphase RR/G cycle must start at a real anchor whose following
        // interval is already usable. Advancing RR to the newest endpoint
        // strands its preceding G phases, then steals G from the next interval.
        // The caller can still display its current real image while warming.
        const bool closing_interval=cursor.valid&&cursor.last_origins[0].generated&&cursor.last_origins[1].generated&&
            cursor.last_origins[0].index==cursor.last_origins[0].count&&cursor.last_origins[1].index==cursor.last_origins[1].count&&
            cursor.last_origins[0].current_source_id==a.source_id&&cursor.last_origins[1].current_source_id==b.source_id;
        if(stereo_batch&&!generated_step&&interval_end==0&&!closing_interval)continue;
        const auto distance=paced?std::abs(presentation-due):0;
        // If a batch arrived after we had to repeat its real anchor, a faster
        // consumer still has time for every phase. Do not skip G1 merely because
        // the old content clock advanced while the producer had nothing ready.
        const bool recover_first=stereo_batch&&generated_step&&paced&&cursor.pattern_step==1&&
            target-cursor.display_time_ns>period+period/2&&
            !cursor.last_origins[0].generated&&!cursor.last_origins[1].generated&&
            l.origin.previous_source_id==cursor.last_origins[0].current_source_id&&
            r.origin.previous_source_id==cursor.last_origins[1].current_source_id&&
            l.origin.count==policy.generated_per_interval&&r.origin.count==policy.generated_per_interval&&
            std::min(l.origin.current_time_ns-l.origin.previous_time_ns,r.origin.current_time_ns-r.origin.previous_time_ns)>=
                period*static_cast<std::int64_t>(policy.generated_per_interval+1);
        bool better=priority>best_priority;
        if(recover_first || best_recovery){
            better=recover_first&&(!best_recovery||presentation<newest);
        }else if((generated_step||stereo_batch)&&paced){
            better=!best_l||distance<best_distance||(distance==best_distance&&
                (skew<best_skew||(skew==best_skew&&(presentation>newest||
                 (presentation==newest&&interval_end>best_interval_end)))));
        }else if(priority==best_priority) {
            if(priority==1&&interval_end!=best_interval_end)better=interval_end>best_interval_end;
            else if(presentation!=newest)better=generated_step?presentation<newest:presentation>newest;
            else better=skew<best_skew;
        }
        if(better){best_l=&l;best_r=&r;newest=presentation;best_skew=skew;best_priority=priority;best_interval_end=interval_end;best_distance=distance;best_recovery=recover_first;}
    }
    if(!best_l) {
        if(cursor.last_selection&&cursor.valid&&cursor.last_selection->eye_skew_ns<=bound&&
            target>=std::max(cursor.eye_times[0],cursor.eye_times[1])&&
            target-std::min(cursor.eye_times[0],cursor.eye_times[1])<=policy.maximum_age_ns) {
            result.frame=cursor.last_selection;result.origins=cursor.last_origins;
        }
        return result;
    }
    auto frame=std::make_shared<StereoFrame>();frame->eyes={best_l->frame,best_r->frame};
    frame->epoch=best_l->frame->epoch;frame->space_epoch=best_l->frame->space_epoch;
    frame->pair_id=++cursor.pair_sequence;frame->cadence=policy.cadence;
    frame->presentation_time_ns=newest;frame->eye_skew_ns=best_skew;frame->max_eye_skew_ns=bound;
    frame->content_time_ns=best_skew?0:newest;
    result.frame=frame;result.origins={best_l->origin,best_r->origin};result.new_presentation=result.pattern_available=true;
    cursor.valid=true;cursor.epoch=frame->epoch;cursor.space_epoch=frame->space_epoch;
    cursor.eye_times={best_l->frame->content_time_ns,best_r->frame->content_time_ns};
    // Retain fractional phase progress while staying on the same timeline.
    // If RR deliberately enters a fresher usable interval after delivery lag
    // improves, rebase once; keeping the old offset would stall each following G.
    // X2 has no fractional sequence of intermediate G phases to preserve.
    // Anchor its pacing clock to each actual real endpoint. Otherwise display
    // quantization drifts behind that endpoint, defers G by another XR tick,
    // and lets the bounded two-real history evict the next required endpoint.
    const bool rebase_real=stereo_batch&&!generated_step&&paced&&
        (policy.generated_per_interval==1||
         (cursor.last_origins[0].generated&&cursor.last_origins[1].generated&&
          newest>std::max(cursor.last_origins[0].current_time_ns,cursor.last_origins[1].current_time_ns)));
    cursor.display_time_ns=target;cursor.content_clock_ns=(generated_step||stereo_batch)&&paced&&!rebase_real?due:newest;
    if(stereo_batch&&!generated_step&&paced&&!rebase_real)
        cursor.content_clock_ns=std::min(cursor.content_clock_ns,newest+period/2);
    cursor.pattern_step=(cursor.pattern_step+1)%unsigned(pattern.size());
    // When a display deadline skipped a phase, follow the chosen vendor ordinal
    // instead of waiting for an extra GG step belonging to a later interval.
    if(stereo_batch&&generated_step&&best_l->origin.count==policy.generated_per_interval&&
       best_r->origin.count==policy.generated_per_interval&&best_l->origin.index==best_r->origin.index)
        cursor.pattern_step=(best_l->origin.index+1)%unsigned(pattern.size());
    cursor.last_selection=frame;cursor.last_origins=result.origins;
    return result;
}

Validation validate_motion_request(const MotionRecordRequest& r) {
    auto usable=[](const MotionGuideSample& s){return s.guides&&s.guides->camera_valid&&s.source_id&&s.time_ns>0&&
        (s.convention==MotionConvention::NativeStreamlineForwardUvScaled||s.convention==MotionConvention::NativeStreamlineBackwardUvScaled||
         s.convention==MotionConvention::BackwardUvNormalized);};
    if(!usable(r.current)||!usable(r.previous))return {false,"Missing measured motion/camera sample"};
    if(r.reset)return {true,{}};
    if(r.current.source_id<=r.previous.source_id||r.current.time_ns<=r.previous.time_ns)return {false,"Nonpositive same-eye interval"};
    auto linked=[](const MotionGuideSample& current,const MotionGuideSample& previous){
        return current.previous_source_id==previous.source_id&&current.previous_time_ns==previous.time_ns&&
            current.previous_native_frame_token&&current.previous_native_frame_token==previous.native_frame_token;
    };
    if(r.middle) {
        if(!usable(*r.middle)||!linked(r.current,*r.middle)||!linked(*r.middle,r.previous)||
           r.middle->time_ns<=r.previous.time_ns||r.middle->time_ns>=r.current.time_ns)
            return {false,"Motion chain does not join current-middle-previous"};
    } else if(!linked(r.current,r.previous))return {false,"Motion span does not identify held same-eye predecessor"};
    if(r.current.guides->reset||r.current.guides->camera_flags[3]||
       (r.middle&&(r.middle->guides->reset||r.middle->guides->camera_flags[3])))return {false,"Native scene/reset cuts temporal span"};
    return {true,{}};
}

std::optional<MotionNormalization> motion_normalization(const SourceGuides& current,const SourceGuides& previous,
    Extent extent,Extent previous_extent,MotionConvention convention) {
    if(!extent.width||!extent.height||!previous_extent.width||!previous_extent.height||!current.camera_valid||!previous.camera_valid||
       (convention!=MotionConvention::NativeStreamlineForwardUvScaled&&convention!=MotionConvention::NativeStreamlineBackwardUvScaled&&
        convention!=MotionConvention::BackwardUvNormalized)||
       current.camera_flags[6]>1)return {};
    MotionNormalization result;
    for(unsigned axis=0;axis<2;axis++) {
        const float scale=current.camera[82+axis],jitter=current.camera[80+axis],previous_jitter=previous.camera[80+axis];
        if(!std::isfinite(scale)||scale==0||std::abs(scale)>1000||!std::isfinite(jitter)||!std::isfinite(previous_jitter)||
            std::abs(jitter)>8||std::abs(previous_jitter)>8)return {};
        if(convention==MotionConvention::BackwardUvNormalized&&(scale!=1||jitter!=0||current.camera_flags[6]))return {};
        const float dimension=float(axis?extent.height:extent.width),previous_dimension=float(axis?previous_extent.height:previous_extent.width);
        result.scale[axis]=convention==MotionConvention::NativeStreamlineForwardUvScaled?-scale:scale;
        result.current_jitter_uv[axis]=jitter/dimension;
        result.bias[axis]=current.camera_flags[6]?jitter/dimension-previous_jitter/previous_dimension:0;
    }
    return result;
}
namespace {
constexpr const char motion_shader[] = R"HLSL(
Texture2D<float2> currentMotion:register(t0);
Texture2D<float2> middleMotion:register(t1);
Texture2D<float> currentDepth:register(t2);
RWTexture2D<float2> resultMotion:register(u0);
RWTexture2D<float> resultDepth:register(u1);
RWTexture2D<float2> srMotion:register(u2);
cbuffer Mapping:register(b0) {
 float2 currentScale;float2 middleScale;
 float2 currentBias;float2 middleBias;
 float2 currentJitter;float2 middleJitter;
 float2 currentInverseSize;float2 middleInverseSize;
 float currentInvalid;float middleInvalid;uint compose;uint width;
 uint height;float invalidOutput;uint resetHistory;uint padding;
};
bool inside(float2 uv){return all(isfinite(uv))&&all(uv>=0)&&all(uv<=1);}
bool good(float2 v,float invalid){
 return all(isfinite(v))&&(!(abs(invalid)>0.0000001&&abs(invalid)<3.0e38)||all(v!=invalid));
}
bool loadMotion(bool middle,float2 uv,out float2 motionValue){
 motionValue=0;if(!inside(uv))return false;
 float2 inv=middle?middleInverseSize:currentInverseSize;
 int2 dimensions=int2(round(1.0/inv));float2 pixel=uv*dimensions-0.5;
 int2 base=int2(floor(pixel));float2 fraction=frac(pixel);
 int2 lo=clamp(base,int2(0,0),dimensions-1),hi=clamp(base+1,int2(0,0),dimensions-1);
 float2 a,b,c,d;
 if(middle){a=middleMotion.Load(int3(lo,0));b=middleMotion.Load(int3(hi.x,lo.y,0));
 c=middleMotion.Load(int3(lo.x,hi.y,0));d=middleMotion.Load(int3(hi,0));}
 else{a=currentMotion.Load(int3(lo,0));b=currentMotion.Load(int3(hi.x,lo.y,0));
 c=currentMotion.Load(int3(lo.x,hi.y,0));d=currentMotion.Load(int3(hi,0));}
 float invalid=middle?middleInvalid:currentInvalid;
 if(!good(a,invalid)||!good(b,invalid)||!good(c,invalid)||!good(d,invalid))return false;
 motionValue=lerp(lerp(a,b,fraction.x),lerp(c,d,fraction.x),fraction.y);
 motionValue=motionValue*(middle?middleScale:currentScale)+(middle?middleBias:currentBias);
 return all(isfinite(motionValue))&&all(abs(motionValue)<=2.0);
}
float2 composedAt(float2 p){
 float2 currentUV=p+currentJitter;float2 first=0,second=0;
 bool valid=inside(currentUV)&&loadMotion(false,currentUV,first);
 float2 q=p+first;
 if(compose!=0&&valid)valid=inside(q)&&loadMotion(true,q+middleJitter,second);
 float2 combined=first+second;
 if(resetHistory==0)valid=valid&&inside(p+combined);
 return valid?combined:float2(invalidOutput,invalidOutput);
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=width||id.y>=height)return;
 float2 p=(float2(id.xy)+0.5)*currentInverseSize;float2 currentUV=p+currentJitter;
 int2 depthPixel=clamp(int2(currentUV/currentInverseSize),int2(0,0),int2(width-1,height-1));
 float depth=currentDepth.Load(int3(depthPixel,0));resultDepth[id.xy]=isfinite(depth)?depth:0;
 resultMotion[id.xy]=composedAt(p);
 // Original SR color/depth stay on the native jittered current raster grid.
 srMotion[id.xy]=composedAt(p-currentJitter);
}
)HLSL";
bool same_device(ID3D12DeviceChild* child,ID3D12Device* expected) {
    ComPtr<ID3D12Device> actual;
    return child&&SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&actual)))&&actual.Get()==expected;
}
bool shape(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width&&d.Height&&d.Width<=16384&&d.Height<=16384&&
        d.MipLevels==1&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1&&!d.SampleDesc.Quality;
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    if(before==after)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
constexpr auto shared_read_state=D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_COPY_SOURCE);
bool matrix_inverse_valid(const float* a,const float* b) {
    for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++){
        double v=0;for(unsigned k=0;k<4;k++)v+=double(a[r*4+k])*b[k*4+c];
        if(!std::isfinite(v)||std::abs(v-(r==c?1:0))>0.0005)return false;
    }return true;
}
void multiply(const float* a,const float* b,float* result) {
    std::array<double,16> v{};
    for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++)for(unsigned k=0;k<4;k++)v[r*4+c]+=double(a[r*4+k])*b[k*4+c];
    for(unsigned i=0;i<16;i++)result[i]=float(v[i]);
}
void identity(float* p) {for(unsigned i=0;i<16;i++)p[i]=(i%5)==0?1.0f:0.0f;}
}

struct MotionRecorder::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    Log log;
};
MotionRecorder::MotionRecorder():impl_(std::make_unique<Impl>()){}
MotionRecorder::~MotionRecorder()=default;
bool MotionRecorder::initialize(ID3D12Device* device,Log log) {
    auto& p=*impl_;if(p.pipeline)return p.device.Get()==device;
    if(!device)return false;p.device=device;p.log=std::move(log);
    struct CompilerModule {HMODULE handle;~CompilerModule(){if(handle)FreeLibrary(handle);}} compiler{
        LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if(!compiler.handle){log_safe(p.log,"D3D shader compiler unavailable");return false;}
    const auto compile=reinterpret_cast<pD3DCompile>(GetProcAddress(compiler.handle,"D3DCompile"));
    ComPtr<ID3DBlob> code,error;
    const auto compiled=compile?compile(motion_shader,sizeof(motion_shader)-1,"original_cpvr_motion",nullptr,nullptr,
        "main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error):E_NOINTERFACE;
    if(FAILED(compiled)){log_safe(p.log,error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Motion compute compilation failed");return false;}
    std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,3};
    std::array<D3D12_ROOT_PARAMETER,2> parameters{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={2,ranges.data()};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants={0,0,24};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=parameters.data();
    ComPtr<ID3DBlob> signature;
    if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error))||
        FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&p.root))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC state{};state.pRootSignature=p.root.Get();state.CS={code->GetBufferPointer(),code->GetBufferSize()};
    return SUCCEEDED(device->CreateComputePipelineState(&state,IID_PPV_ARGS(&p.pipeline)));
}

std::optional<MotionRecorded> MotionRecorder::record(ID3D12GraphicsCommandList* list,const MotionRecordRequest& request) {
    auto& p=*impl_;
    if(!p.pipeline||!list||!same_device(list,p.device.Get())||list->GetType()==D3D12_COMMAND_LIST_TYPE_COPY||
        !validate_motion_request(request).valid)return {};
    const auto& current=*request.current.guides;const auto& previous=*request.previous.guides;
    const auto* middle=request.middle?request.middle->guides.get():&current;
    if(current.camera_flags[1]!=1||current.camera_flags[2]!=0||
       (request.middle&&(middle->camera_flags[1]!=1||middle->camera_flags[2]!=0))) {
        log_safe(p.log,"Motion composition requires measured dense2D native vectors including camera motion");return {};
    }
    const auto& current_motion=current.motion;const auto& middle_motion=middle->motion;
    for(const auto* image:{&current_motion,&middle_motion,&current.depth})
        if(!same_device(image->texture.Get(),p.device.Get()))return {};
    const auto cd=current_motion.texture->GetDesc(),md=middle_motion.texture->GetDesc(),dd=current.depth.texture->GetDesc();
    if(!shape(cd)||!shape(md)||!shape(dd)||cd.Width!=dd.Width||cd.Height!=dd.Height||
       // The existing Texture2D<float2> normalization reads XY only. RGBA16F
       // adds no interpretation of ZW and keeps the same units/sign/jitter.
       (cd.Format!=DXGI_FORMAT_R16G16_FLOAT&&cd.Format!=DXGI_FORMAT_R32G32_FLOAT&&cd.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)||
       (md.Format!=DXGI_FORMAT_R16G16_FLOAT&&md.Format!=DXGI_FORMAT_R32G32_FLOAT&&md.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)||
       (dd.Format!=DXGI_FORMAT_R32_FLOAT&&dd.Format!=DXGI_FORMAT_R32_TYPELESS)||
       ((cd.Flags|md.Flags|dd.Flags)&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return {};
    Extent previous_size{std::uint32_t(cd.Width),cd.Height};
    if(previous.motion.texture){const auto pd=previous.motion.texture->GetDesc();if(!shape(pd))return {};previous_size={std::uint32_t(pd.Width),pd.Height};}
    const Extent cs{std::uint32_t(cd.Width),cd.Height},ms{std::uint32_t(md.Width),md.Height};
    auto current_map=motion_normalization(current,request.middle?*middle:previous,cs,request.middle?ms:previous_size,request.current.convention);
    auto middle_map=request.middle?motion_normalization(*middle,previous,ms,previous_size,request.middle->convention):current_map;
    if(!current_map||!middle_map)return {};
    struct Lease {
        MotionRecordRequest inputs;std::shared_ptr<SourceGuides> result;GpuImage sr_motion;
        ComPtr<ID3D12DescriptorHeap> heap;
        ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    };
    auto lease=std::make_shared<Lease>();lease->inputs=request;lease->result=std::make_shared<SourceGuides>(current);
    lease->root=p.root;lease->pipeline=p.pipeline;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    auto create=[&](GpuImage& image,DXGI_FORMAT format){
        D3D12_RESOURCE_DESC d=cd;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Alignment=0;
        image={};image.resting_state=shared_read_state;
        return SUCCEEDED(p.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,shared_read_state,nullptr,IID_PPV_ARGS(&image.texture)));
    };
    if(!create(lease->result->motion,DXGI_FORMAT_R16G16_FLOAT)||!create(lease->result->depth,DXGI_FORMAT_R32_FLOAT)||
        !create(lease->sr_motion,DXGI_FORMAT_R16G16_FLOAT))return {};
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=6;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(p.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&lease->heap))))return {};
    const auto increment=p.device->GetDescriptorHandleIncrementSize(hd.Type);auto handle=lease->heap->GetCPUDescriptorHandleForHeapStart();
    auto srv=[&](ID3D12Resource* texture,DXGI_FORMAT format){
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
        p.device->CreateShaderResourceView(texture,&d,handle);handle.ptr+=increment;
    };
    srv(current_motion.texture.Get(),cd.Format);srv(middle_motion.texture.Get(),md.Format);srv(current.depth.texture.Get(),DXGI_FORMAT_R32_FLOAT);
    auto uav=[&](ID3D12Resource* texture,DXGI_FORMAT format){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(texture,nullptr,&d,handle);handle.ptr+=increment;};
    uav(lease->result->motion.texture.Get(),DXGI_FORMAT_R16G16_FLOAT);uav(lease->result->depth.texture.Get(),DXGI_FORMAT_R32_FLOAT);
    uav(lease->sr_motion.texture.Get(),DXGI_FORMAT_R16G16_FLOAT);
    // Compose row-major camera transforms in the same order as actual native links.
    auto& camera=lease->result->camera;
    if(request.reset){identity(camera.data()+48);identity(camera.data()+64);}
    else if(request.middle){
        multiply(current.camera.data()+48,middle->camera.data()+48,camera.data()+48);
        multiply(middle->camera.data()+64,current.camera.data()+64,camera.data()+64);
    }
    if(!matrix_inverse_valid(camera.data()+48,camera.data()+64))return {};
    bool lens_valid=true;for(unsigned i=32;i<48;i++)lens_valid&=std::isfinite(camera[i])&&std::abs(camera[i])<3.4e38f;
    if(!lens_valid)identity(camera.data()+32);
    for(unsigned i=84;i<86;i++)if(!std::isfinite(camera[i])||std::abs(camera[i])>=3.4e38f)camera[i]=0;
    camera[80]=camera[81]=0;camera[82]=camera[83]=1;camera[102]=65504;
    if(!std::isfinite(camera[103])||std::abs(camera[103])>=3.4e38f)camera[103]=40;
    for(float v:camera)if(!std::isfinite(v)||std::abs(v)>=3.4e38f)return {};
    lease->result->camera_flags[6]=0;lease->result->camera_flags[3]=request.reset?1u:0u;
    lease->result->reset=request.reset;
    // Allocate/validate everything before recording: caller never sees a partial pass.
    struct Constants {
        std::array<float,2> current_scale,middle_scale,current_bias,middle_bias,current_jitter,middle_jitter,current_inverse,middle_inverse;
        float current_invalid,middle_invalid;std::uint32_t compose,width,height;float invalid_output;std::uint32_t reset,padding;
    } constants{current_map->scale,middle_map->scale,current_map->bias,middle_map->bias,current_map->current_jitter_uv,middle_map->current_jitter_uv,
        {1.0f/float(cs.width),1.0f/float(cs.height)},{1.0f/float(ms.width),1.0f/float(ms.height)},
        current.camera[102],middle->camera[102],request.middle?1u:0u,cs.width,cs.height,65504,request.reset?1u:0u,0};
    static_assert(sizeof(Constants)==24*sizeof(std::uint32_t));
    std::vector<std::pair<const GpuImage*,D3D12_RESOURCE_STATES>> changed;
    for(const auto* image:{&current_motion,&middle_motion,&current.depth}) {
        if((image->resting_state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)!=0)continue;
        if(std::any_of(changed.begin(),changed.end(),[&](const auto& c){return c.first->texture.Get()==image->texture.Get();}))continue;
        changed.emplace_back(image,image->resting_state);
    }
    for(const auto& c:changed)transition(list,c.first->texture.Get(),c.second,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Native command recordings can be replayed. Targets must enter and leave
    // each execution in the same state, including the very first execution.
    for(const auto* output:{&lease->result->motion,&lease->result->depth,&lease->sr_motion})
        transition(list,output->texture.Get(),shared_read_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(p.pipeline.Get());list->SetComputeRootSignature(p.root.Get());ID3D12DescriptorHeap* heaps[]{lease->heap.Get()};
    list->SetDescriptorHeaps(1,heaps);list->SetComputeRootDescriptorTable(0,lease->heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1,24,&constants,0);list->Dispatch((cs.width+7)/8,(cs.height+7)/8,1);
    transition(list,lease->result->motion.texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,shared_read_state);
    transition(list,lease->result->depth.texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,shared_read_state);
    transition(list,lease->sr_motion.texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,shared_read_state);
    for(const auto& c:changed)transition(list,c.first->texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,c.second);
    return MotionRecorded{lease->result,lease->sr_motion,lease};
}

struct GenerationWorker::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool stopping{},retained_unsafe{};
    std::atomic<bool> history_reset_pending{};
    std::atomic<std::uint64_t> history_reset_requests{},history_resets{};
    GenerationConfig config;
    GenerationPolicy policy;
    std::optional<GenerationPolicy> pending_policy;
    std::uint64_t revision{},backend_epoch=1,epoch{},space_epoch{};
    std::int64_t latest_target_ns{}; // XR domain supplied by select(), never CPU steady_clock.
    ComPtr<ID3D12Device> device;
    Log log;
    struct QueuedInput {GenerationInput input;std::int64_t enqueued_ns{};};
    std::deque<QueuedInput> pending;
    // Reservations transfer an existing queue slot; neither owns raw guides.
    struct DeferredReal {std::shared_ptr<const EyeFrame> frame;std::int64_t enqueued_ns{};std::uint64_t revision{};};
    std::array<DeferredReal,2> deferred_real;
    struct ActiveReal {std::uint64_t source{},epoch{},space{},revision{};std::int64_t enqueued_ns{};unsigned eye{};};
    ActiveReal active_real;
    struct TimingWindows {PhaseDurationWindow queued,private_snapshot,backend_batch;};
    std::array<TimingWindows,6> timing_windows;
    struct Stored {GenerationProduct product;bool selected{};};
    std::array<std::deque<Stored>,2> products;
    std::array<std::uint64_t,2> real_publication_source{};
    std::array<std::int64_t,2> real_publication_time{};
    // This is the existing committed cursor, published as one immutable unit.
    // It is never populated by a proposal or an unacknowledged vendor output.
    struct CommittedCursor {GenerationCursor cursor;GenerationPolicy policy;};
    std::atomic<std::shared_ptr<const CommittedCursor>> committed_cursor;
    GenerationCursor cursor_snapshot() const {
        const auto saved=committed_cursor.load(std::memory_order_acquire);
        return saved?saved->cursor:GenerationCursor{};
    }
    struct Proposal {GenerationSelection selection;GenerationCursor cursor;GenerationPolicy policy;std::uint64_t revision{};};
    std::shared_ptr<const Proposal> proposal;
    std::atomic<std::shared_ptr<const Proposal>> published_proposal;
    struct Acceptance {std::shared_ptr<const Proposal> proposal;std::int64_t first_display_time{};};
    std::atomic<std::shared_ptr<const Acceptance>> pending_acceptance;
    void set_proposal(Proposal value){
        proposal=std::make_shared<const Proposal>(std::move(value));
        published_proposal.store(proposal,std::memory_order_release);
    }
    void clear_proposal(){published_proposal.store({},std::memory_order_release);proposal.reset();}
    std::uint64_t next_pair_sequence{};
    GenerationStatus state;
    enum SelectionCounter {SC_Calls,SC_Busy,SC_New,SC_Proposal,SC_Repeat,SC_NoHistory,SC_Expired,SC_Other,SC_LockRepeat,SC_Count};
    std::array<std::atomic<std::uint64_t>,SC_Count> selection_counts{};
    void retire_expired_locked(std::int64_t target) {
        if(const auto saved=committed_cursor.load(std::memory_order_acquire);saved&&saved->cursor.last_selection){
            const auto oldest=std::min(saved->cursor.eye_times[0],saved->cursor.eye_times[1]);
            if(target>oldest&&target-oldest>saved->policy.maximum_age_ns){
                auto metadata=std::make_shared<CommittedCursor>(*saved);
                metadata->cursor.last_selection.reset();committed_cursor.store(std::move(metadata),std::memory_order_release);
            }
        }
        if(proposal){
            const auto& frame=*proposal->selection.frame;
            const auto oldest=std::min(frame.eyes[0]->content_time_ns,frame.eyes[1]->content_time_ns);
            if(proposal->revision!=revision || (target>=proposal->cursor.display_time_ns && target>oldest &&
               target-oldest>proposal->policy.maximum_age_ns))clear_proposal();
        }
        for(auto& stored:products){const auto before=stored.size();
            stored.erase(std::remove_if(stored.begin(),stored.end(),[&](const auto& item){
                const auto time=item.product.frame->content_time_ns;return target>time&&target-time>policy.maximum_age_ns;}),stored.end());
            state.expired_products+=before-stored.size();
        }
    }
    void count_selection(const GenerationSelection& result,std::int64_t target,bool proposal_retry=false) noexcept {
        if(result.frame){++selection_counts[proposal_retry?SC_Proposal:(result.new_presentation?SC_New:SC_Repeat)];return;}
        const auto saved=committed_cursor.load(std::memory_order_acquire);
        if(!saved||!saved->cursor.valid||!saved->cursor.last_selection){++selection_counts[SC_NoHistory];return;}
        if(target-std::min(saved->cursor.eye_times[0],saved->cursor.eye_times[1])>saved->policy.maximum_age_ns)++selection_counts[SC_Expired];
        else ++selection_counts[SC_Other];
    }
    ResourceAccounting private_textures;
    std::unique_ptr<TexturePool> input_pool,output_pool;
    std::atomic<std::shared_ptr<const GenerationStatus>> published;
    struct Backend {
        HMODULE module{};cpvr_nv_configure_fn configure{};cpvr_nv_attach_fn attach{};
        cpvr_nv_run_fn run{};cpvr_nv_run_ready_fn run_ready{};cpvr_nv_status_fn status{};cpvr_nv_queue_fn queue{};cpvr_nv_fence_fn fence{};cpvr_nv_detach_fn detach{};
        cpvr_nv_abort_unstarted_attach_fn abort_unstarted_attach{};
    } backend;
    ComPtr<ID3D12Fence> copy_fence;
    HANDLE copy_event{};std::uint64_t copy_value{};
    MotionRecorder recorder;
    struct Snapshot {
        GenerationInput input;ComPtr<ID3D12Resource> color;
        std::shared_ptr<const SourceGuides> guides;
        std::uint64_t local_sequence{},ready_value{};
        std::vector<std::shared_ptr<PooledTexture>> pooled;
    };
    std::array<std::optional<Snapshot>,2> previous;
    struct Work {
        GenerationInput input;std::optional<Snapshot> old;
        // Raw MAIN guides belong to this executable recording, not to the
        // long-lived same-eye snapshot. A successful Reset/discard retires them.
        std::shared_ptr<void> motion_recording;
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        std::optional<Snapshot> current;
        std::shared_ptr<PooledTexture> presentation;
        std::shared_ptr<const EyeFrame> presentation_frame;
        std::array<std::shared_ptr<PooledTexture>,5> outputs;
    };
    std::shared_ptr<Work> active_work;
    // Only this owner submits this private recording. Keep its dependencies even
    // after completion until successful Reset retires the replayable recording.
    std::shared_ptr<Work> recorded_work;
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    ComPtr<ID3D12GraphicsCommandList> copy_list;
    bool copy_list_closed{};

    void publish_locked() {
        state.active_policy=policy;state.requested_policy=pending_policy?*pending_policy:policy;state.policy_pending=bool(pending_policy);
        if(!policy.generated_per_interval){state.active_policy.presentation_pattern=PresentationPattern::Custom;state.active_policy.custom_sequence="RR";}
        state.queued_inputs=unsigned(pending.size()+bool(deferred_real[0].frame)+bool(deferred_real[1].frame)+bool(active_real.source));state.ready_generated_unpaired=0;
        for(const auto& eye:products)for(const auto& p:eye)if(p.product.frame->generated&&!p.selected)++state.ready_generated_unpaired;
        published.store(std::make_shared<const GenerationStatus>(state),std::memory_order_release);
    }
    void phase(const std::string& message,bool failed=false) {
        {std::lock_guard lock(mutex);state.phase=message;state.failed|=failed;publish_locked();}log_safe(log,message);
    }
    bool ready(const GpuImage& image) const {
        if(!image.texture||!image.ready_fence||!image.ready_value||image.ready_value==UINT64_MAX||
           !same_device(image.texture.Get(),device.Get())||!same_device(image.ready_fence.Get(),device.Get()))return false;
        const auto completed=image.ready_fence->GetCompletedValue();return completed!=UINT64_MAX&&completed>=image.ready_value;
    }
    bool input_ready(const GenerationInput& in) const {
        if(!in.frame||!ready(in.frame->color))return false;
        if(!in.frame->guides)return true;
        if(!ready(in.frame->guides->motion)||!ready(in.frame->guides->depth))return false;
        if(in.immediate_previous.frame&&in.immediate_previous.frame->guides&&
           !ready(in.immediate_previous.frame->guides->motion))return false;
        return true;
    }
    template<class T>bool symbol(T& f,const char* name) {
        f=reinterpret_cast<T>(GetProcAddress(backend.module,name));return f!=nullptr;
    }
    bool attach_backend() {
        backend.module=LoadLibraryExW(config.backend_library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!backend.module)return false;
        if(!symbol(backend.configure,"cpvr_nv_configure")||!symbol(backend.attach,"fg_nv_attach")||!symbol(backend.run,"fg_nv_run")||
           !symbol(backend.status,"fg_nv_get_status")||!symbol(backend.queue,"fg_nv_queue")||!symbol(backend.fence,"fg_nv_fence")||
           !symbol(backend.detach,"fg_nv_detach"))return false;
        symbol(backend.run_ready,"fg_nv_run_ready"); // Optional; the pinned legacy ABI remains usable.
        symbol(backend.abort_unstarted_attach,"fg_nv_abort_unstarted_attach");
        if(!backend.configure(config.driver_library.c_str(),config.driver_sha256.c_str(),config.snippet_sha256.c_str(),config.enable_verified_compatibility_gate?1:0))return false;
        if(!backend.attach(device.Get(),config.snippet_directory.c_str(),config.initialize_ngx?1:0)) {
            const auto rejected=backend.status(); // Abort may reset private state.
            bool clean_rejection=false;
            try {if(backend.abort_unstarted_attach)clean_rejection=backend.abort_unstarted_attach()!=0;}
            catch(...) {clean_rejection=false;}
            retained_unsafe=!clean_rejection;
            phase(std::string("NVIDIA attach failed: ")+rejected.message+
                (clean_rejection?"; pre-NGX rejection cleaned":"; partial backend retained"),true);
            return false;
        }
        phase(std::string("NVIDIA ")+backend.status().message);
        if(!backend.queue()||!backend.fence()||!same_device(backend.queue(),device.Get()))return false;
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copy_fence))))return false;
        copy_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);return copy_event!=nullptr;
    }
    void reset_locked() {
        for(auto& p:products)p.clear();for(auto& p:previous)p.reset();
        real_publication_source={};real_publication_time={};
        deferred_real={};
        committed_cursor.store({},std::memory_order_release);clear_proposal();pending_acceptance.store({});++backend_epoch;
    }
    // Generation-owner step only, between jobs. Requesters only set an atomic
    // gate; they never free renderer leases or wait for this GPU owner.
    bool reset_history_on_owner() {
        if(!history_reset_pending.load(std::memory_order_acquire) || retained_unsafe)return false;
        if(copy_fence && copy_value) {
            const auto done=copy_fence->GetCompletedValue();
            if(done==UINT64_MAX || done<copy_value) {
                retained_unsafe=true;phase("History reset cannot retire unproven private copy work",true);return false;
            }
        }
        // A closed list still owns its recorded descriptors/resources. Drop
        // the completed recording before dropping the dependencies it used.
        if(recorded_work)recorded_work->list.Reset();
        if(active_work)active_work->list.Reset();
        copy_list.Reset();copy_allocator.Reset();recorded_work.reset();active_work.reset();copy_list_closed=true;
        std::lock_guard lock(mutex);
        ++revision;pending.clear();active_real={};reset_locked();state.latest_input_time_ns={};
        state.phase="Generation history reset; waiting for new source epoch";
        history_resets.fetch_add(1,std::memory_order_relaxed);
        history_reset_pending.store(false,std::memory_order_release);publish_locked();return true;
    }
    bool consume_acceptance_locked(){
        if(history_reset_pending.load(std::memory_order_acquire))return false;
        const auto accepted=pending_acceptance.exchange({},std::memory_order_acq_rel);
        if(!accepted||stopping||!proposal||accepted->proposal!=proposal)return false;
        const auto& frame=*proposal->selection.frame;const auto origins=proposal->selection.origins;
        auto committed=std::make_shared<CommittedCursor>();committed->cursor=proposal->cursor;
        const auto previous=cursor_snapshot();
        if(previous.valid&&previous.display_time_ns>0){
            const auto lag=committed->cursor.display_time_ns-committed->cursor.content_clock_ns;
            committed->cursor.content_clock_ns=accepted->first_display_time-lag;
        }
        committed->cursor.display_time_ns=accepted->first_display_time;
        // This policy was actually submitted, even if a newer UI request is
        // already pending. Keep that newer request, but don't lose the shown A.
        policy=proposal->policy;
        if(pending_policy&&proposal->revision==revision&&proposal->policy==*pending_policy)pending_policy.reset();
        committed->policy=policy;committed_cursor.store(std::move(committed),std::memory_order_release);
        ++state.selected_pairs;if(origins[0].generated||origins[1].generated)++state.generated_selected_pairs;
        for(unsigned eye=0;eye<2;eye++)for(auto& product:products[eye])if(product.product.frame==frame.eyes[eye])product.selected=true;
        clear_proposal();publish_locked();return true;
    }
    bool create_texture(const D3D12_RESOURCE_DESC& source,DXGI_FORMAT format,ComPtr<ID3D12Resource>& out,
                        std::vector<std::shared_ptr<PooledTexture>>& leases) {
        auto d=source;d.Format=format;d.Alignment=0;d.Flags=D3D12_RESOURCE_FLAG_NONE;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        auto resource=input_pool->acquire(device.Get(),d,backend_epoch);if(!resource)return false;
        out=resource->texture;leases.push_back(std::move(resource));return true;
    }
    bool copy_image(ID3D12GraphicsCommandList* list,const GpuImage& source,ID3D12Resource* target) {
        // Shared immutable reads must already permit COPY_SOURCE. We never take
        // a shared game/XR resource into a conflicting state on another queue.
        if((source.resting_state&D3D12_RESOURCE_STATE_COPY_SOURCE)==0)return false;
        transition(list,target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(target,source.texture.Get());
        transition(list,target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);return true;
    }
    static MotionGuideSample motion_sample(const GenerationInput& in) {
        return {in.frame?in.frame->guides:nullptr,in.frame?in.frame->source_id:0,in.native_frame_token,
            in.previous_native_frame_token,in.guides_previous_source_id,in.frame?in.frame->content_time_ns:0,
            in.guides_previous_time_ns,in.motion_convention};
    }
    bool retire_completed_copy_recording(){
        if(!recorded_work)return true;
        if(retained_unsafe||!copy_list||!copy_list_closed)return false;
        if(copy_value&&(!copy_fence||copy_fence->GetCompletedValue()==UINT64_MAX||copy_fence->GetCompletedValue()<copy_value)){
            retained_unsafe=true;phase("Private copy retirement lacks completed fence; dependencies retained",true);return false;
        }
        // Waiting for a NEW input to Reset would pin a producer ring cell while
        // that producer may itself be waiting for this lease. Retire the closed
        // recording now, after completion, without submitting any empty work.
        // Do not reset its allocator until the next real snapshot uses it.
        if(FAILED(copy_list->Reset(copy_allocator.Get(),nullptr))){
            retained_unsafe=true;phase("Completed private copy Reset failed; dependencies retained",true);return false;
        }
        copy_list_closed=false;recorded_work.reset();
        if(FAILED(copy_list->Close())){
            retained_unsafe=true;phase("Empty private copy Close failed after retirement",true);return false;
        }
        copy_list_closed=true;
        {std::lock_guard lock(mutex);++state.copy_command_resets;}
        return true;
    }
    bool snapshot_input(const GenerationInput& input,const std::optional<Snapshot>& old,bool span_valid,ID3D12CommandQueue* queue,bool presentation=false) {
        auto work=std::make_shared<Work>();work->input=input;work->old=old;active_work=work;
        const auto& frame=*input.frame;if(!frame.guides||!frame.guides->camera_valid)return false;
        const auto color_desc=frame.color.texture->GetDesc();const auto motion_desc=frame.guides->motion.texture->GetDesc();
        const auto depth_desc=frame.guides->depth.texture->GetDesc();
        // Existing private CopyResource keeps the RGBA8 bits. sRGB/typeless
        // source views belong to this one compatible family; no gamma pass
        // or BGRA conversion is implied. EyeFrame metadata retains encoding.
        const bool rgba8=color_desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM ||
            color_desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || color_desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if(!shape(color_desc)||!shape(motion_desc)||!shape(depth_desc)||!rgba8||
           motion_desc.Width!=depth_desc.Width||motion_desc.Height!=depth_desc.Height)return false;
        if(presentation){
            auto d=color_desc;d.Alignment=0;d.Flags=D3D12_RESOURCE_FLAG_NONE;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
            work->presentation=output_pool->acquire(device.Get(),d,backend_epoch,D3D12_RESOURCE_STATE_GENERIC_READ);
            if(!work->presentation)return false; // Never fall back to a raw REAL pin.
        }
        if(copy_list){
            // snapshot_input waits for its own last submission; failure leaves
            // retained_unsafe set and stops this worker before a second Reset.
            if(FAILED(copy_allocator->Reset())||FAILED(copy_list->Reset(copy_allocator.Get(),nullptr))){
                retained_unsafe=true;phase("Private copy Reset failed; recording dependencies retained",true);return false;
            }
            recorded_work.reset();
            std::lock_guard lock(mutex);++state.copy_command_resets;
        }else{
            if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&copy_allocator)))||
               FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&copy_list))))return false;
            std::lock_guard lock(mutex);++state.copy_command_creations;
        }
        copy_list_closed=false;work->allocator=copy_allocator;work->list=copy_list;recorded_work=work;
        Snapshot current;current.input=input;current.local_sequence=old?old->local_sequence+2:2;
        // The recording owns even a partially built snapshot until Reset.
        work->current=current;auto& owned=*work->current;
        if(current.local_sequence<2||!create_texture(color_desc,DXGI_FORMAT_R8G8B8A8_UNORM,current.color,owned.pooled)||
           !copy_image(work->list.Get(),frame.color,current.color.Get()))return false;
        if(work->presentation){
            transition(work->list.Get(),work->presentation->texture.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_STATE_COPY_DEST);
            work->list->CopyResource(work->presentation->texture.Get(),frame.color.texture.Get());
            transition(work->list.Get(),work->presentation->texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        if(input.motion_convention==MotionConvention::BackwardUvNormalized) {
            if(motion_desc.Format!=DXGI_FORMAT_R16G16_FLOAT||
                (depth_desc.Format!=DXGI_FORMAT_R32_FLOAT&&depth_desc.Format!=DXGI_FORMAT_R32_TYPELESS)||
                frame.guides->camera[82]!=1||frame.guides->camera[83]!=1||frame.guides->camera_flags[6])return false;
            auto guides=std::make_shared<SourceGuides>(*frame.guides);
            guides->motion={};guides->depth={};
            if(!create_texture(motion_desc,DXGI_FORMAT_R16G16_FLOAT,guides->motion.texture,owned.pooled)||
               !create_texture(depth_desc,DXGI_FORMAT_R32_FLOAT,guides->depth.texture,owned.pooled)||
               !copy_image(work->list.Get(),frame.guides->motion,guides->motion.texture.Get())||
               !copy_image(work->list.Get(),frame.guides->depth,guides->depth.texture.Get()))return false;
            current.guides=guides;
        } else {
            MotionRecordRequest request;request.current=motion_sample(input);
            request.previous=old?motion_sample(old->input):request.current;request.reset=!span_valid;
            if(span_valid&&input.immediate_previous.frame) {
                const auto& mid=input.immediate_previous;request.middle=MotionGuideSample{mid.frame->guides,mid.frame->source_id,
                    mid.native_frame_token,mid.previous_native_frame_token,mid.guides_previous_source_id,
                    mid.frame->content_time_ns,mid.guides_previous_time_ns,mid.motion_convention};
            }
            if(!request.reset&&!validate_motion_request(request).valid)return false;
            if(!recorder.initialize(device.Get(),log))return false;
            const auto recorded=recorder.record(work->list.Get(),request);if(!recorded)return false;
            current.guides=recorded->guides;work->motion_recording=recorded->lifetime;
        }
        current.pooled=owned.pooled;work->current=current;
        if(FAILED(work->list->Close())){retained_unsafe=true;phase("Private copy Close failed; recording retained",true);return false;}
        copy_list_closed=true;
        for(const auto* image:{&frame.color,&frame.guides->motion,&frame.guides->depth})
            if(FAILED(queue->Wait(image->ready_fence.Get(),image->ready_value)))return false;
        if(input.immediate_previous.frame&&input.immediate_previous.frame->guides) {
            const auto& motion=input.immediate_previous.frame->guides->motion;
            if(FAILED(queue->Wait(motion.ready_fence.Get(),motion.ready_value)))return false;
        }
        for(const auto& resource:current.pooled)resource->pending();
        if(work->presentation)work->presentation->pending();
        ID3D12CommandList* lists[]{work->list.Get()};queue->ExecuteCommandLists(1,lists);
        retained_unsafe=true;current.ready_value=++copy_value;
        if(FAILED(queue->Signal(copy_fence.Get(),copy_value))||
           FAILED(copy_fence->SetEventOnCompletion(copy_value,copy_event))||WaitForSingleObject(copy_event,5000)!=WAIT_OBJECT_0||
           copy_fence->GetCompletedValue()==UINT64_MAX||copy_fence->GetCompletedValue()<copy_value) {
            phase("Generation private snapshot completion unproven; retained GPU work",true);return false;
        }
        for(const auto& resource:current.pooled)if(!resource->completion(copy_fence.Get(),current.ready_value)){
            phase("Private copy fence identity rejected; retained GPU work",true);return false;
        }
        if(work->presentation){
            if(!work->presentation->completion(copy_fence.Get(),current.ready_value)){
                phase("Presentation copy fence identity rejected; retained GPU work",true);return false;
            }
            auto real=std::make_shared<EyeFrame>(frame);real->guides.reset();
            real->color={work->presentation->texture,copy_fence,current.ready_value,D3D12_RESOURCE_STATE_GENERIC_READ,work->presentation};
            real->color.source_format=frame.color.source_format;real->color.transfer=frame.color.transfer;
            work->presentation_frame=std::move(real);
        }
        // The completed private Snapshot owns its color in current.color.
        // Historical input is used only for source/pose/timing and RAW camera
        // normalization metadata. Its previous jitter must not be replaced by
        // the normalized guides (whose jitter has already been removed).
        // Work::input still retains dependencies of the replayable recording
        // until its successful Reset. Every unknown-completion return above
        // preserves both that recording and the original source lease.
        auto history_frame=std::make_shared<EyeFrame>(*current.input.frame);
        history_frame->color={};
        auto metadata=std::make_shared<SourceGuides>(*history_frame->guides);
        metadata->depth={};
        // Only GetDesc() is needed from the old motion texture. No old pixel is
        // sampled: current MV + immediate-previous MV + current depth are the
        // three actual GPU inputs. COM ownership preserves the immutable desc,
        // while releasing its pool lease allows the producer to overwrite it.
        metadata->motion.lifetime.reset();metadata->motion.ready_fence.Reset();metadata->motion.ready_value=0;
        history_frame->guides=std::move(metadata);current.input.frame=std::move(history_frame);
        current.input.immediate_previous={};
        retained_unsafe=false;work->current=current;return true;
    }
    cpvr_nv_frame native_frame(const Snapshot& s) const {
        cpvr_nv_frame f{};f.color=s.color.Get();f.motion=s.guides->motion.texture.Get();f.depth=s.guides->depth.texture.Get();
        f.resting[0]=D3D12_RESOURCE_STATE_COMMON;f.resting[1]=s.guides->motion.resting_state;f.resting[2]=s.guides->depth.resting_state;
        f.ready[0]=copy_fence.Get();f.ready_value[0]=s.ready_value;
        // Backend-only historical index. Real source IDs/times remain untouched.
        f.source_f=s.local_sequence;f.owner_runtime=std::uint64_t(reinterpret_cast<std::uintptr_t>(this));f.feature=std::uint32_t(s.input.frame->eye);
        std::copy(s.guides->camera.begin(),s.guides->camera.end(),f.camera.f);
        std::copy(s.guides->camera_flags.begin(),s.guides->camera_flags.end(),f.camera.flags);return f;
    }
    bool span_matches(const GenerationInput& input,const Snapshot& old,const GenerationPolicy& active) const {
        const auto& a=*old.input.frame;const auto& b=*input.frame;
        static const bool historyDiagnostic=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
        const auto reject=[&](const char* reason) {
            if(historyDiagnostic&&log){
                static thread_local std::uint64_t failures{};
                const auto ordinal=++failures;
                if(ordinal<=8||ordinal%180==0){
                    char report[1600]{};
                    std::snprintf(report,sizeof(report),"[OWN-NV-SPAN-REJECT] n=%llu reason=%s eye=%u source=%llu/%llu time=%lld/%lld native=%llu/%llu previousNative=%llu/%llu guidesPreviousSource=%llu/%llu guidesPreviousTime=%lld/%lld convention=%u/%u flag3=%u/%u reset=%u/%u epoch=%llu/%llu spaceEpoch=%llu/%llu maxIntervalNs=%lld middle=%u",
                        (unsigned long long)ordinal,reason,unsigned(b.eye),
                        (unsigned long long)a.source_id,(unsigned long long)b.source_id,(long long)a.content_time_ns,(long long)b.content_time_ns,
                        (unsigned long long)old.input.native_frame_token,(unsigned long long)input.native_frame_token,
                        (unsigned long long)old.input.previous_native_frame_token,(unsigned long long)input.previous_native_frame_token,
                        (unsigned long long)old.input.guides_previous_source_id,(unsigned long long)input.guides_previous_source_id,
                        (long long)old.input.guides_previous_time_ns,(long long)input.guides_previous_time_ns,
                        unsigned(old.input.motion_convention),unsigned(input.motion_convention),
                        a.guides?a.guides->camera_flags[3]:0,b.guides?b.guides->camera_flags[3]:0,
                        unsigned(a.guides&&a.guides->reset),unsigned(b.guides&&b.guides->reset),
                        (unsigned long long)a.epoch,(unsigned long long)b.epoch,(unsigned long long)a.space_epoch,(unsigned long long)b.space_epoch,
                        (long long)active.maximum_interval_ns,unsigned(bool(input.immediate_previous.frame)));
                    try {log_safe(log,report);}catch(...) {} // Diagnostics must not change admission.
                }
            }
            return false;
        };
        if(b.source_id<=a.source_id||b.content_time_ns<=a.content_time_ns||b.content_time_ns-a.content_time_ns>active.maximum_interval_ns||
           b.epoch!=a.epoch||b.space_epoch!=a.space_epoch||!b.guides||b.guides->reset||b.guides->camera_flags[3])
            return reject(b.source_id<=a.source_id?"source_not_increasing":b.content_time_ns<=a.content_time_ns?"time_not_increasing":
                b.content_time_ns-a.content_time_ns>active.maximum_interval_ns?"interval_too_large":b.epoch!=a.epoch?"epoch_mismatch":
                b.space_epoch!=a.space_epoch?"space_epoch_mismatch":!b.guides?"missing_guides":b.guides->reset?"guides_reset":"camera_flag3_reset");
        if(input.motion_convention==MotionConvention::BackwardUvNormalized){
            const bool matched=input.guides_previous_source_id==a.source_id&&input.guides_previous_time_ns==a.content_time_ns;
            return matched?true:reject(input.guides_previous_source_id!=a.source_id?"normalized_previous_source_mismatch":"normalized_previous_time_mismatch");
        }
        MotionRecordRequest request;request.previous=motion_sample(old.input);request.current=motion_sample(input);
        if(input.immediate_previous.frame) {const auto& mid=input.immediate_previous;
            request.middle=MotionGuideSample{mid.frame->guides,mid.frame->source_id,mid.native_frame_token,mid.previous_native_frame_token,
                mid.guides_previous_source_id,mid.frame->content_time_ns,mid.guides_previous_time_ns,mid.motion_convention};}
        const auto validation=validate_motion_request(request);
        return validation.valid?true:reject(validation.message.c_str());
    }
    void add_locked(const GenerationProduct& product) {
        // XR products consume only color/pose/time. Retaining raw guides here
        // would pin all four MAIN records even after their GPU copies finish.
        auto display=product;
        if(display.frame->guides){auto frame=std::make_shared<EyeFrame>(*display.frame);frame->guides.reset();display.frame=std::move(frame);}
        auto& row=products[std::uint32_t(product.frame->eye)];row.push_back({std::move(display),false});
        while(row.size()>config.products_per_eye)row.pop_front();
        // Real colors occupy the producer's bounded capture pool. Keep at most
        // two real images per eye so presentation history cannot exhaust it.
        unsigned real=unsigned(std::count_if(row.begin(),row.end(),[](const auto&p){return !p.product.frame->generated;}));
        while(real>2){const auto old=std::find_if(row.begin(),row.end(),[](const auto&p){return !p.product.frame->generated;});row.erase(old);--real;}
        const auto time=product.frame->content_time_ns;
        for(auto& eye:products)while(!eye.empty()&&time>eye.front().product.frame->content_time_ns&&
            time-eye.front().product.frame->content_time_ns>policy.maximum_age_ns)eye.pop_front();
    }
    std::size_t queue_slots_locked() const {return pending.size()+bool(deferred_real[0].frame)+bool(deferred_real[1].frame)+bool(active_real.source);}
    bool private_real_locked(const GenerationInput& input) const {
        const auto generated_stereo=[](const GenerationPolicy& p){return p.generated_per_interval&&p.cadence==Cadence::StereoPairs;};
        return input.frame&&input.frame->guides&&(generated_stereo(policy)||(pending_policy&&generated_stereo(*pending_policy)));
    }
    bool ordered_real_locked() const {
        if(pending_policy||!policy.generated_per_interval||policy.cadence!=Cadence::StereoPairs)return false;
        const auto pattern=pattern_of(policy);
        return pattern.size()==policy.generated_per_interval+1&&!pattern[0][0]&&!pattern[0][1]&&
            std::all_of(pattern.begin()+1,pattern.end(),[](const auto& step){return step[0]&&step[1];});
    }
    bool earlier_real_locked(const EyeFrame& frame) const {
        const auto eye=unsigned(frame.eye);
        const auto earlier=[&](const EyeFrame& f){return f.eye==frame.eye&&f.epoch==frame.epoch&&f.space_epoch==frame.space_epoch&&
            f.source_id>real_publication_source[eye]&&f.source_id<frame.source_id;};
        if(active_real.source>real_publication_source[eye]&&active_real.source<frame.source_id&&active_real.eye==eye&&
           active_real.epoch==frame.epoch&&active_real.space==frame.space_epoch)return true;
        if(deferred_real[eye].frame&&earlier(*deferred_real[eye].frame))return true;
        return std::any_of(pending.begin(),pending.end(),[&](const auto& q){return earlier(*q.input.frame);});
    }
    bool anchor_blocked_locked(const EyeFrame& frame) const {
        if(frame.epoch!=epoch||frame.space_epoch!=space_epoch)return false;
        const auto eye=unsigned(frame.eye);const auto& row=products[eye];
        const auto oldest=std::find_if(row.begin(),row.end(),[](const auto& p){return !p.product.frame->generated;});
        if(oldest==row.end()||std::count_if(row.begin(),row.end(),[](const auto& p){return !p.product.frame->generated;})<2)return false;
        const auto c=cursor_snapshot();
        return !(c.valid&&c.epoch==epoch&&c.space_epoch==space_epoch&&c.eye_times[eye]>oldest->product.frame->content_time_ns);
    }
    bool real_expired_locked(const EyeFrame& f,std::int64_t enqueued,std::int64_t now) const {
        return now-enqueued>policy.maximum_age_ns||
            (latest_target_ns>f.content_time_ns&&latest_target_ns-f.content_time_ns>policy.maximum_age_ns);
    }
    void pump_real_locked() {
        if(retained_unsafe||stopping||history_reset_pending.load(std::memory_order_acquire))return;
        consume_acceptance_locked();const auto now=cpu_time_ns();
        for(auto& item:deferred_real)if(item.frame){
            const auto& f=*item.frame;
            if(!ordered_real_locked()||item.revision!=revision||f.epoch!=epoch||f.space_epoch!=space_epoch||real_expired_locked(f,item.enqueued_ns,now)){
                item={};++state.dropped_inputs;
            }else if(publish_real_locked(item.frame,false))item={};
        }
    }
    bool take_pending_locked(GenerationInput& input,GenerationPolicy& active,std::uint64_t& job_revision) {
        pump_real_locked();if(stopping||history_reset_pending.load(std::memory_order_acquire))return false;
        const bool ordered=ordered_real_locked();const auto now=cpu_time_ns();
        if(ordered)for(auto it=pending.begin();it!=pending.end();) {
            // An aged REAL may still be the reference of a fresh generated midpoint.
            if(now-it->enqueued_ns>policy.maximum_age_ns){it=pending.erase(it);++state.dropped_inputs;}else ++it;
        }
        auto next=std::find_if(pending.begin(),pending.end(),[&](const auto& q){
            const auto& f=*q.input.frame;
            return input_ready(q.input)&&(!ordered||(!deferred_real[unsigned(f.eye)].frame&&!earlier_real_locked(f)));
        });
        if(next==pending.end()){publish_locked();return false;}
        input=next->input;active=pending_policy?*pending_policy:policy;job_revision=revision;
        if((ordered||private_real_locked(input))&&input.frame->source_id>real_publication_source[unsigned(input.frame->eye)]){
            const auto& f=*input.frame;active_real={f.source_id,f.epoch,f.space_epoch,job_revision,next->enqueued_ns,unsigned(f.eye)};
        }
        auto& timing=timing_windows[active.generated_per_interval];timing.queued.observe(next->enqueued_ns,cpu_time_ns());
        state.timings[active.generated_per_interval].queued=timing.queued.summary();pending.erase(next);publish_locked();return true;
    }
    void finish_real_locked(std::shared_ptr<const EyeFrame> frame) {
        const auto saved=active_real;active_real={};
        if(saved.source&&frame&&!retained_unsafe&&!stopping&&!history_reset_pending.load(std::memory_order_acquire)&&
           saved.revision==revision&&frame->epoch==epoch&&frame->space_epoch==space_epoch&&
           !real_expired_locked(*frame,saved.enqueued_ns,cpu_time_ns())&&frame->source_id>real_publication_source[unsigned(frame->eye)]) {
            if(!ordered_real_locked())publish_real_locked(frame,false);
            else {auto& slot=deferred_real[unsigned(frame->eye)];
                if(!slot.frame)slot={std::move(frame),saved.enqueued_ns,saved.revision};else ++state.dropped_inputs;}
        }else if(saved.source&&frame&&frame->source_id>real_publication_source[unsigned(frame->eye)])++state.dropped_inputs;
        pump_real_locked();publish_locked();
    }
    bool publish_real_locked(const std::shared_ptr<const EyeFrame>& frame,bool early){
        // Epoch/history reset remains owned by process(). A completed image of
        // that same epoch can reach XR without waiting behind NVIDIA evaluation.
        if(history_reset_pending.load(std::memory_order_acquire)||!frame||frame->generated||!frame_valid(*frame)||frame->epoch!=epoch||frame->space_epoch!=space_epoch||!ready(frame->color))return false;
        const auto eye=std::uint32_t(frame->eye);
        if(frame->source_id<=real_publication_source[eye]||frame->content_time_ns<=real_publication_time[eye])return false;
        if(ordered_real_locked()&&(earlier_real_locked(*frame)||anchor_blocked_locked(*frame)))return false;
        GenerationOrigin origin{false,frame->source_id,frame->source_id,frame->stereo_pair_id,frame->stereo_pair_id,
            frame->content_time_ns,frame->content_time_ns,0,0};
        add_locked({frame,origin});real_publication_source[eye]=frame->source_id;real_publication_time[eye]=frame->content_time_ns;
        if(early)++state.real_published_early;else ++state.real_published_worker;
        return true;
    }
    bool enqueue_locked(const GenerationInput& input){
        if(history_reset_pending.load(std::memory_order_acquire)||stopping||!state.worker_running||state.failed||!input.frame||input.frame->generated||
           !frame_valid(*input.frame)||!input.frame->color.texture||!input.frame->color.ready_fence)return false;
        if(queue_slots_locked()>=config.queue_capacity){if(pending.empty())return false;pending.pop_front();++state.dropped_inputs;}
        if(!private_real_locked(input))publish_real_locked(input.frame,true);
        state.latest_input_time_ns[std::uint32_t(input.frame->eye)]=input.frame->content_time_ns;
        pending.push_back({input,cpu_time_ns()});++state.accepted_inputs;publish_locked();return true;
    }
    bool publish_generated_locked(const GenerationProduct& product,std::uint64_t job_revision,bool early,std::uint64_t batch_completed){
        if(history_reset_pending.load(std::memory_order_acquire))return false;
        if(stopping||job_revision!=revision||!product.frame||!product.frame->generated||
           product.frame->epoch!=epoch||product.frame->space_epoch!=space_epoch||
           !product_valid(product,product.frame->eye)||!ready(product.frame->color))return false;
        const unsigned eye=std::uint32_t(product.frame->eye);
        for(const auto& old:products[eye])if(old.product.frame->generated&&old.product.origin.previous_source_id==product.origin.previous_source_id&&
            old.product.origin.current_source_id==product.origin.current_source_id&&old.product.origin.index==product.origin.index&&
            old.product.origin.count==product.origin.count)return false;
        const auto completed=product.frame->color.ready_fence->GetCompletedValue();
        if(completed==UINT64_MAX||completed<product.frame->color.ready_value)return false;
        add_locked(product);++state.generated_outputs;++state.generated_per_eye[eye];
        if(early){++state.generated_published_early;if(batch_completed&&completed<batch_completed)++state.generated_ready_ahead;}
        else ++state.generated_published_final;
        const auto shape=product.frame->color.texture->GetDesc();state.actual_generated_sizes[eye]={std::uint32_t(shape.Width),shape.Height};
        state.phase="GPU-ready NVIDIA output available to stereo selection";publish_locked();return true;
    }
    void process(GenerationInput input,GenerationPolicy active,std::uint64_t job_revision,std::shared_ptr<const EyeFrame>* real_output=nullptr) {
        if(history_reset_pending.load(std::memory_order_acquire))return;
        // Callback metadata must outlive clearing every producer dependency.
        auto stable=std::make_shared<EyeFrame>(*input.frame);stable->guides.reset();
        const auto format=stable->color.source_format;const auto transfer=stable->color.transfer;
        stable->color={};stable->color.source_format=format;stable->color.transfer=transfer;
        const auto& frame=*stable;const unsigned eye=std::uint32_t(frame.eye);
        bool private_real=false;
        std::optional<Snapshot> old;
        {
            std::lock_guard lock(mutex);
            // An old real endpoint can still be the predecessor of a fresh G.
            // Keep the bounded reference history; reject work only after the
            // latest possible phase of the complete interval is known below.
            if(latest_target_ns>frame.content_time_ns&&latest_target_ns-frame.content_time_ns>active.maximum_age_ns)
                ++state.late_reference_inputs;
            if(epoch&&(frame.epoch<epoch||frame.space_epoch<space_epoch)){++state.dropped_inputs;publish_locked();return;}
            if(frame.epoch!=epoch||frame.space_epoch!=space_epoch) {reset_locked();epoch=frame.epoch;space_epoch=frame.space_epoch;}
            old=previous[eye];
            state.timing_source_id=frame.source_id;state.timing_native_token=input.native_frame_token;
            state.timing_epoch=frame.epoch;state.timing_revision=job_revision;state.timing_eye=eye;state.timing_generated_count=active.generated_per_interval;
            if(old&&(frame.source_id<=old->input.frame->source_id||frame.content_time_ns<=old->input.frame->content_time_ns)){++state.dropped_inputs;publish_locked();return;}
            private_real=private_real_locked(input);
            if(!private_real){
                if(real_output&&active_real.source){auto f=std::make_shared<EyeFrame>(*input.frame);f->guides.reset();*real_output=std::move(f);}
                publish_real_locked(input.frame,false);
            }
            publish_locked();
        }
        if(!active.generated_per_interval&&!private_real){std::lock_guard lock(mutex);previous[eye].reset();return;}
        bool span=old&&span_matches(input,*old,active);
        if(!span&&old){std::lock_guard lock(mutex);++state.unknown_guide_spans;publish_locked();}
        const auto snapshot_begin=cpu_time_ns();
        const bool snapshot_ready=snapshot_input(input,old,span,backend.queue(),private_real);
        const auto snapshot_end=cpu_time_ns();
        {std::lock_guard lock(mutex);auto& timing=timing_windows[active.generated_per_interval];
            timing.private_snapshot.observe(snapshot_begin,snapshot_end);state.timings[active.generated_per_interval].private_snapshot=timing.private_snapshot.summary();publish_locked();}
        if(!snapshot_ready) {
            if(!retained_unsafe&&copy_list&&!copy_list_closed){
                if(FAILED(copy_list->Close())){retained_unsafe=true;phase("Partial private recording Close failed; retained",true);}
                else copy_list_closed=true;
            }
            if(!retained_unsafe)active_work.reset();
            if(private_real&&!retained_unsafe){std::lock_guard lock(mutex);++state.dropped_inputs;publish_locked();}
            if(!state.failed)phase(private_real?"Private REAL preparation unavailable; no raw fallback":
                "Input shape/shared-read state/motion convention unsupported; real color retained");
            return;
        }
        auto current=*active_work->current;
        if(private_real){
            // Reset the completed recording before dropping its raw inputs.
            // Keep private current/old/targets and stable callback metadata.
            if(!retire_completed_copy_recording())return;
            stable->color=active_work->presentation_frame->color;
            if(real_output)*real_output=stable;
            active_work->input={};active_work->motion_recording.reset();input={};
        }
        {
            std::lock_guard lock(mutex);
            if(job_revision!=revision||history_reset_pending.load(std::memory_order_acquire)){active_work.reset();return;}
            if(active.generated_per_interval)previous[eye]=current;else previous[eye].reset();
        }
        if(!span||!active.generated_per_interval){active_work.reset();return;}
        {
            std::lock_guard lock(mutex);
            const auto latest_g=phase_time(old->input.frame->content_time_ns,frame.content_time_ns,
                active.generated_per_interval,active.generated_per_interval);
            if(latest_target_ns>latest_g&&latest_target_ns-latest_g>active.maximum_age_ns){
                ++state.expired_batches;active_work.reset();
                state.phase="Skipped NVIDIA batch: every generated phase already expired for XR";publish_locked();return;
            }
        }
        const auto cd=current.color->GetDesc(),pd=old->color->GetDesc();
        const auto cm=current.guides->motion.texture->GetDesc(),pm=old->guides->motion.texture->GetDesc();
        if(cd.Width!=pd.Width||cd.Height!=pd.Height||cm.Width!=pm.Width||cm.Height!=pm.Height){active_work.reset();return;}
        std::array<ID3D12Resource*,5> targets{};
        for(unsigned i=0;i<active.generated_per_interval;i++) {
            // Vendor targets enter/leave COMMON, never exposed before success.
            auto d=cd;d.Flags=D3D12_RESOURCE_FLAG_NONE;d.Alignment=0;
            active_work->outputs[i]=output_pool->acquire(device.Get(),d,backend_epoch);
            if(!active_work->outputs[i]){active_work.reset();phase("Generated output pool busy/allocation failed; real color retained");return;}
            targets[i]=active_work->outputs[i]->texture.Get();
        }
        auto a=native_frame(*old),b=native_frame(current);
        struct ReadyContext {
            Impl* owner;const EyeFrame* previous;const EyeFrame* current;unsigned count;
            std::uint64_t revision;std::array<bool,5> seen{};bool failed{};
        } ready_context{this,old->input.frame.get(),&frame,active.generated_per_interval,job_revision};
        const auto on_ready=[](void* opaque,unsigned index,unsigned count,unsigned hint,std::uint64_t completed,std::uint64_t batch_completed) noexcept {
            auto& context=*static_cast<ReadyContext*>(opaque);auto& owner=*context.owner;
            try {
                if(count!=context.count||!index||index>count||context.seen[index-1]||!completed||completed>batch_completed||batch_completed==UINT64_MAX){context.failed=true;return;}
                context.seen[index-1]=true;if(hint)return;
                auto* fence=owner.backend.fence();const auto done=fence?fence->GetCompletedValue():0;
                if(!fence||done==UINT64_MAX||done<completed||!same_device(fence,owner.device.Get())||
                   !owner.active_work->outputs[index-1]->completion(fence,completed)){context.failed=true;return;}
                const auto stamp=interpolation_stamp(*context.previous,*context.current,index,count);
                if(!stamp){context.failed=true;return;}
                auto out=std::make_shared<EyeFrame>(*context.current);out->generated=true;out->stereo_pair_id=0;
                out->content_time_ns=stamp->content_time_ns;out->render_pose=stamp->render_pose;out->fov=stamp->fov;out->guides.reset();
                out->color.texture=owner.active_work->outputs[index-1]->texture;out->color.lifetime=owner.active_work->outputs[index-1];
                out->color.ready_fence=fence;out->color.ready_value=completed;out->color.resting_state=D3D12_RESOURCE_STATE_COMMON;
                std::lock_guard lock(owner.mutex);owner.publish_generated_locked({out,stamp->origin},context.revision,true,batch_completed);
            }catch(...){context.failed=true;} // Never unwind across the C ABI/GPU owner.
        };
        {std::lock_guard lock(mutex);state.evaluating=true;publish_locked();}
        const auto backend_begin=cpu_time_ns();
        const bool progressive=backend.run_ready!=nullptr;
        const bool success=(progressive?backend.run_ready(eye,backend_epoch,&a,&b,active.generated_per_interval,targets.data(),on_ready,&ready_context):
            backend.run(eye,backend_epoch,&a,&b,active.generated_per_interval,targets.data()))!=0;
        const auto backend_end=cpu_time_ns();
        const auto result=backend.status();
        {std::lock_guard lock(mutex);state.evaluating=false;if(!success&&result.hint)++state.vendor_suppressed_batches;
            auto& timing=timing_windows[active.generated_per_interval];timing.backend_batch.observe(backend_begin,backend_end);
            state.timings[active.generated_per_interval].backend_batch=timing.backend_batch.summary();publish_locked();}
        if(ready_context.failed){retained_unsafe=true;phase("Progressive NVIDIA output contract failed; GPU work retained",true);return;}
        if(!success) {
            if(result.code<0){retained_unsafe=true;phase(std::string("NVIDIA batch failed; GPU work retained: ")+result.message,true);return;}
            active_work.reset();phase(std::string("NVIDIA batch not generated: ")+result.message);return;
        }
        auto* completed_fence=backend.fence();
        if(result.code!=1||result.hint||result.count!=active.generated_per_interval||!result.completed||!completed_fence||
           completed_fence->GetCompletedValue()==UINT64_MAX||completed_fence->GetCompletedValue()<result.completed) {
            retained_unsafe=true;phase("Vendor returned success without proven complete output ownership",true);return;
        }
        std::vector<GenerationProduct> generated;
        for(unsigned i=0;i<result.count;i++) {
            if(progressive){
                if(!ready_context.seen[i]){retained_unsafe=true;phase("NVIDIA ready callback omitted a completed phase; retained",true);return;}
                continue; // Already published or discarded under the exact job revision.
            }
            if(!active_work->outputs[i]->completion(completed_fence,result.completed)){
                retained_unsafe=true;phase("Generated pool fence identity rejected; GPU work retained",true);return;
            }
            const auto stamp=interpolation_stamp(*old->input.frame,frame,i+1,result.count);if(!stamp)continue;
            auto out=std::make_shared<EyeFrame>(frame);out->generated=true;out->stereo_pair_id=0;
            out->content_time_ns=stamp->content_time_ns;out->render_pose=stamp->render_pose;out->fov=stamp->fov;out->guides.reset();
            out->color.texture=active_work->outputs[i]->texture;out->color.lifetime=active_work->outputs[i];
            out->color.ready_fence=completed_fence;out->color.ready_value=result.completed;
            out->color.resting_state=D3D12_RESOURCE_STATE_COMMON;generated.push_back({out,stamp->origin});
        }
        {
            std::lock_guard lock(mutex);
            if(job_revision==revision&&frame.epoch==epoch&&frame.space_epoch==space_epoch) {
                for(const auto& p:generated)publish_generated_locked(p,job_revision,false,result.completed);++state.completed_batches;
                state.actual_generated_sizes[eye]={std::uint32_t(cd.Width),cd.Height};state.phase="Complete NVIDIA outputs awaiting requested presentation pattern";publish_locked();
            }
        }
        active_work.reset();
    }
    void run() {
        try {
            if(!attach_backend()){phase("NVIDIA bridge attach/configuration failed",true);goto finish;}
            {std::lock_guard lock(mutex);state.backend_attached=true;state.phase="NVIDIA attached; waiting for owned real inputs";publish_locked();}
            for(;;) {
                GenerationInput input;GenerationPolicy active;std::uint64_t job_revision{};
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock,std::chrono::milliseconds(5),[&]{return stopping||history_reset_pending.load(std::memory_order_acquire)||!pending.empty();});
                    if(stopping)break;
                    if(history_reset_pending.load(std::memory_order_acquire)) {
                        lock.unlock();reset_history_on_owner();
                        {std::lock_guard check(mutex);if(state.failed)break;}
                        continue;
                    }
                    if(pending_policy&&!pending_policy->generated_per_interval)for(auto& old:previous)old.reset();
                    if(!take_pending_locked(input,active,job_revision)) {
                        // Avoid polling a delayed GPU in a busy loop.
                        wake.wait_for(lock,std::chrono::milliseconds(5));continue;
                    }
                }
                std::shared_ptr<const EyeFrame> deferred;
                process(std::move(input),active,job_revision,&deferred);
                if(!retained_unsafe){
                    active_work.reset();
                    if(retire_completed_copy_recording()){
                        input={}; // Drop the owner's last original guide alias BEFORE ACK wait.
                        std::lock_guard lock(mutex);finish_real_locked(std::move(deferred));
                    }
                }
                {std::lock_guard lock(mutex);if(state.failed||retained_unsafe)break;}
            }
        } catch(...) {retained_unsafe=retained_unsafe||bool(active_work)||bool(recorded_work);phase("Exception in generation owner worker; active work retained",true);}
    finish:
        if(backend.module&&!retained_unsafe) {
            if(state.backend_attached&&backend.detach&&!backend.detach()){
                retained_unsafe=true;const auto detail=backend.status();
                phase(std::string("Backend detach failed; retained: code=")+std::to_string(detail.code)+
                    " ngx_result="+std::to_string(detail.ngx_result)+" completed="+std::to_string(detail.completed)+
                    " detail="+detail.message,true);
            }
            if(!retained_unsafe){FreeLibrary(backend.module);backend.module=nullptr;}
        }
        {std::lock_guard lock(mutex);state.worker_running=false;state.evaluating=false;publish_locked();}
    }
};

GenerationWorker::GenerationWorker():impl_(std::make_unique<Impl>()){}
GenerationWorker::~GenerationWorker(){stop();if(impl_->retained_unsafe)(void)impl_.release();}
bool GenerationWorker::request_history_reset() noexcept {
    auto& p=*impl_;
    if(!p.history_reset_pending.exchange(true,std::memory_order_acq_rel))
        p.history_reset_requests.fetch_add(1,std::memory_order_relaxed);
    p.wake.notify_one();return true;
}
bool GenerationWorkerTestAccess::apply_history_reset(GenerationWorker& worker) {
    auto& p=*worker.impl_;if(p.thread.joinable())return false;
    return p.reset_history_on_owner();
}
void GenerationWorkerTestAccess::seed(GenerationWorker& worker,const GenerationPolicy& policy,
    std::span<const GenerationProduct> left,std::span<const GenerationProduct> right){
    auto& p=*worker.impl_;std::lock_guard lock(p.mutex);p.reset_locked();p.policy=policy;
    for(const auto& row:left)p.products[0].push_back({row,false});
    for(const auto& row:right)p.products[1].push_back({row,false});
}
std::unique_lock<std::mutex> GenerationWorkerTestAccess::lock(GenerationWorker& worker){return std::unique_lock(worker.impl_->mutex);}
void GenerationWorkerTestAccess::invalidate(GenerationWorker& worker){std::lock_guard lock(worker.impl_->mutex);worker.impl_->reset_locked();}
GenerationCursor GenerationWorkerTestAccess::committed_cursor(const GenerationWorker& worker){return worker.impl_->cursor_snapshot();}
bool GenerationWorkerTestAccess::configure(GenerationWorker& worker,const GenerationPolicy& policy){
    {std::lock_guard lock(worker.impl_->mutex);worker.impl_->state.worker_running=true;}
    return worker.configure(policy);
}
void GenerationWorkerTestAccess::enable_submission(GenerationWorker& worker,ID3D12Device* device,std::uint64_t epoch,std::uint64_t space,bool backend_ready){
    auto& p=*worker.impl_;std::lock_guard lock(p.mutex);p.device=device;p.epoch=epoch;p.space_epoch=space;
    p.state.worker_running=true;p.state.backend_attached=backend_ready;p.publish_locked();
}
void GenerationWorkerTestAccess::process_real_only(GenerationWorker& worker,const GenerationInput& input){
    auto& p=*worker.impl_;auto policy=p.policy;policy.generated_per_interval=0;p.process(input,policy,p.revision);
}
std::uint64_t GenerationWorkerTestAccess::revision(const GenerationWorker& worker){std::lock_guard lock(worker.impl_->mutex);return worker.impl_->revision;}
bool GenerationWorkerTestAccess::publish_generated(GenerationWorker& worker,const GenerationProduct& product,std::uint64_t revision,std::uint64_t batch_completed){
    std::lock_guard lock(worker.impl_->mutex);return worker.impl_->publish_generated_locked(product,revision,true,batch_completed);
}
bool GenerationWorkerTestAccess::snapshot_only(GenerationWorker& worker,ID3D12Device* device,
        ID3D12CommandQueue* queue,const GenerationInput& input){
    auto& p=*worker.impl_;
    if(p.thread.joinable()||p.retained_unsafe||!device||!queue||!input.frame||unsigned(input.frame->eye)>1||
       !same_device(queue,device)||(p.device&&p.device.Get()!=device))return false;
    if(!p.device){
        p.device=device;p.input_pool=std::make_unique<TexturePool>(p.private_textures,12);
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.copy_fence))))return false;
        p.copy_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!p.copy_event)return false;
        p.epoch=input.frame->epoch;p.space_epoch=input.frame->space_epoch;
    }
    const auto eye=unsigned(input.frame->eye);const auto old=p.previous[eye];
    if(!p.publish_real_locked(input.frame,false))return false;
    if(!p.snapshot_input(input,old,old&&p.span_matches(input,*old,p.policy),queue))return false;
    p.previous[eye]=*p.active_work->current;p.active_work.reset();return p.retire_completed_copy_recording();
}
GenerationInput GenerationWorkerTestAccess::snapshot_history(const GenerationWorker& worker,Eye eye){
    const auto& p=*worker.impl_;return unsigned(eye)<2&&p.previous[unsigned(eye)]?p.previous[unsigned(eye)]->input:GenerationInput{};
}
bool GenerationWorker::start(ID3D12Device* device,const GenerationConfig& config,Log log) {
    auto& p=*impl_;
    if(p.thread.joinable()||p.retained_unsafe||!device||config.backend_library.empty()||config.snippet_directory.empty()||
       config.queue_capacity<2||config.queue_capacity>32||config.products_per_eye<2||config.products_per_eye>32||
       !validate_generation_policy(config.policy).valid)return false;
    std::array<wchar_t,32768> full{};const auto n=GetFullPathNameW(config.backend_library.c_str(),DWORD(full.size()),full.data(),nullptr);
    if(!n||n>=full.size())return false;
    p.config=config;p.config.backend_library=full.data();p.policy=config.policy;p.log=std::move(log);p.device=device;
    p.input_pool=std::make_unique<TexturePool>(p.private_textures,12);
    p.output_pool=std::make_unique<TexturePool>(p.private_textures,2*config.products_per_eye+12);
    p.stopping=false;p.active_real={};p.deferred_real={};p.state={};p.timing_windows={};p.latest_target_ns=0;p.state.worker_running=true;p.state.phase="Generation owner starting; NGX attachment pending";
    {std::lock_guard lock(p.mutex);p.publish_locked();}
    try{p.thread=std::thread([&p]{p.run();});return true;}catch(...){p.phase("Generation worker thread creation failed",true);return false;}
}
bool GenerationWorker::submit(const GenerationInput& input) {
    auto& p=*impl_;std::unique_lock lock(p.mutex,std::try_to_lock);
    if(!lock.owns_lock()||!p.enqueue_locked(input))return false;
    lock.unlock();p.wake.notify_one();return true;
}
bool GenerationWorker::configure(const GenerationPolicy& policy) {
    auto& p=*impl_;if(!validate_generation_policy(policy).valid)return false;
    std::unique_lock lock(p.mutex,std::try_to_lock);if(!lock.owns_lock()||p.stopping||!p.state.worker_running)return false;
    p.consume_acceptance_locked();p.pending_policy=policy;++p.revision;lock.unlock();p.wake.notify_one();return true;
}
bool GenerationWorker::retire_expired(std::int64_t target) {
    auto& p=*impl_;
    if(target<=0||p.history_reset_pending.load(std::memory_order_acquire))return false;
    std::unique_lock lock(p.mutex,std::try_to_lock);
    if(!lock.owns_lock()||p.stopping||target<p.latest_target_ns)return false;
    p.consume_acceptance_locked();
    p.latest_target_ns=target;
    p.retire_expired_locked(target);p.publish_locked();return true;
}
GenerationSelection GenerationWorker::select(std::int64_t target,std::int64_t period,std::span<const GenerationInput> incoming) {
    auto& p=*impl_;++p.selection_counts[Impl::SC_Calls];
    if(p.history_reset_pending.load(std::memory_order_acquire))return {};
    if(target<=0||period<=0||incoming.size()>32)return {};
    std::unique_lock lock(p.mutex,std::try_to_lock);if(!lock.owns_lock()){
        ++p.selection_counts[Impl::SC_Busy];
        if(const auto accepted=p.pending_acceptance.load(std::memory_order_acquire)){
            if(accepted->proposal==p.published_proposal.load(std::memory_order_acquire)){
                auto cursor=accepted->proposal->cursor;cursor.display_time_ns=accepted->first_display_time;
                auto held=choose_generation_pair({}, {},target,period,accepted->proposal->policy,cursor);
                if(p.pending_acceptance.load(std::memory_order_acquire)==accepted&&
                   p.published_proposal.load(std::memory_order_acquire)==accepted->proposal){
                    if(held.frame)++p.selection_counts[Impl::SC_LockRepeat];return held;
                }
            }
        }
        const auto saved=p.committed_cursor.load(std::memory_order_acquire);if(!saved)return {};
        auto cursor=saved->cursor;
        // Empty candidates exercise exactly the normal cursor-repeat guards:
        // no new pair, no policy activation and no age/skew relaxation.
        auto held=choose_generation_pair({}, {},target,period,saved->policy,cursor);
        if(p.committed_cursor.load(std::memory_order_acquire)!=saved)return {};
        if(held.frame)++p.selection_counts[Impl::SC_LockRepeat];return held;
    }
    if(p.stopping)return {};
    bool queued=false;
    for(const auto& input:incoming)queued=p.enqueue_locked(input)||queued;
    const auto select_locked=[&]() -> GenerationSelection {
    p.consume_acceptance_locked();
    const auto committed=p.cursor_snapshot();
    if(committed.valid&&committed.display_time_ns>0&&target<committed.display_time_ns)return {};
    p.latest_target_ns=std::max(p.latest_target_ns,target);p.state.selection_target_ns=target;
    p.retire_expired_locked(target);
    if(p.proposal) {
        if(target<p.proposal->cursor.display_time_ns)return {};
        const auto& frame=*p.proposal->selection.frame;
        const auto oldest=std::min(frame.eyes[0]->content_time_ns,frame.eyes[1]->content_time_ns);
        if(p.proposal->revision==p.revision&&target>=frame.presentation_time_ns&&target-oldest<=p.proposal->policy.maximum_age_ns)
            {p.count_selection(p.proposal->selection,target,true);p.publish_locked();return p.proposal->selection;}
        p.clear_proposal();
    }
    std::array<std::vector<GenerationProduct>,2> rows;
    for(unsigned eye=0;eye<2;eye++) {
        const auto& stored=p.products[eye];
        for(const auto& product:stored)rows[eye].push_back(product.product);
    }
    GenerationSelection result;
    std::string requested_step;
    SelectionDiagnostics requested_diagnostics;
    bool requested_available=false;
    if(p.pending_policy) {
        auto candidate_cursor=p.cursor_snapshot();candidate_cursor.pattern_step=0;candidate_cursor.pair_sequence=p.next_pair_sequence;
        auto candidate=choose_generation_pair(rows[0],rows[1],target,period,*p.pending_policy,candidate_cursor);
        requested_step=candidate.requested_step;requested_available=candidate.pattern_available;requested_diagnostics=candidate.diagnostics;
        if(candidate.new_presentation){p.next_pair_sequence=candidate_cursor.pair_sequence;
            p.set_proposal(Impl::Proposal{candidate,candidate_cursor,*p.pending_policy,p.revision});result=std::move(candidate);}
        else {auto active_cursor=p.cursor_snapshot();active_cursor.pair_sequence=p.next_pair_sequence;
            result=choose_generation_pair(rows[0],rows[1],target,period,p.policy,active_cursor);
            if(result.new_presentation){p.next_pair_sequence=active_cursor.pair_sequence;p.set_proposal(Impl::Proposal{result,active_cursor,p.policy,p.revision});}}
    } else {auto active_cursor=p.cursor_snapshot();active_cursor.pair_sequence=p.next_pair_sequence;
        result=choose_generation_pair(rows[0],rows[1],target,period,p.policy,active_cursor);
        if(result.new_presentation){p.next_pair_sequence=active_cursor.pair_sequence;p.set_proposal(Impl::Proposal{result,active_cursor,p.policy,p.revision});}}
    p.state.requested_pattern_available=p.pending_policy?requested_available:result.pattern_available;
    p.state.waiting_pattern=p.pending_policy?requested_step:result.requested_step;
    p.state.selection=p.pending_policy?requested_diagnostics:result.diagnostics;p.state.selection_examined_ns=target;
    p.count_selection(result,target);p.publish_locked();return result;
    };
    auto result=select_locked();result.inputs_handled=true;
    lock.unlock();if(queued)p.wake.notify_one();return result;
}
bool GenerationWorker::acknowledge(std::uint64_t pair_id,std::uint64_t epoch,std::uint64_t space_epoch,std::int64_t first_display_time) {
    auto& p=*impl_;if(p.history_reset_pending.load(std::memory_order_acquire))return false;
    const auto proposal=p.published_proposal.load(std::memory_order_acquire);
    if(!proposal)return false;
    const auto& frame=*proposal->selection.frame;
    if(frame.pair_id!=pair_id||frame.epoch!=epoch||frame.space_epoch!=space_epoch)return false;
    if(first_display_time<0||(first_display_time&&first_display_time<proposal->cursor.display_time_ns))return false;
    if(!first_display_time)first_display_time=proposal->cursor.display_time_ns;
    const auto acceptance=std::make_shared<const Impl::Acceptance>(Impl::Acceptance{proposal,first_display_time});
    auto existing=p.pending_acceptance.load(std::memory_order_acquire);
    while(!existing||existing->proposal!=proposal){
        if(p.pending_acceptance.compare_exchange_weak(existing,acceptance,std::memory_order_release,std::memory_order_acquire))break;
    }
    // Consumer select/configure/ack calls are serialized by the XR owner. The
    // generation thread can only invalidate this receipt on a genuine reset.
    std::unique_lock lock(p.mutex,std::try_to_lock);if(!lock.owns_lock()||p.stopping)return false;
    return p.consume_acceptance_locked();
}
GenerationStatus GenerationWorker::status()const {
    const auto state=impl_->published.load(std::memory_order_acquire);auto result=state?*state:GenerationStatus{};
    result.history_reset_pending=impl_->history_reset_pending.load(std::memory_order_acquire);
    result.history_reset_requests=impl_->history_reset_requests.load(std::memory_order_relaxed);
    result.history_resets=impl_->history_resets.load(std::memory_order_relaxed);
    const auto& c=impl_->selection_counts;
    result.selection_flow={c[Impl::SC_Calls].load(),c[Impl::SC_Busy].load(),c[Impl::SC_New].load(),c[Impl::SC_Proposal].load(),
        c[Impl::SC_Repeat].load(),c[Impl::SC_NoHistory].load(),c[Impl::SC_Expired].load(),c[Impl::SC_Other].load(),c[Impl::SC_LockRepeat].load()};
    result.private_textures=impl_->private_textures.snapshot();
    if(impl_->input_pool)result.private_inputs_pool=impl_->input_pool->status();
    if(impl_->output_pool)result.generated_pool=impl_->output_pool->status();return result;
}
bool GenerationWorker::stopped_cleanly() const noexcept {
    const auto& p=*impl_;
    return p.stopping&&!p.thread.joinable()&&!p.retained_unsafe&&!p.backend.module&&
        !p.device&&!p.copy_list&&!p.copy_allocator&&!p.copy_fence&&!p.copy_event&&!p.active_work&&!p.recorded_work;
}
void GenerationWorker::stop() {
    auto& p=*impl_;
    {std::lock_guard lock(p.mutex);p.stopping=true;p.committed_cursor.store({},std::memory_order_release);}p.wake.notify_one();
    if(p.thread.joinable()){if(p.thread.get_id()==std::this_thread::get_id())return;p.thread.join();}
    if(!p.retained_unsafe){
        std::lock_guard lock(p.mutex);p.pending.clear();p.active_real={};p.reset_locked();p.history_reset_pending.store(false,std::memory_order_release);
        // Destroy the final recording before dropping its resource leases.
        if(p.recorded_work)p.recorded_work->list.Reset();
        if(p.active_work)p.active_work->list.Reset();
        p.copy_list.Reset();p.copy_allocator.Reset();p.recorded_work.reset();p.active_work.reset();
        if(p.input_pool&&!p.input_pool->close())p.retained_unsafe=true;
        if(p.output_pool&&!p.output_pool->close())p.retained_unsafe=true;
        p.copy_fence.Reset();
        if(p.copy_event)CloseHandle(p.copy_event);p.copy_event=nullptr;p.device.Reset();p.state.backend_attached=false;p.publish_locked();
    }
}

}

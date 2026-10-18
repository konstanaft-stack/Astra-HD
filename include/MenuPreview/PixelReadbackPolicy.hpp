#pragma once
#include <cstdint>
namespace cvr::menu_preview {
struct PreviewCopyEligibility {bool role{},app{},open{},stateKnown{},ambiguous{},sameListGeneration{};uint32_t work{},eye{};bool currentDraw{};};
inline bool AdmitPreviewPixelCopy(const PreviewCopyEligibility& e){return e.role&&e.app&&e.open&&e.stateKnown&&!e.ambiguous&&e.sameListGeneration&&e.work==0x377B58&&e.eye<2&&e.currentDraw;}
struct PreviewRawEndProof {
 PreviewCopyEligibility contents;
 bool lifecycleGood{},listKnown{},capacity{},direct{},currentApplication{};
 // Diagnostic observation of the epilogue binding, separate from the historic
 // descriptor proved when the original Draw returned.
 bool postDrawBindingMatches{};
};
inline bool AdmitPreviewRawEnd(const PreviewRawEndProof& p){return AdmitPreviewPixelCopy(p.contents)&&p.lifecycleGood&&p.listKnown&&p.capacity&&p.direct&&p.currentApplication;}
// State2 is inferred from the valid original 3D Draw contract, never an observed barrier.
struct PreviewPixelResourceState {
 uint32_t value{},provenance{};bool known{};uint64_t drawOrder{};
 void DrawRequired(uint64_t order){value=4;provenance=2;known=order!=0;drawOrder=order;}
 void Invalidate(){known=false;provenance=0;drawOrder=0;}
 void Transition(uint32_t after,bool complete,bool nonsplit){if(!complete||!nonsplit){Invalidate();return;}value=after;known=true;provenance=1;if(after&(8u|0x400u))drawOrder=0;}
 bool MatchesDraw(uint64_t order)const{return known&&order&&drawOrder==order;}
};
struct PreviewDrawEligibility {bool role{},app{},uniqueNodeTarget{},currentBoundTarget{},open{},sameGeneration{},sameApplication{};uint32_t elements{},instances{};};
inline bool AdmitPreviewDrawReceipt(const PreviewDrawEligibility& e){return e.role&&e.app&&e.uniqueNodeTarget&&e.currentBoundTarget&&e.open&&e.sameGeneration&&e.sameApplication&&e.elements&&e.instances;}
// Binding is current Draw admission state; historical drawOrder belongs to contents.
struct PreviewPixelNodeWriter {
 uintptr_t boundList{},boundResource{};bool uniqueBindPending{};uint64_t drawOrder{};
 void OmChange(uintptr_t list,uint32_t count){boundList=list;boundResource=0;uniqueBindPending=count==1;}
 void ResolveTarget(uintptr_t list,uintptr_t resource){if(uniqueBindPending&&boundList==list)boundResource=resource;}
 bool CurrentlyBound(uintptr_t list,uintptr_t resource)const{return uniqueBindPending&&boundList==list&&boundResource==resource;}
};
// Diagnostic sampling only; this does not establish readiness, identity or frame pairing.
inline bool ParsePreviewSampleDelay(const wchar_t* text,uint32_t& milliseconds){milliseconds=0;if(!text)return true;if(!*text)return false;for(;*text;++text){if(*text<L'0'||*text>L'9')return false;milliseconds=milliseconds*10+uint32_t(*text-L'0');if(milliseconds>3000)return false;}return true;}
struct PreviewPixelSampleClock {
 bool started[2]{};uint64_t firstEligible[2]{};
 bool Ready(uint32_t eye,uint64_t now,uint32_t delay,bool eligible,uint64_t& age){age=0;if(eye>1||delay>3000||!eligible)return false;if(!started[eye]){started[eye]=true;firstEligible[eye]=now;}if(now<firstEligible[eye])return false;age=now-firstEligible[eye];return age>=delay;}
};
struct PreviewPixelAttemptBudget {
 uint32_t consumed{};uint64_t started[2]{};
 bool Available(uint32_t eye,uint64_t now){if(eye>1||consumed&(1u<<eye))return false;if(!started[eye])started[eye]=now;if(now<started[eye]||now-started[eye]>5000){Consume(eye);return false;}return true;}
 void Consume(uint32_t eye){if(eye<2)consumed|=1u<<eye;}
};
inline bool PreviewPixelSubmissionGeneration(bool open,uint64_t current,uint64_t recorded){return !open&&current&&current==recorded;}
}

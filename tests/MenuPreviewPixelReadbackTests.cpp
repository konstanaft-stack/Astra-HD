#include "MenuPreview/PixelReadbackPolicy.hpp"
#include <cassert>
#include <iostream>
using namespace cvr::menu_preview;
int main(){
 PreviewCopyEligibility copy{true,true,true,true,false,true,0x377B58,1,true};assert(AdmitPreviewPixelCopy(copy));
 copy.open=false;assert(!AdmitPreviewPixelCopy(copy));copy.open=true;copy.stateKnown=false;assert(!AdmitPreviewPixelCopy(copy));copy.stateKnown=true;
 copy.ambiguous=true;assert(!AdmitPreviewPixelCopy(copy));copy.ambiguous=false;copy.sameListGeneration=false;assert(!AdmitPreviewPixelCopy(copy));copy.sameListGeneration=true;
 copy.role=false;assert(!AdmitPreviewPixelCopy(copy));copy.role=true;copy.app=false;assert(!AdmitPreviewPixelCopy(copy));copy.app=true;copy.currentDraw=false;assert(!AdmitPreviewPixelCopy(copy));copy.currentDraw=true;
 PreviewDrawEligibility draw{true,true,true,true,true,true,true,3,1};assert(AdmitPreviewDrawReceipt(draw));
 draw.elements=0;assert(!AdmitPreviewDrawReceipt(draw));draw.elements=3;draw.instances=0;assert(!AdmitPreviewDrawReceipt(draw));draw.instances=1;
 draw.currentBoundTarget=false;assert(!AdmitPreviewDrawReceipt(draw));draw.currentBoundTarget=true;draw.uniqueNodeTarget=false;assert(!AdmitPreviewDrawReceipt(draw));draw.uniqueNodeTarget=true;
 draw.sameGeneration=false;assert(!AdmitPreviewDrawReceipt(draw));draw.sameGeneration=true;draw.sameApplication=false;assert(!AdmitPreviewDrawReceipt(draw));draw.sameApplication=true;draw.role=false;assert(!AdmitPreviewDrawReceipt(draw));
 PreviewPixelNodeWriter writer;PreviewPixelResourceState contents;assert(!contents.known);
 writer.OmChange(100,1);writer.ResolveTarget(100,200);assert(writer.CurrentlyBound(100,200));writer.drawOrder=7;contents.DrawRequired(7);
 assert(contents.value==4&&contents.provenance==2&&contents.MatchesDraw(writer.drawOrder));
 writer.OmChange(100,0);assert(!writer.CurrentlyBound(100,200));assert(contents.MatchesDraw(writer.drawOrder)); // Draw->unbind preserves historical contents
 writer.OmChange(100,1);writer.ResolveTarget(100,300);assert(!writer.CurrentlyBound(100,200));assert(contents.MatchesDraw(writer.drawOrder)); // rebind alone is not a write
 contents.Transition(0,true,true);assert(contents.known&&contents.provenance==1&&contents.value==0&&contents.MatchesDraw(writer.drawOrder));
 contents.Invalidate();assert(!contents.MatchesDraw(writer.drawOrder));contents.Transition(4,true,true);assert(contents.known&&!contents.MatchesDraw(writer.drawOrder)); // barrier cannot resurrect invalid writer
 contents.DrawRequired(8);assert(!contents.MatchesDraw(writer.drawOrder)); // overwrite ordered writer differs
 contents.DrawRequired(7);contents.Transition(0x400,true,true);assert(contents.known&&!contents.MatchesDraw(writer.drawOrder));
 contents.DrawRequired(7);contents.Transition(8,true,true);assert(!contents.MatchesDraw(writer.drawOrder));
 contents.DrawRequired(7);contents.Transition(4,true,false);assert(!contents.known&&!contents.MatchesDraw(writer.drawOrder));
 contents.DrawRequired(7);contents.Transition(4,false,true);assert(!contents.known);contents.DrawRequired(7);assert(contents.MatchesDraw(writer.drawOrder));
 writer.OmChange(100,2);writer.ResolveTarget(100,200);assert(!writer.CurrentlyBound(100,200)); // future Draw rejects multiple binding
 writer.OmChange(101,1);writer.ResolveTarget(101,200);assert(!writer.CurrentlyBound(100,200));
 PreviewPixelAttemptBudget budget;assert(budget.Available(0,100));assert(budget.Available(0,200));budget.Consume(0);assert(!budget.Available(0,201));assert(budget.Available(1,201));assert(!budget.Available(1,5202));
 PreviewPixelAttemptBudget backwards;assert(backwards.Available(0,100));assert(!backwards.Available(0,99));
 assert(!PreviewPixelSubmissionGeneration(true,1,1));assert(PreviewPixelSubmissionGeneration(false,1,1));assert(!PreviewPixelSubmissionGeneration(false,2,1));assert(!PreviewPixelSubmissionGeneration(false,0,0));
 uint32_t delay=123;assert(ParsePreviewSampleDelay(nullptr,delay)&&delay==0);assert(ParsePreviewSampleDelay(L"0",delay)&&delay==0);assert(ParsePreviewSampleDelay(L"3000",delay)&&delay==3000);
 for(const auto* invalid:{L"",L"-1",L"+1",L" 1",L"1.0",L"3001",L"999999999999999999"})assert(!ParsePreviewSampleDelay(invalid,delay));
 PreviewPixelSampleClock sample;uint64_t age=999;assert(!sample.Ready(1,100,1000,false,age)&&!sample.started[1]);
 assert(!sample.Ready(1,200,1000,true,age)&&age==0);assert(!sample.Ready(1,1199,1000,true,age)&&age==999);
 assert(sample.Ready(1,1200,1000,true,age)&&age==1000);assert(!sample.Ready(1,1201,1000,false,age));assert(!sample.Ready(1,199,1000,true,age));
 assert(sample.Ready(0,0,0,true,age)&&age==0);assert(sample.Ready(0,1,0,true,age)&&age==1); // explicit initialized flag, zero clock allowed
 PreviewPixelAttemptBudget bound;assert(bound.Available(0,100));PreviewPixelSampleClock late;assert(!late.Ready(0,3100,3000,true,age));assert(!bound.Available(0,6100)); // sampling cannot extend 5s budget
 assert(!sample.Ready(2,100,0,true,age));assert(!sample.Ready(0,100,3001,true,age));
 std::cout<<"PASS copy/draw gates; historical Draw->OM unbind/rebind; real hazard/write/order invalidation; state provenance/COMMON; list generation/app; timeout\n";
}

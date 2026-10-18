#include "MenuPreview/DescriptorProvenancePolicy.hpp"
#include <cstdio>
using namespace cvr::menu_preview;
int main(){int failed{};auto check=[&](bool p,const char* label){if(!p){std::printf("FAIL %s\n",label);++failed;}};
 const auto def=MakePreviewRtvViewEvidence(10,1,5,1,true);
 check(def.known&&def.format==10&&def.dimension==4&&def.mip==0,"typed FP16 default mip0 with five-mip resource");
 const PreviewRtvViewEvidence one{10,4,1,true},badDimension{10,5,0,true},badMip{10,4,5,true};
 check(MakePreviewRtvViewEvidence(10,1,5,1,true,&one).known,"explicit native mip1");
 check(!MakePreviewRtvViewEvidence(9,1,5,1,true).known,"typeless default unknown");
 check(!MakePreviewRtvViewEvidence(10,2,5,1,true).known,"array default unknown");
 check(!MakePreviewRtvViewEvidence(10,1,5,4,true).known,"multisample unknown");
 check(!MakePreviewRtvViewEvidence(10,1,5,1,true,&badDimension).known,"unsupported explicit dimension");
 check(!MakePreviewRtvViewEvidence(10,1,5,1,true,&badMip).known,"explicit mip outside resource");
 PreviewDescriptorLedger l;auto heap=l.RegisterHeap(1,2,1024,4,32);auto res=l.RegisterResource(3);
 auto write=[&](PreviewRtvViewEvidence v){auto t=l.BeginMutation(2);l.AddDestination(t,1024,1,32);return l.FinishDirect(t,1024,3,res,v);};
 check(write({10,4,0,true}),"direct view write");auto zero=*l.Read(1024);check(zero.rtvViewKnown&&zero.rtvMip==0&&zero.rtvFormat==10&&zero.rtvDimension==4,"view attached to resource mutation");
 check(write(one),"same resource different mip");auto current=*l.Read(1024);check(current.rtvMip==1&&current.rtvViewKnown&&!l.Validate(zero),"overwrite atomically replaces view");
 auto forged=current;forged.rtvMip=0;check(!l.Validate(forged),"Validate rejects changed mip with same sequence");forged=current;forged.rtvViewKnown=false;check(!l.Validate(forged),"Validate rejects changed known with same sequence");
 forged=current;forged.rtvFormat=9;check(!l.Validate(forged),"Validate rejects changed format");forged=current;forged.rtvDimension=5;check(!l.Validate(forged),"Validate rejects changed dimension");
 auto copy=l.BeginMutation(2);l.AddDestination(copy,1024,1,32);check(!l.Read(1024),"pending copy invalidates resource and view");l.FinishCopies(copy);check(!l.Read(1024)&&!l.Validate(current),"copy keeps view unknown");
 check(write(one),"view restored");current=*l.Read(1024);auto pending=l.BeginMutation(2);l.AddDestination(pending,1024,1,32);l.ForgetHeap(1,heap);heap=l.RegisterHeap(1,2,1024,4,32);check(!l.FinishDirect(pending,1024,3,res,one)&&!l.Validate(current),"heap reuse cannot publish old view");
 check(write(one),"view after new heap");current=*l.Read(1024);l.ForgetResource(3,res);res=l.RegisterResource(3);check(!l.Validate(current),"resource address reuse invalidates view");
 check(l.WriteDirect(2,1024,3,res),"legacy resource-only writer remains valid");check(l.Read(1024)&&!l.Read(1024)->rtvViewKnown,"legacy writer has no invented view");
 std::printf("RTV view contract: %d failures\n",failed);return failed?1:0;}

#include "MenuPreview/PixelReadbackPolicy.hpp"
#include <cstdio>
using namespace cvr::menu_preview;
int main(){int failures{};auto check=[&](bool p,const char* s){if(!p){std::printf("FAIL %s\n",s);++failures;}};
 PreviewPixelNodeWriter writer;PreviewPixelResourceState source;
 writer.OmChange(10,1);writer.ResolveTarget(10,20);writer.drawOrder=7;source.DrawRequired(7);
 auto proof=[&](bool descriptorCurrent=true){return PreviewRawEndProof{{true,true,true,source.known,false,true,0x377B58,0,descriptorCurrent&&source.MatchesDraw(writer.drawOrder)},true,true,true,true,true,writer.CurrentlyBound(10,20)};};
 check(AdmitPreviewRawEnd(proof()),"actual Draw proof");writer.OmChange(10,0);check(AdmitPreviewRawEnd(proof()),"Draw then OM unbind retains source contents");
 writer.OmChange(10,1);writer.ResolveTarget(10,30);check(AdmitPreviewRawEnd(proof()),"binding another RT is not a write to historic source");
 check(!AdmitPreviewRawEnd(proof(false)),"source descriptor rebind/version change rejects");source.Invalidate();check(!AdmitPreviewRawEnd(proof()),"state hazard rejects");
 source.Transition(4,true,true);check(!AdmitPreviewRawEnd(proof()),"state transition cannot invent Draw");source.DrawRequired(8);check(!AdmitPreviewRawEnd(proof()),"source overwritten by different draw rejects");
 source.DrawRequired(7);auto p=proof();p.currentApplication=false;check(!AdmitPreviewRawEnd(p),"app mismatch rejects");p=proof();p.contents.sameListGeneration=false;check(!AdmitPreviewRawEnd(p),"recording generation mismatch rejects");p=proof();p.contents.open=false;check(!AdmitPreviewRawEnd(p),"closed native list rejects");
 std::printf("raw epilogue contract: %d failures\n",failures);return failures?1:0;}

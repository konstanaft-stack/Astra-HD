#include "MenuPreview/EditorDrawPolicy.hpp"
#include <cstdio>
using namespace cvr::menu_preview;
int main(){int failures=0;auto check=[&](EditorDrawEvidence e,UiEditorDrawKind k,const char* name){if(ClassifyEditorDraw(e)!=k){std::printf("FAIL %s\n",name);++failures;}};
 check({},UiEditorDrawKind::Outside,"outside");
 check({true,false,true,true,1,1,0,false,false},UiEditorDrawKind::Unknown,"warmup stays original");
 check({true,true,true,true,1,0,1,false,false},UiEditorDrawKind::Portrait,"alias without draw shape restriction");
 check({true,true,true,true,3,3,0,false,false},UiEditorDrawKind::Panel,"all used bindings non alias");
 check({true,true,true,true,3,2,0,true,false},UiEditorDrawKind::Unknown,"unknown glyph is not negative");
 check({true,true,true,true,1,1,1,false,true},UiEditorDrawKind::Unknown,"native resource contradiction");
 check({true,true,true,true,0,0,0,false,false},UiEditorDrawKind::Panel,"known procedural UI");
 check({true,true,false,false,0,0,0,false,false},UiEditorDrawKind::Unknown,"missing PSV is not procedural");
 check({true,true,true,true,1,0,0,false,false},UiEditorDrawKind::Unknown,"retained intent without fresh join");
 EditorTargetKey learned{1,0,3,4,5,6,7,8};
 auto keyCheck=[&](EditorTargetKey k,bool expected,const char* name){if(EditorTargetMatches(learned,k)!=expected){std::printf("FAIL %s\n",name);++failures;}};
 keyCheck(learned,true,"same target across recording reset");
 auto stale=learned;stale.epoch++;keyCheck(stale,false,"new editor epoch warms up");
 stale=learned;stale.setup++;keyCheck(stale,false,"category setup warms up");
 stale=learned;stale.listLife++;keyCheck(stale,false,"recycled list object");
 stale=learned;stale.targetLife++;keyCheck(stale,false,"recycled resource address");
 stale=learned;stale.node++;keyCheck(stale,false,"different native node");
 std::printf("editor draw contract: %d failures\n",failures);return failures?1:0;}

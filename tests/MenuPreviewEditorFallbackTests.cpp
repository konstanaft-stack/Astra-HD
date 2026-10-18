#include "Runtimes/EditorUiFallbackPolicy.hpp"
#include <cstdio>
#include <cstdlib>
using cvr::xr::EditorUiFallbackBlock;
static void require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
int main(){
#ifdef CPVR_TEST_OLD_EDITOR_FALLBACK
    // Exact prior FrameLoop predicate, evaluated with the main-title inputs.
    const bool editorUiEnabled=true,menuUiSelected=false,editorUiCurrentKnown=false;
    const bool screen2=false,anchorValid=false;
    const bool mainTitleBlocked=editorUiEnabled&&!menuUiSelected&&(!editorUiCurrentKnown||screen2||anchorValid);
#else
    const bool mainTitleBlocked=EditorUiFallbackBlock(true,false,false,false,false);
#endif
    require(!mainTitleBlocked,"main title without an editor scope must keep normal menu fallback");
    require(EditorUiFallbackBlock(true,false,true,false,false),"first fresh editor waits for a complete separated packet");
    require(EditorUiFallbackBlock(true,false,false,true,false),"open category gap must not fall back to the mixed image");
    require(EditorUiFallbackBlock(true,false,false,false,true),"open anchor epoch preserves the editor requirement");
    require(!EditorUiFallbackBlock(true,false,false,false,false),"closed editor and cleaned noneditor state allow normal fallback");
    require(EditorUiFallbackBlock(true,false,false,true,true),"origin change does not end an open editor lifecycle");
    require(!EditorUiFallbackBlock(true,true,true,true,true),"accepted complete pair already satisfies the requirement");
    require(!EditorUiFallbackBlock(false,false,true,true,true),"feature disabled preserves ordinary rendering");
    std::puts("PASS editor fallback: main title, startup, category gap, closed lifecycle, origin, accepted pair and disabled mode");
}

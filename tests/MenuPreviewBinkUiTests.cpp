#include "MenuPreview/UiBinkShaderPolicy.hpp"
#include "MenuPreview/EditorDrawPolicy.hpp"
#include <cstdio>
#include <limits>
using namespace cvr::menu_preview;
// Exact scalar PSV0 declarations from the captured3536B PS/3318B VS.
// The audited DXBC/ASM inputs stay outside Git; no game binary is a test fixture.
static PipelineShaderEvidence Bink(){
 PipelineShaderEvidence s{};s.psHash=0x3D25937C1079FEE4ull;s.vsHash=0x1A5BF587E8197141ull;s.psBytes=3536;s.vsBytes=3318;
 s.srvBindingsKnown=true;s.srvBindingCount=5;s.colorOutputsKnown=true;s.colorOutputMask=1;
 for(uint32_t i=0;i<5;++i)s.srvBindings[i]={0,i,i,i==4?12u:2u};
 return s;
}
int main(){
 int failures=0,checks=0;auto check=[&](bool v,const char* name){++checks;if(!v){++failures;std::printf("FAIL %s\n",name);}};
 auto shader=Bink();
 check(EditorUiShaderLayout(shader)==UiEditorShaderLayout::BinkColorConvert,"actual Bink layout is qualified separately from old VS");
 auto bad=shader;bad.psHash++;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"other PS with four textures and buffer is unknown");
 bad=shader;bad.vsHash++;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"other VS is unknown");
 bad=shader;bad.psBytes++;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"wrong PS size is unknown");
 bad=shader;bad.vsBytes++;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"wrong VS size is unknown");
 bad=shader;bad.srvBindingCount=4;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"missing auxiliary declaration is unknown");
 bad=shader;bad.srvBindings[4].resourceKind=2;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"t4 Texture2D cannot substitute structured buffer");
 bad=shader;bad.srvBindings[4].registerSpace=1;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"wrong register space rejected");
 bad=shader;bad.srvBindings[2].lastRegister++;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"resource array rejected");
 bad=shader;bad.srvBindings[3].firstRegister=bad.srvBindings[3].lastRegister=2;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"duplicate or missing texture register rejected");
 bad=shader;bad.colorOutputMask=3;check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::Unknown,"extra color output rejected");
 bad=shader;bad.vsHash=0x8D749B2D19263CDFull;bad.vsBytes=3042;bad.srvBindingCount=1;bad.srvBindings[0]={0,77,77,2};
 check(EditorUiShaderLayout(bad)==UiEditorShaderLayout::TextureOnly,"old known UI path retained");
 UiSrvIdentity identity{0x100,7,11,true,false,false,false,false};
 UiSrvView buffer{0,96,7,D3D12_SRV_DIMENSION_BUFFER,DXGI_FORMAT_UNKNOWN,1,96,D3D12_BUFFER_SRV_FLAG_NONE,D3D12_RESOURCE_DIMENSION_BUFFER,true};
 check(UiBinkBufferKnown(buffer,identity),"one structured element fits real buffer");
 auto view=buffer;view.firstElement=2;view.elements=2;view.resourceWidth=384;check(UiBinkBufferKnown(view,identity),"complete nonzero-offset view fits buffer");
 view.resourceWidth=383;check(!UiBinkBufferKnown(view,identity),"view end outside resource rejected");
 view=buffer;view.resourceDimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;check(!UiBinkBufferKnown(view,identity),"texture resource cannot be auxiliary buffer");
 view=buffer;view.dimension=D3D12_SRV_DIMENSION_TEXTURE2D;check(!UiBinkBufferKnown(view,identity),"texture descriptor cannot be auxiliary buffer");
 view=buffer;view.flags=D3D12_BUFFER_SRV_FLAG_RAW;check(!UiBinkBufferKnown(view,identity),"RAW buffer rejected");
 view=buffer;view.stride=80;check(!UiBinkBufferKnown(view,identity),"wrong structure stride rejected");
 view=buffer;view.format=DXGI_FORMAT_R32_FLOAT;check(!UiBinkBufferKnown(view,identity),"typed buffer format rejected");
 view=buffer;view.elements=0;check(!UiBinkBufferKnown(view,identity),"zero elements rejected");
 view=buffer;view.firstElement=(std::numeric_limits<uint64_t>::max)();check(!UiBinkBufferKnown(view,identity),"range multiplication overflow rejected");
 view=buffer;view.resourceGeneration++;check(!UiBinkBufferKnown(view,identity),"recycled resource generation rejected");
 auto id=identity;id.present=false;check(!UiBinkBufferKnown(buffer,id),"missing descriptor rejected");
 id=identity;id.pending=true;check(!UiBinkBufferKnown(buffer,id),"pending mutation rejected");
 id=identity;id.overlap=true;check(!UiBinkBufferKnown(buffer,id),"overlapping mutation rejected");
 id=identity;id.unorderedAccess=true;check(!UiBinkBufferKnown(buffer,id),"UAV overwrite rejected");
 id=identity;id.cbvOnly=true;check(!UiBinkBufferKnown(buffer,id),"CBV overwrite rejected");
 id=identity;id.version=0;check(!UiBinkBufferKnown(buffer,id),"unknown descriptor version rejected");
 check(UiCopiedSrvView(buffer,true,true,false)==buffer,"accepted current descriptor copy retains typed payload");
 check(!UiCopiedSrvView(buffer,false,true,false).known,"rejected copy clears typed payload");
 check(!UiCopiedSrvView(buffer,true,false,false).known,"stale source copy clears typed payload");
 check(!UiCopiedSrvView(buffer,true,true,true).known,"pending copy clears typed payload");
 UiSrvView texture{};texture.dimension=D3D12_SRV_DIMENSION_TEXTURE2D;texture.resourceGeneration=7;texture.known=true;
 check(UiBinkTextureKnown(texture,identity),"typed Texture2D input known");
 for(unsigned alias=0;alias<4;++alias){
  EditorDrawEvidence e{true,true,true,true,5,1}; // Known auxiliary buffer is nonportrait.
  for(unsigned t=0;t<4;++t){check(UiBinkTextureKnown(texture,identity),"every texture slot needs typed proof");if(t==alias)++e.alias;else ++e.nonAlias;}
  check(ClassifyEditorDraw(e)==UiEditorDrawKind::Portrait,"portrait alias in each of four texture slots remains excluded");
 }
 EditorDrawEvidence e{true,true,true,true,5,5};check(ClassifyEditorDraw(e)==UiEditorDrawKind::Panel,"all four textures and auxiliary buffer proven nonportrait");
 e.nonAlias=4;e.unknown=true;check(ClassifyEditorDraw(e)==UiEditorDrawKind::Unknown,"one unresolved input never admits panel");
 std::printf("Bink UI policy: %d checks, %d failures\n",checks,failures);return failures?1:0;
}

#include "MenuPreview/RenderDocCapturePolicy.hpp"
#include <cassert>
#include <iostream>
using namespace cvr::menu_preview;
int main(){CaptureRequestIdentity r{42,1,123,200,0x9947,"run"};
 assert(ValidCaptureRequest(r,42,123,"run",100));assert(!ValidCaptureRequest(r,43,123,"run",100));
 assert(!ValidCaptureRequest(r,42,124,"run",100));assert(!ValidCaptureRequest(r,42,123,"foreign",100));
 assert(!ValidCaptureRequest(r,42,123,"run",200));r.previewEpoch=0;assert(!ValidCaptureRequest(r,42,123,"run",100));
 std::cout<<"RenderDoc request identity policy PASS\n";}

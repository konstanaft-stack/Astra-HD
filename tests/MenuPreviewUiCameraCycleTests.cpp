#include "MenuPreview/UiCameraCyclePolicy.hpp"
#include <cstdio>
int main(){
    using cvr::menu_preview::CapturedCameraRevision;
    if(!CapturedCameraRevision(9,0,5,9,0,5)){std::puts("FAIL exact completed camera revision must advance");return 1;}
    if(CapturedCameraRevision(9,0,7,9,0,5)){std::puts("FAIL older same-eye capture reused two turns later");return 2;}
    if(CapturedCameraRevision(9,1,5,9,0,5)){std::puts("FAIL opposite-eye capture admitted");return 3;}
    if(CapturedCameraRevision(10,0,5,9,0,5)){std::puts("FAIL previous menu epoch admitted");return 4;}
    if(CapturedCameraRevision(9,0,0,9,0,0)||CapturedCameraRevision(0,0,5,0,0,5)||CapturedCameraRevision(9,2,5,9,2,5)){std::puts("FAIL uninitialised identity admitted");return 5;}
    std::puts("PASS captured camera revision; rejects stale same-eye, opposite-eye, other menu and empty identities");
}

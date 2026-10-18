#include "MenuPreview/TargetDescriptor.hpp"
#include <cassert>
int main(){using namespace cvr::menu_preview;uintptr_t a;assert(!DefaultTargetSlot(0x10000,0,a));assert(!DefaultTargetSlot(1,1,a));assert(!DefaultTargetSlot(0x10000,65537,a));assert(DefaultTargetSlot(0x10000,1,a)&&a==0x3f1f0);assert(DefaultTargetSlot(0x10000,2,a)&&a==0x3f2a0);assert(!DefaultTargetSlot(UINTPTR_MAX-1,1,a));}

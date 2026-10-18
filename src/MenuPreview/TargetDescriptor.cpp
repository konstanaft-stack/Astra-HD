#include "MenuPreview/TargetDescriptor.hpp"
#include "Utils/MemorySafe.hpp"
#include <cstring>
namespace cvr::menu_preview {
namespace {
bool Pinned(uintptr_t exe)noexcept{
    __try {
        if(!exe)return false;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x1000)return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe+dos->e_lfanew);
        constexpr unsigned char bytes[]{0x8B,0x01,0x4C,0x8B,0x0D,0x27,0xE8,0x23,0x03,0xFF,0xC8,0x4C,0x69,0xC0,0xB0,0x00};
        return nt->Signature==IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp==0x68AF45EA &&
            nt->OptionalHeader.SizeOfImage==0x4EFC000 && !std::memcmp(reinterpret_cast<const void*>(exe+0x1FA1F8),bytes,sizeof(bytes));
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
TargetDescriptor ResolvePreviewDefaultTarget(uintptr_t backing)noexcept{
    TargetDescriptor row;
    static const uintptr_t exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const bool pinned=Pinned(exe);
    if(!pinned || backing<0x10000 || backing>UINTPTR_MAX-0x18)return row;
    if(!ReadU32Safe(backing+0x18,&row.textureID) || !ReadPtrSafe(exe+0x3438A28,&row.manager))return row;
    uintptr_t slot{};if(!DefaultTargetSlot(row.manager,row.textureID,slot))return row;
    uintptr_t managerAgain{};uint32_t idAgain{};
    if(!ReadPtrSafe(slot,&row.expectedHandle) || !row.expectedHandle ||
       !ReadPtrSafe(exe+0x3438A28,&managerAgain) || !ReadU32Safe(backing+0x18,&idAgain) ||
       managerAgain!=row.manager || idAgain!=row.textureID){row.expectedHandle=0;return row;}
    row.known=true;return row;
}
}

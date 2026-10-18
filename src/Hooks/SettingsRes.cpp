// SettingsRes -- one hook, one file. Installed from the registry it registers itself with at the
// bottom; see Hooks/Hook.hpp for why the order is declared here rather than in a boot function.
//
// The render-settings object, as the engine finalises it. The live PPD owner writes pending
// dimensions only here; the legacy author override remains outside that ownership.
// The reported size the XR runtime recommends is a REFERENCE value --
// printing it beside the others has already been misread once as evidence of a forced override.

#include "Core/VrCoreShared.hpp"
#include "Overlay/PpdNativeApply.hpp"
#include "Framegen/NativeFgOutput.hpp"
#include "Hooks/Hook.hpp"
#include "Hooks/CetLifecycle.hpp"
#include "Hooks/NativeSettingsAdmission.hpp"
#include "Hooks/Trampoline.hpp"
#include "Utils/AobScanner.hpp"
#include "Utils/MemorySafe.hpp"

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace {
uint64_t g_settingsResHits = 0;
std::mutex resizeGateMutex;
uint64_t drainedRevision{};
bool drained{};
bool NativeFgResizeReady(){
    if(cvr::cet::LifecycleRequested())return true; // the native apply entry owns quiescence
    if(!cvr::framegen::NativeFgStereoRequested())return true;
    const auto request=cvr::ppd::ReadNativeRequestedSize();
    if(!request||request->stage!=cvr::ppd::NativeApplyStage::Requested)return true;
    std::lock_guard lock(resizeGateMutex);
    if(drainedRevision!=request->revision){
        drainedRevision=request->revision;
        drained=cvr::framegen::DrainNativeFgResizeQueues(2000);
        Log("[NATIVE-FG-RESIZE] revision=%llu allowPendingWrite=%d\n",drainedRevision,int(drained));
    }
    return drained;
}
}  // namespace
extern "C" {__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PpdSettingsHookVisits{0};}

extern "C" void __fastcall OnSettingsResCallback(void* settingsPtr) {
    CyberpunkVR_PpdSettingsHookVisits.fetch_add(1,std::memory_order_relaxed);
    g_settingsResHits++;

    const uintptr_t settings = reinterpret_cast<uintptr_t>(settingsPtr);
    if (settings < 0x10000) {
        return;
    }

    const uintptr_t prev = g_settingsResPtr;
    g_settingsResPtr = settings;
    const bool applyAllowed=!SettingsResolutionOverrideSuspended()&&NativeFgResizeReady();
    if (!cvr::ppd::ProcessNativeSizeAtSettingsHook(settings,applyAllowed,cvr::hooks::AdmitNativePpdWrite,settingsPtr))
        ApplySettingsResolutionOverride(settings);

    if (settings != prev || ((g_settingsResHits % 600) == 1)) {
        uint32_t activeWidth = 0;
        uint32_t activeHeight = 0;
        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;
        ReadU32Safe(settings + 0x18, &activeWidth);
        ReadU32Safe(settings + 0x1C, &activeHeight);
        ReadU32Safe(settings + 0x84, &targetWidth);
        ReadU32Safe(settings + 0x88, &targetHeight);
        // On CHANGE only: this line is a status, and a status that repeats is noise. It printed on
        // every hook call, 201 identical lines in one session.
        static uint32_t s_lastA = 0, s_lastB = 0, s_lastC = 0, s_lastD = 0;
        const bool changed = (activeWidth != s_lastA) || (activeHeight != s_lastB) ||
                             (targetWidth != s_lastC) || (targetHeight != s_lastD);
        s_lastA = activeWidth; s_lastB = activeHeight; s_lastC = targetWidth; s_lastD = targetHeight;
        if (changed)
        Log("SettingsRes hook: ptr=%p active=%ux%u target=%ux%u forced=%ux%u\n",
            reinterpret_cast<void*>(settings),
            activeWidth,
            activeHeight,
            targetWidth,
            targetHeight,
            GetForcedRenderWidthValue(),
            GetForcedRenderHeightValue());
    }
}

bool InstallSettingsResHook() {
    const char* pattern = "\x39\x41\x18\x75\x00\x8B\x81\x88\x00\x00\x00\x39\x41\x1C";
    const char* mask = "xxxx?xxxxxxxxx";
    uint8_t* found = static_cast<uint8_t*>(FindPattern("Cyberpunk2077.exe", pattern, mask));
    if (!found) return false;

    constexpr int replaceLen = 14;
    void* tramp = AllocateTrampoline(found, 512);
    if (!tramp) return false;

    uint8_t* code = static_cast<uint8_t*>(tramp);
    int pos = 0;

    code[pos++] = 0x9C;
    code[pos++] = 0x50;
    code[pos++] = 0x51;
    code[pos++] = 0x52;
    code[pos++] = 0x41; code[pos++] = 0x50;
    code[pos++] = 0x41; code[pos++] = 0x51;
    code[pos++] = 0x41; code[pos++] = 0x52;
    code[pos++] = 0x41; code[pos++] = 0x53;
    code[pos++] = 0x55;

    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xEC; code[pos++] = 0x40;
    code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x04; code[pos++] = 0x24;
    code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x4C; code[pos++] = 0x24; code[pos++] = 0x10;
    code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x54; code[pos++] = 0x24; code[pos++] = 0x20;
    code[pos++] = 0x0F; code[pos++] = 0x11; code[pos++] = 0x5C; code[pos++] = 0x24; code[pos++] = 0x30;

    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0xE5;
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xE4; code[pos++] = 0xF0;
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xEC; code[pos++] = 0x20;

    WriteMovRaxImm64(code, pos, reinterpret_cast<uintptr_t>(OnSettingsResCallback));
    code[pos++] = 0xFF; code[pos++] = 0xD0;

    code[pos++] = 0x48; code[pos++] = 0x89; code[pos++] = 0xEC;

    code[pos++] = 0x0F; code[pos++] = 0x10; code[pos++] = 0x04; code[pos++] = 0x24;
    code[pos++] = 0x0F; code[pos++] = 0x10; code[pos++] = 0x4C; code[pos++] = 0x24; code[pos++] = 0x10;
    code[pos++] = 0x0F; code[pos++] = 0x10; code[pos++] = 0x54; code[pos++] = 0x24; code[pos++] = 0x20;
    code[pos++] = 0x0F; code[pos++] = 0x10; code[pos++] = 0x5C; code[pos++] = 0x24; code[pos++] = 0x30;
    code[pos++] = 0x48; code[pos++] = 0x83; code[pos++] = 0xC4; code[pos++] = 0x40;

    code[pos++] = 0x5D;
    code[pos++] = 0x41; code[pos++] = 0x5B;
    code[pos++] = 0x41; code[pos++] = 0x5A;
    code[pos++] = 0x41; code[pos++] = 0x59;
    code[pos++] = 0x41; code[pos++] = 0x58;
    code[pos++] = 0x5A;
    code[pos++] = 0x59;
    code[pos++] = 0x58;
    code[pos++] = 0x9D;

    code[pos++] = 0x39; code[pos++] = 0x41; code[pos++] = 0x18;
    // Native width JNE leaves the overwritten region and returns true. It must
    // not jump to our relocated height CMP with the saved width still in EAX.
    // CP2077 2.31: game21C58D ->21C5A8, bytes75 19 -> mov al,1; ret.
    const auto* changedTarget = found + 5 + static_cast<int8_t>(found[4]);
    if(found[3]!=0x75 || changedTarget<found+replaceLen ||
       changedTarget[0]!=0xB0 || changedTarget[1]!=0x01 || changedTarget[2]!=0xC3)return false;
    const auto changedRelative = reinterpret_cast<intptr_t>(changedTarget) -
                                 reinterpret_cast<intptr_t>(code+pos+6);
    if(changedRelative<INT32_MIN || changedRelative>INT32_MAX)return false;
    code[pos++] = 0x0F; code[pos++] = 0x85;
    const auto changedRel32=static_cast<int32_t>(changedRelative);
    std::memcpy(code+pos,&changedRel32,sizeof(changedRel32));pos+=4;
    code[pos++] = 0x8B; code[pos++] = 0x81; code[pos++] = 0x88; code[pos++] = 0x00; code[pos++] = 0x00; code[pos++] = 0x00;
    code[pos++] = 0x39; code[pos++] = 0x41; code[pos++] = 0x1C;

    const auto returnRelative = reinterpret_cast<intptr_t>(found+replaceLen) -
                                reinterpret_cast<intptr_t>(code+pos+5);
    if(returnRelative<INT32_MIN || returnRelative>INT32_MAX)return false;
    code[pos++] = 0xE9;
    const auto returnRel32=static_cast<int32_t>(returnRelative);
    std::memcpy(code+pos,&returnRel32,sizeof(returnRel32));
    pos += 4;

    // The shared trampoline arena can have been allocated near another hook.
    // Check THIS forward branch too, before modifying the original code page.
    const auto siteRelative=reinterpret_cast<intptr_t>(code)-reinterpret_cast<intptr_t>(found+5);
    if(siteRelative<INT32_MIN || siteRelative>INT32_MAX)return false;
    const auto siteRel32=static_cast<int32_t>(siteRelative);
    DWORD oldProtect;
    if(!FlushInstructionCache(GetCurrentProcess(),code,pos) ||
       !VirtualProtect(found, replaceLen, PAGE_EXECUTE_READWRITE, &oldProtect))return false;
    found[0] = 0xE9;
    std::memcpy(found+1,&siteRel32,sizeof(siteRel32));
    for (int i = 5; i < replaceLen; ++i) found[i] = 0x90;
    VirtualProtect(found, replaceLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), found, replaceLen);
    return true;
}

CVR_HOOK("SettingsRes", ::cvr::hooks::Stage::Boot, 85, InstallSettingsResHook);

#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "settings-replay-under-test.hpp"

void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct CodePage {
    uint8_t* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ~CodePage() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
};
int main() try {
    CodePage memory; Check(memory.data != nullptr, "allocate executable fixture");
    auto* code = memory.data;
    auto* found = code + 0x202;
    // Pinned CP2077 2.31: predicate RVA21C584, hook21C58A. The first MOV
    // occurred before the hook, so both executable paths receive its saved EAX
    // explicitly in EDX. Exact remaining native bytes are the independent oracle.
    constexpr uint8_t nativeBytes[]{
        0x39,0x41,0x18,0x75,0x19,0x8b,0x81,0x88,0,0,0,0x39,0x41,0x1c,
        0x75,0x0e,0x8a,0x81,0x91,0,0,0,0x38,0x41,0x28,0x75,0x03,0x32,0xc0,0xc3,0xb0,0x01,0xc3};
    std::memcpy(found, nativeBytes, sizeof(nativeBytes));
    found[-2] = 0x8b; found[-1] = 0xc2; // mov eax,edx
    code[0] = 0x8b; code[1] = 0xc2;
    int position = 2;
    Check(EmitProductionReplay(code, position, found), "production emitter refused verified native predicate");
    DWORD prior{};
    Check(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &prior) != 0, "protect executable fixture");
    Check(FlushInstructionCache(GetCurrentProcess(), code, 4096) != 0, "flush executable fixture");
    using Predicate = bool (*)(void*, uint32_t);
    auto original = reinterpret_cast<Predicate>(found - 2);
    auto relocated = reinterpret_cast<Predicate>(code);
    std::array<uint32_t,64> settings{};
    auto* bytes = reinterpret_cast<uint8_t*>(settings.data());
    settings[0x18/4] = 1920; settings[0x1c/4] = 1080;
    settings[0x84/4] = 1080; settings[0x88/4] = 1080;
    Check(original(settings.data(), 1080), "native oracle does not detect changed width");
    Check(relocated(settings.data(), 1080), "width mismatch lost when pending width equals active height");
    unsigned cases = 0;
    for (uint32_t width : {976u,1080u,1600u,1920u})
    for (uint32_t height : {1040u,1080u,1728u,1920u})
    for (uint32_t savedWidth : {976u,1080u,1600u,1728u,1920u})
    for (uint32_t targetHeight : {1040u,1080u,1600u,1728u,1920u})
    for (uint8_t flag : {uint8_t(0),uint8_t(1)}) {
        settings[0x18/4] = width; settings[0x1c/4] = height;
        settings[0x84/4] = savedWidth + 64; // callback can change pending after EAX was saved
        settings[0x88/4] = targetHeight; bytes[0x28] = 0; bytes[0x91] = flag;
        const bool expected = width != savedWidth || height != targetHeight || flag != 0;
        Check(original(settings.data(), savedWidth) == expected, "native oracle disagrees with recovered predicate");
        Check(relocated(settings.data(), savedWidth) == expected, "relocated predicate differs from native bytes");
        ++cases;
    }
    std::cout << "PASS " << cases << " executable native/replay cases: width branch, height branch, mode flag and saved EAX\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

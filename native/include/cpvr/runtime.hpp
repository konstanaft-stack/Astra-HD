#pragma once
#include <windows.h>
namespace cpvr {
bool start_runtime(HMODULE);
void stop_runtime() noexcept;
}

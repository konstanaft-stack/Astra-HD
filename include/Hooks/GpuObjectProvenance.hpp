#pragma once
#include <unknwn.h>

// Names only, no retained references or resource-state changes. Inert unless
// the existing per-process D3D validation diagnostic is explicitly enabled.
void NameGpuObjectProvenance(IUnknown* object,const wchar_t* kind,const void* caller) noexcept;

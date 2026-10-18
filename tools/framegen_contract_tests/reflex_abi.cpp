#if defined(CVR_REFLEX_OFFICIAL)
#include <sl_reflex.h>
#else
#include "Framegen/NativeReflexTypes.hpp"
#endif
#include <cstdio>
#include <cstddef>
int main(){
    std::printf("options=%zu mode=%zu limit=%zu state=%zu available=%zu reports=%zu report=%zu frame=%zu gpuActive=%zu gpuFrame=%zu\n",
        sizeof(sl::ReflexOptions),offsetof(sl::ReflexOptions,mode),offsetof(sl::ReflexOptions,frameLimitUs),sizeof(sl::ReflexState),
        offsetof(sl::ReflexState,latencyReportAvailable),offsetof(sl::ReflexState,frameReport),sizeof(sl::ReflexReport),
        offsetof(sl::ReflexReport,frameID),offsetof(sl::ReflexReport,gpuActiveRenderTimeUs),offsetof(sl::ReflexReport,gpuFrameTimeUs));
}

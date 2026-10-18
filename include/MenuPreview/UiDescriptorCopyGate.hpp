#pragma once
#include <cstdint>

namespace cvr::menu_preview {
// A negative hint can only avoid work already skipped by the locked world path.
// A positive hint, bulk range, or live preview always retains the full versioned
// mutation/invalidation path. No descriptor or resource owner is read here.
template<class Handle,class Recent,class Persistent,class EpochQuery>
bool UiCanSkipInactiveSingleCopy(uint32_t ranges,const Handle* starts,const uint32_t* sizes,
    const Recent& recent,const Persistent& persistent,EpochQuery&& epoch)noexcept {
    return ranges==1&&starts&&(!sizes||sizes[0]==1)&&
        !recent.MayContain(starts[0].ptr)&&!persistent.MayContain(starts[0].ptr)&&epoch()==0;
}
}

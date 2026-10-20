#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace cvr::framegen {
// Caller owns the generator. Its work fence covers every command that reads
// these input leases. A device-removed sentinel is not a completion receipt.
template<class WorkRange>
size_t RetireCompletedInputWork(WorkRange& work,uint64_t completed) noexcept {
    if(completed==std::numeric_limits<uint64_t>::max())return 0;
    size_t retired{};
    for(auto& item:work)if(item.fence&&completed>=item.fence)
        for(auto& input:item.inputs)if(input){input.reset();++retired;}
    return retired;
}
}

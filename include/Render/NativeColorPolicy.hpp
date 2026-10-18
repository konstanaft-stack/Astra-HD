#pragma once
#include <cstdint>
#include <optional>
#include <utility>

namespace cvr::render {
struct NativeColorDescription {
    uint64_t width{};
    uint32_t height{},format{},viewDimension{},mip{};
    bool explicitView{};
};
inline bool NativeColorCandidate(const NativeColorDescription& source,uint32_t width,uint32_t height) {
    // Luke42DA AF0F30: explicit Texture2D RTV, mip zero, native pending game
    // dimensions and resource format27. Formats29/10 belong to a different table.
    return source.explicitView && source.viewDimension==4 && source.mip==0 &&
        source.format==27 && source.width==width && source.height==height &&
        (width>256 || height>256);
}
template<class Payload> class NativeColorSequence {
    std::optional<Payload> previous;
public:
    std::optional<Payload> Bind(bool window,bool candidate,const Payload& value) {
        if(window)return std::exchange(previous,{});
        previous=candidate?std::optional<Payload>{value}:std::nullopt;
        return {};
    }
    void Reset(){previous.reset();}
    const std::optional<Payload>& Previous()const{return previous;}
};
}

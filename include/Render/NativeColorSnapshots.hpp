#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>

namespace cvr::render {
struct NativeColorSnapshot {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    Microsoft::WRL::ComPtr<IUnknown> lifetime;
    explicit operator bool()const{return bool(texture);}
};
// Caller serializes Record. The lifetime token must accompany every GPU read.
// Existing CommandResources retains tokens through reset/replay/completion.
class NativeColorSnapshots {
    struct Cell;
    std::array<std::shared_ptr<Cell>,8> cells;
public:
    NativeColorSnapshot Record(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
};
}

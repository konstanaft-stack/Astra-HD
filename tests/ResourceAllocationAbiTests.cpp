#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
static_assert(offsetof(ID3D12Device4Vtbl,CreateCommittedResource1)/sizeof(void*)==53);
static_assert(offsetof(ID3D12Device8Vtbl,CreateCommittedResource2)/sizeof(void*)==69);
static_assert(offsetof(ID3D12Device10Vtbl,CreateCommittedResource3)/sizeof(void*)==76);
int main(){return 0;}

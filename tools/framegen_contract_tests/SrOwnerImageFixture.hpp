#pragma once
// Test-only packed uploads/readbacks for the actual CP tuple39/34/26.
inline uint16_t OwnerHalf(float value){const uint32_t b=std::bit_cast<uint32_t>(value);const uint32_t sign=(b>>16)&0x8000u;
    const int exponent=int((b>>23)&255)-112;const uint32_t mantissa=b&0x7fffffu;
    if(exponent<=0)return uint16_t(sign);if(exponent>=31)return uint16_t(sign|0x7c00u);
    return uint16_t(sign|(uint32_t(exponent)<<10)|((mantissa+0x1000u)>>13));}
inline float OwnerFloat(uint16_t value){const uint32_t sign=uint32_t(value&0x8000u)<<16;const uint32_t exp=(value>>10)&31u;const uint32_t mant=value&1023u;
    if(!exp)return mant?std::ldexp(float(mant),-24)*(sign?-1.f:1.f):std::bit_cast<float>(sign);
    return std::bit_cast<float>(sign|((exp+112)<<23)|(mant<<13));}
struct OwnerImage {
    ComPtr<ID3D12Resource> resource,upload,readback;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};uint64_t bytes{};
    unsigned width{},height{},channels{},pixelBytes{};DXGI_FORMAT format{};
    OwnerImage(ID3D12Device*device,unsigned c,bool output,bool nativeOutput=false,bool actual=false):channels(c){
        width=actual?960:32;height=actual?960:24;if(actual&&nativeOutput){width*=2;height*=2;}
        format=actual?(c==1?DXGI_FORMAT_R32_TYPELESS:c==2?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R11G11B10_FLOAT):
            (c==1?DXGI_FORMAT_R32_FLOAT:c==2?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT);
        pixelBytes=actual?4:c*4;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
        d.Flags=output||(actual&&c==4)?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&resource)),"typed image");
        device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);upload=Buffer(device,bytes,D3D12_HEAP_TYPE_UPLOAD);if(output)readback=Buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
    }
    DXGI_FORMAT View()const{return format==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_R32_FLOAT:format;}
    template<class Pixel>void Upload(ID3D12GraphicsCommandList*list,Pixel pixel,D3D12_RESOURCE_STATES state){void*mapped{};D3D12_RANGE none{};Hr(upload->Map(0,&none,&mapped),"typed upload");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto*out=static_cast<char*>(mapped)+y*footprint.Footprint.RowPitch+x*pixelBytes;
            if(format==DXGI_FORMAT_R16G16_FLOAT){uint16_t v[2]{OwnerHalf(pixel(x,y,0)),OwnerHalf(pixel(x,y,1))};std::memcpy(out,v,4);}
            else if(format==DXGI_FORMAT_R11G11B10_FLOAT){const uint32_t r=(OwnerHalf(pixel(x,y,0))+8u)>>4,g=(OwnerHalf(pixel(x,y,1))+8u)>>4,b=(OwnerHalf(pixel(x,y,2))+16u)>>5;
                const uint32_t packed=(r&2047u)|((g&2047u)<<11)|((b&1023u)<<22);std::memcpy(out,&packed,4);}
            else for(unsigned c=0;c<channels;++c){const float v=pixel(x,y,c);std::memcpy(out+c*4,&v,4);}}
        upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=footprint;b.pResource=resource.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&b,0,0,0,&a,nullptr);Barrier(list,resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);
    }
    void CopyBack(ID3D12GraphicsCommandList*list){Barrier(list,resource.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION a{},b{};
        a.pResource=resource.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;list->CopyTextureRegion(&b,0,0,0,&a,nullptr);}
    float ReadMotion(unsigned x,unsigned y){void*mapped{};D3D12_RANGE range{0,size_t(bytes)};Hr(readback->Map(0,&range,&mapped),"typed readback");const auto*p=static_cast<char*>(mapped)+y*footprint.Footprint.RowPitch+x*pixelBytes;float value{};
        if(format==DXGI_FORMAT_R16G16_FLOAT){uint16_t half{};std::memcpy(&half,p,2);value=OwnerFloat(half);}else std::memcpy(&value,p,4);D3D12_RANGE none{};readback->Unmap(0,&none);return value;}
};
